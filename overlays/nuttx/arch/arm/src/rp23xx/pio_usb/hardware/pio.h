/****************************************************************************
 * arch/arm/src/rp23xx/pio_usb/hardware/pio.h
 *
 * Compatibility shim for Pico-PIO-USB on NuttX RP23XX.
 *
 ****************************************************************************/

#ifndef __ARCH_ARM_SRC_RP23XX_PIO_USB_HARDWARE_PIO_H
#define __ARCH_ARM_SRC_RP23XX_PIO_USB_HARDWARE_PIO_H

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#include "../../rp23xx_pio.h"
#include "../../rp23xx_pio_instructions.h"
#include "../../hardware/rp23xx_memorymap.h"
#include "gpio.h"

#ifndef PICO_NO_HARDWARE
#  define PICO_NO_HARDWARE 0
#endif

#ifndef PICO_SDK_VERSION_MAJOR
#  define PICO_SDK_VERSION_MAJOR 2
#endif

#ifndef PICO_SDK_VERSION_MINOR
#  define PICO_SDK_VERSION_MINOR 1
#endif

#ifndef PICO_RP2350
#  define PICO_RP2350 1
#endif

#ifndef __unused
#  define __unused __attribute__((unused))
#endif

#ifndef __always_inline
#  define __always_inline inline __attribute__((always_inline))
#endif

#ifndef __force_inline
#  define __force_inline inline __attribute__((always_inline))
#endif

#ifndef __no_inline_not_in_flash_func
#  define __no_inline_not_in_flash_func(x) x
#endif

#ifndef __not_in_flash_func
#  define __not_in_flash_func(x) x
#endif

#ifndef __not_in_flash
#  define __not_in_flash(x)
#endif

typedef volatile uint32_t io_rw_32;
typedef volatile uint32_t io_ro_32;
typedef volatile uint32_t io_wo_32;

typedef struct pio_sm_hw
{
  io_rw_32 clkdiv;
  io_rw_32 execctrl;
  io_rw_32 shiftctrl;
  io_ro_32 addr;
  io_wo_32 instr;
  io_rw_32 pinctrl;
} pio_sm_hw_t;

typedef struct pio_hw
{
  io_rw_32 ctrl;
  io_ro_32 fstat;
  io_rw_32 fdebug;
  io_ro_32 flevel;
  io_wo_32 txf[4];
  io_ro_32 rxf[4];
  io_rw_32 irq;
  io_rw_32 irq_force;
  io_rw_32 input_sync_bypass;
  io_ro_32 dbg_padout;
  io_ro_32 dbg_padoe;
  io_ro_32 dbg_cfginfo;
  io_rw_32 instr_mem[32];
  pio_sm_hw_t sm[4];
  io_rw_32 rxf_putget[16];
  io_rw_32 gpiobase;
  io_rw_32 intr;
  io_rw_32 irq0_inte;
  io_rw_32 irq0_intf;
  io_ro_32 irq0_ints;
  io_rw_32 irq1_inte;
  io_rw_32 irq1_intf;
  io_ro_32 irq1_ints;
} pio_hw_t;

typedef pio_hw_t *PIO;
typedef rp23xx_pio_sm_config pio_sm_config;
#define pio_program rp23xx_pio_program
typedef rp23xx_pio_program_t pio_program_t;

#define PIO0_BASE RP23XX_PIO0_BASE
#define PIO1_BASE RP23XX_PIO1_BASE
#define PIO2_BASE RP23XX_PIO2_BASE

#define PIO_FIFO_JOIN_NONE RP23XX_PIO_FIFO_JOIN_NONE
#define PIO_FIFO_JOIN_TX   RP23XX_PIO_FIFO_JOIN_TX
#define PIO_FIFO_JOIN_RX   RP23XX_PIO_FIFO_JOIN_RX

#define PIO_FDEBUG_TXSTALL_LSB RP23XX_PIO_FDEBUG_TXSTALL_SHIFT

#define PIO_INSTANCE(instance) ((pio_hw_t *)RP23XX_PIO_BASE(instance))

static inline uint32_t pio_to_index(PIO pio)
{
  return ((uintptr_t)pio - RP23XX_PIO0_BASE) >> 20;
}

static inline PIO pio_get_instance(uint32_t instance)
{
  return PIO_INSTANCE(instance);
}

static inline pio_sm_config pio_get_default_sm_config(void)
{
  return rp23xx_pio_get_default_sm_config();
}

static inline void sm_config_set_wrap(pio_sm_config *c, uint32_t target,
                                      uint32_t wrap)
{
  rp23xx_sm_config_set_wrap(c, target, wrap);
}

static inline void sm_config_set_sideset(pio_sm_config *c, uint32_t bit_count,
                                         bool optional, bool pindirs)
{
  rp23xx_sm_config_set_sideset(c, bit_count, optional, pindirs);
}

static inline void sm_config_set_sideset_pins(pio_sm_config *c,
                                              uint32_t base)
{
  rp23xx_sm_config_set_sideset_pins(c, base);
}

static inline void sm_config_set_out_shift(pio_sm_config *c,
                                           bool shift_right, bool autopull,
                                           uint32_t pull_threshold)
{
  rp23xx_sm_config_set_out_shift(c, shift_right, autopull, pull_threshold);
}

static inline void sm_config_set_in_shift(pio_sm_config *c,
                                          bool shift_right, bool autopush,
                                          uint32_t push_threshold)
{
  rp23xx_sm_config_set_in_shift(c, shift_right, autopush, push_threshold);
}

static inline void sm_config_set_fifo_join(pio_sm_config *c, int join)
{
  rp23xx_sm_config_set_fifo_join(c, join);
}

static inline void sm_config_set_clkdiv(pio_sm_config *c, float div)
{
  rp23xx_sm_config_set_clkdiv(c, div);
}

static inline void sm_config_set_in_pins(pio_sm_config *c, uint32_t base)
{
  rp23xx_sm_config_set_in_pins(c, base);
}

static inline void sm_config_set_jmp_pin(pio_sm_config *c, uint32_t pin)
{
  rp23xx_sm_config_set_jmp_pin(c, pin);
}

static inline void pio_sm_init(PIO pio, uint32_t sm, uint32_t initial_pc,
                               const pio_sm_config *config)
{
  rp23xx_pio_sm_init(pio_to_index(pio), sm, initial_pc, config);
}

static inline void pio_sm_claim(PIO pio, uint32_t sm)
{
  rp23xx_pio_sm_claim(pio_to_index(pio), sm);
}

static inline void pio_calculate_clkdiv_from_float(float div,
                                                   uint16_t *div_int,
                                                   uint8_t *div_frac)
{
  uint32_t div256;

  if (div < 1.0f)
    {
      div = 1.0f;
    }

  div256 = (uint32_t)(div * 256.0f + 0.5f);
  if (div256 < 256)
    {
      div256 = 256;
    }

  *div_int = div256 >> 8;
  *div_frac = div256 & 0xff;
}

static inline void pio_sm_set_enabled(PIO pio, uint32_t sm, bool enabled)
{
  rp23xx_pio_sm_set_enabled(pio_to_index(pio), sm, enabled);
}

static inline void pio_sm_restart(PIO pio, uint32_t sm)
{
  rp23xx_pio_sm_restart(pio_to_index(pio), sm);
}

static inline void pio_sm_clear_fifos(PIO pio, uint32_t sm)
{
  rp23xx_pio_sm_clear_fifos(pio_to_index(pio), sm);
}

static inline void pio_sm_exec(PIO pio, uint32_t sm, uint32_t instr)
{
  rp23xx_pio_sm_exec(pio_to_index(pio), sm, instr);
}

static inline uint32_t pio_sm_get(PIO pio, uint32_t sm)
{
  return rp23xx_pio_sm_get(pio_to_index(pio), sm);
}

static inline uint32_t pio_sm_get_rx_fifo_level(PIO pio, uint32_t sm)
{
  return rp23xx_pio_sm_get_rx_fifo_level(pio_to_index(pio), sm);
}

static inline void pio_sm_set_clkdiv_int_frac(PIO pio, uint32_t sm,
                                              uint16_t div_int,
                                              uint8_t div_frac)
{
  rp23xx_pio_sm_set_clkdiv_int_frac(pio_to_index(pio), sm, div_int,
                                    div_frac);
}

static inline void pio_sm_set_jmp_pin(PIO pio, uint32_t sm,
                                      uint32_t jmp_pin)
{
  pio->sm[sm].execctrl =
    (pio->sm[sm].execctrl & ~RP23XX_PIO_SM_EXECCTRL_JMP_PIN_MASK) |
    (jmp_pin << RP23XX_PIO_SM_EXECCTRL_JMP_PIN_SHIFT);
}

static inline void pio_sm_set_in_pins(PIO pio, uint32_t sm, uint32_t base)
{
  rp23xx_pio_sm_set_in_pins(pio_to_index(pio), sm, base);
}

static inline void pio_sm_set_out_pins(PIO pio, uint32_t sm, uint32_t base,
                                       uint32_t count)
{
  rp23xx_pio_sm_set_out_pins(pio_to_index(pio), sm, base, count);
}

static inline void pio_sm_set_set_pins(PIO pio, uint32_t sm, uint32_t base,
                                       uint32_t count)
{
  rp23xx_pio_sm_set_set_pins(pio_to_index(pio), sm, base, count);
}

static inline void pio_sm_set_sideset_pins(PIO pio, uint32_t sm,
                                           uint32_t base)
{
  rp23xx_pio_sm_set_sideset_pins(pio_to_index(pio), sm, base);
}

static inline void pio_sm_set_consecutive_pindirs(PIO pio, uint32_t sm,
                                                  uint32_t base,
                                                  uint32_t count,
                                                  bool is_out)
{
  uint32_t mask = ((1u << count) - 1u) << base;
  rp23xx_pio_sm_set_pindirs_with_mask(pio_to_index(pio), sm,
                                      is_out ? mask : 0, mask);
}

static inline void pio_sm_set_pins_with_mask(PIO pio, uint32_t sm,
                                             uint32_t pin_values,
                                             uint32_t pin_mask)
{
  rp23xx_pio_sm_set_pins_with_mask(pio_to_index(pio), sm, pin_values,
                                   pin_mask);
}

static inline void pio_sm_set_pins_with_mask64(PIO pio, uint32_t sm,
                                               uint64_t pin_values,
                                               uint64_t pin_mask)
{
  pio_sm_set_pins_with_mask(pio, sm, (uint32_t)pin_values,
                            (uint32_t)pin_mask);
}

static inline void pio_sm_set_pindirs_with_mask(PIO pio, uint32_t sm,
                                                uint32_t pin_values,
                                                uint32_t pin_mask)
{
  rp23xx_pio_sm_set_pindirs_with_mask(pio_to_index(pio), sm, pin_values,
                                      pin_mask);
}

static inline void pio_sm_set_pindirs_with_mask64(PIO pio, uint32_t sm,
                                                  uint64_t pin_values,
                                                  uint64_t pin_mask)
{
  pio_sm_set_pindirs_with_mask(pio, sm, (uint32_t)pin_values,
                               (uint32_t)pin_mask);
}

static inline void pio_gpio_init(PIO pio, uint32_t pin)
{
  rp23xx_pio_gpio_init(pio_to_index(pio), pin);
}

static inline uint32_t pio_add_program(PIO pio,
                                       const pio_program_t *program)
{
  return rp23xx_pio_add_program(pio_to_index(pio), program);
}

static inline void pio_add_program_at_offset(PIO pio,
                                             const pio_program_t *program,
                                             uint32_t offset)
{
  rp23xx_pio_add_program_at_offset(pio_to_index(pio), program, offset);
}

static inline uint32_t pio_get_dreq(PIO pio, uint32_t sm, bool is_tx)
{
  return rp23xx_pio_get_dreq(pio_to_index(pio), sm, is_tx);
}

static inline void pio_set_gpio_base(PIO pio, uint32_t base)
{
  rp23xx_pio_set_gpio_base(pio_to_index(pio), base);
}

#endif /* __ARCH_ARM_SRC_RP23XX_PIO_USB_HARDWARE_PIO_H */
