TARGET = main
MCU = atmega328p
F_CPU = 16000000UL
PORT = /dev/ttyUSB0
PROGRAMMER = arduino
# Nano mit altem Bootloader: 57600, mit neuem: 115200
BAUD = 57600

CC = avr-gcc
OBJCOPY = avr-objcopy
CFLAGS = -Wall -Os -DF_CPU=$(F_CPU) -mmcu=$(MCU) -std=gnu99

all: $(TARGET).hex

$(TARGET).elf: $(TARGET).c
	$(CC) $(CFLAGS) -o $(TARGET).elf $(TARGET).c

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom $(TARGET).elf $(TARGET).hex

size: $(TARGET).elf
	avr-size $(TARGET).elf

flash: $(TARGET).hex
	avrdude -F -V -c $(PROGRAMMER) -p $(MCU) -P $(PORT) -b $(BAUD) -U flash:w:$(TARGET).hex:i

monitor:
	stty -F $(PORT) 9600 cs8 -cstopb -parenb raw clocal -echo
	cat $(PORT)

# PC-Test: dieselbe main.c mit -DHOST_SIM, Abtastwerte aus Datei
#   make test RAW=dwd.raw     (uint16 little endian, 8 kHz, 10-Bit-Werte)
RAW ?= dwd.raw
host_sim: host_sim.c $(TARGET).c
	gcc -O1 -Wall -std=gnu99 -o host_sim host_sim.c

test: host_sim
	./host_sim $(RAW)

# Komplette Testbatterie (synthetische Signale + echte Aufnahme)
testall: host_sim
	python3 tools/test_all.py

# Aufnahme -> Rohdatei:  make raw WAV=testsignal/dwd_146_0kHz.wav RAW=dwd.raw
raw:
	python3 tools/wav2raw.py $(WAV) $(RAW)

clean:
	rm -rf $(TARGET).elf $(TARGET).hex host_sim build_test
