/*
 * FatFs-Anbindung an HAL_SD: 4-Bit-SDIO mit DMA.
 * Wichtig: reine Polling-Transfers (HAL_SD_ReadBlocks/WriteBlocks) laufen bei aktivem Audio-Interrupt in FIFO-Unterlaeufe
 * (Schreibfehler -> FatFs sperrt die Datei: WAV abgeschnitten, Header 0). Mit DMA + Wiederholung tritt das nicht auf.
 */
#include "ff.h"
#include "diskio.h"
#include "stm32f4xx_hal.h"

extern SD_HandleTypeDef hsd;
#define SD_TIMEOUT_MS 1000u
#define SD_RETRIES    4
#define BOUNCE_SECT   4                                       /* 2 kB: ausgerichteter Zwischenpuffer */

static uint32_t bounce[BOUNCE_SECT * 128] __attribute__((aligned(16)));
static volatile uint32_t sd_done;                             /* 1 = fertig, 2 = Fehler */

void HAL_SD_TxCpltCallback(SD_HandleTypeDef *h) { (void)h; sd_done = 1; }
void HAL_SD_RxCpltCallback(SD_HandleTypeDef *h) { (void)h; sd_done = 1; }
void HAL_SD_ErrorCallback(SD_HandleTypeDef *h)  { (void)h; sd_done = 2; }
void HAL_SD_AbortCallback(SD_HandleTypeDef *h)  { (void)h; }

static int wait_transfer(void)
{
    uint32_t t0 = HAL_GetTick();
    while (HAL_SD_GetCardState(&hsd) != HAL_SD_CARD_TRANSFER)
        if (HAL_GetTick() - t0 > SD_TIMEOUT_MS) return -1;
    return 0;
}

/* buf muss 16-Byte-ausgerichtet sein */
static int sd_xfer(int write, uint8_t *buf, uint32_t sector, uint32_t count)
{
    for (int attempt = 0; attempt < SD_RETRIES; attempt++) {
        if (attempt) { HAL_SD_Abort(&hsd); HAL_Delay(2); }     /* Karte nach Fehler (CMD12) wieder in den Transfer-Zustand */
        if (wait_transfer()) continue;
        sd_done = 0;
        HAL_StatusTypeDef r = write ? HAL_SD_WriteBlocks_DMA(&hsd, buf, sector, count)
                                    : HAL_SD_ReadBlocks_DMA(&hsd, buf, sector, count);
        if (r != HAL_OK) continue;
        uint32_t t0 = HAL_GetTick();
        while (!sd_done && HAL_GetTick() - t0 < SD_TIMEOUT_MS) {}
        if (sd_done == 1 && !wait_transfer()) return 0;
    }
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
