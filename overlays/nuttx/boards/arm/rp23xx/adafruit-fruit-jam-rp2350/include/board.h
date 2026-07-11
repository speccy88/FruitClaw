/****************************************************************************
 * boards/arm/rp23xx/adafruit-fruit-jam-rp2350/include/board.h
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __BOARDS_ARM_RP23XX_ADAFRUIT_FRUIT_JAM_RP2350_INCLUDE_BOARD_H
#define __BOARDS_ARM_RP23XX_ADAFRUIT_FRUIT_JAM_RP2350_INCLUDE_BOARD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "rp23xx_i2cdev.h"
#include "rp23xx_spidev.h"
#include "rp23xx_i2sdev.h"
#include "rp23xx_spisd.h"

#ifndef __ASSEMBLY__
#  include <stdint.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Clocking *****************************************************************/

#define MHZ                     1000000

#define BOARD_XOSC_FREQ         (12 * MHZ)
#define BOARD_XOSC_STARTUPDELAY 11

/* Match the known-good wili8jam clock plan: raise VREG to 1.30 V, run
 * clk_sys at 252 MHz, then divide CLK_HSTX to the exact 126 MHz DVI clock.
 * CLK_HSTX only has an integer divider, so the default 150 MHz SYS PLL
 * cannot produce the required 126 MHz HSTX clock.
 */

#define BOARD_VREG_VSEL         0x0f /* 1.30 V */
#define BOARD_PLL_SYS_REFDIV    1
#define BOARD_PLL_SYS_VCO_FREQ  (1260 * MHZ)
#define BOARD_PLL_SYS_POSTDIV1  5
#define BOARD_PLL_SYS_POSTDIV2  1
#define BOARD_PLL_SYS_FREQ      (252 * MHZ)
#define BOARD_PLL_USB_FREQ      (48 * MHZ)

#define BOARD_REF_FREQ          (12 * MHZ)
#define BOARD_SYS_FREQ          (252 * MHZ)
#define BOARD_PERI_FREQ         BOARD_SYS_FREQ
#define BOARD_USB_FREQ          (48 * MHZ)
#define BOARD_ADC_FREQ          (48 * MHZ)
#ifdef CONFIG_RP23XX_HSTX_DVI
#  define BOARD_HSTX_FREQ       (126 * MHZ)
#else
#  define BOARD_HSTX_FREQ       BOARD_SYS_FREQ
#endif

#define BOARD_UART_BASEFREQ     BOARD_PERI_FREQ

#define BOARD_TICK_CLOCK        (1 * MHZ)

/* Fruit Jam fixed hardware pins *********************************************/

#define BOARD_UART1_TX_PIN       8
#define BOARD_UART1_RX_PIN       9

#define BOARD_BUTTON1_PIN        0
#define BOARD_BUTTON2_PIN        4
#define BOARD_BUTTON3_PIN        5

#define BOARD_I2C0_SDA_PIN       20
#define BOARD_I2C0_SCL_PIN       21

#define BOARD_AUDIO_DIN_PIN      24
#define BOARD_AUDIO_MCLK_PIN     25
#define BOARD_AUDIO_BCLK_PIN     26
#define BOARD_AUDIO_WS_PIN       27
#define BOARD_AUDIO_IRQ_PIN      23

#define BOARD_NEOPIXEL_PIN       32
#define BOARD_NNEOPIXELS         5

#define BOARD_USBHOST_DP_PIN     1
#define BOARD_USBHOST_DM_PIN     2
#define BOARD_USBHOST_POWER_PIN  11

#define BOARD_DVI_D0_PIN         12
#define BOARD_DVI_D1_PIN         13
#define BOARD_DVI_D2_PIN         14
#define BOARD_DVI_D3_PIN         15
#define BOARD_DVI_D4_PIN         16
#define BOARD_DVI_D5_PIN         17
#define BOARD_DVI_D6_PIN         18
#define BOARD_DVI_D7_PIN         19

#define BOARD_SD_SPI_BUS         0
#define BOARD_SD_MISO_PIN        36
#define BOARD_SD_CS_PIN          39
#define BOARD_SD_SCK_PIN         34
#define BOARD_SD_MOSI_PIN        35
#define BOARD_SD_DETECT_PIN      33

#define BOARD_NINA_SPI_BUS       1
#define BOARD_NINA_READY_PIN     3
#define BOARD_NINA_RESET_PIN     22
#define BOARD_NINA_IRQ_PIN       23
#define BOARD_NINA_MISO_PIN      28
#define BOARD_NINA_SCK_PIN       30
#define BOARD_NINA_MOSI_PIN      31
#define BOARD_NINA_CS_PIN        46

#define BOARD_ADC_BASE_PIN       40
#define BOARD_PSRAM_CS_PIN       47

/* GPIO definitions *********************************************************/

#define BOARD_GPIO_LED_PIN      29
#define BOARD_LED_ACTIVE_LOW    1
#define BOARD_NGPIOOUT          1
#define BOARD_NGPIOIN           3
#define BOARD_NGPIOINT          3

/* LED definitions **********************************************************/

/* If CONFIG_ARCH_LEDS is not defined, then the user can control the LEDs
 * in any way. The following definitions are used to access individual LEDs.
 */

/* LED index values for use with board_userled() */

#define BOARD_LED1        0
#define BOARD_NLEDS       1

#define BOARD_LED_RED     BOARD_LED1

/* LED bits for use with board_userled_all() */

#define BOARD_LED1_BIT    (1 << BOARD_LED1)

/* This LED is not used by the board port unless CONFIG_ARCH_LEDS is
 * defined.  In that case, the usage by the board port is defined in
 * include/board.h and src/rp23xx_autoleds.c. The LED is used to encode
 * OS-related events as follows:
 *
 *   -------------------- ----------------------------- ------
 *   SYMBOL                   Meaning                   LED
 *   -------------------- ----------------------------- ------
 */

#define LED_STARTED       0  /* NuttX has been started  OFF    */
#define LED_HEAPALLOCATE  0  /* Heap has been allocated OFF    */
#define LED_IRQSENABLED   0  /* Interrupts enabled      OFF    */
#define LED_STACKCREATED  1  /* Idle stack created      ON     */
#define LED_INIRQ         2  /* In an interrupt         N/C    */
#define LED_SIGNAL        2  /* In a signal handler     N/C    */
#define LED_ASSERTION     2  /* An assertion failed     N/C    */
#define LED_PANIC         3  /* The system has crashed  FLASH  */
#undef  LED_IDLE             /* Not used                       */

/* Thus if the LED is statically on, NuttX has successfully  booted and is,
 * apparently, running normally.  If the LED is flashing at approximately
 * 2Hz, then a fatal error has been detected and the system has halted.
 */

/* BUTTON definitions *******************************************************/

#define NUM_BUTTONS       3

#define BUTTON_USER1      0
#define BUTTON_USER2      1
#define BUTTON_USER3      2
#define BUTTON_USER1_BIT  (1 << BUTTON_USER1)
#define BUTTON_USER2_BIT  (1 << BUTTON_USER2)
#define BUTTON_USER3_BIT  (1 << BUTTON_USER3)

/****************************************************************************
 * Public Types
 ****************************************************************************/

#ifndef __ASSEMBLY__

/****************************************************************************
 * Public Data
 ****************************************************************************/

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD
void board_fruitjam_bootguard_arm(void);
void board_fruitjam_bootguard_arm_timeout(uint32_t timeout_ms);
void board_fruitjam_bootguard_disarm(void);
#endif

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED
int board_fruitjam_esp_hosted_start(void);
int board_fruitjam_esp_hosted_recover(void);
#endif

#ifdef CONFIG_ARCH_BOARD_ADAFRUIT_FRUIT_JAM_RP2350
#  ifdef CONFIG_RP23XX_I2S
int board_fruitjam_audio_codec_recover(void);
#  endif
int board_fruitjam_shared_peripherals_recover(void);
#endif

/****************************************************************************
 * Name: rp23xx_boardearlyinitialize
 *
 * Description:
 *
 ****************************************************************************/

void rp23xx_boardearlyinitialize(void);

/****************************************************************************
 * Name: rp23xx_boardinitialize
 *
 * Description:
 *
 ****************************************************************************/

void rp23xx_boardinitialize(void);

#undef EXTERN
#if defined(__cplusplus)
}
#endif
#endif /* __ASSEMBLY__ */
#endif /* __BOARDS_ARM_RP23XX_ADAFRUIT_FRUIT_JAM_RP2350_INCLUDE_BOARD_H */
