/****************************************************************************
 * boards/arm/rp23xx/adafruit-fruit-jam-rp2350/src/rp23xx_fruitjam_audio.c
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
#include <nuttx/i2c/i2c_master.h>

#include <arch/board/board.h>

#include "rp23xx_gpio.h"
#include "rp23xx_i2c.h"

#ifdef CONFIG_RP23XX_I2S

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FRUITJAM_DAC_I2C_ADDR       0x18
#define FRUITJAM_DAC_I2C_FREQ       I2C_SPEED_STANDARD
#define FRUITJAM_DAC_RESET_PIN      BOARD_NINA_RESET_PIN

#define CHECK_CODEC_OP(op)          \
  do                                \
    {                               \
      ret = (op);                   \
      if (ret < 0)                  \
        {                           \
          return ret;               \
        }                           \
    }                               \
  while (0)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR struct i2c_master_s *g_codec_i2c;
static bool g_codec_initialized;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int fruitjam_codec_write(uint8_t reg, uint8_t value)
{
  uint8_t buffer[2] =
    {
      reg, value
    };
  struct i2c_msg_s msg =
    {
      .frequency = FRUITJAM_DAC_I2C_FREQ,
      .addr      = FRUITJAM_DAC_I2C_ADDR,
      .flags     = 0,
      .buffer    = buffer,
      .length    = sizeof(buffer),
    };

  return I2C_TRANSFER(g_codec_i2c, &msg, 1);
}

static int fruitjam_codec_read(uint8_t reg, FAR uint8_t *value)
{
  struct i2c_msg_s msg[2] =
    {
      {
        .frequency = FRUITJAM_DAC_I2C_FREQ,
        .addr      = FRUITJAM_DAC_I2C_ADDR,
        .flags     = I2C_M_NOSTOP,
        .buffer    = &reg,
        .length    = 1,
      },
      {
        .frequency = FRUITJAM_DAC_I2C_FREQ,
        .addr      = FRUITJAM_DAC_I2C_ADDR,
        .flags     = I2C_M_READ,
        .buffer    = value,
        .length    = 1,
      },
    };

  return I2C_TRANSFER(g_codec_i2c, msg, 2);
}

static int fruitjam_codec_modify(uint8_t reg, uint8_t mask, uint8_t value)
{
  uint8_t current;
  int ret;

  ret = fruitjam_codec_read(reg, &current);
  if (ret < 0)
    {
      return ret;
    }

  return fruitjam_codec_write(reg, (current & ~mask) | (value & mask));
}

static int fruitjam_codec_set_page(uint8_t page)
{
  return fruitjam_codec_write(0x00, page);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_i2sdev_codec_initialize(int port)
{
  int ret;

  if (port != 0)
    {
      return OK;
    }

  if (g_codec_initialized)
    {
      return OK;
    }

  g_codec_i2c = rp23xx_i2cbus_initialize(0);
  if (g_codec_i2c == NULL)
    {
      return -ENODEV;
    }

  rp23xx_gpio_init(FRUITJAM_DAC_RESET_PIN);
  rp23xx_gpio_setdir(FRUITJAM_DAC_RESET_PIN, true);
  rp23xx_gpio_put(FRUITJAM_DAC_RESET_PIN, true);
  up_mdelay(100);

  CHECK_CODEC_OP(fruitjam_codec_write(0x01, 0x01));
  up_mdelay(10);

  CHECK_CODEC_OP(fruitjam_codec_modify(0x1b, 0xc0, 0x00));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x1b, 0x30, 0x00));

  CHECK_CODEC_OP(fruitjam_codec_modify(0x04, 0x03, 0x03));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x04, 0x0c, 0x04));

  CHECK_CODEC_OP(fruitjam_codec_write(0x06, 0x20));
  CHECK_CODEC_OP(fruitjam_codec_write(0x08, 0x00));
  CHECK_CODEC_OP(fruitjam_codec_write(0x07, 0x00));

  CHECK_CODEC_OP(fruitjam_codec_modify(0x05, 0x0f, 0x02));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x05, 0x70, 0x10));

  CHECK_CODEC_OP(fruitjam_codec_modify(0x0b, 0x7f, 0x08));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x0b, 0x80, 0x80));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x0c, 0x7f, 0x02));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x0c, 0x80, 0x80));

  CHECK_CODEC_OP(fruitjam_codec_modify(0x12, 0x7f, 0x08));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x12, 0x80, 0x80));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x13, 0x7f, 0x02));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x13, 0x80, 0x80));

  CHECK_CODEC_OP(fruitjam_codec_modify(0x05, 0x80, 0x80));

  CHECK_CODEC_OP(fruitjam_codec_set_page(1));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x2e, 0xff, 0x0b));
  CHECK_CODEC_OP(fruitjam_codec_set_page(0));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x43, 0x80, 0x80));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x30, 0x80, 0x80));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x33, 0x3c, 0x14));

  CHECK_CODEC_OP(fruitjam_codec_modify(0x3f, 0xc0, 0xc0));

  CHECK_CODEC_OP(fruitjam_codec_set_page(1));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x23, 0xc0, 0x40));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x23, 0x0c, 0x04));

  CHECK_CODEC_OP(fruitjam_codec_set_page(0));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x40, 0x0c, 0x00));
  CHECK_CODEC_OP(fruitjam_codec_write(0x41, 0x00));
  CHECK_CODEC_OP(fruitjam_codec_write(0x42, 0x00));

  CHECK_CODEC_OP(fruitjam_codec_modify(0x51, 0x80, 0x80));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x52, 0x80, 0x00));
  CHECK_CODEC_OP(fruitjam_codec_write(0x53, 0x68));

  CHECK_CODEC_OP(fruitjam_codec_set_page(1));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x1f, 0xc0, 0xc0));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x28, 0x04, 0x04));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x29, 0x04, 0x04));
  CHECK_CODEC_OP(fruitjam_codec_write(0x24, 0x0a));
  CHECK_CODEC_OP(fruitjam_codec_write(0x25, 0x0a));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x28, 0x78, 0x40));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x29, 0x78, 0x40));

  CHECK_CODEC_OP(fruitjam_codec_modify(0x20, 0x80, 0x80));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x2a, 0x04, 0x04));
  CHECK_CODEC_OP(fruitjam_codec_modify(0x2a, 0x18, 0x08));
  CHECK_CODEC_OP(fruitjam_codec_write(0x26, 0x0a));

  CHECK_CODEC_OP(fruitjam_codec_set_page(0));

  g_codec_initialized = true;
  return OK;
}

int fruitjam_audio_codec_initialize(void)
{
  return board_i2sdev_codec_initialize(0);
}

int board_fruitjam_audio_codec_recover(void)
{
  g_codec_initialized = false;
  return board_i2sdev_codec_initialize(0);
}

#endif /* CONFIG_RP23XX_I2S */
