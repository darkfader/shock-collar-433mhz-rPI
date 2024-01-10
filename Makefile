# GNU make makefile :3

.PHONY: all
all: main

main: main.c
	gcc -o $@ $+ -lwiringPi -lwiringPiDev -O3 -Wall -Wextra -fmax-errors=5

.PHONY: clean
clean:
	rm -rf main
