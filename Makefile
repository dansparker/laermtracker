CC ?= gcc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Wpedantic

SRC := core/noise_core.c core/noise_core.h

build/test_core: tests/test_core.c $(SRC)
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_core.c core/noise_core.c -lm

build/test_edge: tests/test_edge.c $(SRC)
	mkdir -p build
	$(CC) $(CFLAGS) -Wno-unused-result -o $@ tests/test_edge.c core/noise_core.c -lm

# Kern muss ohne Warnungen uebersetzbar sein
lint:
	$(CC) -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror -c core/noise_core.c -o /dev/null

test: build/test_core
	./build/test_core out

test-edge: build/test_edge
	./build/test_edge out_edge

.PHONY: test test-edge lint
