/****************************************************************************
 * arch/arm/src/rp23xx/rp23xx_pio_sdio.c
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

#ifdef CONFIG_RP23XX_PIO_SDIO

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/cache.h>
#include <nuttx/clock.h>
#include <nuttx/debug.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/sdio.h>
#include <nuttx/semaphore.h>
#include <nuttx/wqueue.h>

#include <arch/board/board.h>

#include "arm_internal.h"
#include "hardware/rp23xx_pads_bank0.h"
#include "hardware/rp23xx_pio.h"
#include "rp23xx_dmac.h"
#include "rp23xx_gpio.h"
#include "rp23xx_pio.h"
#include "rp23xx_pio_instructions.h"
#include "rp23xx_pio_sdio.h"
#include "rp23xx_pio_sdio_programs.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RP23XX_SDIO_MAX_BLOCK_SIZE       512
#define RP23XX_SDIO_RX_WORDS             (RP23XX_SDIO_MAX_BLOCK_SIZE / 4 + 2)
#define RP23XX_SDIO_TX_WORDS             (RP23XX_SDIO_MAX_BLOCK_SIZE / 4 + 3)
#define RP23XX_SDIO_COMMAND_TIMEOUT_MS    100
#define RP23XX_SDIO_READ_TIMEOUT_MS       250
#define RP23XX_SDIO_WRITE_TIMEOUT_MS      1000
#define RP23XX_SDIO_DEBOUNCE_MS           20

#define RP23XX_SDIO_XFR_NONE              0
#define RP23XX_SDIO_XFR_READ              1
#define RP23XX_SDIO_XFR_WRITE             2

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rp23xx_pio_sdio_dev_s
{
  struct sdio_dev_s dev;
  struct rp23xx_pio_sdio_config_s config;

  sem_t waitsem;
  sem_t dmasem;
  sdio_eventset_t waitevents;
  volatile sdio_eventset_t wkupevent;
  uint32_t wait_timeout;

  worker_t callback;
  FAR void *cbarg;
  sdio_eventset_t cbevents;
  sdio_statset_t cdstatus;
  struct work_s cdwork;
  struct work_s cbwork;

  DMA_HANDLE rx_dma;
  DMA_HANDLE tx_dma;
  volatile uint8_t dma_status;

  int sm_cmd;
  int sm_aux;
  int cmd_offset;
  int rdclk_offset;
  int rddata_offset;
  int wrdata_offset;
  int wrresp_offset;
  uint16_t rddata_instructions[4];
  rp23xx_pio_program_t rddata_program;

  uint32_t clock_frequency;
  uint32_t blocklen;
  uint32_t nblocks;
  FAR uint8_t *buffer;
  size_t buflen;
  uint8_t transfer;

  uint32_t last_cmd;
  int last_cmd_error;
  uint32_t response[4];

  uint32_t rx_words[RP23XX_SDIO_RX_WORDS];
  uint32_t tx_words[RP23XX_SDIO_TX_WORDS];
  uint32_t response_word;
  uint32_t clock_fill;

  bool attached;
  bool initialized;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void rp23xx_sdio_reset(FAR struct sdio_dev_s *dev);
static sdio_capset_t rp23xx_sdio_capabilities(FAR struct sdio_dev_s *dev);
static sdio_statset_t rp23xx_sdio_status(FAR struct sdio_dev_s *dev);
static void rp23xx_sdio_widebus(FAR struct sdio_dev_s *dev, bool wide);
static void rp23xx_sdio_clock(FAR struct sdio_dev_s *dev,
                              enum sdio_clock_e rate);
static int rp23xx_sdio_attach(FAR struct sdio_dev_s *dev);
static int rp23xx_sdio_sendcmd(FAR struct sdio_dev_s *dev,
                               uint32_t cmd, uint32_t arg);
#ifdef CONFIG_SDIO_BLOCKSETUP
static void rp23xx_sdio_blocksetup(FAR struct sdio_dev_s *dev,
                                   unsigned int blocklen,
                                   unsigned int nblocks);
#endif
static int rp23xx_sdio_recvsetup(FAR struct sdio_dev_s *dev,
                                 FAR uint8_t *buffer, size_t nbytes);
static int rp23xx_sdio_sendsetup(FAR struct sdio_dev_s *dev,
                                 FAR const uint8_t *buffer, size_t nbytes);
static int rp23xx_sdio_cancel(FAR struct sdio_dev_s *dev);
static int rp23xx_sdio_waitresponse(FAR struct sdio_dev_s *dev,
                                    uint32_t cmd);
static int rp23xx_sdio_recvshort(FAR struct sdio_dev_s *dev,
                                 uint32_t cmd, FAR uint32_t *response);
static int rp23xx_sdio_recvlong(FAR struct sdio_dev_s *dev,
                                uint32_t cmd, FAR uint32_t response[4]);
static void rp23xx_sdio_waitenable(FAR struct sdio_dev_s *dev,
                                   sdio_eventset_t eventset,
                                   uint32_t timeout);
static sdio_eventset_t rp23xx_sdio_eventwait(FAR struct sdio_dev_s *dev);
static void rp23xx_sdio_callbackenable(FAR struct sdio_dev_s *dev,
                                       sdio_eventset_t eventset);
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
static int rp23xx_sdio_registercallback(FAR struct sdio_dev_s *dev,
                                        worker_t callback, FAR void *arg);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rp23xx_pio_sdio_dev_s g_sdio =
{
  .dev =
  {
    .mutex            = NXMUTEX_INITIALIZER,
    .reset            = rp23xx_sdio_reset,
    .capabilities     = rp23xx_sdio_capabilities,
    .status           = rp23xx_sdio_status,
    .widebus          = rp23xx_sdio_widebus,
    .clock            = rp23xx_sdio_clock,
    .attach           = rp23xx_sdio_attach,
    .sendcmd          = rp23xx_sdio_sendcmd,
#ifdef CONFIG_SDIO_BLOCKSETUP
    .blocksetup       = rp23xx_sdio_blocksetup,
#endif
    .recvsetup        = rp23xx_sdio_recvsetup,
    .sendsetup        = rp23xx_sdio_sendsetup,
    .cancel           = rp23xx_sdio_cancel,
    .waitresponse     = rp23xx_sdio_waitresponse,
    .recv_r1          = rp23xx_sdio_recvshort,
    .recv_r2          = rp23xx_sdio_recvlong,
    .recv_r3          = rp23xx_sdio_recvshort,
    .recv_r4          = rp23xx_sdio_recvshort,
    .recv_r5          = rp23xx_sdio_recvshort,
    .recv_r6          = rp23xx_sdio_recvshort,
    .recv_r7          = rp23xx_sdio_recvshort,
    .waitenable       = rp23xx_sdio_waitenable,
    .eventwait        = rp23xx_sdio_eventwait,
    .callbackenable   = rp23xx_sdio_callbackenable,
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
    .registercallback = rp23xx_sdio_registercallback,
#endif
#ifdef CONFIG_SDIO_DMA
    .dmarecvsetup     = rp23xx_sdio_recvsetup,
    .dmasendsetup     = rp23xx_sdio_sendsetup,
#endif
  },
  .waitsem      = SEM_INITIALIZER(0),
  .dmasem       = SEM_INITIALIZER(0),
  .sm_cmd       = -1,
  .sm_aux       = -1,
  .cmd_offset   = -1,
  .rdclk_offset = -1,
  .rddata_offset = -1,
  .wrdata_offset = -1,
  .wrresp_offset = -1,
  .clock_fill   = UINT32_MAX,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline bool rp23xx_sdio_present(
  FAR struct rp23xx_pio_sdio_dev_s *priv)
{
  bool level = rp23xx_gpio_get(priv->config.cd_pin);
  return level == priv->config.cd_active_high;
}

static bool rp23xx_sdio_timedout(clock_t start, uint32_t timeout_ms)
{
  return clock_systime_ticks() - start >= MSEC2TICK(timeout_ms);
}

static void rp23xx_sdio_drainsem(FAR sem_t *sem)
{
  while (nxsem_trywait(sem) == OK)
    {
    }
}

static uint8_t rp23xx_sdio_crc7(FAR const uint8_t *data, size_t len)
{
  uint8_t crc = 0;
  size_t i;
  int bit;

  for (i = 0; i < len; i++)
    {
      uint8_t value = data[i];

      for (bit = 0; bit < 8; bit++)
        {
          crc <<= 1;
          if (((value ^ crc) & 0x80) != 0)
            {
              crc ^= 0x09;
            }

          value <<= 1;
        }
    }

  return (crc << 1) | 1;
}

/* Four-lane SD CRC16, adapted from Adafruit SdFat's MIT-licensed PIO SD
 * implementation.  Each bit in a nibble belongs to one DAT lane.
 */

static inline uint64_t rp23xx_sdio_crc16(uint64_t crc, uint32_t input)
{
  uint32_t output = crc >> 32;
  uint64_t value;

  crc <<= 32;
  output ^= output >> 16;
  output ^= input >> 16;
  value = output ^ input;
  crc ^= value;
  crc ^= value << 20;
  crc ^= value << 48;
  return crc;
}

static uint32_t rp23xx_sdio_div256(uint32_t frequency)
{
  uint64_t numerator;
  uint32_t divisor;

  DEBUGASSERT(frequency > 0);
  numerator = ((uint64_t)BOARD_SYS_FREQ << 8) + 4 * frequency - 1;
  divisor = numerator / (4 * frequency);
  return divisor < 256 ? 256 : divisor;
}

static void rp23xx_sdio_dma_callback(DMA_HANDLE handle, uint8_t status,
                                     FAR void *arg)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv = arg;

  UNUSED(handle);
  priv->dma_status = status;
  nxsem_post(&priv->dmasem);
}

static int rp23xx_sdio_dmawait(FAR struct rp23xx_pio_sdio_dev_s *priv,
                               uint32_t timeout_ms)
{
  int ret;

  ret = nxsem_tickwait_uninterruptible(&priv->dmasem,
                                       MSEC2TICK(timeout_ms));
  if (ret < 0)
    {
      return ret;
    }

  return priv->dma_status == 0 ? OK : -EIO;
}

static void rp23xx_sdio_finish_event(
  FAR struct rp23xx_pio_sdio_dev_s *priv, sdio_eventset_t event)
{
  irqstate_t flags;

  flags = enter_critical_section();
  if ((priv->waitevents & event) != 0 ||
      (event & (SDIOWAIT_ERROR | SDIOWAIT_TIMEOUT)) != 0)
    {
      priv->wkupevent = event;
      nxsem_post(&priv->waitsem);
    }

  leave_critical_section(flags);
}

static void rp23xx_sdio_disable_sms(
  FAR struct rp23xx_pio_sdio_dev_s *priv)
{
  uint32_t mask = (1u << priv->sm_cmd) | (1u << priv->sm_aux);
  rp23xx_pio_set_sm_mask_enabled(priv->config.pio, mask, false);
}

static int rp23xx_sdio_read_block(
  FAR struct rp23xx_pio_sdio_dev_s *priv, FAR uint8_t *buffer,
  size_t count)
{
  rp23xx_pio_sm_config data_config;
  rp23xx_pio_sm_config clock_config;
  dma_config_t rxconfig;
  dma_config_t txconfig;
  uint32_t div256;
  uint32_t nwords;
  uint32_t total_words;
  uint32_t mask;
  uint64_t crc = 0;
  uint64_t expected;
  uint32_t i;
  int ret;

  if (count == 0 || count > RP23XX_SDIO_MAX_BLOCK_SIZE ||
      (count & 3) != 0)
    {
      return -EINVAL;
    }

  div256 = rp23xx_sdio_div256(priv->clock_frequency);
  nwords = count >> 2;
  total_words = nwords + 2;
  mask = (1u << priv->sm_cmd) | (1u << priv->sm_aux);

  data_config = rp23xx_sdio_rddata_config(priv->rddata_offset,
    rp23xx_pio_gpio_to_pin(priv->config.pio, priv->config.dat0_pin),
    div256);
  clock_config = rp23xx_sdio_rdclk_config(priv->rdclk_offset,
    rp23xx_pio_gpio_to_pin(priv->config.pio, priv->config.dat0_pin),
    rp23xx_pio_gpio_to_pin(priv->config.pio, priv->config.clk_pin),
    div256);

  rp23xx_pio_sm_init(priv->config.pio, priv->sm_cmd,
                     priv->rddata_offset, &data_config);
  rp23xx_pio_sm_init(priv->config.pio, priv->sm_aux,
                     priv->rdclk_offset, &clock_config);
  putreg32(1u << RP23XX_SDIO_PIO_IRQ, RP23XX_PIO_IRQ(priv->config.pio));

  rp23xx_sdio_drainsem(&priv->dmasem);
  priv->dma_status = 0;
  memset(priv->rx_words, 0, total_words * sizeof(uint32_t));
  up_clean_dcache((uintptr_t)&priv->clock_fill,
                  (uintptr_t)&priv->clock_fill + sizeof(priv->clock_fill));
  up_invalidate_dcache((uintptr_t)priv->rx_words,
                       (uintptr_t)priv->rx_words +
                       total_words * sizeof(uint32_t));

  rxconfig.dreq = rp23xx_pio_get_dreq(priv->config.pio,
                                      priv->sm_cmd, false);
  rxconfig.size = RP23XX_DMA_SIZE_WORD;
  rxconfig.noincr = false;
  rp23xx_rxdmasetup(priv->rx_dma,
                    RP23XX_PIO_RXF(priv->config.pio, priv->sm_cmd),
                    (uintptr_t)priv->rx_words,
                    total_words * sizeof(uint32_t), rxconfig);

  txconfig.dreq = rp23xx_pio_get_dreq(priv->config.pio,
                                      priv->sm_aux, true);
  txconfig.size = RP23XX_DMA_SIZE_BYTE;
  txconfig.noincr = true;
  rp23xx_txdmasetup(priv->tx_dma,
                    RP23XX_PIO_TXF(priv->config.pio, priv->sm_aux),
                    (uintptr_t)&priv->clock_fill, total_words, txconfig);

  rp23xx_dmastart(priv->rx_dma, rp23xx_sdio_dma_callback, priv);
  rp23xx_dmastart(priv->tx_dma, NULL, NULL);
  rp23xx_pio_enable_sm_mask_in_sync(priv->config.pio, mask);

  ret = rp23xx_sdio_dmawait(priv, RP23XX_SDIO_READ_TIMEOUT_MS);
  rp23xx_sdio_disable_sms(priv);
  if (ret < 0)
    {
      rp23xx_dmastop(priv->rx_dma);
      rp23xx_dmastop(priv->tx_dma);
      return ret;
    }

  up_invalidate_dcache((uintptr_t)priv->rx_words,
                       (uintptr_t)priv->rx_words +
                       total_words * sizeof(uint32_t));
  for (i = 0; i < nwords; i++)
    {
      uint32_t word = priv->rx_words[i];
      uint32_t host = __builtin_bswap32(word);

      crc = rp23xx_sdio_crc16(crc, word);
      memcpy(buffer + i * sizeof(host), &host, sizeof(host));
    }

  expected = ((uint64_t)priv->rx_words[nwords] << 32) |
             priv->rx_words[nwords + 1];
  return crc == expected ? OK : -EILSEQ;
}

static int rp23xx_sdio_wait_dat0(
  FAR struct rp23xx_pio_sdio_dev_s *priv, uint32_t timeout_ms)
{
  clock_t start = clock_systime_ticks();

  while (!rp23xx_gpio_get(priv->config.dat0_pin))
    {
      if (!rp23xx_sdio_present(priv))
        {
          return -ENODEV;
        }

      if (rp23xx_sdio_timedout(start, timeout_ms))
        {
          return -ETIMEDOUT;
        }
    }

  return OK;
}

static int rp23xx_sdio_write_block(
  FAR struct rp23xx_pio_sdio_dev_s *priv, FAR const uint8_t *buffer,
  size_t count)
{
  rp23xx_pio_sm_config data_config;
  rp23xx_pio_sm_config response_config;
  dma_config_t rxconfig;
  dma_config_t txconfig;
  uint32_t div256;
  uint32_t nwords;
  uint32_t mask;
  uint64_t crc = 0;
  uint32_t i;
  int ret;

  if (count != RP23XX_SDIO_MAX_BLOCK_SIZE)
    {
      return -ENOTSUP;
    }

  ret = rp23xx_sdio_wait_dat0(priv, RP23XX_SDIO_WRITE_TIMEOUT_MS);
  if (ret < 0)
    {
      return ret;
    }

  nwords = count >> 2;
  for (i = 0; i < nwords; i++)
    {
      uint32_t host;
      uint32_t word;

      memcpy(&host, buffer + i * sizeof(host), sizeof(host));
      word = __builtin_bswap32(host);
      priv->tx_words[i] = word;
      crc = rp23xx_sdio_crc16(crc, word);
    }

  priv->tx_words[nwords] = crc >> 32;
  priv->tx_words[nwords + 1] = crc;
  priv->tx_words[nwords + 2] = UINT32_MAX;
  priv->response_word = 0;
  div256 = rp23xx_sdio_div256(priv->clock_frequency);
  mask = (1u << priv->sm_cmd) | (1u << priv->sm_aux);

  data_config = rp23xx_sdio_wrdata_config(priv->wrdata_offset,
    rp23xx_pio_gpio_to_pin(priv->config.pio, priv->config.dat0_pin),
    rp23xx_pio_gpio_to_pin(priv->config.pio, priv->config.clk_pin),
    div256);
  response_config = rp23xx_sdio_wrresp_config(priv->wrresp_offset,
    rp23xx_pio_gpio_to_pin(priv->config.pio, priv->config.dat0_pin),
    rp23xx_pio_gpio_to_pin(priv->config.pio, priv->config.clk_pin),
    div256);

  rp23xx_pio_sm_init(priv->config.pio, priv->sm_cmd,
                     priv->wrdata_offset, &data_config);
  rp23xx_pio_sm_init(priv->config.pio, priv->sm_aux,
                     priv->wrresp_offset, &response_config);
  putreg32(1048, RP23XX_PIO_TXF(priv->config.pio, priv->sm_cmd));
  rp23xx_pio_sm_exec(priv->config.pio, priv->sm_cmd,
                     pio_encode_out(pio_x, 32));
  rp23xx_pio_sm_exec(priv->config.pio, priv->sm_cmd,
                     pio_encode_set(pio_pindirs, 0x0f));
  putreg32(0xfffffff0, RP23XX_PIO_TXF(priv->config.pio, priv->sm_cmd));
  putreg32(1u << RP23XX_SDIO_PIO_IRQ, RP23XX_PIO_IRQ(priv->config.pio));

  rp23xx_sdio_drainsem(&priv->dmasem);
  priv->dma_status = 0;
  up_clean_dcache((uintptr_t)priv->tx_words,
                  (uintptr_t)priv->tx_words +
                  (nwords + 3) * sizeof(uint32_t));
  up_invalidate_dcache((uintptr_t)&priv->response_word,
                       (uintptr_t)&priv->response_word +
                       sizeof(priv->response_word));

  txconfig.dreq = rp23xx_pio_get_dreq(priv->config.pio,
                                      priv->sm_cmd, true);
  txconfig.size = RP23XX_DMA_SIZE_WORD;
  txconfig.noincr = false;
  rp23xx_txdmasetup(priv->tx_dma,
                    RP23XX_PIO_TXF(priv->config.pio, priv->sm_cmd),
                    (uintptr_t)priv->tx_words,
                    (nwords + 3) * sizeof(uint32_t), txconfig);

  rxconfig.dreq = rp23xx_pio_get_dreq(priv->config.pio,
                                      priv->sm_aux, false);
  rxconfig.size = RP23XX_DMA_SIZE_WORD;
  rxconfig.noincr = false;
  rp23xx_rxdmasetup(priv->rx_dma,
                    RP23XX_PIO_RXF(priv->config.pio, priv->sm_aux),
                    (uintptr_t)&priv->response_word,
                    sizeof(priv->response_word), rxconfig);

  rp23xx_dmastart(priv->rx_dma, rp23xx_sdio_dma_callback, priv);
  rp23xx_dmastart(priv->tx_dma, NULL, NULL);
  rp23xx_pio_enable_sm_mask_in_sync(priv->config.pio, mask);

  ret = rp23xx_sdio_dmawait(priv, RP23XX_SDIO_WRITE_TIMEOUT_MS);
  rp23xx_sdio_disable_sms(priv);
  if (ret < 0)
    {
      rp23xx_dmastop(priv->rx_dma);
      rp23xx_dmastop(priv->tx_dma);
      rp23xx_pio_sm_set_consecutive_pindirs(
        priv->config.pio, priv->sm_cmd,
        rp23xx_pio_gpio_to_pin(priv->config.pio, priv->config.dat0_pin),
        4, false);
      return ret;
    }

  up_invalidate_dcache((uintptr_t)&priv->response_word,
                       (uintptr_t)&priv->response_word +
                       sizeof(priv->response_word));
  return (priv->response_word & 0x1f) == 0x05 ? OK : -EIO;
}

static int rp23xx_sdio_transfer(FAR struct rp23xx_pio_sdio_dev_s *priv)
{
  size_t offset = 0;
  uint32_t blocklen;
  int ret = OK;

  if (priv->buffer == NULL || priv->buflen == 0)
    {
      return -EINVAL;
    }

  blocklen = priv->blocklen == 0 ? priv->buflen : priv->blocklen;
  if (blocklen > RP23XX_SDIO_MAX_BLOCK_SIZE ||
      priv->buflen % blocklen != 0)
    {
      return -EINVAL;
    }

  while (offset < priv->buflen)
    {
      if (!rp23xx_sdio_present(priv))
        {
          ret = -ENODEV;
          break;
        }

      if (priv->transfer == RP23XX_SDIO_XFR_READ)
        {
          ret = rp23xx_sdio_read_block(priv, priv->buffer + offset,
                                       blocklen);
        }
      else if (priv->transfer == RP23XX_SDIO_XFR_WRITE)
        {
          ret = rp23xx_sdio_write_block(priv, priv->buffer + offset,
                                        blocklen);
        }
      else
        {
          ret = -EINVAL;
        }

      if (ret < 0)
        {
          break;
        }

      offset += blocklen;
    }

  priv->buffer = NULL;
  priv->buflen = 0;
  priv->transfer = RP23XX_SDIO_XFR_NONE;
  return ret;
}

static int rp23xx_sdio_command(FAR struct rp23xx_pio_sdio_dev_s *priv,
                               uint32_t cmd, uint32_t arg)
{
  rp23xx_pio_sm_config config;
  uint8_t frame[6];
  uint8_t raw[17];
  uint32_t response_type = cmd & MMCSD_RESPONSE_MASK;
  uint32_t index = cmd & 0x3f;
  uint32_t nresponse;
  uint32_t div256;
  clock_t start;
  uint32_t i;
  int ret = OK;

  switch (response_type)
    {
      case MMCSD_NO_RESPONSE:
        nresponse = 0;
        break;
      case MMCSD_R2_RESPONSE:
        nresponse = 17;
        break;
      default:
        nresponse = 6;
        break;
    }

  div256 = rp23xx_sdio_div256(priv->clock_frequency);
  config = rp23xx_sdio_cmd_config(priv->cmd_offset,
    rp23xx_pio_gpio_to_pin(priv->config.pio, priv->config.cmd_pin),
    rp23xx_pio_gpio_to_pin(priv->config.pio, priv->config.clk_pin),
    div256);

  rp23xx_pio_sm_set_enabled(priv->config.pio, priv->sm_aux, false);
  rp23xx_pio_sm_init(priv->config.pio, priv->sm_cmd,
                     priv->cmd_offset, &config);
  rp23xx_pio_sm_exec(priv->config.pio, priv->sm_cmd,
                     pio_encode_set(pio_pindirs, 1));
  putreg8(55, RP23XX_PIO_TXF(priv->config.pio, priv->sm_cmd));
  rp23xx_pio_sm_exec(priv->config.pio, priv->sm_cmd,
                     pio_encode_out(pio_x, 8));
  putreg8(nresponse == 0 ? 0 : 8 * nresponse - 1,
          RP23XX_PIO_TXF(priv->config.pio, priv->sm_cmd));
  rp23xx_pio_sm_exec(priv->config.pio, priv->sm_cmd,
                     pio_encode_out(pio_y, 8));

  frame[0] = index | 0x40;
  frame[1] = arg >> 24;
  frame[2] = arg >> 16;
  frame[3] = arg >> 8;
  frame[4] = arg;
  frame[5] = rp23xx_sdio_crc7(frame, 5);

  putreg8(0xff, RP23XX_PIO_TXF(priv->config.pio, priv->sm_cmd));
  rp23xx_pio_sm_set_enabled(priv->config.pio, priv->sm_cmd, true);
  start = clock_systime_ticks();
  for (i = 0; i < sizeof(frame); i++)
    {
      while (rp23xx_pio_sm_is_tx_fifo_full(priv->config.pio,
                                            priv->sm_cmd))
        {
          if (rp23xx_sdio_timedout(start,
                                   RP23XX_SDIO_COMMAND_TIMEOUT_MS))
            {
              ret = -ETIMEDOUT;
              goto out;
            }
        }

      putreg8(frame[i], RP23XX_PIO_TXF(priv->config.pio, priv->sm_cmd));
    }

  if (nresponse == 0)
    {
      uint32_t stall = 1u <<
        (RP23XX_PIO_FDEBUG_TXSTALL_SHIFT + priv->sm_cmd);

      putreg32(stall, RP23XX_PIO_FDEBUG(priv->config.pio));
      while ((getreg32(RP23XX_PIO_FDEBUG(priv->config.pio)) & stall) == 0)
        {
          if (rp23xx_sdio_timedout(start,
                                   RP23XX_SDIO_COMMAND_TIMEOUT_MS))
            {
              ret = -ETIMEDOUT;
              goto out;
            }
        }
    }
  else
    {
      for (i = 0; i < nresponse; i++)
        {
          while (rp23xx_pio_sm_is_rx_fifo_empty(priv->config.pio,
                                                 priv->sm_cmd))
            {
              if (rp23xx_sdio_timedout(start,
                                       RP23XX_SDIO_COMMAND_TIMEOUT_MS))
                {
                  ret = -ETIMEDOUT;
                  goto out;
                }
            }

          raw[i] = getreg8(RP23XX_PIO_RXF(priv->config.pio,
                                          priv->sm_cmd));
        }

      if (response_type == MMCSD_R3_RESPONSE ||
          response_type == MMCSD_R4_RESPONSE)
        {
          if (raw[0] != 0x3f || raw[5] != 0xff)
            {
              ret = -EIO;
              goto out;
            }
        }
      else
        {
          FAR const uint8_t *crcdata = raw;
          size_t crclen = nresponse - 1;

          if (response_type == MMCSD_R2_RESPONSE)
            {
              crcdata++;
              crclen--;
            }

          if (raw[nresponse - 1] != rp23xx_sdio_crc7(crcdata, crclen))
            {
              ret = -EILSEQ;
              goto out;
            }
        }

      if (nresponse == 6)
        {
          priv->response[0] = ((uint32_t)raw[1] << 24) |
                              ((uint32_t)raw[2] << 16) |
                              ((uint32_t)raw[3] << 8) | raw[4];
        }
      else
        {
          for (i = 0; i < 4; i++)
            {
              size_t base = 1 + i * 4;
              priv->response[i] = ((uint32_t)raw[base] << 24) |
                                  ((uint32_t)raw[base + 1] << 16) |
                                  ((uint32_t)raw[base + 2] << 8) |
                                  raw[base + 3];
            }
        }
    }

out:
  rp23xx_pio_sm_set_enabled(priv->config.pio, priv->sm_cmd, false);
  return ret;
}

static void rp23xx_sdio_callback_worker(FAR void *arg)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv = arg;
  worker_t callback;
  FAR void *cbarg;
  irqstate_t flags;

  flags = enter_critical_section();
  callback = priv->callback;
  cbarg = priv->cbarg;
  priv->cbevents = 0;
  leave_critical_section(flags);

  if (callback != NULL)
    {
      callback(cbarg);
    }
}

static void rp23xx_sdio_schedule_callback(
  FAR struct rp23xx_pio_sdio_dev_s *priv)
{
  sdio_eventset_t event;

  event = (priv->cdstatus & SDIO_STATUS_PRESENT) != 0 ?
          SDIOMEDIA_INSERTED : SDIOMEDIA_EJECTED;
  if ((priv->cbevents & event) != 0 &&
      work_available(&priv->cbwork))
    {
      work_queue(HPWORK, &priv->cbwork, rp23xx_sdio_callback_worker,
                 priv, 0);
    }
}

static void rp23xx_sdio_cd_worker(FAR void *arg)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv = arg;
  sdio_statset_t status;

  status = rp23xx_sdio_present(priv) ? SDIO_STATUS_PRESENT : 0;
  if (status != priv->cdstatus)
    {
      priv->cdstatus = status;
      if (status == 0)
        {
          rp23xx_sdio_cancel(&priv->dev);
          rp23xx_sdio_finish_event(priv, SDIOWAIT_ERROR);
        }

      rp23xx_sdio_schedule_callback(priv);
    }

  rp23xx_gpio_enable_irq(priv->config.cd_pin);
}

static int rp23xx_sdio_cd_interrupt(int irq, FAR void *context,
                                    FAR void *arg)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv = arg;

  UNUSED(irq);
  UNUSED(context);
  rp23xx_gpio_disable_irq(priv->config.cd_pin);
  if (work_available(&priv->cdwork))
    {
      work_queue(HPWORK, &priv->cdwork, rp23xx_sdio_cd_worker, priv,
                 MSEC2TICK(RP23XX_SDIO_DEBOUNCE_MS));
    }

  return OK;
}

static void rp23xx_sdio_powerup_clocks(
  FAR struct rp23xx_pio_sdio_dev_s *priv)
{
  int i;

  rp23xx_gpio_init(priv->config.cmd_pin);
  rp23xx_gpio_set_pulls(priv->config.cmd_pin, true, false);
  rp23xx_gpio_set_drive_strength(priv->config.cmd_pin,
                                 RP23XX_PADS_BANK0_GPIO_DRIVE_8MA);
  rp23xx_gpio_setdir(priv->config.cmd_pin, true);
  rp23xx_gpio_put(priv->config.cmd_pin, true);

  rp23xx_gpio_init(priv->config.clk_pin);
  rp23xx_gpio_set_drive_strength(priv->config.clk_pin,
                                 RP23XX_PADS_BANK0_GPIO_DRIVE_8MA);
  rp23xx_gpio_set_slew_fast(priv->config.clk_pin, true);
  rp23xx_gpio_setdir(priv->config.clk_pin, true);

  for (i = 0; i < 160; i++)
    {
      rp23xx_gpio_put(priv->config.clk_pin, i & 1);
      up_udelay(2);
    }

  rp23xx_gpio_put(priv->config.clk_pin, false);
}

static void rp23xx_sdio_unload_programs(
  FAR struct rp23xx_pio_sdio_dev_s *priv)
{
  if (priv->wrresp_offset >= 0)
    {
      rp23xx_pio_remove_program(priv->config.pio,
                                &g_rp23xx_sdio_wrresp_program,
                                priv->wrresp_offset);
      priv->wrresp_offset = -1;
    }

  if (priv->wrdata_offset >= 0)
    {
      rp23xx_pio_remove_program(priv->config.pio,
                                &g_rp23xx_sdio_wrdata_program,
                                priv->wrdata_offset);
      priv->wrdata_offset = -1;
    }

  if (priv->rdclk_offset >= 0)
    {
      rp23xx_pio_remove_program(priv->config.pio,
                                &g_rp23xx_sdio_rdclk_program,
                                priv->rdclk_offset);
      priv->rdclk_offset = -1;
    }

  if (priv->cmd_offset >= 0)
    {
      rp23xx_pio_remove_program(priv->config.pio,
                                &g_rp23xx_sdio_cmd_program,
                                priv->cmd_offset);
      priv->cmd_offset = -1;
    }

  if (priv->rddata_offset >= 0)
    {
      rp23xx_pio_remove_program(priv->config.pio,
                                &priv->rddata_program,
                                priv->rddata_offset);
      priv->rddata_offset = -1;
    }
}

static int rp23xx_sdio_load_programs(
  FAR struct rp23xx_pio_sdio_dev_s *priv)
{
  uint32_t clk_pin;

  if (!rp23xx_pio_can_add_program(priv->config.pio,
                                   &g_rp23xx_sdio_reservation_program))
    {
      return -ENOSPC;
    }

  priv->sm_cmd = rp23xx_pio_claim_unused_sm(priv->config.pio, false);
  priv->sm_aux = rp23xx_pio_claim_unused_sm(priv->config.pio, false);
  if (priv->sm_cmd < 0 || priv->sm_aux < 0)
    {
      if (priv->sm_cmd >= 0)
        {
          rp23xx_pio_sm_unclaim(priv->config.pio, priv->sm_cmd);
          priv->sm_cmd = -1;
        }

      return -EBUSY;
    }

  clk_pin = rp23xx_pio_gpio_to_pin(priv->config.pio,
                                   priv->config.clk_pin);
  memcpy(priv->rddata_instructions, g_rp23xx_sdio_rddata_template,
         sizeof(priv->rddata_instructions));
  priv->rddata_instructions[1] = pio_encode_wait_gpio(false, clk_pin);
  priv->rddata_instructions[2] = pio_encode_wait_gpio(true, clk_pin);
  priv->rddata_program.instructions = priv->rddata_instructions;
  priv->rddata_program.length = 4;
  priv->rddata_program.origin = -1;

  priv->rddata_offset = rp23xx_pio_add_program(priv->config.pio,
                                                &priv->rddata_program);
  priv->cmd_offset = rp23xx_pio_add_program(priv->config.pio,
                                             &g_rp23xx_sdio_cmd_program);
  priv->rdclk_offset = rp23xx_pio_add_program(priv->config.pio,
                                               &g_rp23xx_sdio_rdclk_program);
  priv->wrdata_offset = rp23xx_pio_add_program(
    priv->config.pio, &g_rp23xx_sdio_wrdata_program);
  priv->wrresp_offset = rp23xx_pio_add_program(
    priv->config.pio, &g_rp23xx_sdio_wrresp_program);
  return OK;
}

static int rp23xx_sdio_configure_pio(
  FAR struct rp23xx_pio_sdio_dev_s *priv)
{
  uint32_t base;
  uint32_t dat0;
  uint32_t cmd;
  uint32_t clk;
  int i;
  int ret;

  base = priv->config.clk_pin > 31 || priv->config.cmd_pin > 31 ||
         priv->config.dat0_pin + 3 > 31 ? RP23XX_PIO_GPIOBASE_16 :
                                         RP23XX_PIO_GPIOBASE_0;
  rp23xx_pio_set_gpio_base(priv->config.pio, base);
  dat0 = rp23xx_pio_gpio_to_pin(priv->config.pio, priv->config.dat0_pin);
  cmd = rp23xx_pio_gpio_to_pin(priv->config.pio, priv->config.cmd_pin);
  clk = rp23xx_pio_gpio_to_pin(priv->config.pio, priv->config.clk_pin);

  ret = rp23xx_sdio_load_programs(priv);
  if (ret < 0)
    {
      return ret;
    }

  for (i = 0; i < 4; i++)
    {
      rp23xx_gpio_set_pulls(priv->config.dat0_pin + i, true, false);
      rp23xx_pio_gpio_init(priv->config.pio, priv->config.dat0_pin + i);
      rp23xx_pio_set_input_sync_bypass(priv->config.pio,
                                       priv->config.dat0_pin + i, true);
    }

  rp23xx_gpio_set_pulls(priv->config.cmd_pin, true, false);
  rp23xx_gpio_set_pulls(priv->config.clk_pin, true, false);
  rp23xx_gpio_set_drive_strength(priv->config.clk_pin,
                                 RP23XX_PADS_BANK0_GPIO_DRIVE_8MA);
  rp23xx_gpio_set_slew_fast(priv->config.clk_pin, true);
  rp23xx_pio_gpio_init(priv->config.pio, priv->config.cmd_pin);
  rp23xx_pio_gpio_init(priv->config.pio, priv->config.clk_pin);
  rp23xx_pio_set_input_sync_bypass(priv->config.pio,
                                   priv->config.cmd_pin, true);
  rp23xx_pio_set_input_sync_bypass(priv->config.pio,
                                   priv->config.clk_pin, true);

  rp23xx_pio_sm_init(priv->config.pio, priv->sm_cmd,
                     priv->cmd_offset, NULL);
  rp23xx_pio_sm_set_consecutive_pindirs(priv->config.pio, priv->sm_cmd,
                                        clk, 1, true);
  rp23xx_pio_sm_set_consecutive_pindirs(priv->config.pio, priv->sm_cmd,
                                        cmd, 1, true);
  rp23xx_pio_sm_set_consecutive_pindirs(priv->config.pio, priv->sm_cmd,
                                        dat0, 4, false);
  return OK;
}

static void rp23xx_sdio_reset(FAR struct sdio_dev_s *dev)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv =
    (FAR struct rp23xx_pio_sdio_dev_s *)dev;

  rp23xx_sdio_cancel(dev);
  priv->last_cmd = 0;
  priv->last_cmd_error = OK;
  memset(priv->response, 0, sizeof(priv->response));
  priv->blocklen = RP23XX_SDIO_MAX_BLOCK_SIZE;
  priv->nblocks = 1;
  priv->clock_frequency = priv->config.idmode_frequency;
}

static sdio_capset_t rp23xx_sdio_capabilities(FAR struct sdio_dev_s *dev)
{
  UNUSED(dev);
  return SDIO_CAPS_4BIT_ONLY | SDIO_CAPS_DMASUPPORTED |
         SDIO_CAPS_DMABEFOREWRITE;
}

static sdio_statset_t rp23xx_sdio_status(FAR struct sdio_dev_s *dev)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv =
    (FAR struct rp23xx_pio_sdio_dev_s *)dev;

  priv->cdstatus = rp23xx_sdio_present(priv) ? SDIO_STATUS_PRESENT : 0;
  return priv->cdstatus;
}

static void rp23xx_sdio_widebus(FAR struct sdio_dev_s *dev, bool wide)
{
  UNUSED(dev);
  UNUSED(wide);
}

static void rp23xx_sdio_clock(FAR struct sdio_dev_s *dev,
                              enum sdio_clock_e rate)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv =
    (FAR struct rp23xx_pio_sdio_dev_s *)dev;

  switch (rate)
    {
      case CLOCK_IDMODE:
        priv->clock_frequency = priv->config.idmode_frequency;
        break;
      case CLOCK_MMC_TRANSFER:
      case CLOCK_SD_TRANSFER_1BIT:
      case CLOCK_SD_TRANSFER_4BIT:
        priv->clock_frequency = priv->config.transfer_frequency;
        break;
      case CLOCK_SDIO_DISABLED:
      default:
        rp23xx_sdio_disable_sms(priv);
        break;
    }
}

static int rp23xx_sdio_attach(FAR struct sdio_dev_s *dev)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv =
    (FAR struct rp23xx_pio_sdio_dev_s *)dev;
  int ret;

  if (priv->attached)
    {
      return OK;
    }

  ret = rp23xx_gpio_irq_attach(priv->config.cd_pin,
                               RP23XX_GPIO_INTR_EDGE_BOTH,
                               rp23xx_sdio_cd_interrupt, priv);
  if (ret >= 0)
    {
      rp23xx_gpio_enable_irq(priv->config.cd_pin);
      priv->attached = true;
    }

  return ret;
}

static int rp23xx_sdio_sendcmd(FAR struct sdio_dev_s *dev,
                               uint32_t cmd, uint32_t arg)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv =
    (FAR struct rp23xx_pio_sdio_dev_s *)dev;
  int transfer_ret = OK;

  if (!rp23xx_sdio_present(priv))
    {
      return -ENODEV;
    }

  priv->last_cmd = cmd;
  priv->last_cmd_error = rp23xx_sdio_command(priv, cmd, arg);
  if (priv->last_cmd_error == OK &&
      (cmd & MMCSD_DATAXFR_MASK) != MMCSD_NODATAXFR)
    {
      transfer_ret = rp23xx_sdio_transfer(priv);
      if (transfer_ret == -ETIMEDOUT)
        {
          rp23xx_sdio_finish_event(priv, SDIOWAIT_TIMEOUT);
        }
      else if (transfer_ret < 0)
        {
          rp23xx_sdio_finish_event(priv, SDIOWAIT_ERROR);
        }
      else
        {
          rp23xx_sdio_finish_event(priv, SDIOWAIT_TRANSFERDONE);
        }
    }

  return priv->last_cmd_error;
}

#ifdef CONFIG_SDIO_BLOCKSETUP
static void rp23xx_sdio_blocksetup(FAR struct sdio_dev_s *dev,
                                   unsigned int blocklen,
                                   unsigned int nblocks)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv =
    (FAR struct rp23xx_pio_sdio_dev_s *)dev;

  priv->blocklen = blocklen;
  priv->nblocks = nblocks;
}
#endif

static int rp23xx_sdio_recvsetup(FAR struct sdio_dev_s *dev,
                                 FAR uint8_t *buffer, size_t nbytes)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv =
    (FAR struct rp23xx_pio_sdio_dev_s *)dev;

  if (buffer == NULL || nbytes == 0)
    {
      return -EINVAL;
    }

  priv->buffer = buffer;
  priv->buflen = nbytes;
  priv->transfer = RP23XX_SDIO_XFR_READ;
  return OK;
}

static int rp23xx_sdio_sendsetup(FAR struct sdio_dev_s *dev,
                                 FAR const uint8_t *buffer, size_t nbytes)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv =
    (FAR struct rp23xx_pio_sdio_dev_s *)dev;

  if (buffer == NULL || nbytes == 0)
    {
      return -EINVAL;
    }

  priv->buffer = (FAR uint8_t *)buffer;
  priv->buflen = nbytes;
  priv->transfer = RP23XX_SDIO_XFR_WRITE;
  return OK;
}

static int rp23xx_sdio_cancel(FAR struct sdio_dev_s *dev)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv =
    (FAR struct rp23xx_pio_sdio_dev_s *)dev;

  if (priv->initialized)
    {
      rp23xx_sdio_disable_sms(priv);
      rp23xx_dmastop(priv->rx_dma);
      rp23xx_dmastop(priv->tx_dma);
    }

  priv->buffer = NULL;
  priv->buflen = 0;
  priv->transfer = RP23XX_SDIO_XFR_NONE;
  nxsem_post(&priv->dmasem);
  return OK;
}

static int rp23xx_sdio_waitresponse(FAR struct sdio_dev_s *dev,
                                    uint32_t cmd)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv =
    (FAR struct rp23xx_pio_sdio_dev_s *)dev;

  return cmd == priv->last_cmd ? priv->last_cmd_error : -EINVAL;
}

static int rp23xx_sdio_recvshort(FAR struct sdio_dev_s *dev,
                                 uint32_t cmd, FAR uint32_t *response)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv =
    (FAR struct rp23xx_pio_sdio_dev_s *)dev;

  if (cmd != priv->last_cmd)
    {
      return -EINVAL;
    }

  if (priv->last_cmd_error < 0)
    {
      return priv->last_cmd_error;
    }

  if (response != NULL)
    {
      *response = priv->response[0];
    }

  return OK;
}

static int rp23xx_sdio_recvlong(FAR struct sdio_dev_s *dev,
                                uint32_t cmd, FAR uint32_t response[4])
{
  FAR struct rp23xx_pio_sdio_dev_s *priv =
    (FAR struct rp23xx_pio_sdio_dev_s *)dev;

  if (cmd != priv->last_cmd)
    {
      return -EINVAL;
    }

  if (priv->last_cmd_error < 0)
    {
      return priv->last_cmd_error;
    }

  if (response != NULL)
    {
      memcpy(response, priv->response, sizeof(priv->response));
    }

  return OK;
}

static void rp23xx_sdio_waitenable(FAR struct sdio_dev_s *dev,
                                   sdio_eventset_t eventset,
                                   uint32_t timeout)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv =
    (FAR struct rp23xx_pio_sdio_dev_s *)dev;
  irqstate_t flags;

  rp23xx_sdio_drainsem(&priv->waitsem);
  flags = enter_critical_section();
  priv->waitevents = eventset;
  priv->wkupevent = 0;
  priv->wait_timeout = timeout;
  leave_critical_section(flags);
}

static sdio_eventset_t rp23xx_sdio_eventwait(FAR struct sdio_dev_s *dev)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv =
    (FAR struct rp23xx_pio_sdio_dev_s *)dev;
  sdio_eventset_t event;
  irqstate_t flags;
  int ret = OK;

  if (priv->wkupevent == 0)
    {
      if ((priv->waitevents & SDIOWAIT_TIMEOUT) != 0)
        {
          ret = nxsem_tickwait_uninterruptible(
            &priv->waitsem, MSEC2TICK(priv->wait_timeout));
        }
      else
        {
          ret = nxsem_wait_uninterruptible(&priv->waitsem);
        }
    }

  flags = enter_critical_section();
  event = priv->wkupevent;
  priv->waitevents = 0;
  priv->wkupevent = 0;
  leave_critical_section(flags);

  if (ret < 0 || event == 0)
    {
      event = SDIOWAIT_TIMEOUT;
    }

  return event;
}

static void rp23xx_sdio_callbackenable(FAR struct sdio_dev_s *dev,
                                       sdio_eventset_t eventset)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv =
    (FAR struct rp23xx_pio_sdio_dev_s *)dev;

  priv->cbevents = eventset;
  rp23xx_sdio_schedule_callback(priv);
}

#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
static int rp23xx_sdio_registercallback(FAR struct sdio_dev_s *dev,
                                        worker_t callback, FAR void *arg)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv =
    (FAR struct rp23xx_pio_sdio_dev_s *)dev;

  priv->callback = callback;
  priv->cbarg = arg;
  return OK;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

FAR struct sdio_dev_s *
rp23xx_pio_sdio_initialize(
  unsigned int slotno,
  FAR const struct rp23xx_pio_sdio_config_s *config)
{
  FAR struct rp23xx_pio_sdio_dev_s *priv = &g_sdio;
  int ret;

  if (slotno != 0 || config == NULL || config->pio >= RP23XX_PIO_NUM ||
      config->clk_pin >= RP23XX_GPIO_NUM ||
      config->cmd_pin >= RP23XX_GPIO_NUM ||
      config->dat0_pin + 3 >= RP23XX_GPIO_NUM ||
      config->cd_pin >= RP23XX_GPIO_NUM ||
      config->idmode_frequency == 0 ||
      config->idmode_frequency > 400000 ||
      config->transfer_frequency == 0 ||
      config->transfer_frequency > 25000000)
    {
      set_errno(EINVAL);
      return NULL;
    }

  if (priv->initialized)
    {
      return &priv->dev;
    }

  memcpy(&priv->config, config, sizeof(*config));
  rp23xx_gpio_init(priv->config.cd_pin);
  rp23xx_gpio_setdir(priv->config.cd_pin, false);
  rp23xx_gpio_set_pulls(priv->config.cd_pin, true, false);
  priv->cdstatus = rp23xx_sdio_present(priv) ? SDIO_STATUS_PRESENT : 0;

  rp23xx_sdio_powerup_clocks(priv);
  ret = rp23xx_sdio_configure_pio(priv);
  if (ret < 0)
    {
      rp23xx_sdio_unload_programs(priv);
      if (priv->sm_cmd >= 0)
        {
          rp23xx_pio_sm_unclaim(priv->config.pio, priv->sm_cmd);
          priv->sm_cmd = -1;
        }

      if (priv->sm_aux >= 0)
        {
          rp23xx_pio_sm_unclaim(priv->config.pio, priv->sm_aux);
          priv->sm_aux = -1;
        }

      set_errno(-ret);
      return NULL;
    }

  priv->rx_dma = rp23xx_dmachannel();
  priv->tx_dma = rp23xx_dmachannel();
  priv->initialized = true;
  rp23xx_sdio_reset(&priv->dev);
  return &priv->dev;
}

#endif /* CONFIG_RP23XX_PIO_SDIO */
