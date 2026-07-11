/****************************************************************************
 * arch/arm/src/rp23xx/pio_usb/hardware/dma.h
 ****************************************************************************/

#ifndef __ARCH_ARM_SRC_RP23XX_PIO_USB_HARDWARE_DMA_H
#define __ARCH_ARM_SRC_RP23XX_PIO_USB_HARDWARE_DMA_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "../../rp23xx_dmac.h"

typedef struct
{
  uint8_t dreq;
  uint8_t size;
  bool read_increment;
  bool write_increment;
} dma_channel_config;

#define DMA_SIZE_8 RP23XX_DMA_SIZE_BYTE

static DMA_HANDLE g_pio_usb_dma_handle;
static dma_config_t g_pio_usb_dma_config =
{
  .dreq   = 0,
  .size   = RP23XX_DMA_SIZE_BYTE,
  .noincr = 0,
};
static uintptr_t g_pio_usb_dma_write_addr;

static inline dma_channel_config dma_channel_get_default_config(uint8_t ch)
{
  dma_channel_config config =
  {
    .dreq            = 0,
    .size            = RP23XX_DMA_SIZE_BYTE,
    .read_increment  = false,
    .write_increment = false,
  };

  (void)ch;
  return config;
}

static inline void channel_config_set_read_increment(
                              dma_channel_config *config, bool increment)
{
  config->read_increment = increment;
  g_pio_usb_dma_config.noincr = increment ? 0 : 1;
}

static inline void channel_config_set_write_increment(
                              dma_channel_config *config, bool increment)
{
  config->write_increment = increment;
}

static inline void channel_config_set_transfer_data_size(
                              dma_channel_config *config, uint8_t size)
{
  config->size = size;
  g_pio_usb_dma_config.size = size;
}

static inline void channel_config_set_dreq(dma_channel_config *config,
                                           uint8_t dreq)
{
  config->dreq = dreq;
  g_pio_usb_dma_config.dreq = dreq;
}

static inline void dma_claim_mask(uint32_t mask)
{
  (void)mask;

  if (g_pio_usb_dma_handle == NULL)
    {
      g_pio_usb_dma_handle = rp23xx_dmachannel();
    }
}

static inline void dma_channel_set_config(uint8_t ch,
                                          const dma_channel_config *config,
                                          bool trigger)
{
  (void)ch;
  (void)trigger;

  g_pio_usb_dma_config.dreq = config->dreq;
  g_pio_usb_dma_config.size = config->size;
  g_pio_usb_dma_config.noincr = config->read_increment ? 0 : 1;
}

static inline void dma_channel_set_write_addr(uint8_t ch, volatile void *addr,
                                              bool trigger)
{
  (void)ch;
  (void)trigger;
  g_pio_usb_dma_write_addr = (uintptr_t)addr;
}

static inline void dma_channel_transfer_from_buffer_now(uint8_t ch,
                                                        const void *buffer,
                                                        size_t len)
{
  (void)ch;

  if (g_pio_usb_dma_handle == NULL)
    {
      g_pio_usb_dma_handle = rp23xx_dmachannel();
    }

  rp23xx_txdmasetup(g_pio_usb_dma_handle, g_pio_usb_dma_write_addr,
                    (uintptr_t)buffer, len, g_pio_usb_dma_config);
  rp23xx_dmastart(g_pio_usb_dma_handle, NULL, NULL);
}

#endif
