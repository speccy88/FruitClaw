/****************************************************************************
 * arch/arm/src/rp23xx/pio_usb/hardware/clocks.h
 ****************************************************************************/

#ifndef __ARCH_ARM_SRC_RP23XX_PIO_USB_HARDWARE_CLOCKS_H
#define __ARCH_ARM_SRC_RP23XX_PIO_USB_HARDWARE_CLOCKS_H

#include <nuttx/config.h>

#include <stdint.h>

#include <arch/board/board.h>

enum
{
  clk_sys = 0
};

static inline uint32_t clock_get_hz(int clk)
{
  (void)clk;
  return BOARD_SYS_FREQ;
}

#endif
