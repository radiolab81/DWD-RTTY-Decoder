#!/usr/bin/env python3
"""ref_decoder.py - UNABHAENGIGER Referenzdecoder in Python/numpy/scipy (Gleitkomma,
lineare FIR-Filter, Abtastung genau in Bitmitte).  Nur dazu da, einen
Soll-Text fuer die echte Aufnahme zu liefern, gegen den der ATmega-Code
(ganz anderer Algorithmus, Festkomma) verglichen wird.
  python3 ref_decoder.py aufnahme.wav [mitte_hz] > referenz.txt
"""
import sys, wave
import numpy as np
from scipy import signal
LT = "\0E\nA SIU\rDRJNFCKTZLWHYPQOBG\x1bMXV\x1f"
FG = "\x003\n- '87\r$4\x07,!:(5+)2#6019?&\x1b./=\x1f"
w = wave.open(sys.argv[1]); fs = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(float)
fc = float(sys.argv[2]) if len(sys.argv) > 2 else 1298.0
n = np.arange(len(x))
z = x * np.exp(-2j*np.pi*fc*n/fs)
z = signal.lfilter(signal.firwin(201, 90, fs=fs), 1, z)[::7]; fr = fs/7
d = np.angle(z[1:]*np.conj(z[:-1]))*fr/2/np.pi
d = signal.lfilter(np.ones(5)/5, 1, d)
b = (d < 0).astype(int)                      # 1 = Mark (tieferer Ton)
spb = fr/50; out = []; i = 0; fig = False
while i < len(b) - int(9*spb):
    if b[i] == 1 and b[i+1] == 0 and b[i+2] == 0:
        t0 = i+1
        if b[int(t0+0.5*spb)] == 0:
            bits = [b[int(t0+(k+0.5)*spb)] for k in range(1, 7)]
            if bits[5] == 1:
                v = sum(bits[k] << k for k in range(5))
                if v == 27: fig = True
                elif v == 31: fig = False
                else:
                    c = (FG if fig else LT)[v]
                    if c not in "\0\x07\r": out.append(c)
                i = int(t0 + 7.0*spb); continue
    i += 1
sys.stdout.write("".join(out))
