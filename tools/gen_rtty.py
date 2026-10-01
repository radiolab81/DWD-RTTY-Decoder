#!/usr/bin/env python3
"""gen_rtty.py - synthetisches DWD-artiges RTTY-Signal (F1B, 50 Baud, ITA2, 5N1.5)
als Audio bei 8 kHz, direkt als Rohdatei fuer host_sim (uint16 LE, Bias 512).

Mit Absicht so gebaut, dass man dem Decoder Schwierigkeiten macht:
  --center   Audio-Mitte in Hz          --shift   Hub (Abstand der Toene) in Hz
  --snr      S/N in dB im 2.4-kHz-Audiokanal (300..2700 Hz), 99 = rauschfrei
  --drift    lineare BFO-Drift in Hz/s  --wobble  sinusfoermige Drift (Hz Spitze)
  --invert   Mark = hoeherer Ton (LSB-Empfang)
  --fade     Schwund (Tiefe 0..1, ca. 0.25 Hz Rate)
  --idle     Sekunden reines Mark (nur ein Ton) vor den Daten
  --dur      Gesamtdauer in s           --seed    Zufallszahlen
  --text     Textdatei (sonst eingebauter DWD-Text)
Zusaetzlich schreibt es <ausgabe>.txt: der gesendete Text (Referenz).
"""
import argparse
import numpy as np
from scipy.signal import butter, lfilter

LTRS = "\0E\nA SIU\rDRJNFCKTZLWHYPQOBG\x1bMXV\x1f"
FIGS = "\x003\n- '87\r$4\x07,!:(5+)2#6019?&\x1b./=\x1f"

DEFAULT_TEXT = (
    "RYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRY\r\r\n"
    "ZCZC 550\r\r\nFQEN50 EDZW 270000\r\r\n"
    "SEEWETTERBERICHT FUER NORD- UND OSTSEE\r\r\n"
    "HERAUSGEGEBEN VOM SEEWETTERDIENST HAMBURG\r\r\n"
    "27.09.26, 00 UTC:\r\r\n"
    "WETTERLAGE: TIEF UEBER SKANDINAVIEN 995 HPA, ZIEHT OSTWAERTS.\r\r\n"
    "DEUTSCHE BUCHT: WEST 5-6, ZUNEHMEND 7, BOEEN 8, SEEGANG 2-3 M.\r\r\n"
    "SUEDL. NORDSEE: SW 4, TAGSUEBER ABNEHMEND 3, SICHT 10 KM (SM).\r\r\n"
    "WESTL. OSTSEE: NW 6-7, SEEGANG 2.5 M, VEREINZELT REGEN //\r\r\n"
    "AUSSICHTEN: NACH 12 UTC ABNEHMENDER WIND, ZEITWEISE REGEN.\r\r\n"
    "NNNN\r\r\n")

def encode(text):
    """ITA2 mit LTRS nach Leerzeichen (USOS) wie beim DWD."""
    out, fig = [], False
    for ch in text:
        if ch == " ":
            out += [4]
            if fig:
                out += [31]; fig = False   # LTRS nach Wortzwischenraum
            continue
        if ch in LTRS[:27] + LTRS[28:31] and ch != "\0":
            if fig:
                out += [31]; fig = False
            out += [LTRS.index(ch)]
        elif ch in FIGS and ch not in "\0\x1b\x1f":
            if not fig:
                out += [27]; fig = True
            out += [FIGS.index(ch)]
    return out

def bits_of(codes):
    """Startbit(0) + 5 Datenbits LSB zuerst + 1.5 Stopbit(1), in halben Bit."""
    b = []
    for c in codes:
        b += [0]*2
        for k in range(5):
            b += [(c >> k) & 1]*2
        b += [1]*3
    return np.array(b, dtype=np.int8)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("--center", type=float, default=1500)
    ap.add_argument("--shift", type=float, default=450)
    ap.add_argument("--snr", type=float, default=99)
    ap.add_argument("--drift", type=float, default=0)
    ap.add_argument("--wobble", type=float, default=0)
    ap.add_argument("--invert", action="store_true")
    ap.add_argument("--fade", type=float, default=0)
    ap.add_argument("--idle", type=float, default=0)
    ap.add_argument("--gap", type=float, nargs=2, default=None,
                    help="Signal von s1 bis s2 ausblenden (Rauschen bleibt)")
    ap.add_argument("--dur", type=float, default=120)
    ap.add_argument("--peak", type=float, default=350)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--text", default=None)
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)
    fs = 8000
    text = DEFAULT_TEXT if not a.text else open(a.text).read()
    n = int(a.dur * fs)
    # Datenstrom in Halbbit-Raster (100 Halbbit/s), Text wird wiederholt
    codes = encode(text)
    nhb_needed = int((a.dur - a.idle) * 100) + 10
    bits = bits_of(codes)
    reps = int(np.ceil(nhb_needed / len(bits))) + 1
    bits = np.tile(bits, reps)
    idle_hb = int(a.idle * 100)
    bits = np.concatenate([np.ones(idle_hb, dtype=np.int8), bits])
    # Referenz: der Text, genug oft wiederholt (Fenstervergleich in test_all.py)
    ref = text * (reps + 1)
    t = np.arange(n) / fs
    hb = np.minimum((t * 100).astype(int), len(bits) - 1)
    mark = bits[hb].astype(float)
    # Mark = tiefer (USB) oder hoch (--invert)
    sgn = (mark - 0.5) * 2 * (1 if a.invert else -1)   # +1 = hoeherer Ton
    fc = a.center + a.drift * t + a.wobble * np.sin(2*np.pi*t/37.0)
    f_inst = fc + sgn * a.shift / 2
    # Phasenstetiges FSK: die Phase ist das Integral (Summe) der Momentanfrequenz
    phase = 2*np.pi*np.cumsum(f_inst) / fs
    sig = np.sin(phase)
    if a.fade > 0:
        sig *= 1 - a.fade * (0.5 + 0.5*np.sin(2*np.pi*0.25*t + 1.0))
    if a.gap:
        sig[(t >= a.gap[0]) & (t < a.gap[1])] = 0
    # Kanalfilter 300..2700 Hz (wie der ZF-Filter eines Kommunikationsempfaengers)
    bb, ab = butter(4, [300/4000, 2700/4000], btype="band")
    sig = lfilter(bb, ab, sig)
    if a.snr < 90:
        noise = lfilter(bb, ab, rng.standard_normal(n))
        ps = np.mean(sig[sig != 0] ** 2) if np.any(sig != 0) else 1.0
        pn = np.mean(noise ** 2)
        noise *= np.sqrt(ps / pn / 10 ** (a.snr / 10))
        sig = sig + noise
    else:
        sig = sig + 1e-3 * lfilter(bb, ab, rng.standard_normal(n))
    sig *= a.peak / np.percentile(np.abs(sig), 99.9) / 1.0
    raw = np.clip(np.round(sig) + 512, 0, 1023).astype("<u2")
    raw.tofile(a.raw)
    open(a.raw + ".txt", "w").write(ref)
    print(f"{a.raw}: {a.dur}s, Mitte {a.center} Hz, Hub {a.shift} Hz, S/N {a.snr} dB, "
          f"Drift {a.drift} Hz/s, Wobble {a.wobble} Hz{', invertiert' if a.invert else ''}")

if __name__ == "__main__":
    main()
