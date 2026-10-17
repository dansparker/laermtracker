/*
 * FatFs-Anbindung an HAL_SD (4-Bit-SDIO, Polling).
 * Der Audio-Interrupt dauert laenger als der SDIO-FIFO (32 Byte = ca. 5 us) braucht, um leerzulaufen. Deshalb werden die
 * Datenphasen mit gesperrten Interrupts ausgefuehrt (max. 4 Sektoren = ca. 0,4 ms; I2S-DMA laeuft in der Hardware weiter,
 * der Halbpuffer hat 8 ms Reserve). Die Wartezeit auf die Karte (Programmierung) laeuft mit freigegebenen Interrupts.
 * Schlaegt ein Transfer fehl, wird er nach CMD12 bis zu 4x wiederholt.
 */
#include "ff.h"
#include "diskio.h"
#include "stm32f4xx_hal.h"
#include "dbg.h"

extern SD_HandleTypeDef hsd;
#define SD_TIMEOUT_MS 1000u
#define SD_RETRIES    4
#define BOUNCE_SECT   4                                       /* 2 kB: ausgerichteter Zwischenpuffer */

static uint32_t bounce[BOUNCE_SECT * 128] __attribute__((aligned(16)));

static int wait_transfer(void)
{
    uint32_t t0 = HAL_GetTick();
    while (HAL_SD_GetCardState(&hsd) != HAL_SD_CARD_TRANSFER) {
        WDG_KICK();
        if (HAL_GetTick() - t0 > SD_TIMEOUT_MS) return -1;
    }
    return 0;
}

static int sd_xfer(int write, uint8_t *buf, uint32_t sector, uint32_t count)
{
    DBG_SD_OP = ((write ? 2u : 1u) << 24) | count; DBG_SD_SECT = sector;
    for (int attempt = 0; attempt < SD_RETRIES; attempt++) {
        WDG_KICK();
        if (attempt) { HAL_SD_Abort(&hsd); HAL_Delay(2); }     /* Karte nach Fehler (CMD12) wieder in den Transfer-Zustand */
        if (wait_transfer()) continue;
        __disable_irq();
        HAL_StatusTypeDef r = write ? HAL_SD_WriteBlocks(&hsd, buf, sector, count, SD_TIMEOUT_MS)
                                    : HAL_SD_ReadBlocks(&hsd, buf, sector, count, SD_TIMEOUT_MS);
        __enable_irq();
        if (r == HAL_OK && !wait_transfer()) { DBG_SD_OP = 0; return 0; }
        DBG_SD_ERR = hsd.ErrorCode;
    }
    DBG_SD_OP = 0;
    return -1;
}

DSTATUS disk_status(BYTE pdrv) { (void)pdrv; return hsd.State == HAL_SD_STATE_READY ? 0 : STA_NOINIT; }
DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    while (count) {
        UINT n = count > BOUNCE_SECT ? BOUNCE_SECT : count;
        if (sd_xfer(0, (uint8_t *)bounce, (uint32_t)sector, n)) return RES_ERROR;
        for (UINT k = 0; k < n * 512; k++) buff[k] = ((uint8_t *)bounce)[k];
        buff += n * 512; sector += n; count -= n;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    while (count) {
        UINT n = count > BOUNCE_SECT ? BOUNCE_SECT : count;
        for (UINT k = 0; k < n * 512; k++) ((uint8_t *)bounce)[k] = buff[k];
        if (sd_xfer(1, (uint8_t *)bounce, (uint32_t)sector, n)) return RES_ERROR;
        buff += n * 512; sector += n; count -= n;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    HAL_SD_CardInfoTypeDef ci;
    switch (cmd) {
    case CTRL_SYNC: return wait_transfer() ? RES_ERROR : RES_OK;
    case GET_SECTOR_COUNT: HAL_SD_GetCardInfo(&hsd, &ci); *(LBA_t *)buff = ci.LogBlockNbr; return RES_OK;
    case GET_SECTOR_SIZE:  *(WORD *)buff = 512; return RES_OK;
    case GET_BLOCK_SIZE:   HAL_SD_GetCardInfo(&hsd, &ci); *(DWORD *)buff = ci.LogBlockSize / 512; return RES_OK;
    default: return RES_PARERR;
    }
}
