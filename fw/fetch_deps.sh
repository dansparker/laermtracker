#!/bin/sh
# Holt HAL, CMSIS und FatFs nach fw/deps (nicht im Repo enthalten).
set -e
cd "$(dirname "$0")"
mkdir -p deps && cd deps
[ -d hal ]          || git clone -q --depth 1 https://github.com/STMicroelectronics/stm32f4xx_hal_driver hal
[ -d cmsis_device ] || git clone -q --depth 1 https://github.com/STMicroelectronics/cmsis_device_f4 cmsis_device
[ -d cmsis_core ]   || git clone -q --depth 1 https://github.com/STMicroelectronics/cmsis_core cmsis_core
if [ ! -d fatfs ]; then
  curl -fsSL -o ff15.zip https://elm-chan.org/fsw/ff/arc/ff15.zip
  mkdir fatfs && unzip -q ff15.zip -d fatfs && rm ff15.zip
  find fatfs -name ffconf.h -delete        # eigene Konfiguration in fw/Inc/ffconf.h
fi
