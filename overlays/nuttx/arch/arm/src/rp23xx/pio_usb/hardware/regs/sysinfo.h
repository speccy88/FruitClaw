/****************************************************************************
 * arch/arm/src/rp23xx/pio_usb/hardware/regs/sysinfo.h
 ****************************************************************************/

#ifndef __ARCH_ARM_SRC_RP23XX_PIO_USB_HARDWARE_REGS_SYSINFO_H
#define __ARCH_ARM_SRC_RP23XX_PIO_USB_HARDWARE_REGS_SYSINFO_H

#include <stdint.h>

#include "../../../hardware/rp23xx_memorymap.h"
#include "../../../hardware/rp23xx_pads_bank0.h"

#define SYSINFO_BASE RP23XX_SYSINFO_BASE
#define SYSINFO_CHIP_ID_OFFSET 0x00000000
#define SYSINFO_CHIP_ID_REVISION_LSB 28
#define SYSINFO_CHIP_ID_REVISION_BITS (0x0f << SYSINFO_CHIP_ID_REVISION_LSB)

#define PADS_BANK0_GPIO0_IE_BITS RP23XX_PADS_BANK0_GPIO_IE

typedef struct
{
  volatile uint32_t voltage_select;
  volatile uint32_t io[48];
} pads_bank0_hw_t;

#define pads_bank0_hw ((pads_bank0_hw_t *)RP23XX_PADS_BANK0_BASE)

#define hw_clear_bits(addr, mask) \
  clrbits_reg32((mask), (uintptr_t)(addr))

#define hw_set_bits(addr, mask) \
  setbits_reg32((mask), (uintptr_t)(addr))

#endif
