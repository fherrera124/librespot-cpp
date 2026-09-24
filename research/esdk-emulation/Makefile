# Native build (Raspberry Pi armhf, or any ARM/x86 with a matching .so).
# The eSDK .so is dlopen'd at runtime, so the host arch must match it
# (esdk-1.20.0-armhf-v7.so = 32-bit ARMv7 hard-float).
CC     ?= cc
CFLAGS ?= -O2 -Wall

esdk_server: src/esdk_server.c
	$(CC) $(CFLAGS) -o $@ $< -ldl -lm

clean:
	rm -f esdk_server

.PHONY: clean
