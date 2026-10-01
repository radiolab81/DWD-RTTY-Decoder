/*
 * main.c - RTTY-Decoder fuer den Seewetterdienst des DWD (DDH47, DDH9, DDK9 ...)
 *          fuer ATmega328P, bare metal (avr-gcc / avr-libc)
 * ==============================================================================
 *
 * Reines avr-gcc / avr-libc, KEIN Arduino-Core, kein printf, kein float.
 * Ausgabe: der empfangene Klartext auf USART0 (9600 8N1).
 *
 * WAS WIRD EMPFANGEN?
 * -------------------
 * Der Deutsche Wetterdienst sendet Seewetterberichte, Warnungen und Wetter-
 * meldungen als Funkfernschreiben (RTTY, Sendeart F1B) aus Pinneberg:
 *
 *      Frequenz      Rufzeichen   Baud   Hub (Abstand der Toene)
 *      147.3   kHz   DDH47        50     85 Hz   (+/- 42.5 Hz)   Langwelle
 *      4583 ... 15988 kHz  DDK2, DDH7, DDK9, DDH9, DDH8 ...
 *                                 50     450 Hz  (+/- 225 Hz)    Kurzwelle
 *
 * Alle Sender arbeiten mit 50 Baud; unterschiedlich ist nur der Hub.  Genau
 * das erkennt der Decoder selbst - es muss nichts eingestellt werden.
 *
 * FSK, MARK UND SPACE
 * -------------------
 * FSK (Frequency Shift Keying) sendet zwei Toene: "Mark" (logisch 1, auch der
 * Ruhezustand der Leitung) und "Space" (logisch 0).  Der Empfaenger (SSB-
 * Stellung) setzt sie mit seinem BFO auf Audiotoene um, z.B. 1257.5 / 1342.5
 * Hz bei 85 Hz Hub und Mitte 1300 Hz.  Ob Mark der hoehere oder der tiefere
 * Ton ist, haengt von Seitenband (USB/LSB) und Sender ab - auch das wird
 * automatisch erkannt (siehe "POLARITAET").
 *
 * DAS ZEICHENFORMAT: ITA2 (Baudot-Murray), asynchron
 * --------------------------------------------------
 *   Ruhe = Mark
 *   ___     ___________________________________________
 *      |___|  D0  D1  D2  D3  D4 | Stop 1.5 Bit ...
 *      Start(Space)  5 Datenbits, LSB zuerst
 *
 *   Ein Zeichen dauert 1 + 5 + 1.5 = 7.5 Bit = 150 ms (bei 50 Baud, 20 ms/Bit).
 *   Weil es keinen durchlaufenden Takt gibt, beginnt jedes Zeichen mit der
 *   fallenden Flanke des Startbits; der Empfaenger synchronisiert seinen
 *   Bittakt bei JEDEM Zeichen neu und tastet dann in der Bitmitte ab.
 *
 *   Wichtige Regel fuer die Einrastung: Ein echtes Startbit folgt immer auf
 *   das 1.5 Bit lange Stopbit, vor der Flanke muss also mindestens ca. 1.15
 *   Bit lang Mark gelegen haben.  Das Phasing-Muster "RYRYRY..." hat eine
 *   Periode von 15 Bit; ohne diese Regel kann sich ein Empfaenger auf eine
 *   Flanke MITTEN im Zeichen einrasten und dort dauerhaft haengen bleiben.
 *   (Innere Flanken von RYRY haben nur 1 Bit Mark davor.)
 *
 *   5 Bit koennen nur 32 Zeichen darstellen: darum gibt es zwei "Ebenen",
 *   umgeschaltet mit den Codes LTRS (11111 = Buchstaben) und FIGS (11011 =
 *   Ziffern/Zeichen).  Der DWD sendet nach jedem Wortzwischenraum ein LTRS.
 *
 * SIGNALKETTE (alles Festkomma, ganzzahlig)
 * -----------------------------------------
 *
 *  ADC0 --8 kHz--> [DC-Abzug] --+--> [Goertzel-Sweep 625..2500 Hz]  (nur beim SUCHEN)
 *                               |        |
 *                               |        v  Spektrum -> belegte Bandbreite
 *                               |        |             -> Mitte + Hub-Klasse
 *                               v        |
 *                    [NCO-Mischer cos/sin] <--- NCO-Startfrequenz
 *                               |
 *                          [CIC3 / 4]
 *                               |
 *                        2 kHz komplex (I,Q)
 *                               |
 *                [2 Einpol-Tiefpaesse, Bandbreite je nach Hub]
 *                               |
 *                    [Begrenzer: |z| = konstant]
 *                               |
 *                [Diskriminator  Im(z[n]*conj(z[n-1]))]  -> Momentanfrequenz
 *                               |
 *              +----------------+------------------+
 *              |                                   |
 *   [Ton-Tracker: misst Lage der           [2 Zeichenempfaenger, Polaritaet
 *    Mark/Space-Toene, fuehrt NCO nach]     "Mark tief" und "Mark hoch"]
 *                                                  |
 *                                        [ITA2 -> ASCII] -> UART
 *
 * WARUM 8 kHz / 2 kHz?
 *   - Timer1 teilt 16 MHz exakt durch 8000 (OCR1A = 1999): kein Rundungsfehler.
 *   - 8 kHz erlaubt Audiotoene bis 3 kHz (Nyquist 4 kHz) - die Signalmitte
 *     darf also frei irgendwo zwischen 625 und 2500 Hz liegen (Suchbereich).
 *   - Nach Mischen und Dezimation 4:1 bleiben 2000 Hz komplex, d.h. +/-1000 Hz
 *     Basisband: genug fuer +/-225 Hz Hub samt Modulationsseitenbaendern.
 *   - 2000 / 50 Baud = genau 40 Abtastwerte je Bit: der Bittakt-Zaehler ist
 *     exakt ganzzahlig, es gibt keinen Rundungsfehler.
 *
 * AUTOMATISCHE ERKENNUNG VON FREQUENZ UND HUB ("SUCHEN")
 * ------------------------------------------------------
 * Wo im Audio steht das Signal, und wie gross ist der Hub?  Beides steckt im
 * Spektrum.  Ein Goertzel-Filter berechnet die Energie EINER Frequenz aus
 * einem Block von N Abtastwerten - ein DFT-Punkt mit nur zwei Speicher-
 * zellen und einer Multiplikation je Abtastwert (siehe unten).  Wir rechnen
 * nacheinander Bloecke mit N = 256 (32 ms) fuer die DFT-Punkte k = 20 ... 80,
 * also 625 ... 2500 Hz in 31.25-Hz-Schritten (61 Punkte, ein Durchlauf
 * dauert knapp 2 s).  Die Ergebnisse werden ueber mehrere Durchlaeufe
 * gemittelt (die Daten wechseln ja zwischen den Toenen).
 *
 * Problem: Bei 85 Hz Hub liegen die zwei Toene nur gut 2.5 Punkte auseinander
 * und verschmieren mit den Modulationsseitenbaendern zu EINEM Berg - "zwei
 * Spitzen suchen" funktioniert hier nicht.  Darum wird stattdessen die
 * BELEGTE BANDBREITE gemessen (Details bei scan_detect()):
 *   - Es muss eine deutliche Spitze geben (>= 10.8 dB ueber dem Rauschboden).
 *   - Die Frequenzen, an denen das Spektrum 9 dB unter die Spitze faellt,
 *     liegen links und rechts des Berges; ihre Mitte ist die Signalmitte
 *     (-> NCO-Startfrequenz), ihr Abstand die Bandbreite.
 *   - Gemessene Bandbreite (Hub 85 / 170 / 450 Hz): ca. 150-170 / 260 /
 *     530 Hz.  Daraus wird die Hub-Klasse bestimmt; sie legt die Filter-
 *     bandbreite fest und dient im Betrieb als Sollwert der Hub-Kontrolle.
 * Sendet ein Sender gerade nur Ruhe-Mark (ein einziger Ton, nur ca. 55 Hz
 * breit), wird das als unmoduliert abgelehnt: der Decoder wartet dann, bis
 * Daten fliessen.
 *
 * TRAEGERNACHFUEHRUNG ("BFO wandert")
 * ------------------------------------
 * Ein Communications-Receiver steht nicht "felsenfest": Temperaturdrift,
 * Nachstimmen, Doppler.  Wandert der BFO, wandert die ganze Tonpaarung.
 * Der Ton-Tracker mittelt alle 64 Abtastwerte (32 ms) die Momentanfrequenz
 * getrennt fuer die "obere" und die "untere" Haelfte des Signals:
 *   - sind BEIDE Toene im Block vorhanden:  Mitte = (oben + unten) / 2
 *     -> unabhaengig davon, wie unausgeglichen die Daten sind.
 *   - ist NUR EIN Ton da (Ruhe-Mark, lange gleiche Bits): die Lage dieses
 *     Tones minus der Hub/2, auf der Seite, auf der er liegt, ergibt die Mitte.
 * Der Fehler steuert die NCO-Frequenz in kleinen Schritten nach (eine
 * Frequenzregelschleife, "FLL").  Der Hub selbst wird dabei als bekannt
 * (Klassenwert 85 / 170 / 450 Hz) angenommen; die Suche hat ihn ja gerade
 * aus dem Spektrum bestimmt.
 * Fangbereich: ca. +/- 0.6 x Hub/2 (also +/-25 Hz bei 85 Hz Hub!).  Reicht
 * das nicht, geht der Tracker verloren und die Suche beginnt neu.
 * Weil nur die Frequenz (nicht die Phase) ausgewertet wird, ist keine
 * Phasenverriegelung noetig - der Empfang ist nichtkoharent.
 *
 * POLARITAET
 * ----------
 * Zwei komplette Zeichenempfaenger laufen parallel: einer mit "Mark = tieferer
 * Ton", einer mit "Mark = hoeherer Ton".  Bei falscher Polaritaet fehlt das
 * Stopbit fast immer (Rahmenfehler) und der Text ist Muell.  Jeder Empfaenger
 * fuehrt Punkte (richtiges Zeichen +1, Rahmenfehler -2); ausgegeben wird der
 * mit den meisten Punkten.
 *
 * WANN WIRD NEU GESUCHT?  (vier unabhaengige Waechter)
 * -----------------------------------------------------
 *   1. Kein Signal: Der Tracker findet laenger als LOSS_TIMEOUT_S keinen Ton,
 *      oder der Pegel liegt > 12 dB unter dem Wert beim Einrasten.
 *   2. Fehlerrate: Beide Polaritaets-Empfaenger haben Punktestand <
 *      SCORE_LOST, d.h. es kommen nur noch Rahmenfehler (Muell).
 *   3. Hub-Kontrolle: Der Tracker misst bei jedem Block mit beiden Toenen
 *      den Tonabstand; weicht er laenger als 3 s um > 35 % von der Klasse
 *      ab, wird neu gesucht (Sender gewechselt, z.B. 85 -> 450 Hz).
 *   4. Zeichen-Watchdog: WATCHDOG_S Sekunden kein einziges gueltiges Zeichen.
 *      Das faengt den heimtueckischen Fall ab, dass der Tracker nach einem
 *      Senderwechsel auf EINEN der beiden neuen Toene einrastet - fuer ihn
 *      sieht das aus wie Ruhe-Mark.  Bei echtem Ruhe-Mark kostet das
 *      Neusuchen nichts: die Suche lehnt einen unmodulierten Ton ab und
 *      wartet auf Daten.  Preis: nach einer laengeren Sendepause gehen die
 *      ersten ca. 5 s der naechsten Sendung (Suchlauf) verloren - der DWD
 *      sendet davor aber ein langes RYRYRY-Einlaufmuster.
 *
 * AUFGABENTEILUNG INTERRUPT / HAUPTSCHLEIFE UND SPEICHER
 * ------------------------------------------------------
 * Der ATmega328P hat nur 2 KB RAM (dieses Programm belegt ca. 0.8 KB).
 * Der Interrupt macht nur das Noetigste (DC-Abzug, Goertzel, Mischer, CIC)
 * und legt (I,Q) in einen Ringpuffer.  Die schwere Rechnung (Filter,
 * Begrenzer, Diskriminator, Tracker, Zeichenempfang) laeuft mit 2 kHz in
 * der Hauptschleife.  Ausgaben laufen ueber einen interruptgetriebenen
 * UART-Ringpuffer, damit die DSP-Kette nie auf die serielle Schnittstelle
 * warten muss.
 *
 * PC-TEST
 * -------
 * Mit -DHOST_SIM laesst sich dieselbe Datei auf dem PC uebersetzen (siehe
 * host_sim.c / "make test"): die Abtastwerte werden dann aus einer Datei
 * gelesen statt vom ADC.  Der DSP- und Dekodiercode ist in beiden Faellen
 * unveraendert.
 */

#ifdef HOST_SIM
/* ---- PC-Testumgebung: AVR-Spezialitaeten wegdefinieren ---------------- */
#include <stdint.h>
#include <stdio.h>
#define PROGMEM
#define PSTR(s)            (s)
#define pgm_read_byte(p)   (*(const uint8_t *)(p))
#define pgm_read_word(p)   (*(const uint16_t *)(p))
#define cli()              do { } while (0)
#define sei()              do { } while (0)
#else
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <stdint.h>
#endif

/* ------------------------------------------------------------------ */
/* Konfiguration                                                      */
/* ------------------------------------------------------------------ */

#define UART_BAUD        9600UL
#define STATUS_PERIOD_S  30       /* Statuszeile alle n s (0 = aus)          */
#define LOSS_TIMEOUT_S   6        /* so lange kein Signal = "weg"            */
#define WATCHDOG_S       20       /* so lange kein gueltiges Zeichen = neu suchen */

/* Feste Raster - siehe Kopfkommentar, nicht ohne Nachdenken aendern */
#define FS_HZ            8000UL   /* ADC-Rate (Timer1)                       */
#define DECIM            4        /* CIC-Dezimation -> 2 kHz komplex         */
#define FB_HZ            2000UL   /* komplexe Rate nach der Dezimation       */
#define BAUD_RTTY        50UL     /* alle DWD-Sender: 50 Baud                */
#define SPB              (FB_HZ / BAUD_RTTY)   /* Abtastwerte je Bit = 40    */

/* Goertzel-Suchlauf: N = 256 -> DFT-Punkte im Abstand 8000/256 = 31.25 Hz */
#define GZ_N             256
#define GZ_KMIN          20       /* 625 Hz                                  */
#define GZ_KMAX          80       /* 2500 Hz                                 */
#define GZ_NBINS         (GZ_KMAX - GZ_KMIN + 1)
/* Klassengrenzen fuer die belegte Bandbreite (-9 dB) in Hz.  Gemessen:
 *   Hub  85 Hz -> 151 Hz (synthetisch), 171 Hz (echte DDH47-Aufnahme)
 *   Hub 170 Hz -> 261 Hz,   Hub 450 Hz -> 529 Hz
 * Die Grenzen liegen jeweils ungefaehr in der Mitte zwischen den Klassen.
 * Ein unmodulierter Ton (nur Ruhe-Mark) misst ca. 55 Hz. */
#define BW_MIN_HZ        100      /* darunter: unmodulierter Ton, ablehnen   */
#define BW_NARROW_HZ     215      /* bis hier: 85-Hz-Klasse                  */
#define BW_MEDIUM_HZ     395      /* bis hier: 170-Hz-Klasse, darueber 450   */
#define BW_MAX_HZ        800      /* darueber: kein RTTY-Signal              */
#define MIN_SWEEPS       3        /* so viele Durchlaeufe, bevor entschieden wird */

/* NCO: 16-Bit-Phasenakkumulator, 1 LSB = 8000/65536 Hz = 0.122 Hz */
#define HZ10_TO_INC(h10) ((uint16_t)(((uint32_t)(h10) * 65536UL + 40000UL) / 80000UL))
#define NCO_MIN_HZ       500      /* NCO ausserhalb dieses Bereichs: Tracker     */
#define NCO_MAX_HZ       2700     /* weggelaufen -> neu suchen (Suchbereich 625..2500) */

/* Diskriminator-Einheit "dv": Der Diskriminator liefert
 *   di = ZAMP^2 * sin(2*pi*f/FB),  dv = di >> 8
 * mit ZAMP = 2048.  Das ist die Einheit fuer alle Tonlagen im Tracker.
 * Achtung: das ist ein SINUS der Frequenz, keine Gerade - bei +/-225 Hz
 * liegt er schon 8 % unter der Kleinwinkelnaeherung.  Die Klassentabelle
 * unten enthaelt darum die exakten dv-Werte der Tonlage +/- Hub/2. */
#define ZAMP             2048

#define TRK_BLOCK        64       /* Abtastwerte je Tracker-Schritt (32 ms)  */
/* FLL-Schleifenverstaerkung: NCO-Korrektur je Block = Fehler(dv) / FLL_DIV.
 * Ein NCO-Schritt (0.122 Hz) entspricht ca. 6.3 dv, FLL_DIV = 6.3 wuerde den
 * Fehler also in einem Block komplett ausregeln; 32 korrigiert ca. 20 % je
 * Block (32 ms) - Zeitkonstante ca. 0.15 s: schnell genug fuer driftenden
 * BFO, traege genug, um Rauschen und Datenmuster zu glaetten. */
#define FLL_DIV          32
#define HUB_BAD_MAX      96       /* Tracker-Bloecke (je 32 ms) mit falschem Hub = neu suchen */
#define SCORE_LOST       (-24)    /* beide Empfaenger darunter = Signal unbrauchbar */
#define MK_MIN           46       /* Mark-Mindestlaenge vor einem Startbit (1.15 Bit) */
#define WIN_LO           10       /* Abtastfenster im Bit: Abtastwerte 10..29 */
#define WIN_HI           30

#define TXBUF            256      /* UART-Sendepuffer (Zweierpotenz!)        */
#define TXLINE_MAX       110      /* laengste Ereigniszeile                  */
#define RXRING           32       /* Ringpuffer ISR -> Hauptschleife         */

/* Hub-Klassen (85 / 170 / 450 Hz):
 *   class_half_hz10  halber Hub in Hz*10 (nur fuer die Textausgabe)
 *   class_half_dv    dv-Wert eines Tones im Abstand Hub/2 von der NCO-Frequenz:
 *                    ZAMP^2 * sin(2*pi*(Hub/2)/2000) / 256 fuer 42.5/85/225 Hz
 *   class_alpha      Koeffizient (alpha/256) der beiden Einpol-Tiefpaesse */
enum { CL_NARROW = 0, CL_MEDIUM, CL_WIDE, NCLASS };
static const uint16_t class_half_hz10[NCLASS] PROGMEM = {  425,  850, 2250 };
static const int16_t  class_half_dv[NCLASS]   PROGMEM = { 2181, 4323, 10641 };
static const uint8_t  class_alpha[NCLASS]     PROGMEM = {   64,  112,  176 };

/* ------------------------------------------------------------------ */
/* UART-Ausgabe (interruptgetrieben, ohne stdio)                       */
/* ------------------------------------------------------------------ */

#ifndef HOST_SIM
static uint8_t txbuf[TXBUF];
#endif
static volatile uint8_t tx_head, tx_tail;

#ifndef HOST_SIM
static void uart_init(void)
{
    uint16_t ubrr = (uint16_t)((F_CPU / (16UL * UART_BAUD)) - 1);
    UBRR0H = (uint8_t)(ubrr >> 8);
    UBRR0L = (uint8_t)ubrr;
    UCSR0B = (1 << TXEN0);                       /* nur Sender               */
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);      /* 8N1                      */
}

/* Sendeinterrupt: solange Daten im Ring liegen, Byte fuer Byte nachladen. */
ISR(USART_UDRE_vect)
{
    if (tx_tail == tx_head) {
        UCSR0B &= (uint8_t)~(1 << UDRIE0);       /* Ring leer: IRQ aus       */
        return;
    }
    UDR0 = txbuf[tx_tail];
    tx_tail = (uint8_t)((tx_tail + 1) & (TXBUF - 1));
}
#endif

/* Freier Platz im Sendepuffer (fuer die Zeilenlogik). */
static uint8_t tx_free(void)
{
#ifdef HOST_SIM
    return 255;
#else
    return (uint8_t)((tx_tail - tx_head - 1) & (TXBUF - 1));
#endif
}

/* Ein Zeichen in den Sendepuffer.  Voller Puffer: Zeichen verwerfen. */
static void tx_put(char c)
{
#ifdef HOST_SIM
    putchar(c);
#else
    uint8_t n = (uint8_t)((tx_head + 1) & (TXBUF - 1));
    if (n == tx_tail) {
        return;
    }
    txbuf[tx_head] = (uint8_t)c;
    tx_head = n;
    UCSR0B |= (1 << UDRIE0);
#endif
}

/* String aus dem FLASH (immer als puts_P(PSTR("...")) aufrufen,
 * sonst landen Literale im knappen RAM). */
static void puts_P(const char *s)
{
    char c;
    while ((c = (char)pgm_read_byte(s++)) != '\0') {
        tx_put(c);
    }
}

static void put_udec(uint32_t v)
{
    char buf[11];
    uint8_t i = 10;
    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    while (buf[i]) {
        tx_put(buf[i++]);
    }
}

/* Festkomma: v ist der Wert * 10^dec, z.B. put_fixed(-266, 2) -> "-2.66" */
static void put_fixed(int32_t v, uint8_t dec)
{
    uint32_t p = 1, u, f;
    uint8_t i;
    if (v < 0) {
        tx_put('-');
        u = (uint32_t)(-v);
    } else {
        u = (uint32_t)v;
    }
    for (i = 0; i < dec; i++) {
        p *= 10;
    }
    put_udec(u / p);
    tx_put('.');
    f = u % p;
    for (i = 0; i < dec; i++) {                  /* Nachkommastellen mit 0   */
        p /= 10;
        tx_put((char)('0' + (f / p) % 10));
    }
}

/* ------------------------------------------------------------------ */
/* Tabellen im Flash                                                   */
/* ------------------------------------------------------------------ */

/* Sinustabelle (1 Periode, 256 Werte, Amplitude 127) fuer den NCO.
 * cos(x) = sin(x + 64).  Die Phasenquantisierung von 8 Bit erzeugt
 * Nebenlinien ca. 48 dB unter dem Nutzton - im Basisband harmlos. */
static const int8_t sin_tab[256] PROGMEM = {
0,    3,    6,    9,   12,   16,   19,   22,   25,   28,   31,   34,   37,   40,   43,   46,
      49,   51,   54,   57,   60,   63,   65,   68,   71,   73,   76,   78,   81,   83,   85,   88,
      90,   92,   94,   96,   98,  100,  102,  104,  106,  107,  109,  111,  112,  113,  115,  116,
     117,  118,  120,  121,  122,  122,  123,  124,  125,  125,  126,  126,  126,  127,  127,  127,
     127,  127,  127,  127,  126,  126,  126,  125,  125,  124,  123,  122,  122,  121,  120,  118,
     117,  116,  115,  113,  112,  111,  109,  107,  106,  104,  102,  100,   98,   96,   94,   92,
      90,   88,   85,   83,   81,   78,   76,   73,   71,   68,   65,   63,   60,   57,   54,   51,
      49,   46,   43,   40,   37,   34,   31,   28,   25,   22,   19,   16,   12,    9,    6,    3,
       0,   -3,   -6,   -9,  -12,  -16,  -19,  -22,  -25,  -28,  -31,  -34,  -37,  -40,  -43,  -46,
     -49,  -51,  -54,  -57,  -60,  -63,  -65,  -68,  -71,  -73,  -76,  -78,  -81,  -83,  -85,  -88,
     -90,  -92,  -94,  -96,  -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116,
    -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
    -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
    -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100,  -98,  -96,  -94,  -92,
     -90,  -88,  -85,  -83,  -81,  -78,  -76,  -73,  -71,  -68,  -65,  -63,  -60,  -57,  -54,  -51,
     -49,  -46,  -43,  -40,  -37,  -34,  -31,  -28,  -25,  -22,  -19,  -16,  -12,   -9,   -6,   -3,};


/* Viertelperiode des Cosinus in Q14: cos_q14[i] = 16384*cos(2*pi*i/256),
 * i = 0..64.  Die Goertzel-Koeffizienten cos(2*pi*k/N) fuer N = 256 sind
 * genau diese Werte (k <= 64) bzw. deren Spiegelung (64 < k <= 128). */
static const int16_t cos_q14[65] PROGMEM = {
    16384, 16379, 16364, 16340, 16305, 16261, 16207, 16143, 16069, 15986,
    15893, 15791, 15679, 15557, 15426, 15286, 15137, 14978, 14811, 14635,
    14449, 14256, 14053, 13842, 13623, 13395, 13160, 12916, 12665, 12406,
    12140, 11866, 11585, 11297, 11003, 10702, 10394, 10080,  9760,  9434,
     9102,  8765,  8423,  8076,  7723,  7366,  7005,  6639,  6270,  5897,
     5520,  5139,  4756,  4370,  3981,  3590,  3196,  2801,  2404,  2006,
     1606,  1205,   804,   402,     0
};

/* ITA2 (CCITT Nr. 2).  Index = 5-Bit-Code, Bit 0 (LSB) wurde zuerst gesendet.
 * Zwei Ebenen: Buchstaben (LTRS) und Ziffern/Zeichen (FIGS).
 *   Code 27 (11011) = FIGS, Code 31 (11111) = LTRS  - werden selbst nicht
 *   ausgegeben.  \0 = NUL (ignoriert), \a = BEL (ignoriert).
 * In der Ziffernebene sind einige Codes je nach Land unterschiedlich
 * belegt; hier die internationale Variante, wie sie der DWD nutzt
 * (+ - . , : ( ) ? = / ' und die Ziffern). */
static const char ita2_ltrs[32] PROGMEM =
    "\0E\nA SIU\rDRJNFCKTZLWHYPQOBG\033MXV\037";
static const char ita2_figs[32] PROGMEM =
    "\0003\n- '87\r$4\a,!:(5+)2#6019?&\033./=\037";

/* ------------------------------------------------------------------ */
/* Front-End im Interrupt: DC-Abzug, Goertzel, NCO-Mischer, CIC3       */
/* ------------------------------------------------------------------ */

typedef struct { int16_t i, q; } cplx16_t;

static cplx16_t rx_ring[RXRING];
static volatile uint8_t rx_head, rx_tail;

static uint16_t nco_phase;                       /* 0..65535 = 0..360 Grad   */
static volatile uint16_t nco_inc;                /* Phasenschritt je Abtast. */

/* CIC-Filter (Cascaded Integrator-Comb) 3. Ordnung, Dezimation 4:
 * drei Integratoren mit 8 kHz, dann jeder 4. Wert durch drei Kammfilter
 * (Differenzen).  Vorteile: nur Additionen, keine Koeffizienten.  Die
 * Nullstellen liegen bei 2000/4000/... Hz - genau dort, wo beim Herunter-
 * tasten auf 2 kHz sonst Stoerungen ins Basisband faltet.  Die Verstaerkung
 * ist 4^3 = 64.  Unsigned-Arithmetik: Ueberlaeufe sind gewollt und heben
 * sich in den Kammfiltern wieder auf (Modulo-2^32-Trick). */
static uint32_t ci1, ci2, ci3, cd1, cd2, cd3;    /* I-Kanal */
static uint32_t cq1, cq2, cq3, ce1, ce2, ce3;    /* Q-Kanal */
static uint8_t  dec_cnt;
static int32_t  dc_acc;                          /* DC-Schaetzer             */

/* Goertzel-Filter.  Rekursion fuer die Frequenz w = 2*pi*k/N:
 *      s[n] = x[n] + 2*cos(w)*s[n-1] - s[n-2]
 * Nach N Abtastwerten ergibt
 *      P = s[N-1]^2 + s[N-2]^2 - 2*cos(w)*s[N-1]*s[N-2]
 * die Energie im DFT-Punkt k - ohne die volle Fouriertransformation zu
 * rechnen.  Je Punkt genuegen zwei Speicherzellen. */
static volatile uint8_t  gz_run;                 /* 1 = ISR rechnet mit      */
static volatile uint8_t  gz_done;                /* 1 = Block fertig         */
static int16_t  gz_c;                            /* cos(w) in Q14            */
static int32_t  gz_s1, gz_s2;                    /* Zustandsvariablen        */
static int32_t  gz_r1, gz_r2;                    /* Ergebnis fuer Hauptschleife */
static uint16_t gz_n;

static void dsp_sample(int16_t x)                /* wird mit 8 kHz gerufen   */
{
    /* 1) Gleichanteil (ADC-Bias) mit sehr langsamem Tiefpass (~1.2 Hz)
     *    schaetzen und abziehen.  Sonst liefe ein DC-Rest als Ton bei
     *    -f_NCO durch den Mischer. */
    dc_acc += x - (dc_acc >> 10);
    x -= (int16_t)(dc_acc >> 10);

    /* 2) Goertzel (nur waehrend der Suche).  x wird um 2 Bit verkleinert,
     *    damit die 32-Bit-Zustaende auch bei Vollaussteuerung nie
     *    ueberlaufen (max. ca. 70000, Faktor 16384 -> < 2^31). */
    if (gz_run) {
        int32_t s0 = (int32_t)(x >> 2) + (((int32_t)gz_c * gz_s1) >> 13) - gz_s2;
        gz_s2 = gz_s1;
        gz_s1 = s0;
        if (++gz_n >= GZ_N) {
            gz_r1 = gz_s1;
            gz_r2 = gz_s2;
            gz_run = 0;
            gz_done = 1;
        }
    }

    /* 3) Komplexer Mischer:  z = x * e^(-j*phi)  ->  I = x*cos, Q = -x*sin */
    uint8_t ph = (uint8_t)(nco_phase >> 8);
    int8_t  c  = (int8_t)pgm_read_byte(&sin_tab[(uint8_t)(ph + 64)]);
    int8_t  s  = (int8_t)pgm_read_byte(&sin_tab[ph]);
    nco_phase += nco_inc;
    int32_t mi =  (int32_t)x * c;
    int32_t mq = -(int32_t)x * s;

    /* 4) Integratoren (laufen mit jeder Abtastung) */
    ci1 += (uint32_t)mi;  ci2 += ci1;  ci3 += ci2;
    cq1 += (uint32_t)mq;  cq2 += cq1;  cq3 += cq2;

    if (++dec_cnt < DECIM) {
        return;
    }
    dec_cnt = 0;

    /* 5) Kammfilter (nur jeden 4. Wert) */
    uint32_t t, u;
    t = ci3 - cd1;  cd1 = ci3;  u = t - cd2;  cd2 = t;  t = u - cd3;  cd3 = u;
    int32_t oi = (int32_t)t;
    t = cq3 - ce1;  ce1 = cq3;  u = t - ce2;  ce2 = t;  t = u - ce3;  ce3 = u;
    int32_t oq = (int32_t)t;

    /* Verstaerkung 64 * (127/2) -> mit >>7 bleibt |I|,|Q| < 32768 bei
     * voller ADC-Aussteuerung. */
    uint8_t nh = (uint8_t)((rx_head + 1) & (RXRING - 1));
    if (nh != rx_tail) {                         /* Ring voll: Wert verlieren */
        rx_ring[rx_head].i = (int16_t)(oi >> 7);
        rx_ring[rx_head].q = (int16_t)(oq >> 7);
        rx_head = nh;
    }
}

#ifndef HOST_SIM
/* Timer1 (CTC, 8000 Hz) startet jede ADC-Wandlung, der ADC-Interrupt holt
 * den Wert.  (bewusst keine Auto-Trigger-Magie.) */
ISR(TIMER1_COMPA_vect)
{
    ADCSRA |= (1 << ADSC);
}

/* Optional: -DDEBUG_LOAD legt PB0 (Arduino Pin 8) waehrend der ADC-ISR auf 1.
 * Das Tastverhaeltnis dieses Pins (Oszilloskop oder Multimeter im
 * Mittelwertbetrieb: U/5 V) ist die Rechenlast der Interrupt-Kette, ohne
 * Hauptschleife.  Zusammen mit dem Rest (Hauptschleife) muss die Summe < 100 %
 * bleiben, sonst laufen Ringpuffer ueber und Abtastwerte gehen verloren. */
ISR(ADC_vect)
{
#ifdef DEBUG_LOAD
    PORTB |= (1 << PB0);
#endif
    uint16_t raw = ADC;
    dsp_sample((int16_t)raw - 512);              /* Bias Vcc/2 -> 0          */
#ifdef DEBUG_LOAD
    PORTB &= (uint8_t)~(1 << PB0);
#endif
}

/* ADC-Takt 250 kHz (Vorteiler 64): Wandlung dauert 13 Takte = 52 us.  Bei
 * 8 kHz Abtastrate bleiben 125 us je Abtastwert - die ADC-ISR hat also
 * gut 70 us (ca. 1100 Zyklen) Zeit, bevor die naechste Wandlung startet. */
static void adc_init(void)
{
#ifdef DEBUG_LOAD
    DDRB |= (1 << PB0);
#endif
    ADMUX  = (1 << REFS0);                       /* AVcc-Referenz, Kanal ADC0 */
    ADCSRA = (1 << ADEN) | (1 << ADIE)
           | (1 << ADPS2) | (1 << ADPS1);        /* Vorteiler 64             */
}

static void timer1_init(void)
{
    TCCR1A = 0;
    TCCR1B = (1 << WGM12) | (1 << CS10);         /* CTC, Vorteiler 1         */
    OCR1A  = (uint16_t)(F_CPU / FS_HZ - 1);      /* 1999 -> exakt 8000 Hz    */
    TIMSK1 = (1 << OCIE1A);
}
#endif

/* ------------------------------------------------------------------ */
/* Hilfsroutinen der Hauptschleife                                     */
/* ------------------------------------------------------------------ */

static uint8_t rx_pop(cplx16_t *out)
{
    if (rx_tail == rx_head) {
        return 0;
    }
    *out = rx_ring[rx_tail];
    rx_tail = (uint8_t)((rx_tail + 1) & (RXRING - 1));
    return 1;
}

static void nco_set_inc(uint16_t v)              /* 16 Bit sind nicht atomar */
{
    cli();
    nco_inc = v;
    sei();
}

/* ------------------------------------------------------------------ */
/* Zustand                                                             */
/* ------------------------------------------------------------------ */

enum { ST_SCAN = 0, ST_TRACK };
static uint8_t state = ST_SCAN;

/* Ereignis-Flags fuer die Ausgabe */
#define EV_LOCK  0x01
#define EV_LOSS  0x02
#define EV_STAT  0x04
#define EV_POL   0x08
static volatile uint8_t ev_flags;

static uint16_t uptime_s, ms_cnt;
static uint16_t sig_last_s;                      /* letzter Zeitpunkt mit Ton */
static uint16_t ok_last_s;                       /* letztes gueltiges Zeichen */
static uint16_t lock_s;                          /* Zeitpunkt des Einrastens  */
static uint8_t  low_s;                           /* Sekunden mit zu wenig Pegel */

/* Signalpegel vor dem Begrenzer (gleitender Mittelwert) */
static uint16_t level_avg, level_ref;

/* ------------------------------------------------------------------ */
/* SUCHEN: Goertzel-Durchlauf und Messung der belegten Bandbreite      */
/* ------------------------------------------------------------------ */

static uint32_t S[GZ_NBINS];                     /* gemittelte Energie je DFT-Punkt */
static uint8_t  gz_k;                            /* aktueller DFT-Punkt      */
static uint8_t  sweeps;                          /* fertige Durchlaeufe      */
static int32_t  det_center_q4;                   /* Ergebnis: Mitte, Hz*4    */
static uint8_t  det_class;                       /* Ergebnis: Hub-Klasse     */

/* cos(2*pi*k/256) in Q14 fuer k = 0..128 aus der Viertelperiode. */
static int16_t goertzel_coef(uint8_t k)
{
    if (k <= 64) {
        return (int16_t)pgm_read_word(&cos_q14[k]);
    }
    return (int16_t)(-(int16_t)pgm_read_word(&cos_q14[128 - k]));
}

/* Neuen Goertzel-Block fuer DFT-Punkt k starten.  Die ISR fasst die
 * Zustandsvariablen nur an, solange gz_run = 1 ist - hier ist es 0. */
static void gz_start(uint8_t k)
{
    gz_k  = k;
    gz_c  = goertzel_coef(k);
    gz_s1 = 0;
    gz_s2 = 0;
    gz_n  = 0;
    gz_run = 1;
}

static void start_scan(void)
{
    uint8_t i;
    for (i = 0; i < GZ_NBINS; i++) {
        S[i] = 0;
    }
    sweeps = 0;
    state = ST_SCAN;
    gz_start(GZ_KMIN);
}

/* Lage der Flanke, an der das Spektrum die Schwelle T durchlaeuft, als
 * Bruchteil (Q8, 0..256) des Punktabstands: hi_v >= T liegt innen, lo_v < T
 * aussen.  Lineare Interpolation der Energie.  Werte vorher um 6 Bit
 * verkleinert, damit die Rechnung in 32 Bit bleibt. */
static uint16_t edge_frac_q8(uint32_t hi_v, uint32_t lo_v, uint32_t T)
{
    uint32_t num = (hi_v - T) >> 6;
    uint32_t den = ((hi_v - lo_v) >> 6) + 1;
    uint32_t f = (num * 256UL) / den;
    return (uint16_t)(f > 256 ? 256 : f);
}

/* Entscheidung nach jedem vollen Durchlauf.  Rueckgabe 1 = FSK-Signal gefunden.
 *
 * Idee: Ein FSK-Signal belegt ein Band, dessen Breite vom Hub abhaengt und
 * dessen MITTE die Traegermitte ist.  Bei 85 Hz Hub und nur 31 Hz Aufloesung
 * sind die zwei Toene im Spektrum kaum getrennt (siehe Kopfkommentar) -
 * darum suchen wir nicht "zwei Spitzen", sondern messen die belegte
 * Bandbreite:
 *  1) Hoechste Spitze suchen, Rauschboden aus den "leeren" Punkten schaetzen.
 *     Spitze muss >= 12x (10.8 dB) ueber dem Boden liegen.
 *  2) Schwelle T = Spitze/8 (-9 dB), mindestens 5x Boden.
 *  3) Aeusserster Punkt unterhalb und oberhalb (max. 22 Punkte = 690 Hz von
 *     der Spitze), der ueber T liegt und einen ebenfalls ueber T liegenden
 *     Nachbarn hat (Einzelspitzen = Rauschen zaehlen nicht).
 *  4) Flanken zwischen den Punkten linear interpolieren.  Mitte = Mitte
 *     zwischen den beiden Flanken -> NCO-Startfrequenz.
 *     Breite = Abstand der Flanken -> Hub-Klasse (Grenzwerte BW_*_HZ).  Ein
 *     einzelner unmodulierter Ton (nur Ruhe-Mark) ist nur ca. 55 Hz breit
 *     und wird abgelehnt: der Decoder wartet, bis Daten fliessen. */
static uint8_t scan_detect(void)
{
    uint8_t i, k1 = 0, cnt = 0, lo = 0xFF, hi = 0xFF;
    uint32_t s1 = 0, sum = 0, floor_e, T, thr;
    int32_t p_lo, p_hi, f_lo, f_hi, bw_q4;

    for (i = 0; i < GZ_NBINS; i++) {
        if (S[i] > s1) {
            s1 = S[i];
            k1 = i;
        }
    }
    if (s1 == 0) {
        return 0;
    }
    thr = s1 >> 4;
    for (i = 0; i < GZ_NBINS; i++) {
        if (S[i] < thr) {
            sum += S[i];
            cnt++;
        }
    }
    if (cnt < 8) {
        return 0;                                /* Spektrum ueberall laut   */
    }
    floor_e = sum / cnt;
    if (floor_e == 0) {
        floor_e = 1;
    }
    if (s1 < 12 * floor_e) {
        return 0;                                /* keine klare Spitze       */
    }

    T = s1 >> 3;
    if (T < 5 * floor_e) {
        T = 5 * floor_e;
    }
    for (i = 0; i < GZ_NBINS; i++) {
        int16_t dist = (int16_t)i - (int16_t)k1;
        uint32_t l = (i > 0) ? S[i - 1] : 0;
        uint32_t r = (i < GZ_NBINS - 1) ? S[i + 1] : 0;
        if (dist > 22 || dist < -22 || S[i] < T || (l < T && r < T)) {
            continue;
        }
        if (lo == 0xFF) {
            lo = i;
        }
        hi = i;
    }
    if (lo == 0xFF) {
        return 0;
    }

    /* Flanken in Punkt-Einheiten (Q8), dann in Frequenz (Hz*4) */
    p_lo = (int32_t)lo * 256;
    if (lo > 0) {
        p_lo -= edge_frac_q8(S[lo], S[lo - 1], T);
    }
    p_hi = (int32_t)hi * 256;
    if (hi < GZ_NBINS - 1) {
        p_hi += edge_frac_q8(S[hi], S[hi + 1], T);
    }
    /* Punkt i entspricht k = GZ_KMIN + i, Frequenz = k * 31.25 Hz = k*125 (Hz*4) */
    f_lo = (((int32_t)GZ_KMIN * 256 + p_lo) * 125L) >> 8;
    f_hi = (((int32_t)GZ_KMIN * 256 + p_hi) * 125L) >> 8;
    bw_q4 = f_hi - f_lo;
    det_center_q4 = (f_lo + f_hi) / 2;

    if (bw_q4 < BW_MIN_HZ * 4L)        return 0;             /* unmodulierter Ton */
    else if (bw_q4 < BW_NARROW_HZ * 4L) det_class = CL_NARROW;   /* 85 Hz  */
    else if (bw_q4 < BW_MEDIUM_HZ * 4L) det_class = CL_MEDIUM;   /* 170 Hz */
    else if (bw_q4 <= BW_MAX_HZ * 4L)   det_class = CL_WIDE;     /* 450 Hz */
    else                                return 0;
    return 1;
}

static void start_track(void);                   /* weiter unten definiert   */

/* Wird von der Hauptschleife gerufen: verarbeitet einen fertigen Goertzel-
 * Block, startet den naechsten und entscheidet nach jedem vollen Durchlauf,
 * ob ein Signal gefunden wurde. */
static void gz_service(void)
{
    int32_t a, b, term, p;
    uint8_t i;

    if (!gz_done) {
        return;
    }
    gz_done = 0;

    /* Energie des DFT-Punktes; s1,s2 vorher um 3 Bit verkleinert (Ueberlauf) */
    a = gz_r1 >> 3;
    b = gz_r2 >> 3;
    term = (((int32_t)gz_c * a) >> 13) * b;      /* 2*cos(w)*s1*s2           */
    p = a * a + b * b - term;
    if (p < 0) {
        p = 0;
    }
    i = (uint8_t)(gz_k - GZ_KMIN);
    /* exponentieller Mittelwert ueber ca. 4 Durchlaeufe */
    S[i] = S[i] - (S[i] >> 2) + ((uint32_t)p >> 2);

    if (gz_k >= GZ_KMAX) {
        sweeps++;
        if (sweeps >= MIN_SWEEPS && scan_detect()) {
            start_track();
            return;
        }
        gz_start(GZ_KMIN);
    } else {
        gz_start((uint8_t)(gz_k + 1));
    }
}

/* ------------------------------------------------------------------ */
/* TRACK: Filter, Begrenzer, Diskriminator                             */
/* ------------------------------------------------------------------ */

static int32_t a1i, a1q, a2i, a2q;               /* Tiefpass-Zustaende       */
static int16_t zi_p, zq_p;                       /* z[n-1] (Diskriminator)   */
static int16_t dv_p;                             /* dv[n-1] (Glaettung)      */
static uint8_t f_alpha;                          /* Filterkoeffizient/256    */

/* Tracker */
static int16_t  trk_h;                           /* halber Hub (Klassenwert) in dv */
static uint8_t  blk_n, n_hi, n_lo, hub_bad;
static int32_t  s_hi, s_lo;

/* NCO-Frequenz in Hz*10 (fuer Anzeige und Grenzpruefung) */
static int32_t nco_hz10(void)
{
    uint16_t inc;
    cli();
    inc = nco_inc;
    sei();
    return ((int32_t)inc * 625L) / 512L;         /* inc * 80000 / 65536      */
}

/* Ein Tracker-Schritt (alle 32 ms): siehe Kopfkommentar "TRAEGERNACHFUEHRUNG". */
static void trk_update(void)
{
    int16_t mh = n_hi ? (int16_t)(s_hi / n_hi) : 0;
    int16_t ml = n_lo ? (int16_t)(s_lo / n_lo) : 0;
    int16_t err = 0;
    uint8_t have = 0;

    if (n_hi >= 6 && n_lo >= 6) {
        int16_t hm = (int16_t)((mh - ml) / 2);   /* gemessener halber Hub    */
        int16_t dev = (int16_t)(hm - trk_h);
        err = (int16_t)((mh + ml) / 2);          /* beide Toene: Mitte       */
        have = 1;
        /* Hub-Kontrolle: weicht der gemessene Tonabstand um mehr als
         * 35 % von der Klasse ab, stimmt die Klasse (Filter!) nicht mehr
         * -> neu messen.  Nach ca. 3 s (96 Bloecke) Abweichung wird die
         * Suche neu gestartet; ein einzelner guter Block setzt zurueck. */
        if ((dev < 0 ? -dev : dev) > (int16_t)((trk_h * 7) / 20)) {
            if (hub_bad < 255) hub_bad++;
        } else {
            hub_bad = 0;
        }
        if (hub_bad >= HUB_BAD_MAX) {
            ev_flags |= EV_LOSS;
            start_scan();
            return;
        }
    } else if (n_hi >= 12 && n_lo < 6) {
        /* Nur der obere Ton (z.B. Ruhe-Mark): er soll bei +Hub/2 liegen;
         * die Abweichung davon ist der Frequenzfehler. */
        err = (int16_t)(mh - trk_h);
        have = 1;
    } else if (n_lo >= 12 && n_hi < 6) {
        err = (int16_t)(ml + trk_h);             /* nur unterer Ton, bei -Hub/2 */
        have = 1;
    }

    if (have) {
        int32_t hz10, inc;
        sig_last_s = uptime_s;
        /* Frequenzregelschleife: NCO um einen Bruchteil des Fehlers
         * verschieben.  dv/6.28 = Schritte von 0.122 Hz (siehe Kommentar
         * bei FLL_DIV). */
        inc = (int32_t)nco_inc + (int32_t)err / FLL_DIV;
        nco_set_inc((uint16_t)inc);
        hz10 = nco_hz10();
        if (hz10 < NCO_MIN_HZ * 10L || hz10 > NCO_MAX_HZ * 10L) {
            ev_flags |= EV_LOSS;                 /* aus dem Suchbereich gelaufen */
            start_scan();
        }
    }
    blk_n = 0;
    n_hi = n_lo = 0;
    s_hi = s_lo = 0;
}

/* ------------------------------------------------------------------ */
/* Zeichenempfaenger (je Polaritaet einer)                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  active;     /* 1 = mitten in einem Zeichen                     */
    uint8_t  mk, sp;     /* Laenge des laufenden Mark-Laufs / Space-Serie   */
    uint16_t cnt;        /* Abtastwerte seit Erkennung des Startbits        */
    uint16_t bstart;     /* Beginn des aktuellen Bits (in cnt)              */
    int8_t   sum;        /* Summe der Vorzeichen im Abtastfenster           */
    uint8_t  nbit;       /* 0 = Startbit, 1..5 = Daten, 6 = Stopbit         */
    uint8_t  code;       /* bisher empfangene Datenbits                     */
    uint8_t  fig;        /* 1 = Ziffernebene aktiv                          */
    int8_t   score;      /* Punktestand: richtige Zeichen +1, Rahmenfehler -2 */
    uint16_t ok, bad;    /* Zaehler fuer die Statuszeile                    */
} rtty_t;

static rtty_t rx[2];                             /* 0: Mark = tiefer Ton, 1: Mark = hoher Ton */
static uint8_t sel;                              /* gewaehlter Empfaenger    */
static uint8_t txt_col;                          /* Spalte der Textausgabe   */

/* Ausgabe eines ITA2-Zeichens (nur fuer den gewaehlten Empfaenger). */
static void ita2_out(rtty_t *R, uint8_t code)
{
    char ch;
    if (code == 27) {                            /* FIGS */
        R->fig = 1;
        return;
    }
    if (code == 31) {                            /* LTRS */
        R->fig = 0;
        return;
    }
    ch = (char)pgm_read_byte(R->fig ? &ita2_figs[code] : &ita2_ltrs[code]);
    if (tx_free() < 4) {
        return;
    }
    if (ch == '\n') {
        tx_put('\r');
        tx_put('\n');
        txt_col = 0;
    } else if (ch >= ' ' && ch < 127) {
        tx_put(ch);
        txt_col++;
    }                                            /* CR, NUL, BEL: ignorieren */
}

/* Ein Zeichen ist fertig (ok = 1) oder hatte einen Rahmenfehler. */
static void char_done(uint8_t li, uint8_t ok)
{
    rtty_t *R = &rx[li];
    if (ok) {
        R->ok++;
        if (R->score < 60) {
            R->score++;
        }
        if (li == sel && R->score >= 6) {
            ita2_out(R, R->code);
            ok_last_s = uptime_s;
        }
    } else {
        R->bad++;
        if (R->score > -60) {
            R->score -= 2;
        }
    }
    /* Fehlerratenwaechter: liefern BEIDE Polaritaeten nur Rahmenfehler, ist
     * die Tonlage/der Hub nicht mehr stimmig (Sender gewechselt, Empfaenger
     * umgestellt, Signal weg) -> sofort neu suchen.  Im Normalbetrieb steht
     * der Punktestand des richtigen Empfaengers bei 0 ... +60. */
    if (rx[0].score < SCORE_LOST && rx[1].score < SCORE_LOST) {
        ev_flags |= EV_LOSS;
        start_scan();
        return;
    }
    /* Auswahl mit Hysterese: der andere Empfaenger muss deutlich besser sein */
    if (rx[li].score > rx[sel].score + 4 && li != sel) {
        sel = li;
        ev_flags |= EV_POL;
    }
}

/* Ein Abtastwert (2 kHz) je Empfaenger.  mark = 1: Leitung im Mark-Zustand.
 *
 * Ruhe: Wir warten auf den Wechsel Mark -> Space (3 Space-Werte in Folge,
 *       entprellt gegen Rauschen), und zwar nur, wenn davor mindestens
 *       MK_MIN (46 = 1.15 Bit) Mark-Werte lagen - siehe Kopfkommentar.
 *       Das ist die fallende Flanke des Startbits.  Der Zaehler cnt startet
 *       bei 3, weil die Erkennung 3 Abtastwerte nach der Flanke erfolgt.
 * Zeichen: Jedes der 7 Bits (Start, 5 x Daten, Stop) dauert 40 Abtastwerte.
 *       Abgetastet wird nicht in einem Punkt, sondern als Mehrheit ueber die
 *       mittleren 20 Abtastwerte des Bits (Nr. 10..29) - das ist ein
 *       "Integrate & Dump" mit halber Bitbreite und unempfindlich gegen
 *       Rauschspitzen und einige Abtastwerte Zeitfehler.
 *       Startbit muss Space sein (sonst war es nur eine Stoerspitze),
 *       Stopbit muss Mark sein (sonst Rahmenfehler -> Polaritaet oder
 *       Takt falsch). */
static void lane_sample(uint8_t li, uint8_t mark)
{
    rtty_t *R = &rx[li];

    if (!R->active) {
        if (mark) {
            if (R->mk < 255) R->mk++;
            R->sp = 0;
        } else {
            if (R->sp < 255) R->sp++;
            if (R->sp == 3) {                    /* 3 Space in Folge = Flanke */
                if (R->mk >= MK_MIN) {           /* lange genug Mark davor: Startbit */
                    R->active = 1;
                    R->cnt = 3;
                    R->bstart = 0;
                    R->nbit = 0;
                    R->sum = 0;
                    R->code = 0;
                }
                R->mk = 0;                       /* neuer Mark-Lauf beginnt  */
            }
        }
        return;
    }

    /* mitten im Zeichen */
    {
        uint16_t ph = R->cnt - R->bstart;        /* 0..39 innerhalb des Bits */
        if (ph >= WIN_LO && ph < WIN_HI) {
            R->sum += mark ? 1 : -1;
        }
        if (ph == WIN_HI - 1) {                  /* Fenster zu Ende: entscheiden */
            uint8_t v = (R->sum > 0);            /* 1 = Mark                 */
            R->sum = 0;
            if (R->nbit == 0) {
                if (v) {                         /* Startbit nicht Space     */
                    R->active = 0;
                    R->mk = 0;
                    R->sp = 0;
                    return;
                }
            } else if (R->nbit <= 5) {
                R->code |= (uint8_t)(v << (R->nbit - 1));   /* LSB zuerst    */
            } else {                             /* Stopbit                  */
                R->active = 0;
                R->sp = 0;
                /* Das Stopbit ist Mark und bis hierher (Abtastwert 29 des
                 * Bits) waren das mindestens 30 Abtastwerte.  Diese zaehlen
                 * wir als Beginn des naechsten Mark-Laufs (mk) vor, weil
                 * mk waehrend des Zeichens nicht mitlaeuft - sonst wuerde
                 * das MK_MIN-Kriterium beim naechsten Startbit verfehlt. */
                R->mk = v ? 30 : 0;
                char_done(li, v);
                return;
            }
            R->nbit++;
            R->bstart += SPB;
        }
        R->cnt++;
    }
}

/* ------------------------------------------------------------------ */
/* Einrasten / Verlieren                                               */
/* ------------------------------------------------------------------ */

static void start_track(void)
{
    uint8_t i;
    uint16_t inc;

    gz_run = 0;
    inc = HZ10_TO_INC(det_center_q4 * 10 / 4);   /* Hz*4 -> Hz*10            */
    nco_set_inc(inc);

    trk_h = (int16_t)pgm_read_word(&class_half_dv[det_class]);
    f_alpha = pgm_read_byte(&class_alpha[det_class]);

    a1i = a1q = a2i = a2q = 0;
    zi_p = zq_p = 0;
    dv_p = 0;
    blk_n = n_hi = n_lo = 0;
    hub_bad = 0;
    s_hi = s_lo = 0;
    for (i = 0; i < 2; i++) {
        rx[i].active = 0;
        rx[i].mk = rx[i].sp = 0;
        rx[i].fig = 0;
        rx[i].score = 0;
    }
    sel = 0;
    level_ref = 0;
    low_s = 0;
    lock_s = uptime_s;
    sig_last_s = uptime_s;
    ok_last_s = uptime_s;
    state = ST_TRACK;
    ev_flags |= EV_LOCK;
}

/* Einmal je Sekunde: Zeitueberwachung. */
static void second_tick(void)
{
    uptime_s++;
    if (state == ST_TRACK) {
        /* Referenzpegel 3 s nach dem Einrasten merken */
        if ((uint16_t)(uptime_s - lock_s) == 3) {
            level_ref = level_avg;
        }
        if (level_ref && level_avg < (level_ref >> 2)) {
            low_s++;                             /* Pegel um > 12 dB gefallen */
        } else {
            low_s = 0;
        }
        if ((uint16_t)(uptime_s - sig_last_s) > LOSS_TIMEOUT_S
            || low_s > LOSS_TIMEOUT_S
            || (uint16_t)(uptime_s - ok_last_s) > WATCHDOG_S) {
            ev_flags |= EV_LOSS;
            start_scan();
        }
    }
#if STATUS_PERIOD_S
    if ((uptime_s % STATUS_PERIOD_S) == 0) {
        ev_flags |= EV_STAT;
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Ereignis-Ausgabe (Statuszeilen), blockiert nie                       */
/* ------------------------------------------------------------------ */

static void line_begin(void)
{
    if (txt_col) {                               /* laufende Textzeile beenden */
        tx_put('\r');
        tx_put('\n');
        txt_col = 0;
    }
}

/* Hub in Hz (Klassenwert 85 / 170 / 450, nicht der exakt gemessene Wert). */
static void put_hub(void)
{
    put_udec((uint32_t)pgm_read_word(&class_half_hz10[det_class]) / 5);  /* 2 * halber Hub[Hz*10] / 10 */
}

static void events_pump(void)
{
    uint8_t ev;
    if (tx_free() < TXLINE_MAX) {
        return;
    }
    cli();
    ev = ev_flags;
    if (ev & EV_LOSS)      ev_flags &= (uint8_t)~EV_LOSS;
    else if (ev & EV_LOCK) ev_flags &= (uint8_t)~EV_LOCK;
    else if (ev & EV_POL)  ev_flags &= (uint8_t)~EV_POL;
    else if (ev & EV_STAT) ev_flags &= (uint8_t)~EV_STAT;
    sei();

    if (ev & EV_LOSS) {
        line_begin();
        puts_P(PSTR("*** Signal verloren - suche 625..2500 Hz ***\r\n"));
    } else if (ev & EV_LOCK) {
        line_begin();
        puts_P(PSTR("*** SIGNAL: NF-Mitte "));
        put_fixed(nco_hz10(), 1);
        puts_P(PSTR(" Hz, Hub ca. "));
        put_hub();
        puts_P(PSTR(" Hz, 50 Baud ***\r\n"));
    } else if (ev & EV_POL) {
        line_begin();
        puts_P(PSTR("*** Polaritaet: Mark = "));
        puts_P(sel ? PSTR("hoeherer") : PSTR("tieferer"));
        puts_P(PSTR(" Ton ***\r\n"));
    } else if (ev & EV_STAT) {
        line_begin();
        if (state == ST_TRACK) {
            puts_P(PSTR("--- Status: NF-Mitte "));
            put_fixed(nco_hz10(), 1);
            puts_P(PSTR(" Hz, Hub "));
            put_hub();
            puts_P(PSTR(" Hz, Pegel ~"));
            put_udec(level_avg);
            puts_P(PSTR(", Zeichen ok "));
            put_udec(rx[sel].ok);
            puts_P(PSTR(", Rahmenfehler "));
            put_udec(rx[sel].bad);
            puts_P(PSTR(", Mark = "));
            puts_P(sel ? PSTR("hoch") : PSTR("tief"));
            puts_P(PSTR(" ---\r\n"));
        } else {
            puts_P(PSTR("--- Status: KEIN SIGNAL, suche ---\r\n"));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Verarbeitung eines komplexen Abtastwerts (2 kHz)                     */
/* ------------------------------------------------------------------ */

static void process_sample(int16_t I, int16_t Q)
{
    /* Zeitbasis (immer) */
    if (++ms_cnt >= FB_HZ) {
        ms_cnt = 0;
        second_tick();
    }
    if (state != ST_TRACK) {
        return;                                  /* beim Suchen nichts zu demodulieren */
    }

    /* --- 1) Bandbreite: zwei Einpol-Tiefpaesse  y += alpha*(x - y).
     *        Bei 85 Hz Hub genuegen +/-60 Hz, bei 450 Hz Hub brauchen wir
     *        +/-300 Hz.  Zu breit = mehr Rauschen im Diskriminator, zu
     *        schmal = Intersymbol-Stoerung (die Toene werden "verschmiert").
     *        alpha kommt aus der Hub-Klasse. ---------------------------- */
    a1i += (((int32_t)I - a1i) * f_alpha) >> 8;
    a1q += (((int32_t)Q - a1q) * f_alpha) >> 8;
    a2i += ((a1i - a2i) * f_alpha) >> 8;
    a2q += ((a1q - a2q) * f_alpha) >> 8;
    I = (int16_t)a2i;
    Q = (int16_t)a2q;

    /* --- 2) Begrenzer (wie im FM-Empfaenger): Betrag auf ZAMP normieren.
     *        |z| ~ (61*max + 25*min)/64  ("alpha-max-beta-min", Fehler
     *        -5 % .. +3 %, Mittelwert 0.4 %).  Ergebnis: nur noch die
     *        Phase zaehlt, Schwund und Pegel sind egal. ---------------- */
    uint16_t ai = (uint16_t)(I < 0 ? -I : I);
    uint16_t aq = (uint16_t)(Q < 0 ? -Q : Q);
    uint16_t mx = ai > aq ? ai : aq;
    uint16_t mn = ai > aq ? aq : ai;
    uint16_t m  = (uint16_t)(((uint32_t)mx * 61 + (uint32_t)mn * 25) >> 6);
    level_avg = (uint16_t)(level_avg + (((int32_t)m - level_avg) >> 6));
    if (m < 4) {
        m = 4;
    }
    int32_t g  = ((int32_t)ZAMP << 8) / m;       /* eine Division je Abtastwert */
    int16_t zi = (int16_t)(((int32_t)I * g) >> 8);
    int16_t zq = (int16_t)(((int32_t)Q * g) >> 8);

    /* --- 3) Frequenzdiskriminator (Verzoegerung um 1 Abtastwert):
     *        Im(z[n] * conj(z[n-1])) ~ sin(Phasenschritt) ~ Momentanfrequenz.
     *        Positiv = Signal liegt ueber der NCO-Frequenz. ------------- */
    int32_t di = (int32_t)zq * zi_p - (int32_t)zi * zq_p;
    zi_p = zi;
    zq_p = zq;
    int16_t dv = (int16_t)(di >> 8);
    int16_t ds = (int16_t)(((int32_t)dv + dv_p) >> 1);   /* 2-Punkt-Glaettung */
    dv_p = dv;

    /* --- 4) Tracker-Statistik: nur die "aeusseren" Werte (|ds| > 0.4 * Hub/2)
     *        zaehlen; Uebergaenge zwischen den Toenen liegen in der Mitte
     *        und wuerden die Mittelwerte verfaelschen.  0.4 statt 0.5 gibt
     *        einen groesseren Fangbereich (Ton darf bis 0.6*Hub/2 daneben
     *        liegen). ------------------------------------------------- */
    {
        int16_t th = (int16_t)((trk_h * 2) / 5);
        if (ds > th) {
            n_hi++;
            s_hi += ds;
        } else if (ds < -th) {
            n_lo++;
            s_lo += ds;
        }
        if (++blk_n >= TRK_BLOCK) {
            trk_update();
            if (state != ST_TRACK) {
                return;
            }
        }
    }

    /* --- 5) Zeichenempfang, beide Polaritaeten -------------------------- */
    lane_sample(0, ds < 0);                      /* Mark = tieferer Ton      */
    lane_sample(1, ds > 0);                      /* Mark = hoeherer Ton      */
}

/* ------------------------------------------------------------------ */
/* Initialisierung und Hauptschleife                                    */
/* ------------------------------------------------------------------ */

static void rtty_init(void)
{
    nco_inc = HZ10_TO_INC(13000);                /* irgendein Startwert      */
    start_scan();
}

/* Alles, was die Hauptschleife je Durchlauf tut. */
static void main_step(void)
{
    cplx16_t z;
    gz_service();
    while (rx_pop(&z)) {
        process_sample(z.i, z.q);
    }
    events_pump();
}

#ifndef HOST_SIM
int main(void)
{
    rtty_init();
    uart_init();
    timer1_init();
    adc_init();
    sei();

    puts_P(PSTR("DWD-RTTY-Decoder (F1B, 50 Baud, Hub automatisch) ATmega328P, 8 kHz ADC\r\n"));
    puts_P(PSTR("Erwarte NF-Signal 625..2500 Hz an ADC0 ...\r\n"));

    for (;;) {
        main_step();
    }
}
#endif
