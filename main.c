#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <wiringPi.h>
#include <time.h>
#include <signal.h>

/*
From OpenShock website (https://openshock.org/hardware/shockers/caixianlin/)

Name	Value	Length	Remarks
Prefix	0xFC	2 bits
Transmitter ID	0 - 65535	16 bits	The collar will be Paired to this
Channel Number	0 - 2	4 bits	The collar will be Paired to this
Action Command	1 - 3	4 bits	1 = Shock, 2 = Vibrate, 3 = Beep
Command Intensity	0 - 99	8 bits	Should always be 0 for beep
Message checksum	Calculated	8 bits	(sum of everything in front except prefix) modulo 256
Postfix	0x88	2 bits

[PREFIX        ] = XX
[TRANSMITTER ID] =   XXXXXXXXXXXXXXXX
[CHANNEL       ] =                   XXXX
[MODE          ] =                       XXXX
[STRENGTH      ] =                           XXXXXXXX
[CHECKSUM      ] =                                   XXXXXXXX
[END           ] =                                           XX

Every 1 is encoded as 1110 or 0xE Eveery 0 is encoded as 1000 or 0x8

fc e8ee8e88e88e8eee 8888 88ee 88888888 8e88eee8 88

1 kHz
*/

enum channel {
    CHANNEL_1 = 0,
    CHANNEL_2 = 1,
    CHANNEL_3 = 2,
};

enum mode {
    MODE_SHOCK   = 1,
    MODE_VIBRATE = 2,
    MODE_BEEP    = 3,
};

struct command_params {
    uint16_t     transmitterId;
    enum channel channel;
    enum mode    mode;
    uint8_t      strength;

    unsigned int repeat;
};

static volatile bool keepRunning = true;

#define QUARTER_RATE 4000 // 250 usec

static unsigned quarter_phase_delay_usec = 179; // 250 * 0.7;

static inline void send_quarter_phase(int signalValue) {
    digitalWrite(0, signalValue ? HIGH : LOW);
    delayMicroseconds(quarter_phase_delay_usec);
}

static inline void send_single_bit(int bitval) {
    if (bitval == 1) {
        send_quarter_phase(1);
        send_quarter_phase(1);
        send_quarter_phase(1);
        send_quarter_phase(0);
    } else {
        send_quarter_phase(1);
        send_quarter_phase(0);
        send_quarter_phase(0);
        send_quarter_phase(0);
    }
}

// MSB first
static inline void send_multiple_bits(uint32_t value, size_t bits) {
    for (int bit = bits - 1; bit >= 0; bit--) {
        int bitval = value >> bit & 1;
        send_single_bit(bitval);
    }
}

/*
 */
void send_command(struct command_params *params) {
    uint8_t checksum = (params->transmitterId >> 8) + (params->transmitterId & 0xFF) + (params->channel << 4 | params->mode) + params->strength;
    for (unsigned int tx = 0; keepRunning && tx < params->repeat; tx++) {
        const char *p = "{ICMSK}";
        while (*p) {
            if (*p == '{') {
                send_quarter_phase(1);
                send_quarter_phase(1);
                send_quarter_phase(1);
                send_quarter_phase(1);
                send_quarter_phase(1);
                send_quarter_phase(1);
                send_quarter_phase(0);
                send_quarter_phase(0);
            } else if (*p == '}') {
                send_single_bit(0);
                send_single_bit(0);
            } else if (*p == 'I') {
                send_multiple_bits(params->transmitterId, 16);
            } else if (*p == 'C') {
                send_multiple_bits(params->channel, 4);
            } else if (*p == 'M') {
                send_multiple_bits(params->mode, 4); // 3 = beep
            } else if (*p == 'S') {
                send_multiple_bits(params->strength, 8);
            } else if (*p == 'K') {
                send_multiple_bits(checksum, 8);
            }
            p++;
        }
    }
}

static int64_t timespecDiff(struct timespec *timeA_p, struct timespec *timeB_p) {
    return ((timeA_p->tv_sec * 1000000000) + timeA_p->tv_nsec) -
           ((timeB_p->tv_sec * 1000000000) + timeB_p->tv_nsec);
}

void intHandler() {
    keepRunning = false;
}

/*
 * main
 */
int main(int argc, char **argv) {
    if (wiringPiSetup() == -1) {
        fprintf(stderr, "Failed to initialize wiringPi!\n");
        exit(EXIT_FAILURE);
    }

    pinMode(0, OUTPUT);

    struct command_params params = {
        .transmitterId = 46231,
        .channel       = CHANNEL_1,
        .mode          = MODE_BEEP,
        .strength      = 0, // 0..99
        .repeat        = 1,
    };

    int opt;
    while ((opt = getopt(argc, argv, "d:i:c:bv:s:r:")) != -1) {
        switch (opt) {
            case 'd': {
                quarter_phase_delay_usec = atoi(optarg);
                break;
            }
            case 'c': {
                params.channel = atoi(optarg) - 1;
                break;
            }
            case 'b': {
                params.mode     = MODE_BEEP;
                params.strength = 0;
                break;
            }
            case 'v': {
                params.mode     = MODE_VIBRATE;
                params.strength = atoi(optarg);
                break;
            }
            case 's': {
                params.mode     = MODE_SHOCK;
                params.strength = atoi(optarg);
                break;
            }
            case 'r': {
                params.repeat = atoi(optarg);
                break;
            }
            default: /* '?' */
                fprintf(stderr, "Usage: %s [-d usec] [-i id] [-c channel] [-b] [-v strength] [-s strength] [-r count]\n", argv[0]);
                fprintf(stderr, "  d: delay. 0 for automatic calibration.\n");
                fprintf(stderr, "  i: id. default 46231.\n");
                fprintf(stderr, "  c: channel. 1..3. default 1.\n");
                fprintf(stderr, "  b: beep mode. default.\n");
                fprintf(stderr, "  v: vibrate mode. strength: 0..99.\n");
                fprintf(stderr, "  s: shock mode. strength: 0..99.\n");
                fprintf(stderr, "  r: repeat. default 1.\n");
                exit(EXIT_FAILURE);
        }
    }

    if (quarter_phase_delay_usec == 0) {
        printf("Calibrating\n");
        quarter_phase_delay_usec = 1000 * 1000 / QUARTER_RATE;
        for (int z = 0; z < 10; z++) {
            printf("%u\n", quarter_phase_delay_usec);
            struct timespec start, end;
            clock_gettime(CLOCK_MONOTONIC, &start);
            struct command_params testparams = {
                .transmitterId = 0x0000,
                .channel       = 0,
                .mode          = 0,
                .strength      = 0,
                .repeat        = 1,
            };
            send_command(&testparams);
            clock_gettime(CLOCK_MONOTONIC, &end);
            uint64_t     timeElapsed   = timespecDiff(&end, &start);
            unsigned int measured_usec = timeElapsed / (44 /*bits*/ * 4) / 1000;
            quarter_phase_delay_usec   = 1000 * 1000 / QUARTER_RATE * quarter_phase_delay_usec / measured_usec;
        }
    }

    signal(SIGINT, intHandler);

    /*while (keepRunning)*/ {
        printf(".\n");
        send_command(&params);
        // delayMicroseconds(10000);
    }

    printf("Exiting...\n");

    pinMode(0, INPUT);

    exit(EXIT_SUCCESS);
}
