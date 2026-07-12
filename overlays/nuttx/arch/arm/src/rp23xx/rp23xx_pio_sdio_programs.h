/****************************************************************************
 * arch/arm/src/rp23xx/rp23xx_pio_sdio_programs.h
 *
 * Copyright (c) 2011-2025 Bill Greiman
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * The instruction streams below are adapted from Adafruit SdFat's
 * SdCard/PioSdio/PioSdioCard.pio.  The configuration helpers are native
 * NuttX/RP23xx glue.
 ****************************************************************************/

#ifndef __ARCH_ARM_SRC_RP23XX_RP23XX_PIO_SDIO_PROGRAMS_H
#define __ARCH_ARM_SRC_RP23XX_RP23XX_PIO_SDIO_PROGRAMS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "rp23xx_pio.h"
#include "rp23xx_pio_instructions.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RP23XX_SDIO_PIO_IRQ             7
#define RP23XX_SDIO_PROGRAM_LENGTH       27

#define RP23XX_SDIO_CMD_WRAP_TARGET      0
#define RP23XX_SDIO_CMD_WRAP             9
#define RP23XX_SDIO_RDCLK_WRAP_TARGET    3
#define RP23XX_SDIO_RDCLK_WRAP           4
#define RP23XX_SDIO_RDDATA_WRAP_TARGET   1
#define RP23XX_SDIO_RDDATA_WRAP          3
#define RP23XX_SDIO_WRDATA_WRAP_TARGET   3
#define RP23XX_SDIO_WRDATA_WRAP          3
#define RP23XX_SDIO_WRRESP_WRAP_TARGET   2
#define RP23XX_SDIO_WRRESP_WRAP          3

static const uint16_t g_rp23xx_sdio_cmd_instructions[] =
{
  0x7101, /* out pins, 1       side 0 [1] */
  0x1940, /* jmp x--, 0        side 1 [1] */
  0x1160, /* jmp !y, 0         side 0 [1] */
  0xfb80, /* set pindirs, 0    side 1 [3] */
  0xb342, /* nop               side 0 [3] */
  0xba42, /* nop               side 1 [2] */
  0x00c4, /* jmp pin, 4                   */
  0x4001, /* in pins, 1                   */
  0x9260, /* push iffull block side 0 [2] */
  0x1987, /* jmp y--, 7        side 1 [1] */
};

static const uint16_t g_rp23xx_sdio_rdclk_instructions[] =
{
  0xb342, /* nop               side 0 [3] */
  0x1bc0, /* jmp pin, 0        side 1 [3] */
  0xc007, /* irq nowait 7                 */
  0x7261, /* out null, 1       side 0 [2] */
  0xb942, /* nop               side 1 [1] */
};

static const uint16_t g_rp23xx_sdio_rddata_template[] =
{
  0x20c7, /* wait 1 irq, 7 */
  0x2000, /* wait 0 gpio, patched clock pin */
  0x2080, /* wait 1 gpio, patched clock pin */
  0x4004, /* in pins, 4 */
};

static const uint16_t g_rp23xx_sdio_wrdata_instructions[] =
{
  0x7104, /* out pins, 4       side 0 [1] */
  0x1940, /* jmp x--, 0        side 1 [1] */
  0xc007, /* irq nowait 7                 */
  0xa042, /* nop                          */
};

static const uint16_t g_rp23xx_sdio_wrresp_instructions[] =
{
  0x20c7, /* wait 1 irq, 7                */
  0xe180, /* set pindirs, 0          [1]  */
  0x5c01, /* in pins, 1        side 1 [4] */
  0x9440, /* push iffull noblock side 0 [4] */
};

static const uint16_t g_rp23xx_sdio_reservation_instructions[27];

static const rp23xx_pio_program_t g_rp23xx_sdio_cmd_program =
{
  g_rp23xx_sdio_cmd_instructions, 10, -1
};

static const rp23xx_pio_program_t g_rp23xx_sdio_rdclk_program =
{
  g_rp23xx_sdio_rdclk_instructions, 5, -1
};

static const rp23xx_pio_program_t g_rp23xx_sdio_wrdata_program =
{
  g_rp23xx_sdio_wrdata_instructions, 4, -1
};

static const rp23xx_pio_program_t g_rp23xx_sdio_wrresp_program =
{
  g_rp23xx_sdio_wrresp_instructions, 4, -1
};

static const rp23xx_pio_program_t g_rp23xx_sdio_reservation_program =
{
  g_rp23xx_sdio_reservation_instructions,
  RP23XX_SDIO_PROGRAM_LENGTH,
  -1
};

static inline void rp23xx_sdio_set_clkdiv(FAR rp23xx_pio_sm_config *config,
                                          uint32_t div256)
{
  uint32_t integer = div256 >> 8;

  if (integer == 0)
    {
      integer = 1;
      div256 = 1 << 8;
    }

  rp23xx_sm_config_set_clkdiv_int_frac(config, integer,
                                        div256 & 0xff);
}

static inline rp23xx_pio_sm_config
rp23xx_sdio_cmd_config(uint32_t offset, uint32_t cmd_pin,
                       uint32_t clk_pin, uint32_t div256)
{
  rp23xx_pio_sm_config config = rp23xx_pio_get_default_sm_config();

  rp23xx_sm_config_set_wrap(&config,
                            offset + RP23XX_SDIO_CMD_WRAP_TARGET,
                            offset + RP23XX_SDIO_CMD_WRAP);
  rp23xx_sm_config_set_sideset(&config, 2, true, false);
  rp23xx_sm_config_set_sideset_pins(&config, clk_pin);
  rp23xx_sm_config_set_out_pins(&config, cmd_pin, 1);
  rp23xx_sm_config_set_in_pins(&config, cmd_pin);
  rp23xx_sm_config_set_set_pins(&config, cmd_pin, 1);
  rp23xx_sm_config_set_jmp_pin(&config, cmd_pin);
  rp23xx_sm_config_set_in_shift(&config, false, false, 8);
  rp23xx_sm_config_set_out_shift(&config, false, true, 8);
  rp23xx_sdio_set_clkdiv(&config, div256);
  return config;
}

static inline rp23xx_pio_sm_config
rp23xx_sdio_rdclk_config(uint32_t offset, uint32_t dat0_pin,
                         uint32_t clk_pin, uint32_t div256)
{
  rp23xx_pio_sm_config config = rp23xx_pio_get_default_sm_config();

  rp23xx_sm_config_set_wrap(&config,
                            offset + RP23XX_SDIO_RDCLK_WRAP_TARGET,
                            offset + RP23XX_SDIO_RDCLK_WRAP);
  rp23xx_sm_config_set_sideset(&config, 2, true, false);
  rp23xx_sm_config_set_fifo_join(&config, RP23XX_PIO_FIFO_JOIN_TX);
  rp23xx_sm_config_set_sideset_pins(&config, clk_pin);
  rp23xx_sm_config_set_in_pins(&config, dat0_pin);
  rp23xx_sm_config_set_jmp_pin(&config, dat0_pin);
  rp23xx_sm_config_set_out_shift(&config, false, true, 8);
  rp23xx_sdio_set_clkdiv(&config, div256);
  return config;
}

static inline rp23xx_pio_sm_config
rp23xx_sdio_rddata_config(uint32_t offset, uint32_t dat0_pin,
                          uint32_t div256)
{
  rp23xx_pio_sm_config config = rp23xx_pio_get_default_sm_config();

  rp23xx_sm_config_set_wrap(&config,
                            offset + RP23XX_SDIO_RDDATA_WRAP_TARGET,
                            offset + RP23XX_SDIO_RDDATA_WRAP);
  rp23xx_sm_config_set_fifo_join(&config, RP23XX_PIO_FIFO_JOIN_RX);
  rp23xx_sm_config_set_in_pins(&config, dat0_pin);
  rp23xx_sm_config_set_in_shift(&config, false, true, 32);
  rp23xx_sdio_set_clkdiv(&config, div256);
  return config;
}

static inline rp23xx_pio_sm_config
rp23xx_sdio_wrdata_config(uint32_t offset, uint32_t dat0_pin,
                          uint32_t clk_pin, uint32_t div256)
{
  rp23xx_pio_sm_config config = rp23xx_pio_get_default_sm_config();

  rp23xx_sm_config_set_wrap(&config,
                            offset + RP23XX_SDIO_WRDATA_WRAP_TARGET,
                            offset + RP23XX_SDIO_WRDATA_WRAP);
  rp23xx_sm_config_set_sideset(&config, 2, true, false);
  rp23xx_sm_config_set_fifo_join(&config, RP23XX_PIO_FIFO_JOIN_TX);
  rp23xx_sm_config_set_sideset_pins(&config, clk_pin);
  rp23xx_sm_config_set_out_pins(&config, dat0_pin, 4);
  rp23xx_sm_config_set_set_pins(&config, dat0_pin, 4);
  rp23xx_sm_config_set_out_shift(&config, false, true, 32);
  rp23xx_sdio_set_clkdiv(&config, div256);
  return config;
}

static inline rp23xx_pio_sm_config
rp23xx_sdio_wrresp_config(uint32_t offset, uint32_t dat0_pin,
                          uint32_t clk_pin, uint32_t div256)
{
  rp23xx_pio_sm_config config = rp23xx_pio_get_default_sm_config();

  rp23xx_sm_config_set_wrap(&config,
                            offset + RP23XX_SDIO_WRRESP_WRAP_TARGET,
                            offset + RP23XX_SDIO_WRRESP_WRAP);
  rp23xx_sm_config_set_sideset(&config, 2, true, false);
  rp23xx_sm_config_set_sideset_pins(&config, clk_pin);
  rp23xx_sm_config_set_in_pins(&config, dat0_pin);
  rp23xx_sm_config_set_set_pins(&config, dat0_pin, 4);
  rp23xx_sm_config_set_in_shift(&config, false, false, 8);
  rp23xx_sdio_set_clkdiv(&config, div256);
  return config;
}

#endif /* __ARCH_ARM_SRC_RP23XX_RP23XX_PIO_SDIO_PROGRAMS_H */
