/* Watchdog und Absturzdiagnose (Backup-Register bleiben beim Watchdog-Reset erhalten) */
#ifndef DBG_H
#define DBG_H
#include "stm32f4xx.h"

#define WDG_KICK()   (IWDG->KR = 0xAAAAu)

/* Backup-Register: 0 = RTC-Magic (main.c), 1 = letzter SD-Zugriff, 2 = HAL-Fehlercode, 3 = Sektor, 4 = HardFault-PC */
#define DBG_SD_OP    (RTC->BKP1R)     /* (op << 24) | Anzahl Sektoren; op: 1 Lesen, 2 Schreiben, 3 Statusabfrage; 0 = nichts aktiv */
#define DBG_SD_ERR   (RTC->BKP2R)
#define DBG_SD_SECT  (RTC->BKP3R)
#define DBG_FAULT_PC (RTC->BKP4R)

#endif
