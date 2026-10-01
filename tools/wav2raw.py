#!/usr/bin/env python3
"""wav2raw.py - WAV-Datei -> Rohdatei fuer host_sim (8 kHz, uint16 LE, Bias 512).

Die Samplerate der WAV-Datei wird aus dem Header gelesen (die Testaufnahme
hat "krumme" 7119 Hz) und mit einem Polyphasen-Resampler exakt auf die
8000 Hz des ATmega-ADC umgerechnet.  Danach wird auf eine Spitzenaussteuerung
von --peak ADC-Stufen (Standard 400 von max. 511) normiert.

  python3 wav2raw.py eingabe.wav ausgabe.raw [--peak 400] [--start s] [--dauer s]
"""
import argparse, wave
from math import gcd
import numpy as np
from scipy.signal import resample_poly

ap = argparse.ArgumentParser()
ap.add_argument("wav"); ap.add_argument("raw")
ap.add_argument("--peak", type=float, default=400.0)
ap.add_argument("--start", type=float, default=0.0)
ap.add_argument("--dauer", type=float, default=0.0)
a = ap.parse_args()

w = wave.open(a.wav)
fs = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(float)
if w.getnchannels() > 1:
    x = x.reshape(-1, w.getnchannels())[:, 0]
i0 = int(a.start * fs)
i1 = len(x) if a.dauer <= 0 else min(len(x), i0 + int(a.dauer * fs))
x = x[i0:i1]
g = gcd(8000, fs)
y = resample_poly(x, 8000 // g, fs // g)
y *= a.peak / np.max(np.abs(y))
raw = (np.round(y) + 512).astype("<u2")
raw.tofile(a.raw)
print(f"{a.wav}: {fs} Hz -> 8000 Hz, {len(raw)/8000:.1f} s, Spitze {a.peak:.0f}")
