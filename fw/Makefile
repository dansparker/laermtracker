# Firmware bauen:  sh fetch_deps.sh && make
# Benoetigt arm-none-eabi-gcc. HSE-Quarz anders als 8 MHz:  make HSE_VALUE=25000000U
PREFIX ?= arm-none-eabi-
CC := $(PREFIX)gcc
SIZE := $(PREFIX)size
OBJCOPY := $(PREFIX)objcopy
HSE_VALUE ?= 8000000U

D := deps
HAL := $(D)/hal
FF := $(shell dirname $$(find $(D)/fatfs -name ff.c | head -n1))
HAL_SRC := stm32f4xx_hal stm32f4xx_hal_cortex stm32f4xx_hal_dma stm32f4xx_hal_flash stm32f4xx_hal_flash_ex \
  stm32f4xx_hal_gpio stm32f4xx_hal_i2s stm32f4xx_hal_pwr stm32f4xx_hal_pwr_ex stm32f4xx_hal_rcc stm32f4xx_hal_rcc_ex \
  stm32f4xx_hal_rtc stm32f4xx_hal_rtc_ex stm32f4xx_hal_sd stm32f4xx_ll_sdmmc

SRCS := Src/main.c Src/diskio.c Src/stm32f4xx_it.c ../core/noise_core.c \
  $(HAL_SRC:%=$(HAL)/Src/%.c) $(FF)/ff.c \
  $(D)/cmsis_device/Source/Templates/system_stm32f4xx.c
ASMS := $(D)/cmsis_device/Source/Templates/gcc/startup_stm32f446xx.s

CPU := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
INC := -IInc -I../core -I$(FF) -I$(HAL)/Inc -I$(D)/cmsis_device/Include -I$(D)/cmsis_core/Include
DEF := -DUSE_HAL_DRIVER -DSTM32F446xx -DHSE_VALUE=$(HSE_VALUE)
CFLAGS := $(CPU) $(DEF) $(INC) -Os -g -std=gnu11 -Wall -ffunction-sections -fdata-sections
LDFLAGS := $(CPU) -TSTM32F446RETx_FLASH.ld --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -Wl,-Map=build/laermtracker.map -lm

OBJS := $(SRCS:%.c=build/%.o) $(ASMS:%.s=build/%.o)

all: build/laermtracker.bin
	$(SIZE) build/laermtracker.elf

build/laermtracker.elf: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
build/laermtracker.bin: build/laermtracker.elf
	$(OBJCOPY) -O binary $< $@
build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
build/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(CPU) -x assembler-with-cpp -c $< -o $@
clean:
	rm -rf build
.PHONY: all clean
