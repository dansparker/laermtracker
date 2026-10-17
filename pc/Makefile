# Linux:   sudo apt install libasound2-dev && make -C pc      -> pc/laermtracker (Mikrofon ueber ALSA)
# Windows (MinGW/MSYS2):  make -C pc windows                   -> pc/laermtracker.exe
CC ?= gcc
SRC := laermtracker.c ../core/noise_core.c

laermtracker: $(SRC)
	$(CC) -std=gnu99 -O2 -Wall -Wextra -DHAVE_ALSA -o $@ $(SRC) -lasound -lm

windows: $(SRC)
	$(CC) -std=gnu99 -O2 -Wall -Wextra -static -o laermtracker.exe $(SRC) -lwinmm -lm

.PHONY: windows
