/****************************************************************************
 * boards/arm/rp23xx/adafruit-fruit-jam-rp2350/src/rp23xx_fruitjam_ir.c
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

#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

#include <nuttx/arch.h>
#include <nuttx/lirc.h>
#include <nuttx/rc/lirc_dev.h>

#include <arch/board/board.h>

#include "hardware/rp23xx_timer.h"
#include "rp23xx_gpio.h"

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_IR_RX

#undef RP23XX_TIMER_BASE
#define RP23XX_TIMER_BASE RP23XX_TIMER0_BASE

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FRUITJAM_IR_DEVNO             0
#define FRUITJAM_IR_GPIO              BOARD_GPIO_LED_PIN
#define FRUITJAM_IR_BUFFER_EVENTS     256
#define FRUITJAM_IR_MIN_TIMEOUT_US    1000
#define FRUITJAM_IR_MAX_TIMEOUT_US    1000000
#define FRUITJAM_IR_DEFAULT_TIMEOUT   125000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct fruitjam_irrx_s
{
  struct lirc_lowerhalf_s lower;
  uint64_t                last_us;
  bool                    last_level;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  fruitjam_irrx_open(FAR struct lirc_lowerhalf_s *lower);
static void fruitjam_irrx_close(FAR struct lirc_lowerhalf_s *lower);
static int  fruitjam_irrx_set_timeout(FAR struct lirc_lowerhalf_s *lower,
                                      unsigned int timeout);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct lirc_ops_s g_fruitjam_irrx_ops =
{
  LIRC_DRIVER_IR_RAW,          /* driver_type */
  fruitjam_irrx_open,         /* open */
  fruitjam_irrx_close,        /* close */
  NULL,                       /* s_tx_mask */
  NULL,                       /* s_tx_carrier */
  NULL,                       /* s_tx_duty_cycle */
  NULL,                       /* s_rx_carrier_range */
  NULL,                       /* tx_ir */
  NULL,                       /* tx_scancode */
  NULL,                       /* s_learning_mode */
  NULL,                       /* s_carrier_report */
  fruitjam_irrx_set_timeout   /* s_timeout */
};

static struct fruitjam_irrx_s g_fruitjam_irrx =
{
  .lower =
    {
      .ops           = &g_fruitjam_irrx_ops,
      .timeout       = FRUITJAM_IR_DEFAULT_TIMEOUT,
      .min_timeout   = FRUITJAM_IR_MIN_TIMEOUT_US,
      .max_timeout   = FRUITJAM_IR_MAX_TIMEOUT_US,
      .buffer_bytes  = FRUITJAM_IR_BUFFER_EVENTS * sizeof(unsigned int),
      .rx_resolution = 1,
    },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t fruitjam_irrx_time_us(void)
{
  uint32_t hi;
  uint32_t lo;
  uint32_t next_hi;

  do
    {
      hi = getreg32(RP23XX_TIMER_TIMERAWH);
      lo = getreg32(RP23XX_TIMER_TIMERAWL);
      next_hi = getreg32(RP23XX_TIMER_TIMERAWH);
    }
  while (hi != next_hi);

  return ((uint64_t)hi << 32) | lo;
}

static int fruitjam_irrx_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct fruitjam_irrx_s *priv = arg;
  uint64_t now;
  uint64_t elapsed;
  bool level;

  now = fruitjam_irrx_time_us();
  level = rp23xx_gpio_get(FRUITJAM_IR_GPIO);

  elapsed = now - priv->last_us;
  if (elapsed > 0)
    {
      if (elapsed > LIRC_VALUE_MASK)
        {
          elapsed = LIRC_VALUE_MASK;
        }

      lirc_sample_event(&priv->lower,
                        priv->last_level ? LIRC_SPACE(elapsed) :
                                           LIRC_PULSE(elapsed));
    }

  priv->last_us = now;
  priv->last_level = level;

  return OK;
}

static int fruitjam_irrx_open(FAR struct lirc_lowerhalf_s *lower)
{
  FAR struct fruitjam_irrx_s *priv = (FAR struct fruitjam_irrx_s *)lower;

  rp23xx_gpio_disable_irq(FRUITJAM_IR_GPIO);
  rp23xx_gpio_init(FRUITJAM_IR_GPIO);
  rp23xx_gpio_setdir(FRUITJAM_IR_GPIO, false);
  rp23xx_gpio_set_input_hysteresis_enabled(FRUITJAM_IR_GPIO, true);

  priv->last_level = rp23xx_gpio_get(FRUITJAM_IR_GPIO);
  priv->last_us = fruitjam_irrx_time_us();

  rp23xx_gpio_irq_attach(FRUITJAM_IR_GPIO,
                         RP23XX_GPIO_INTR_EDGE_BOTH,
                         fruitjam_irrx_interrupt, priv);
  rp23xx_gpio_enable_irq(FRUITJAM_IR_GPIO);

  return OK;
}

static void fruitjam_irrx_close(FAR struct lirc_lowerhalf_s *lower)
{
  rp23xx_gpio_disable_irq(FRUITJAM_IR_GPIO);
  rp23xx_gpio_irq_attach(FRUITJAM_IR_GPIO, RP23XX_GPIO_INTR_EDGE_LOW,
                         NULL, NULL);
  rp23xx_gpio_init(FRUITJAM_IR_GPIO);
  rp23xx_gpio_setdir(FRUITJAM_IR_GPIO, false);
}

static int fruitjam_irrx_set_timeout(FAR struct lirc_lowerhalf_s *lower,
                                     unsigned int timeout)
{
  if (timeout < FRUITJAM_IR_MIN_TIMEOUT_US ||
      timeout > FRUITJAM_IR_MAX_TIMEOUT_US)
    {
      return -EINVAL;
    }

  lower->timeout = timeout;
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int fruitjam_irrx_initialize(void)
{
  rp23xx_gpio_init(FRUITJAM_IR_GPIO);
  rp23xx_gpio_setdir(FRUITJAM_IR_GPIO, false);
  rp23xx_gpio_set_input_hysteresis_enabled(FRUITJAM_IR_GPIO, true);

  return lirc_register(&g_fruitjam_irrx.lower, FRUITJAM_IR_DEVNO);
}

#endif /* CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_IR_RX */
