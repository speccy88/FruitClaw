/****************************************************************************
 * boards/arm/rp23xx/raspberrypi-pico-2-w/src/rp23xx_boardinitialize.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <arch/board/board.h>

#include "rp23xx_gpio.h"

#ifdef CONFIG_RP23XX_PICO_BOOTSEL_ON_WATCHDOG
#  include "hardware/rp23xx_watchdog.h"
#endif

#ifdef CONFIG_RP23XX_PSRAM
#include "rp23xx_psram.h"
#endif

#ifdef CONFIG_ARCH_BOARD_COMMON
#include "rp23xx_common_initialize.h"
#endif /* CONFIG_ARCH_BOARD_COMMON */

#include "rp23xx_pico.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_RP23XX_PICO_BOOTSEL_ON_WATCHDOG
#  define RP2350_BOOTSEL_GUARD_MAGIC 0x464a4247 /* "FJBG" */

static void pico2w_watchdog_bootsel_check(void)
{
  uint32_t reason = getreg32(RP23XX_WATCHDOG_REASON);

  if (getreg32(RP23XX_WATCHDOG_SCRATCH(0)) ==
        RP2350_BOOTSEL_GUARD_MAGIC &&
      (reason & (RP23XX_WATCHDOG_REASON_TIMER |
                 RP23XX_WATCHDOG_REASON_FORCE)) != 0)
    {
      putreg32(0, RP23XX_WATCHDOG_SCRATCH(0));
      putreg32(0, RP23XX_WATCHDOG_SCRATCH(1));
      putreg32(0, RP23XX_WATCHDOG_SCRATCH(2));
      board_reset(3);
    }
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rp23xx_boardearlyinitialize
 *
 * Description:
 *
 ****************************************************************************/

void rp23xx_boardearlyinitialize(void)
{
#ifdef CONFIG_RP23XX_PICO_BOOTSEL_ON_WATCHDOG
  pico2w_watchdog_bootsel_check();
#endif

#ifdef CONFIG_ARCH_BOARD_COMMON
  rp23xx_common_earlyinitialize();
#endif

  /* --- Place any board specific early initialization here --- */

  /* Pico 2 W's user LED is WL_GPIO0 behind the CYW43439, not RP2350 GPIO25. */

#ifdef BOARD_GPIO_LED_PIN
  rp23xx_gpio_init(BOARD_GPIO_LED_PIN);
  rp23xx_gpio_setdir(BOARD_GPIO_LED_PIN, true);
  rp23xx_gpio_put(BOARD_GPIO_LED_PIN, true);
#endif
}

/****************************************************************************
 * Name: rp23xx_boardinitialize
 *
 * Description:
 *
 ****************************************************************************/

void rp23xx_boardinitialize(void)
{
#ifdef CONFIG_ARCH_BOARD_COMMON
  rp23xx_common_initialize();
#endif

#ifdef CONFIG_RP23XX_PSRAM
  rp23xx_psramconfig();
#endif

  /* --- Place any board specific initialization here --- */

#ifdef CONFIG_RP23XX_AUTO_BOOTSEL_ON_BOOT
  syslog(LOG_INFO, "rebooting to BOOTSEL from board initialization\n");
  board_reset(3);
#endif
}

/****************************************************************************
 * Name: board_late_initialize
 ****************************************************************************/

#ifdef CONFIG_BOARD_LATE_INITIALIZE
void board_late_initialize(void)
{
  rp23xx_bringup(0);
}
#endif
