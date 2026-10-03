CC ?= gcc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Wpedantic

build/test_core: tests/test_core.c core/noise_core.c core/noise_core.h
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_core.c core/noise_core.c -lm

test: build/test_core
	./build/test_core out

.PHONY: test
