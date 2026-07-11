/****************************************************************************
 * boards/arm/rp23xx/adafruit-fruit-jam-rp2350/src/rp23xx_boardinitialize.c
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

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/debug.h>

#include <nuttx/board.h>
#include <nuttx/clock.h>
#include <nuttx/wqueue.h>
#include <arch/board/board.h>

#include "rp23xx_gpio.h"

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD
#  include "hardware/rp23xx_powman.h"
#  include "hardware/rp23xx_psm.h"
#  include "hardware/rp23xx_ticks.h"
#  include "hardware/rp23xx_watchdog.h"
#  include "rp23xx_rom.h"
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

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_DIAG
static void fruitjam_bootdiag_led(bool ledon)
{
  rp23xx_gpio_init(GPIO_LED1);
  rp23xx_gpio_setdir(GPIO_LED1, true);
  rp23xx_gpio_put(GPIO_LED1, !ledon);
}
#endif

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD
#define FRUITJAM_BOOT_GUARD_MAGIC 0x464a4247 /* "FJBG" */
#define FRUITJAM_BOOT_GUARD_WD_ENABLE RP23XX_WATCHDOG_CTRL_ENABLE
#define FRUITJAM_BOOT_GUARD_WD_MASK \
  (RP23XX_WATCHDOG_CTRL_ENABLE | RP23XX_WATCHDOG_CTRL_PAUSE_DBG0 | \
   RP23XX_WATCHDOG_CTRL_PAUSE_DBG1 | RP23XX_WATCHDOG_CTRL_PAUSE_JTAG)
#define FRUITJAM_BOOT_GUARD_TICK_CYCLES (BOARD_REF_FREQ / 1000000)
#define FRUITJAM_BOOT_GUARD_HW_SLICE_MS 15000
#define FRUITJAM_BOOT_GUARD_REFRESH_MS 12000
#define FRUITJAM_BOOT_GUARD_MAX_MS 600000

static struct work_s g_fruitjam_bootguard_work;
static uint64_t g_fruitjam_bootguard_deadline_ms;

static void fruitjam_bootguard_schedule(void);

static uint64_t fruitjam_bootguard_now_ms(void)
{
  return (uint64_t)TICK2MSEC(clock_systime_ticks());
}

static void fruitjam_bootguard_start_tick(void)
{
  putreg32(FRUITJAM_BOOT_GUARD_TICK_CYCLES,
           RP23XX_TICKS_WATCHDOG_CYCLES);
  putreg32(RP23XX_TICKS_WATCHDOG_CTRL_EN,
           RP23XX_TICKS_WATCHDOG_CTRL);
}

static void fruitjam_bootguard_clear(void)
{
  g_fruitjam_bootguard_deadline_ms = 0;
  putreg32(0, RP23XX_POWMAN_SCRATCH0);
  putreg32(0, RP23XX_POWMAN_SCRATCH1);
  putreg32(0, RP23XX_POWMAN_SCRATCH2);
  putreg32(0, RP23XX_WATCHDOG_SCRATCH(0));
  putreg32(0, RP23XX_WATCHDOG_SCRATCH(1));
  putreg32(0, RP23XX_WATCHDOG_SCRATCH(2));
  modreg32(0, RP23XX_WATCHDOG_CTRL_ENABLE, RP23XX_WATCHDOG_CTRL);
}

static bool fruitjam_bootguard_armed(void)
{
  return getreg32(RP23XX_POWMAN_SCRATCH0) == FRUITJAM_BOOT_GUARD_MAGIC ||
         getreg32(RP23XX_WATCHDOG_SCRATCH(0)) == FRUITJAM_BOOT_GUARD_MAGIC;
}

static uint32_t fruitjam_bootguard_diag(void)
{
  uint32_t diag = getreg32(RP23XX_POWMAN_SCRATCH2);

  if (diag == 0)
    {
      diag = getreg32(RP23XX_WATCHDOG_SCRATCH(2));
    }

  return diag;
}

static void fruitjam_bootguard_enter_bootsel(void)
{
  uint32_t diag;
  rom_reboot_fn reboot;

  diag = fruitjam_bootguard_diag();
  fruitjam_bootguard_clear();

  reboot = (rom_reboot_fn)rom_func_lookup(ROM_FUNC_REBOOT);
  if (reboot != NULL)
    {
      reboot(REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL |
             REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 10, 0, diag);
    }

  board_reset(3);
}

static void fruitjam_bootguard_program_hw(uint32_t timeout_ms)
{
  uint32_t timeout_us;

  if (timeout_ms == 0)
    {
      timeout_ms = 1;
    }

  if (timeout_ms > FRUITJAM_BOOT_GUARD_HW_SLICE_MS)
    {
      timeout_ms = FRUITJAM_BOOT_GUARD_HW_SLICE_MS;
    }

  timeout_us = timeout_ms * USEC_PER_MSEC;
  if (timeout_us > RP23XX_WATCHDOG_LOAD_MASK)
    {
      timeout_us = RP23XX_WATCHDOG_LOAD_MASK;
    }

  fruitjam_bootguard_start_tick();
  putreg32(timeout_us, RP23XX_WATCHDOG_LOAD);
  putreg32(RP23XX_PSM_WDSEL_BITS & ~(RP23XX_PSM_XOSC | RP23XX_PSM_ROSC),
           RP23XX_PSM_WDSEL);
  modreg32(FRUITJAM_BOOT_GUARD_WD_ENABLE,
           FRUITJAM_BOOT_GUARD_WD_MASK,
           RP23XX_WATCHDOG_CTRL);
}

static void fruitjam_bootguard_check_previous_reset(void)
{
  if (fruitjam_bootguard_armed() &&
      ((getreg32(RP23XX_WATCHDOG_REASON) &
        (RP23XX_WATCHDOG_REASON_TIMER | RP23XX_WATCHDOG_REASON_FORCE)) != 0 ||
       (getreg32(RP23XX_POWMAN_CHIP_RESET) &
        (RP23XX_POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_RSM |
         RP23XX_POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_SWCORE |
         RP23XX_POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN |
         RP23XX_POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_ASYNC)) != 0))
    {
      fruitjam_bootguard_enter_bootsel();
    }
}

static void fruitjam_bootguard_arm_timeout(uint32_t timeout_ms)
{
  if (timeout_ms == 0)
    {
      timeout_ms = CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD_TIMEOUT_MS;
    }

  if (timeout_ms > FRUITJAM_BOOT_GUARD_MAX_MS)
    {
      timeout_ms = FRUITJAM_BOOT_GUARD_MAX_MS;
    }

  g_fruitjam_bootguard_deadline_ms =
    fruitjam_bootguard_now_ms() + timeout_ms;

  putreg32(FRUITJAM_BOOT_GUARD_MAGIC, RP23XX_POWMAN_SCRATCH0);
  putreg32(timeout_ms, RP23XX_POWMAN_SCRATCH1);
  putreg32(0, RP23XX_POWMAN_SCRATCH2);
  putreg32(FRUITJAM_BOOT_GUARD_MAGIC, RP23XX_WATCHDOG_SCRATCH(0));
  putreg32(timeout_ms, RP23XX_WATCHDOG_SCRATCH(1));
  putreg32(0, RP23XX_WATCHDOG_SCRATCH(2));
  fruitjam_bootguard_program_hw(timeout_ms);
}

static void fruitjam_bootguard_arm(void)
{
  fruitjam_bootguard_arm_timeout(
    CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD_TIMEOUT_MS);
}

static void fruitjam_bootguard_worker(FAR void *arg)
{
  uint64_t now;
  uint64_t remaining;

  UNUSED(arg);

  if (fruitjam_bootguard_armed() && fruitjam_bootguard_diag() == 0)
    {
      now = fruitjam_bootguard_now_ms();
      if (g_fruitjam_bootguard_deadline_ms == 0 ||
          now >= g_fruitjam_bootguard_deadline_ms)
        {
          fruitjam_bootguard_enter_bootsel();
          return;
        }

      remaining = g_fruitjam_bootguard_deadline_ms - now;
      fruitjam_bootguard_program_hw(
        remaining > FRUITJAM_BOOT_GUARD_HW_SLICE_MS ?
        FRUITJAM_BOOT_GUARD_HW_SLICE_MS : (uint32_t)remaining);
      fruitjam_bootguard_schedule();
    }
}

static void fruitjam_bootguard_schedule(void)
{
  uint64_t now;
  uint64_t remaining;
  clock_t delay;

  now = fruitjam_bootguard_now_ms();
  remaining = g_fruitjam_bootguard_deadline_ms > now ?
              g_fruitjam_bootguard_deadline_ms - now : 1;
  if (remaining > FRUITJAM_BOOT_GUARD_REFRESH_MS)
    {
      remaining = FRUITJAM_BOOT_GUARD_REFRESH_MS;
    }

  delay = MSEC2TICK((uint32_t)remaining);

  if (work_available(&g_fruitjam_bootguard_work))
    {
      work_queue(HPWORK, &g_fruitjam_bootguard_work,
                 fruitjam_bootguard_worker, NULL, delay);
    }
}

void board_fruitjam_bootguard_arm(void)
{
  fruitjam_bootguard_arm();
  fruitjam_bootguard_schedule();
}

void board_fruitjam_bootguard_arm_timeout(uint32_t timeout_ms)
{
  fruitjam_bootguard_arm_timeout(timeout_ms);
  fruitjam_bootguard_schedule();
}

void board_fruitjam_bootguard_disarm(void)
{
  work_cancel(HPWORK, &g_fruitjam_bootguard_work);
  fruitjam_bootguard_clear();
}
#endif

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_NINA
static void fruitjam_nina_initialize(void)
{
  rp23xx_gpio_init(BOARD_NINA_RESET_PIN);
  rp23xx_gpio_setdir(BOARD_NINA_RESET_PIN, true);
  rp23xx_gpio_put(BOARD_NINA_RESET_PIN, true);

  rp23xx_gpio_init(BOARD_NINA_READY_PIN);
  rp23xx_gpio_setdir(BOARD_NINA_READY_PIN, false);

  rp23xx_gpio_init(BOARD_NINA_IRQ_PIN);
  rp23xx_gpio_setdir(BOARD_NINA_IRQ_PIN, false);

  rp23xx_gpio_init(BOARD_NINA_CS_PIN);
  rp23xx_gpio_setdir(BOARD_NINA_CS_PIN, true);
  rp23xx_gpio_put(BOARD_NINA_CS_PIN, true);
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
  #ifdef CONFIG_ARCH_BOARD_COMMON
  rp23xx_common_earlyinitialize();
  #endif

  /* --- Place any board specific early initialization here --- */

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD
  fruitjam_bootguard_check_previous_reset();
  fruitjam_bootguard_arm();
#endif

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_DIAG
  fruitjam_bootdiag_led(true);
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

  #ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD
  fruitjam_bootguard_arm();
  #endif

  #ifdef CONFIG_RP23XX_PSRAM
  rp23xx_psramconfig();
  #endif

  /* --- Place any board specific initialization here --- */

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_NINA
  fruitjam_nina_initialize();
#endif
}

/****************************************************************************
 * Name: board_late_initialize
 *
 * Description:
 *   If CONFIG_BOARD_LATE_INITIALIZE is selected, then an additional
 *   initialization call will be performed in the boot-up sequence to a
 *   function called board_late_initialize(). board_late_initialize() will be
 *   called immediately after up_initialize() is called and just before the
 *   initial application is started.  This additional initialization phase
 *   may be used, for example, to initialize board-specific device drivers.
 *
 ****************************************************************************/

#ifdef CONFIG_BOARD_LATE_INITIALIZE
void board_late_initialize(void)
{
  #ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD
  fruitjam_bootguard_schedule();
  #endif

  rp23xx_bringup(0);

  #ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD
  fruitjam_bootguard_arm();
  #endif
}
#endif

#if defined(CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD) && \
    defined(CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD_CDC_DISARM) && \
    defined(CONFIG_CDCACM)
void board_cdcacm_connected(int minor, bool connected)
{
  if (connected && minor == 0 && fruitjam_bootguard_armed() &&
      fruitjam_bootguard_diag() == 0)
    {
      fruitjam_bootguard_clear();
    }
}
#endif
