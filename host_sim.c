/* host_sim.c - PC-Testumgebung: liest 10-Bit-Rohwerte (uint16 LE, 8 kHz)
 * aus einer Datei und speist sie in dieselbe Dekoderkette wie der ADC.
 * Aufruf: ./host_sim datei.raw          (Ausgabe = UART-Text)          */
#define HOST_SIM
#define F_CPU 16000000UL
#include "main.c"

int main(int argc, char **argv)
{
    FILE *f;
    uint16_t v;
    if (argc < 2 || !(f = fopen(argv[1], "rb"))) {
        fprintf(stderr, "Aufruf: %s datei.raw\n", argv[0]);
        return 1;
    }
    rtty_init();
    while (fread(&v, 2, 1, f) == 1) {
        dsp_sample((int16_t)v - 512);
        main_step();
    }
    fclose(f);
    return 0;
}
