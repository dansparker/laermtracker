/*
 * Laermtracker - Firmware fuer WeAct STM32F446 CoreBoard
 *   ICS-43434 an I2S2 (PB12 WS, PB13 CK, PB15 SD), microSD an SDIO (4 Bit), RTC mit LSE.
 *   LED PB2 (low-aktiv): an waehrend ein Ereignis aufgezeichnet wird; schnelles Blinken = Fehler.
 */
#include "stm32f4xx_hal.h"
#include "ff.h"
#include "noise_core.h"
#include "config.h"
#include <stdio.h>
#include <string.h>

SD_HandleTypeDef hsd;                         /* von diskio.c benutzt */
DMA_HandleTypeDef hdma_spi2_rx;               /* von stm32f4xx_it.c benutzt */
static I2S_HandleTypeDef  hi2s2;
static RTC_HandleTypeDef  hrtc;
static FATFS   fatfs;
static FIL     files[2];
static uint8_t file_used[2];
static nc_t    nc;
static int16_t ring[RING_SAMPLES];
static uint16_t dma_buf[DMA_WORDS * 2];       /* je 32-bit-Wort zwei Halbworte (hoch, tief) */

#define RTC_MAGIC 0x32F2u
#define HALF_SAMPLES (DMA_WORDS / 4u)         /* Frames (L+R) je Halbpuffer */

/* ------------------------------------------------------------------ LED / Fehler */
static void led(int on) { HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, on ? GPIO_PIN_RESET : GPIO_PIN_SET); }
static void fatal(void) { for (;;) { HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_2); HAL_Delay(100); } }

/* ------------------------------------------------------------------ Takte */
static void clock_init(void)
{
    RCC_OscInitTypeDef o = {0};
    RCC_ClkInitTypeDef c = {0};
    RCC_PeriphCLKInitTypeDef p = {0};
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    o.OscillatorType = RCC_OSCILLATORTYPE_HSE | RCC_OSCILLATORTYPE_LSE;
    o.HSEState = RCC_HSE_ON;
    o.LSEState = RCC_LSE_ON;
    o.PLL.PLLState = RCC_PLL_ON;
    o.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    o.PLL.PLLM = HSE_VALUE / 1000000u;        /* VCO-Eingang 1 MHz */
    o.PLL.PLLN = 336;
    o.PLL.PLLP = RCC_PLLP_DIV2;               /* 168 MHz */
    o.PLL.PLLQ = 7;                           /* 48 MHz fuer SDIO */
    o.PLL.PLLR = 2;
    if (HAL_RCC_OscConfig(&o) != HAL_OK) {    /* ohne LSE weiter (RTC dann ohne Quarz) */
        o.OscillatorType = RCC_OSCILLATORTYPE_HSE; o.LSEState = RCC_LSE_OFF;
        if (HAL_RCC_OscConfig(&o) != HAL_OK) fatal();
    }
    c.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    c.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    c.AHBCLKDivider = RCC_SYSCLK_DIV1;
    c.APB1CLKDivider = RCC_HCLK_DIV4;
    c.APB2CLKDivider = RCC_HCLK_DIV2;
    if (HAL_RCC_ClockConfig(&c, FLASH_LATENCY_5) != HAL_OK) fatal();

    p.PeriphClockSelection = RCC_PERIPHCLK_I2S_APB1 | RCC_PERIPHCLK_RTC;
    p.PLLI2S.PLLI2SM = HSE_VALUE / 1000000u;  /* 1 MHz */
    p.PLLI2S.PLLI2SN = 256;                   /* VCO 256 MHz */
    p.PLLI2S.PLLI2SP = RCC_PLLP_DIV2;
    p.PLLI2S.PLLI2SQ = 2;
    p.PLLI2S.PLLI2SR = 5;                     /* I2S-Takt 51,2 MHz => fs = 51,2 MHz / (64 * 25) = 32 kHz */
    p.I2sApb1ClockSelection = RCC_I2SAPB1CLKSOURCE_PLLI2S;
    p.RTCClockSelection = __HAL_RCC_GET_FLAG(RCC_FLAG_LSERDY) ? RCC_RTCCLKSOURCE_LSE : RCC_RTCCLKSOURCE_LSI;
    if (HAL_RCCEx_PeriphCLKConfig(&p) != HAL_OK) fatal();
}

/* ------------------------------------------------------------------ MSP (GPIO/DMA/Takte) */
void HAL_I2S_MspInit(I2S_HandleTypeDef *h)
{
    if (h->Instance != SPI2) return;
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_SPI2_CLK_ENABLE(); __HAL_RCC_GPIOB_CLK_ENABLE(); __HAL_RCC_DMA1_CLK_ENABLE();
    g.Pin = GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_15;   /* WS, CK, SD */
    g.Mode = GPIO_MODE_AF_PP; g.Pull = GPIO_NOPULL; g.Speed = GPIO_SPEED_FREQ_HIGH; g.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(GPIOB, &g);
    hdma_spi2_rx.Instance = DMA1_Stream3;
    hdma_spi2_rx.Init.Channel = DMA_CHANNEL_0;
    hdma_spi2_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_spi2_rx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_spi2_rx.Init.MemInc = DMA_MINC_ENABLE;
    hdma_spi2_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_spi2_rx.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    hdma_spi2_rx.Init.Mode = DMA_CIRCULAR;
    hdma_spi2_rx.Init.Priority = DMA_PRIORITY_HIGH;
    hdma_spi2_rx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_spi2_rx) != HAL_OK) fatal();
    __HAL_LINKDMA(h, hdmarx, hdma_spi2_rx);
    HAL_NVIC_SetPriority(DMA1_Stream3_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream3_IRQn);
}

void HAL_SD_MspInit(SD_HandleTypeDef *h)
{
    if (h->Instance != SDIO) return;
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_SDIO_CLK_ENABLE(); __HAL_RCC_GPIOC_CLK_ENABLE(); __HAL_RCC_GPIOD_CLK_ENABLE();
    g.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11;      /* D0..D3 */
    g.Mode = GPIO_MODE_AF_PP; g.Pull = GPIO_PULLUP; g.Speed = GPIO_SPEED_FREQ_VERY_HIGH; g.Alternate = GPIO_AF12_SDIO;
    HAL_GPIO_Init(GPIOC, &g);
    g.Pin = GPIO_PIN_12; g.Pull = GPIO_NOPULL;                         /* CK */
    HAL_GPIO_Init(GPIOC, &g);
    g.Pin = GPIO_PIN_2; g.Pull = GPIO_PULLUP;                          /* CMD */
    HAL_GPIO_Init(GPIOD, &g);
}

/* ------------------------------------------------------------------ RTC / Zeit */
static int64_t rtc_now_ms(void *u)
{
    (void)u;
    RTC_TimeTypeDef t; RTC_DateTypeDef d;
    HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);
    int ms = (int)((t.SecondFraction - t.SubSeconds) * 1000u / (t.SecondFraction + 1u));
    return nc_ms_from_civil(2000 + d.Year, d.Month, d.Date, t.Hours, t.Minutes, t.Seconds, ms);
}

static void rtc_set(int y, int mo, int d, int h, int mi, int s)
{
    RTC_TimeTypeDef t = {0}; RTC_DateTypeDef dt = {0};
    int64_t days = nc_ms_from_civil(y, mo, d, 0, 0, 0, 0) / 86400000;
    dt.WeekDay = (uint8_t)((days + 3) % 7 + 1); dt.Month = (uint8_t)mo; dt.Date = (uint8_t)d; dt.Year = (uint8_t)(y - 2000);
    t.Hours = (uint8_t)h; t.Minutes = (uint8_t)mi; t.Seconds = (uint8_t)s;
    HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BIN);
    HAL_RTC_SetDate(&hrtc, &dt, RTC_FORMAT_BIN);
    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, RTC_MAGIC);
}

static void rtc_init(void)
{
    __HAL_RCC_RTC_ENABLE();
    hrtc.Instance = RTC;
    hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
    hrtc.Init.AsynchPrediv = 127;
    hrtc.Init.SynchPrediv = 255;
    hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
    hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
    hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
    if (HAL_RTC_Init(&hrtc) != HAL_OK) fatal();
    if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0) != RTC_MAGIC) rtc_set(2026, 1, 1, 0, 0, 0);
}

/* Datei /TIME.TXT ("2026-10-03 14:30:00", lokale Zeit) stellt die Uhr und wird in TIME.OLD umbenannt */
static void apply_time_file(void)
{
    FIL f; char b[40]; UINT n = 0; int y, mo, d, h, mi, s;
    if (f_open(&f, "/TIME.TXT", FA_READ) != FR_OK) return;
    f_read(&f, b, sizeof b - 1, &n); f_close(&f); b[n] = 0;
    if (sscanf(b, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6 && y >= 2020 && y < 2100 &&
        mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h < 24 && mi < 60 && s < 60)
        rtc_set(y, mo, d, h, mi, s);
    f_unlink("/TIME.OLD");
    f_rename("/TIME.TXT", "/TIME.OLD");
}

DWORD get_fattime(void)                          /* FatFs: Zeitstempel der Dateien */
{
    int y, mo, d, h, mi, s, ms;
    nc_civil_from_ms(rtc_now_ms(NULL), &y, &mo, &d, &h, &mi, &s, &ms);
    return ((DWORD)(y - 1980) << 25) | ((DWORD)mo << 21) | ((DWORD)d << 16) | ((DWORD)h << 11) | ((DWORD)mi << 5) | (DWORD)(s / 2);
}

/* ------------------------------------------------------------------ Dateizugriff fuer den Kern (FatFs) */
static int io_mkdir(void *u, const char *p) { (void)u; FRESULT r = f_mkdir(p); return (r == FR_OK || r == FR_EXIST) ? 0 : -1; }
static void *io_open(void *u, const char *p, char mode)
{
    (void)u;
    for (int i = 0; i < 2; i++) if (!file_used[i]) {
        BYTE fl = mode == 'r' ? FA_READ : (mode == 'a' ? (FA_WRITE | FA_OPEN_APPEND) : (FA_WRITE | FA_CREATE_ALWAYS));
        if (f_open(&files[i], p, fl) != FR_OK) return NULL;
        file_used[i] = 1; return &files[i];
    }
    return NULL;
}
static int io_write(void *u, void *h, const void *b, uint32_t n) { (void)u; UINT w; return (f_write((FIL *)h, b, n, &w) == FR_OK && w == n) ? 0 : -1; }
static int io_seek(void *u, void *h, uint32_t pos) { (void)u; return f_lseek((FIL *)h, pos) == FR_OK ? 0 : -1; }
static int io_close(void *u, void *h) { (void)u; file_used[(FIL *)h - files] = 0; return f_close((FIL *)h) == FR_OK ? 0 : -1; }

/* ------------------------------------------------------------------ Audio (DMA-Callbacks) */
static void push_half(const uint16_t *p)
{
    int32_t s[HALF_SAMPLES];
    for (uint32_t i = 0; i < HALF_SAMPLES; i++) {
        uint32_t k = (i * 2u + MIC_CHANNEL) * 2u;                       /* Halbwort-Index des Wortes */
        s[i] = (int32_t)(((uint32_t)p[k] << 16) | p[k + 1]) >> 8;       /* 24 bit, vorzeichenrichtig */
    }
    nc_push24(&nc, s, HALF_SAMPLES);
}
void HAL_I2S_RxHalfCpltCallback(I2S_HandleTypeDef *h) { (void)h; push_half(dma_buf); }
void HAL_I2S_RxCpltCallback(I2S_HandleTypeDef *h)     { (void)h; push_half(dma_buf + DMA_WORDS); }

/* ------------------------------------------------------------------ main */
int main(void)
{
    HAL_Init();
    clock_init();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitTypeDef g = { .Pin = GPIO_PIN_2, .Mode = GPIO_MODE_OUTPUT_PP, .Pull = GPIO_NOPULL, .Speed = GPIO_SPEED_FREQ_LOW };
    HAL_GPIO_Init(GPIOB, &g);
    led(0);

    hsd.Instance = SDIO;
    hsd.Init.ClockEdge = SDIO_CLOCK_EDGE_RISING;
    hsd.Init.ClockBypass = SDIO_CLOCK_BYPASS_DISABLE;
    hsd.Init.ClockPowerSave = SDIO_CLOCK_POWER_SAVE_DISABLE;
    hsd.Init.BusWide = SDIO_BUS_WIDE_1B;
    hsd.Init.HardwareFlowControl = SDIO_HARDWARE_FLOW_CONTROL_DISABLE;
    hsd.Init.ClockDiv = SD_CLKDIV;
    if (HAL_SD_Init(&hsd) != HAL_OK) fatal();
    if (HAL_SD_ConfigWideBusOperation(&hsd, SDIO_BUS_WIDE_4B) != HAL_OK) fatal();
    if (f_mount(&fatfs, "", 1) != FR_OK) fatal();

    rtc_init();
    apply_time_file();

    nc_config_t cfg; nc_default_config(&cfg);
    cfg.fs = SAMPLE_RATE_HZ; cfg.cal_offset_db = CAL_OFFSET_DB; cfg.trig_delta_db = TRIG_DELTA_DB;
    cfg.trig_min_dba = TRIG_MIN_DBA; cfg.pre_ms = PRE_MS; cfg.post_ms = POST_MS; cfg.slam_lafmax_dba = SLAM_LAFMAX_DBA;
    nc_io_t io = { NULL, rtc_now_ms, io_mkdir, io_open, io_write, io_seek, io_close };
    nc_init(&nc, &cfg, &io, ring, RING_SAMPLES);

    hi2s2.Instance = SPI2;
    hi2s2.Init.Mode = I2S_MODE_MASTER_RX;
    hi2s2.Init.Standard = I2S_STANDARD_PHILIPS;
    hi2s2.Init.DataFormat = I2S_DATAFORMAT_32B;
    hi2s2.Init.MCLKOutput = I2S_MCLKOUTPUT_DISABLE;
    hi2s2.Init.AudioFreq = SAMPLE_RATE_HZ;
    hi2s2.Init.CPOL = I2S_CPOL_LOW;
    hi2s2.Init.ClockSource = I2S_CLOCK_PLL;
    if (HAL_I2S_Init(&hi2s2) != HAL_OK) fatal();
    if (HAL_I2S_Receive_DMA(&hi2s2, dma_buf, DMA_WORDS) != HAL_OK) fatal();

    for (int i = 0; i < 3; i++) { led(1); HAL_Delay(80); led(0); HAL_Delay(120); }   /* bereit */
    for (;;) {
        nc_poll(&nc);
        led(nc.state == NC_ACTIVE);
        __WFI();                                                                /* DMA-Interrupt weckt alle 8 ms */
    }
}
