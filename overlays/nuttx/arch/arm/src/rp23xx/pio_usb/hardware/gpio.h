/****************************************************************************
 * arch/arm/src/rp23xx/pio_usb/hardware/gpio.h
 ****************************************************************************/

#ifndef __ARCH_ARM_SRC_RP23XX_PIO_USB_HARDWARE_GPIO_H
#define __ARCH_ARM_SRC_RP23XX_PIO_USB_HARDWARE_GPIO_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include "../../rp23xx_gpio.h"
#include "../../hardware/rp23xx_io_bank0.h"
#include "../../hardware/rp23xx_pads_bank0.h"

#define GPIO_OVERRIDE_NORMAL  0
#define GPIO_OVERRIDE_INVERT  1
#define GPIO_OVERRIDE_LOW     2
#define GPIO_OVERRIDE_HIGH    3

#define GPIO_SLEW_RATE_FAST       1
#define GPIO_DRIVE_STRENGTH_12MA  RP23XX_PADS_BANK0_GPIO_DRIVE_12MA

static inline bool gpio_get(uint32_t gpio)
{
  return rp23xx_gpio_get(gpio);
}

static inline void gpio_pull_down(uint32_t gpio)
{
  rp23xx_gpio_set_pulls(gpio, false, true);
}

static inline void gpio_set_slew_rate(uint32_t gpio, int rate)
{
  rp23xx_gpio_set_slew_fast(gpio, rate == GPIO_SLEW_RATE_FAST);
}

static inline void gpio_set_drive_strength(uint32_t gpio, uint32_t strength)
{
  rp23xx_gpio_set_drive_strength(gpio, strength);
}

static inline void gpio_set_inover(uint32_t gpio, uint32_t value)
{
  modbits_reg32(value << RP23XX_IO_BANK0_GPIO_CTRL_INOVER_SHIFT,
                0x03 << RP23XX_IO_BANK0_GPIO_CTRL_INOVER_SHIFT,
                RP23XX_IO_BANK0_GPIO_CTRL(gpio));
}

static inline void gpio_set_outover(uint32_t gpio, uint32_t value)
{
  modbits_reg32(value << RP23XX_IO_BANK0_GPIO_CTRL_OUTOVER_SHIFT,
                0x03 << RP23XX_IO_BANK0_GPIO_CTRL_OUTOVER_SHIFT,
                RP23XX_IO_BANK0_GPIO_CTRL(gpio));
}

static inline void gpio_set_oeover(uint32_t gpio, uint32_t value)
{
  modbits_reg32(value << RP23XX_IO_BANK0_GPIO_CTRL_OEOVER_SHIFT,
                0x03 << RP23XX_IO_BANK0_GPIO_CTRL_OEOVER_SHIFT,
                RP23XX_IO_BANK0_GPIO_CTRL(gpio));
}

#endif
