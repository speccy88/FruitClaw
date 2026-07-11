/****************************************************************************
 * arch/arm/src/rp23xx/rp23xx_psram.c
 *
 * SPDX-License-Identifier: Apache-2.0
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
#include <stddef.h>

#include <nuttx/compiler.h>
#include <nuttx/irq.h>
#include <arch/board/board.h>

#include "arm_internal.h"
#include "hardware/rp23xx_io_bank0.h"
#include "hardware/rp23xx_pads_bank0.h"
#include "hardware/rp23xx_powman.h"
#include "hardware/rp23xx_qmi.h"
#include "hardware/rp23xx_watchdog.h"
#include "hardware/rp23xx_xip.h"
#include "rp23xx_psram.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RP23XX_GPIO_FUNC_XIP_CS1        0x09
#define RP23XX_GPIO_PAD_NORMAL          0x52

#define RP23XX_PSRAM_CMD_READ_ID        0x9f
#define RP23XX_PSRAM_CMD_ENTER_QPI      0x35
#define RP23XX_PSRAM_CMD_EXIT_QPI       0xf5
#define RP23XX_PSRAM_KGD                0x5d

#define RP23XX_PSRAM_QPI_CLKDIV         10
#define RP23XX_PSRAM_DETECT_CLKDIV      30
#define RP23XX_PSRAM_WAIT_TIMEOUT       CONFIG_RP23XX_PSRAM_WAIT_TIMEOUT
#define RP23XX_PSRAM_FS_PER_SEC         1000000000000000ull
#define RP23XX_PSRAM_MIN_DESELECT_NS    18
#define RP23XX_PSRAM_TIMING_MAX_SELECT_UNIT_NS 125
#define RP23XX_PSRAM_DIAG_WORD(stage, value) \
  ((((uint32_t)(stage) & 0xff) << 24) | ((uint32_t)(value) & 0x00ffffff))

#define RP23XX_PSRAM_RAMFUNC \
  noinline_function __attribute__((section(".time_critical.rp23xx_psram")))

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_psram_ready;
static size_t g_psram_size;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t RP23XX_PSRAM_RAMFUNC rp23xx_psram_getreg(uint32_t addr)
{
  return *(volatile uint32_t *)addr;
}

static void RP23XX_PSRAM_RAMFUNC rp23xx_psram_putreg(uint32_t value,
                                                     uint32_t addr)
{
  *(volatile uint32_t *)addr = value;
}

static void RP23XX_PSRAM_RAMFUNC rp23xx_psram_barrier(void)
{
  __asm__ __volatile__ ("dsb sy\nisb sy" ::: "memory");
}

static void RP23XX_PSRAM_RAMFUNC rp23xx_psram_diag(uint32_t stage,
                                                   uint32_t value)
{
  rp23xx_psram_putreg(RP23XX_PSRAM_DIAG_WORD(stage, value),
                      RP23XX_WATCHDOG_SCRATCH(2));
  rp23xx_psram_putreg(value, RP23XX_WATCHDOG_SCRATCH(3));
  rp23xx_psram_putreg(RP23XX_PSRAM_DIAG_WORD(stage, value),
                      RP23XX_POWMAN_SCRATCH2);
  rp23xx_psram_putreg(value, RP23XX_POWMAN_SCRATCH3);
}

static bool RP23XX_PSRAM_RAMFUNC rp23xx_qmi_wait_busy(void)
{
  uint32_t timeout = RP23XX_PSRAM_WAIT_TIMEOUT;

  while ((rp23xx_psram_getreg(RP23XX_QMI_DIRECT_CSR) &
          RP23XX_QMI_DIRECT_CSR_BUSY) != 0)
    {
      if (timeout-- == 0)
        {
          return false;
        }
    }

  return true;
}

static bool RP23XX_PSRAM_RAMFUNC rp23xx_qmi_wait_txempty(void)
{
  uint32_t timeout = RP23XX_PSRAM_WAIT_TIMEOUT;

  while ((rp23xx_psram_getreg(RP23XX_QMI_DIRECT_CSR) &
          RP23XX_QMI_DIRECT_CSR_TXEMPTY) == 0)
    {
      if (timeout-- == 0)
        {
          return false;
        }
    }

  return true;
}

static bool RP23XX_PSRAM_RAMFUNC rp23xx_qmi_xfer8(uint32_t tx,
                                                  uint8_t *rx)
{
  rp23xx_psram_putreg(tx, RP23XX_QMI_DIRECT_TX);

  if (!rp23xx_qmi_wait_txempty())
    {
      return false;
    }

  if (!rp23xx_qmi_wait_busy())
    {
      return false;
    }

  *rx = rp23xx_psram_getreg(RP23XX_QMI_DIRECT_RX) & 0xff;
  return true;
}

static bool RP23XX_PSRAM_RAMFUNC rp23xx_qmi_enter_qpi(void)
{
  rp23xx_psram_putreg((RP23XX_PSRAM_QPI_CLKDIV <<
                       RP23XX_QMI_DIRECT_CSR_CLKDIV_SHIFT) |
                      RP23XX_QMI_DIRECT_CSR_EN |
                      RP23XX_QMI_DIRECT_CSR_AUTO_CS1N,
                      RP23XX_QMI_DIRECT_CSR);
  if (!rp23xx_qmi_wait_busy())
    {
      return false;
    }

  rp23xx_psram_putreg(RP23XX_QMI_DIRECT_TX_NOPUSH |
                      RP23XX_PSRAM_CMD_ENTER_QPI,
                      RP23XX_QMI_DIRECT_TX);

  if (!rp23xx_qmi_wait_txempty())
    {
      return false;
    }

  return rp23xx_qmi_wait_busy();
}

static bool RP23XX_PSRAM_RAMFUNC rp23xx_psram_probe(size_t psram_size)
{
  volatile uint32_t *psram =
    (volatile uint32_t *)(uintptr_t)RP23XX_PSRAM_NOCACHE_BASE;
  size_t last_word;

  if (psram_size < sizeof(uint32_t))
    {
      return false;
    }

  rp23xx_psram_diag(0x40, psram_size);
  psram[0] = 0x12345678;
  rp23xx_psram_diag(0x41, psram[0]);
  if (psram[0] != 0x12345678)
    {
      return false;
    }

  rp23xx_psram_diag(0x42, psram_size);
  last_word = (psram_size / sizeof(uint32_t)) - 1;
  psram[last_word] = 0xa5a55a5a;
  rp23xx_psram_diag(0x43, psram[last_word]);
  if (psram[last_word] != 0xa5a55a5a)
    {
      return false;
    }

  return true;
}

static size_t RP23XX_PSRAM_RAMFUNC rp23xx_psram_detect(void)
{
  uint32_t regval;
  uint8_t kgd = 0;
  uint8_t eid = 0;
  uint8_t size_id;
  uint8_t rx;
  int i;

  rp23xx_psram_diag(0x20, 0);

  regval = (RP23XX_PSRAM_DETECT_CLKDIV <<
            RP23XX_QMI_DIRECT_CSR_CLKDIV_SHIFT) |
            RP23XX_QMI_DIRECT_CSR_EN;
  rp23xx_psram_putreg(regval, RP23XX_QMI_DIRECT_CSR);
  if (!rp23xx_qmi_wait_busy())
    {
      goto fail;
    }

  regval |= RP23XX_QMI_DIRECT_CSR_ASSERT_CS1N;
  rp23xx_psram_putreg(regval, RP23XX_QMI_DIRECT_CSR);

  if (!rp23xx_qmi_xfer8(RP23XX_QMI_DIRECT_TX_OE |
                        RP23XX_QMI_DIRECT_TX_IWIDTH_QUAD |
                        RP23XX_PSRAM_CMD_EXIT_QPI, &rx))
    {
      goto fail;
    }

  regval &= ~RP23XX_QMI_DIRECT_CSR_ASSERT_CS1N;
  rp23xx_psram_putreg(regval, RP23XX_QMI_DIRECT_CSR);

  regval |= RP23XX_QMI_DIRECT_CSR_ASSERT_CS1N;
  rp23xx_psram_putreg(regval, RP23XX_QMI_DIRECT_CSR);

  for (i = 0; i < 12; i++)
    {
      if (!rp23xx_qmi_xfer8(i == 0 ? RP23XX_PSRAM_CMD_READ_ID : 0xff,
                            &rx))
        {
          goto fail;
        }

      if (i == 5)
        {
          kgd = rx;
        }
      else if (i == 6)
        {
          eid = rx;
        }
    }

  regval &= ~(RP23XX_QMI_DIRECT_CSR_ASSERT_CS1N |
              RP23XX_QMI_DIRECT_CSR_EN);
  rp23xx_psram_putreg(regval, RP23XX_QMI_DIRECT_CSR);
  rp23xx_psram_barrier();
  rp23xx_psram_diag(0x21, ((uint32_t)kgd << 8) | eid);

  if (kgd != RP23XX_PSRAM_KGD)
    {
      return 0;
    }

  size_id = eid >> 5;

  if (size_id == 4)
    {
      return 16 * 1024 * 1024;
    }

  if (eid == 0x26)
    {
      return 8 * 1024 * 1024;
    }

  if (size_id == 3)
    {
      return 8 * 1024 * 1024;
    }

  if (size_id == 2)
    {
      return 8 * 1024 * 1024;
    }

  if (size_id < 2)
    {
      return (2 * 1024 * 1024) << size_id;
    }

  return 1024 * 1024;

fail:
  rp23xx_psram_putreg(0, RP23XX_QMI_DIRECT_CSR);
  rp23xx_psram_barrier();
  return 0;
}

static size_t RP23XX_PSRAM_RAMFUNC rp23xx_psram_init(void)
{
  uint32_t clock_hz = BOARD_SYS_FREQ;
  uint32_t max_freq = CONFIG_RP23XX_PSRAM_MAX_SCK_HZ;
  uint32_t divisor;
  uint32_t rxdelay;
  uint32_t max_select;
  uint32_t min_deselect;
  uint64_t clock_period_fs;
  uint32_t qmi_width;
  uint32_t fmt;
  size_t psram_size;

  psram_size = rp23xx_psram_detect();
  rp23xx_psram_diag(0x30, psram_size);
  if (psram_size == 0)
    {
      return 0;
    }

  if (!rp23xx_qmi_enter_qpi())
    {
      goto fail;
    }

  rp23xx_psram_diag(0x31, psram_size);
  rp23xx_psram_putreg(0, RP23XX_QMI_DIRECT_CSR);

  divisor = (clock_hz + max_freq - 1) / max_freq;
  if (divisor == 1 && clock_hz > 100000000)
    {
      divisor = 2;
    }
  else if (divisor == 0)
    {
      divisor = 1;
    }
  else if (divisor > 255)
    {
      divisor = 255;
    }

  rxdelay = divisor;
  if (clock_hz / divisor > 100000000)
    {
      rxdelay += 1;
    }

  if (rxdelay > 7)
    {
      rxdelay = 7;
    }

  clock_period_fs = RP23XX_PSRAM_FS_PER_SEC / clock_hz;
  max_select = (RP23XX_PSRAM_TIMING_MAX_SELECT_UNIT_NS * 1000000ull) /
               clock_period_fs;
  min_deselect = (RP23XX_PSRAM_MIN_DESELECT_NS * 1000000ull +
                  clock_period_fs - 1) / clock_period_fs;

  if (min_deselect > (divisor + 1) / 2)
    {
      min_deselect -= (divisor + 1) / 2;
    }
  else
    {
      min_deselect = 0;
    }

  if (max_select > 0x3f)
    {
      max_select = 0x3f;
    }

  if (min_deselect > 0x1f)
    {
      min_deselect = 0x1f;
    }

  rp23xx_psram_putreg((1 << RP23XX_QMI_TIMING_COOLDOWN_SHIFT) |
                      RP23XX_QMI_TIMING_PAGEBREAK_1024 |
                      ((max_select << RP23XX_QMI_TIMING_MAX_SELECT_SHIFT) &
                       RP23XX_QMI_TIMING_MAX_SELECT_MASK) |
                      ((min_deselect <<
                        RP23XX_QMI_TIMING_MIN_DESELECT_SHIFT) &
                       RP23XX_QMI_TIMING_MIN_DESELECT_MASK) |
                      ((rxdelay << RP23XX_QMI_TIMING_RXDELAY_SHIFT) &
                       RP23XX_QMI_TIMING_RXDELAY_MASK) |
                      (divisor & RP23XX_QMI_TIMING_CLKDIV_MASK),
                      RP23XX_QMI_M1_TIMING);
  rp23xx_psram_diag(0x32, (divisor << 16) | (rxdelay << 8) |
                    min_deselect);

  qmi_width = RP23XX_QMI_FMT_WIDTH_QUAD;
  fmt = (qmi_width << RP23XX_QMI_FMT_PREFIX_WIDTH_SHIFT) |
        (qmi_width << RP23XX_QMI_FMT_ADDR_WIDTH_SHIFT) |
        (qmi_width << RP23XX_QMI_FMT_SUFFIX_WIDTH_SHIFT) |
        (qmi_width << RP23XX_QMI_FMT_DUMMY_WIDTH_SHIFT) |
        (qmi_width << RP23XX_QMI_FMT_DATA_WIDTH_SHIFT) |
        RP23XX_QMI_FMT_PREFIX_LEN |
        (6 << RP23XX_QMI_FMT_DUMMY_LEN_SHIFT);

  rp23xx_psram_putreg(fmt, RP23XX_QMI_M1_RFMT);
  rp23xx_psram_putreg(0xeb, RP23XX_QMI_M1_RCMD);

  fmt = (qmi_width << RP23XX_QMI_FMT_PREFIX_WIDTH_SHIFT) |
        (qmi_width << RP23XX_QMI_FMT_ADDR_WIDTH_SHIFT) |
        (qmi_width << RP23XX_QMI_FMT_SUFFIX_WIDTH_SHIFT) |
        (qmi_width << RP23XX_QMI_FMT_DUMMY_WIDTH_SHIFT) |
        (qmi_width << RP23XX_QMI_FMT_DATA_WIDTH_SHIFT) |
        RP23XX_QMI_FMT_PREFIX_LEN;

  rp23xx_psram_putreg(fmt, RP23XX_QMI_M1_WFMT);
  rp23xx_psram_putreg(0x38, RP23XX_QMI_M1_WCMD);

  rp23xx_psram_putreg(0, RP23XX_QMI_DIRECT_CSR);
  rp23xx_psram_putreg(rp23xx_psram_getreg(RP23XX_XIP_CTRL) |
                      RP23XX_XIP_CTRL_WRITABLE_M1,
                      RP23XX_XIP_CTRL);
  rp23xx_psram_barrier();
  rp23xx_psram_diag(0x33, psram_size);

  if (!rp23xx_psram_probe(psram_size))
    {
      return 0;
    }

  return psram_size;

fail:
  rp23xx_psram_putreg(0, RP23XX_QMI_DIRECT_CSR);
  rp23xx_psram_barrier();
  return 0;
}

static void rp23xx_psram_gpio_config(void)
{
  uint32_t gpio = CONFIG_RP23XX_PSRAM_CS1_GPIO;

  rp23xx_psram_diag(0x15, gpio);
  rp23xx_psram_diag(0x16, gpio);
  putreg32(RP23XX_GPIO_PAD_NORMAL, RP23XX_PADS_BANK0_GPIO(gpio));
  rp23xx_psram_diag(0x17, getreg32(RP23XX_PADS_BANK0_GPIO(gpio)));
  putreg32(RP23XX_GPIO_FUNC_XIP_CS1 &
           RP23XX_IO_BANK0_GPIO_CTRL_FUNCSEL_MASK,
           RP23XX_IO_BANK0_GPIO_CTRL(gpio));
  rp23xx_psram_diag(0x18, getreg32(RP23XX_IO_BANK0_GPIO_CTRL(gpio)));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

size_t rp23xx_psramconfig(void)
{
  irqstate_t flags;
  size_t psram_size;

  if (g_psram_ready)
    {
      return g_psram_size;
    }

  rp23xx_psram_diag(0x10, CONFIG_RP23XX_PSRAM_CS1_GPIO);
  rp23xx_psram_gpio_config();
  rp23xx_psram_diag(0x11, CONFIG_RP23XX_PSRAM_CS1_GPIO);

  flags = up_irq_save();
  rp23xx_psram_barrier();
  rp23xx_psram_diag(0x12, 0);
  psram_size = rp23xx_psram_init();
  rp23xx_psram_diag(0x13, psram_size);
  rp23xx_psram_barrier();
  up_irq_restore(flags);

  if (psram_size > CONFIG_RP23XX_PSRAM_SIZE)
    {
      psram_size = CONFIG_RP23XX_PSRAM_SIZE;
    }

  if (psram_size > 0)
    {
      g_psram_size = psram_size;
      g_psram_ready = true;
    }

  return g_psram_size;
}

size_t rp23xx_psramsize(void)
{
  return g_psram_size;
}
