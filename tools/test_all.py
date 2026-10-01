#!/usr/bin/env python3
"""test_all.py - Testbatterie fuer den DWD-RTTY-Decoder (host_sim).

Erzeugt synthetische Signale (gen_rtty.py), laesst host_sim darueber laufen und
vergleicht den dekodierten Text mit dem gesendeten.  Die echte Aufnahme
(testsignal/dwd_146_0kHz.wav) wird ueber wav2raw.py eingebunden.

Kennzahlen je Test:
  Zeichen   Anzahl dekodierter Textzeichen
  richtig   Anteil der 16-Zeichen-Fenster des Dekodiertexts, die woertlich im
            gesendeten Text stehen (ein einziger Fehler macht ein Fenster falsch)
  Mitte     zuletzt gemeldete NF-Mitte (Vergleich mit Sollwert)
  Klasse    zuletzt gemeldeter Hub
  Sync      Anzahl "SIGNAL"-Meldungen / Verluste
"""
import re, subprocess, sys, os
here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
gen = os.path.join(here, "gen_rtty.py")
sim = os.path.join(root, "host_sim")
tmp = os.path.join(root, "build_test"); os.makedirs(tmp, exist_ok=True)

def run(raw):
    return subprocess.run([sim, raw], capture_output=True).stdout.decode("latin1")

def analyse(out, ref, center=None):
    lines = out.replace("\r", "").split("\n")
    text = "\n".join(l for l in lines if not l.startswith(("***", "---")))
    locks = len(re.findall(r"\*\*\* SIGNAL", out))
    loss = len(re.findall(r"Signal verloren", out))
    mid = re.findall(r"NF-Mitte ([0-9.]+) Hz", out)
    cls = re.findall(r"Hub (?:ca\. )?(\d+) Hz", out)
    pol = re.findall(r"Mark = (\w+)", out)
    # Fensterweiser Vergleich: Leerraum entfernen (Statuszeilen unterbrechen den
    # Text), den Dekodiertext in Fenster zu 16 Zeichen schneiden und zaehlen,
    # wie viele davon woertlich im (periodisch wiederholten) Sendetext stehen.
    a = re.sub(r"\s+", "", text); b = re.sub(r"\s+", "", ref)
    W = 16
    wins = [a[i:i+W] for i in range(0, len(a) - W + 1, W)]
    found = sum(1 for w in wins if w in b)
    ok = found / max(1, len(wins))
    return dict(n=len(a), ok=ok, mid=mid[-1] if mid else "-", cls=cls[-1] if cls else "-",
                pol=pol[-1] if pol else "-", locks=locks, loss=loss)

TESTS = [
    # name, gen-Argumente, Soll-Mitte, Soll-Hub
    ("450 Hz, sauber",           "--center 1500 --shift 450 --dur 60", 1500, 450),
    ("450 Hz, Mitte 1000",       "--center 1000 --shift 450 --dur 60", 1000, 450),
    ("450 Hz, Mitte 2100",       "--center 2100 --shift 450 --dur 60", 2100, 450),
    ("170 Hz, sauber",           "--center 1700 --shift 170 --dur 60", 1700, 170),
    ("85 Hz, sauber",            "--center 1300 --shift 85 --dur 60", 1300, 85),
    ("85 Hz, Mitte 900",         "--center 900 --shift 85 --dur 60", 900, 85),
    ("85 Hz, Mitte 2200 LSB",    "--center 2200 --shift 85 --dur 60 --invert", 2200, 85),
    ("450 Hz LSB (Mark hoch)",   "--center 1400 --shift 450 --dur 60 --invert", 1400, 450),
    ("85 Hz, Drift +0.3 Hz/s",   "--center 1200 --shift 85 --dur 120 --drift 0.3", 1236, 85),
    ("450 Hz, Drift -1 Hz/s",    "--center 1800 --shift 450 --dur 120 --drift -1", 1680, 450),
    ("85 Hz, Wobble +/-8 Hz",    "--center 1500 --shift 85 --dur 120 --wobble 8", 1500, 85),
    ("450 Hz, S/N 12 dB",        "--center 1500 --shift 450 --dur 90 --snr 12", 1500, 450),
    ("450 Hz, S/N 8 dB",         "--center 1500 --shift 450 --dur 90 --snr 8", 1500, 450),
    ("85 Hz, S/N 12 dB",         "--center 1300 --shift 85 --dur 90 --snr 12", 1300, 85),
    ("450 Hz, Schwund 90 %, 15 dB", "--center 1500 --shift 450 --dur 90 --snr 15 --fade 0.9", 1500, 450),
    ("85 Hz, Schwund 90 %, 15 dB",  "--center 1300 --shift 85 --dur 90 --snr 15 --fade 0.9", 1300, 85),
    ("450 Hz, 25 s Ruhe-Mark vorab", "--center 1500 --shift 450 --dur 90 --idle 25", 1500, 450),
    ("85 Hz, 25 s Ruhe-Mark vorab",  "--center 1300 --shift 85 --dur 90 --idle 25", 1300, 85),
    ("85 Hz, Ausfall 20..45 s",  "--center 1300 --shift 85 --dur 120 --gap 20 45", 1300, 85),
    ("450 Hz, Ausfall 20..45 s", "--center 1500 --shift 450 --dur 120 --gap 20 45", 1500, 450),
]

if __name__ == "__main__":
    subprocess.run(["make", "-s", "-C", root, "host_sim"], check=True)
    sel = sys.argv[1:]
    print(f"{'Test':34s} {'Zeichen':>7s} {'richtig':>8s} {'Mitte':>8s} {'Soll':>6s} {'Hub':>4s} {'Pol':>5s} Sync/Verl")
    for name, args, c0, h0 in TESTS:
        if sel and not any(s in name for s in sel):
            continue
        raw = os.path.join(tmp, "t.raw")
        subprocess.run([sys.executable, gen, raw] + args.split(), check=True, capture_output=True)
        out = run(raw)
        ref = open(raw + ".txt").read()
        r = analyse(out, ref)
        print(f"{name:34s} {r['n']:7d} {100*r['ok']:7.1f}% {r['mid']:>8s} {c0:6d} {r['cls']:>4s} {r['pol']:>5s} {r['locks']}/{r['loss']}")
    # Senderwechsel mitten im Strom: 450 Hz/1500 Hz -> 85 Hz/1100 Hz -> 170 Hz/2000 Hz
    parts = ["--center 1500 --shift 450 --dur 50 --seed 3",
             "--center 1100 --shift 85 --dur 60 --seed 4",
             "--center 2000 --shift 170 --dur 60 --seed 5"]
    if not sel or any(s in "Senderwechsel" for s in sel):
        raw = os.path.join(tmp, "sw.raw"); data = b""; ref = ""
        for k, args in enumerate(parts):
            p = os.path.join(tmp, f"p{k}.raw")
            subprocess.run([sys.executable, gen, p] + args.split(), check=True, capture_output=True)
            data += open(p, "rb").read(); ref += open(p + ".txt").read()
        open(raw, "wb").write(data)
        out = run(raw)
        r = analyse(out, ref)
        allhub = re.findall(r"SIGNAL: NF-Mitte ([0-9.]+) Hz, Hub ca. (\d+)", out)
        print(f"{'Senderwechsel 450->85->170 Hz':34s} {r['n']:7d} {100*r['ok']:7.1f}% {'/'.join(m for m,_ in allhub):>8s} {'':>6s} {'/'.join(h for _,h in allhub):>4s} {r['pol']:>5s} {r['locks']}/{r['loss']}")
    # echte Aufnahme
    wav = os.path.join(root, "testsignal", "dwd_146_0kHz.wav")
    if os.path.exists(wav) and (not sel or any(s in "Aufnahme ECHT" for s in sel)):
        raw = os.path.join(tmp, "dwd.raw")
        subprocess.run([sys.executable, os.path.join(here, "wav2raw.py"), wav, raw], check=True, capture_output=True)
        out = run(raw)
        refp = os.path.join(root, "testsignal", "dwd_146_0kHz_referenz.txt")
        r = analyse(out, open(refp).read() if os.path.exists(refp) else "")
        print(f"{'ECHT: dwd_146_0kHz.wav (85 Hz)':34s} {r['n']:7d} {100*r['ok']:7.1f}% {r['mid']:>8s} {'~1298':>6s} {r['cls']:>4s} {r['pol']:>5s} {r['locks']}/{r['loss']}")
