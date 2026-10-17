#!/bin/sh
# Holt HAL, CMSIS und FatFs nach fw/deps (nicht im Repo enthalten).
set -e
cd "$(dirname "$0")"
mkdir -p deps && cd deps
[ -d hal ]          || git clone -q --depth 1 https://github.com/STMicroelectronics/stm32f4xx_hal_driver hal
[ -d cmsis_device ] || git clone -q --depth 1 https://github.com/STMicroelectronics/cmsis_device_f4 cmsis_device
[ -d cmsis_core ]   || git clone -q --depth 1 https://github.com/STMicroelectronics/cmsis_core cmsis_core
# FatFs R0.15 liegt in fw/third_party/fatfs (BSD-Lizenz), kein Download noetig
