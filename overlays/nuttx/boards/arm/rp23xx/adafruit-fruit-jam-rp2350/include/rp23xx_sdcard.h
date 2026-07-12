/****************************************************************************
 * boards/arm/rp23xx/adafruit-fruit-jam-rp2350/include/rp23xx_sdcard.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __BOARDS_ARM_RP23XX_ADAFRUIT_FRUIT_JAM_RP2350_INCLUDE_RP23XX_SDCARD_H
#define __BOARDS_ARM_RP23XX_ADAFRUIT_FRUIT_JAM_RP2350_INCLUDE_RP23XX_SDCARD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__
#ifdef __cplusplus
extern "C"
{
#endif

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_SD_PIO
int board_piosd_initialize(void);
#endif

#if defined(CONFIG_ADAFRUIT_FRUIT_JAM_SD_SPI) || \
    defined(CONFIG_ADAFRUIT_FRUIT_JAM_SD_PIO)
int board_sdcard_mount(void);
#endif

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_SD_SPI
int board_sdcard_carddetect_initialize(void);
#endif

#ifdef __cplusplus
}
#endif
#endif /* __ASSEMBLY__ */
#endif /* __BOARDS_ARM_RP23XX_ADAFRUIT_FRUIT_JAM_RP2350_INCLUDE_RP23XX_SDCARD_H */
