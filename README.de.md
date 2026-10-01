# DWD-RTTY-Decoder für den ATmega328P

🇬🇧 English (README.md) | [🇩🇪 Deutsch]

Bare-metal-Decoder (avr-gcc / avr-libc, kein Arduino-Core, kein printf, kein float)
für die Funkfernschreib-Aussendungen des Deutschen Wetterdienstes (DDH47 auf 147,3 kHz,
DDK2/DDH7/DDK9/DDH9/DDH8 auf Kurzwelle).

Ausgabe: der empfangene Klartext auf USART0 (9600 8N1), dazwischen kurze Statuszeilen
(`*** SIGNAL ...`, `--- Status ...`).

## Was automatisch erkannt wird

Eingestellt werden muss nichts. Alle DWD-Sender arbeiten mit 50 Baud, ITA2, 1 Start +
5 Daten + 1,5 Stopbit; sie unterscheiden sich nur im Hub.

| Größe | Wie |
|---|---|
| Lage im Audio (625 … 2500 Hz) | Goertzel-Suchlauf (61 DFT-Punkte, 31,25 Hz Raster) misst die belegte Bandbreite; deren Mitte ist die NCO-Startfrequenz |
| Hub 85 / 170 / 450 Hz | aus der belegten Bandbreite (gemessen ca. 150–170 / 260 / 530 Hz); legt die Filterbandbreite fest |
| Polarität (USB/LSB, Mark hoch/tief) | zwei Zeichenempfänger laufen parallel, ein Punktestand entscheidet |
| BFO-Drift | Frequenzregelschleife (FLL) auf der Mitte der Tonlagen; Fangbereich rechnerisch 0,6 × Hub/2 (ca. ±25 Hz bei 85 Hz Hub, ca. ±135 Hz bei 450 Hz), nicht separat vermessen |
| Senderwechsel / Signalausfall | vier Wächter (Pegel, Fehlerrate, Hub-Kontrolle, Zeichen-Watchdog) starten bei Bedarf die Suche neu |

## Hardware

* NF-Signal vom Empfänger (SSB) auf **ADC0** (A0), auf Vcc/2 vorgespannt, davor ein
  Tiefpass bei ca. 3 kHz (Anti-Alias; Abtastrate 8 kHz).
* USART0, 9600 8N1.
* Empfänger so abstimmen, dass das Signal bei **625 … 2500 Hz** im Audio liegt
  (z. B. 147,3 kHz in USB auf 146,0 kHz abgestimmt -> Mitte 1300 Hz).
* Optional `-DDEBUG_LOAD`: PB0 (Arduino Pin 8) ist während der ADC-Interrupt-Routine
  high. Das Tastverhältnis ist die Rechenlast der Interruptkette.

## Bauen und Testen

```sh
make                     # main.hex  (ca. 7,4 KB Flash, ca. 0,8 KB RAM)
make flash               # Port/Baud im Makefile anpassen
make raw WAV=testsignal/dwd_146_0kHz.wav RAW=dwd.raw   # WAV -> 8-kHz-Rohdatei
make test RAW=dwd.raw    # PC-Simulation, derselbe main.c-Code
make testall             # komplette Testbatterie
```

Für die Testwerkzeuge in `tools/` braucht man Python 3 mit numpy und scipy.

| Datei | Zweck |
|---|---|
| `main.c` | der Decoder (AVR und, mit `-DHOST_SIM`, PC) |
| `host_sim.c` | PC-Testrahmen: liest Abtastwerte aus einer Datei statt vom ADC |
| `tools/wav2raw.py` | WAV -> 8-kHz-Rohdatei; liest die Samplerate aus dem Header (die Testaufnahme hat 7119 Hz) und rechnet mit Polyphasen-Resampler exakt auf 8000 Hz um |
| `tools/gen_rtty.py` | synthetisches RTTY-Signal mit einstellbarem Hub, Mitte, Drift, Rauschen, Schwund, Polarität |
| `tools/ref_decoder.py` | unabhängiger Referenzdecoder in Python (Gleitkomma, anderer Algorithmus) |
| `tools/test_all.py` | Testbatterie, schreibt eine Ergebnistabelle |
| `testsignal/` | Testaufnahme, Referenztext, dekodierter Text, `testergebnis.txt` |

## Testergebnisse

Vollständige Tabelle: `testsignal/testergebnis.txt`.

* **Echte Aufnahme** `dwd_146_0kHz.wav` (260 s, DDH47): Mitte ca. 1297 Hz, Hub 85 Hz,
  Mark = tiefer Ton. Der Text stimmt über die gesamte Dauer mit dem unabhängigen
  Referenzdecoder überein, bis auf das Einschwingen am Anfang; der Suchlauf braucht ca. 5 s.
* **Synthetische Signale:** Hub 85/170/450 Hz, Mitte 900 … 2200 Hz, USB und LSB,
  Drift +0,3 / −1 Hz/s, Wobble ±8 Hz, Rauschen, 25 s Ruhe-Mark vor den Daten,
  25 s Signalausfall: durchweg 100 % richtige Textfenster.
* **Senderwechsel mitten im Strom** (450 -> 85 -> 170 Hz, jeweils andere Mitte): alle
  drei Signale werden erkannt; die Zeichen während des Umschaltens gehen verloren (92 %).
* **Empfindlichkeit** (S/N im 2,4-kHz-Audiokanal): Hub 85 Hz fehlerfrei bis mindestens
  3 dB, Hub 450 Hz fehlerfrei bis ca. 6 dB. Tiefer Schwund (90 %) bei 450 Hz: 71 %.

## Bekannte Grenzen

* Nach einer Sendepause oder einem Senderwechsel dauert das Neueinrasten ca. 5 s
  (Suchlauf); in dem Sonderfall, dass der Tracker auf nur einen der beiden neuen Töne
  einrastet, greift nach 20 s der Zeichen-Watchdog.
* Schnelles Nachstimmen am Empfänger um mehr als den Fangbereich der Nachführung
  führt zu einem neuen Suchlauf (Meldung `Signal verloren`).
* Die Rechenlast des AVR ist abgeschätzt (Interrupt höchstens ca. 700 Takte plus
  Multiplikations-Bibliotheksaufrufe bei 2000 Takten Budget je Abtastwert); mit
  `-DDEBUG_LOAD` lässt sie sich nachmessen.
* ITA2-Ziffernebene: internationale Belegung, wie sie die DWD-Texte zeigen. Klingel und
  Wagenrücklauf werden nicht ausgegeben.
