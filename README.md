# DWD RTTY Decoder for the ATmega328P

🇬🇧 English | [🇩🇪 Deutsch](README.de.md)

Bare-metal decoder (avr-gcc / avr-libc, no Arduino core, no printf, no float) for the
radioteletype (RTTY) transmissions of the German Weather Service (DWD): DDH47 on
147.3 kHz and DDK2/DDH7/DDK9/DDH9/DDH8 on shortwave. 

Output: the received plain text on USART0 (9600 8N1), interspersed with short status
lines (`*** SIGNAL ...`, `--- Status ...`).

## What is detected automatically

Nothing has to be configured. All DWD transmitters use 50 baud, ITA2, 1 start bit +
5 data bits + 1.5 stop bits; they differ only in frequency shift (hub).

| Parameter | How |
|---|---|
| Position in the audio (625 … 2500 Hz) | A Goertzel sweep (61 DFT bins, 31.25 Hz spacing) measures the occupied bandwidth; its center is the NCO start frequency |
| Shift 85 / 170 / 450 Hz | From the occupied bandwidth (measured approx. 150–170 / 260 / 530 Hz); determines the filter bandwidth |
| Polarity (USB/LSB, mark high/low) | Two character receivers run in parallel, a score decides which one is used |
| BFO drift | Frequency-locked loop (FLL) on the center of the tone positions; capture range theoretically 0.6 × shift/2 (approx. ±25 Hz at 85 Hz shift, approx. ±135 Hz at 450 Hz), not separately measured |
| Transmitter change / signal loss | Four watchdogs (level, error rate, shift check, character watchdog) restart the search when needed |

## Hardware

* Audio from the receiver (SSB mode) to **ADC0** (A0), biased to Vcc/2, preceded by a
  low-pass filter at about 3 kHz (anti-aliasing; sample rate 8 kHz).
* USART0, 9600 8N1.
* Tune the receiver so that the signal appears at **625 … 2500 Hz** in the audio
  (e.g. 147.3 kHz in USB, receiver set to 146.0 kHz -> center at 1300 Hz).
* Optional `-DDEBUG_LOAD`: PB0 (Arduino pin 8) is high during the ADC interrupt
  routine. Its duty cycle is the CPU load of the interrupt chain.

## Building and testing

```sh
make                     # main.hex  (approx. 7.4 KB flash, approx. 0.8 KB RAM)
make flash               # adjust port/baud rate in the Makefile
make raw WAV=testsignal/dwd_146_0kHz.wav RAW=dwd.raw   # WAV -> 8 kHz raw file
make test RAW=dwd.raw    # PC simulation, same main.c code
make testall             # complete test suite
```

The test tools in `tools/` require Python 3 with numpy and scipy.

| File | Purpose |
|---|---|
| `main.c` | The decoder (AVR and, with `-DHOST_SIM`, PC) |
| `host_sim.c` | PC test harness: reads samples from a file instead of the ADC |
| `tools/wav2raw.py` | WAV -> 8 kHz raw file; reads the sample rate from the header (the test recording uses 7119 Hz) and resamples to exactly 8000 Hz with a polyphase resampler |
| `tools/gen_rtty.py` | Synthetic RTTY signal with adjustable shift, center, drift, noise, fading and polarity |
| `tools/ref_decoder.py` | Independent reference decoder in Python (floating point, different algorithm) |
| `tools/test_all.py` | Test suite, writes a result table |
| `testsignal/` | Test recording, reference text, decoded text, `testergebnis.txt` (test results) |

## Test results

Full table: `testsignal/testergebnis.txt`.

* **Real recording** `dwd_146_0kHz.wav` (260 s, DDH47): center approx. 1297 Hz, shift
  85 Hz, mark = lower tone. The text matches the independent reference decoder over
  the entire duration, apart from the settling phase at the start; the search takes
  about 5 s.
* **Synthetic signals:** shift 85/170/450 Hz, center 900 … 2200 Hz, USB and LSB,
  drift +0.3 / −1 Hz/s, wobble ±8 Hz, noise, 25 s of idle mark before the data,
  25 s signal dropout: consistently 100 % correct text windows.
* **Transmitter change in mid-stream** (450 -> 85 -> 170 Hz, each with a different
  center): all three signals are detected; characters during the switchover are
  lost (92 %).
* **Sensitivity** (S/N in the 2.4 kHz audio channel): 85 Hz shift error-free down to at
  least 3 dB, 450 Hz shift error-free down to approx. 6 dB. Deep fading (90 %) at
  450 Hz: 71 %.

## Known limitations

* After a transmission pause or a transmitter change, re-acquisition takes about 5 s
  (search sweep); in the special case that the tracker locks onto only one of the two
  new tones, the character watchdog kicks in after 20 s.
* Retuning the receiver faster than the tracker's capture range allows triggers a new
  search (message `Signal verloren`, i.e. "signal lost").
* The AVR's CPU load is an estimate (interrupt at most approx. 700 cycles plus
  multiplication library calls, against a budget of 2000 cycles per sample); it can
  be measured with `-DDEBUG_LOAD`.
* ITA2 figures case: international assignment, as seen in the DWD texts. Bell and
  carriage return are not output.
* Program output (status lines, messages) is in German.