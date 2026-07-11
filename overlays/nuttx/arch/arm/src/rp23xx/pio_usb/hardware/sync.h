/****************************************************************************
 * arch/arm/src/rp23xx/pio_usb/hardware/sync.h
 ****************************************************************************/

#ifndef __ARCH_ARM_SRC_RP23XX_PIO_USB_HARDWARE_SYNC_H
#define __ARCH_ARM_SRC_RP23XX_PIO_USB_HARDWARE_SYNC_H

#include <nuttx/config.h>

#include <nuttx/irq.h>

typedef irqstate_t critical_section_t;

static inline critical_section_t save_and_disable_interrupts(void)
{
  return enter_critical_section();
}

static inline void restore_interrupts(critical_section_t flags)
{
  leave_critical_section(flags);
}

#endif
