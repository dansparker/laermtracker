/* FatFs-Anbindung an HAL_SD (Polling, 4-Bit-SDIO). Die Karte wird in main() initialisiert. */
#include "ff.h"
#include "diskio.h"
#include "stm32f4xx_hal.h"

extern SD_HandleTypeDef hsd;
#define SD_TIMEOUT_MS 2000u

static uint32_t abuf[128];                                   /* ausgerichteter Zwischenpuffer (HAL braucht Wortzugriff) */

static int wait_transfer(void)
{
    uint32_t t0 = HAL_GetTick();
    while (HAL_SD_GetCardState(&hsd) != HAL_SD_CARD_TRANSFER)
        if (HAL_GetTick() - t0 > SD_TIMEOUT_MS) return -1;
    return 0;
}

DSTATUS disk_status(BYTE pdrv) { (void)pdrv; return hsd.State == HAL_SD_STATE_READY ? 0 : STA_NOINIT; }
DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (wait_transfer()) return RES_ERROR;
    if (((uintptr_t)buff & 3u) == 0)
        return HAL_SD_ReadBlocks(&hsd, buff, sector, count, SD_TIMEOUT_MS) == HAL_OK ? RES_OK : RES_ERROR;
    for (UINT i = 0; i < count; i++) {
        if (wait_transfer() || HAL_SD_ReadBlocks(&hsd, (uint8_t *)abuf, sector + i, 1, SD_TIMEOUT_MS) != HAL_OK) return RES_ERROR;
        for (int k = 0; k < 512; k++) buff[i * 512 + k] = ((uint8_t *)abuf)[k];
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (wait_transfer()) return RES_ERROR;
    if (((uintptr_t)buff & 3u) == 0)
        return HAL_SD_WriteBlocks(&hsd, (uint8_t *)buff, sector, count, SD_TIMEOUT_MS) == HAL_OK ? RES_OK : RES_ERROR;
    for (UINT i = 0; i < count; i++) {
        for (int k = 0; k < 512; k++) ((uint8_t *)abuf)[k] = buff[i * 512 + k];
        if (wait_transfer() || HAL_SD_WriteBlocks(&hsd, (uint8_t *)abuf, sector + i, 1, SD_TIMEOUT_MS) != HAL_OK) return RES_ERROR;
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
