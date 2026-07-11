/****************************************************************************
 * arch/arm/src/rp23xx/pio_usb/pico/stdlib.h
 ****************************************************************************/

#ifndef __ARCH_ARM_SRC_RP23XX_PIO_USB_PICO_STDLIB_H
#define __ARCH_ARM_SRC_RP23XX_PIO_USB_PICO_STDLIB_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include "../../hardware/rp23xx_timer.h"

#undef RP23XX_TIMER_BASE
#define RP23XX_TIMER_BASE RP23XX_TIMER0_BASE

#ifndef __time_critical_func
#  define __time_critical_func(x) x
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

typedef void alarm_pool_t;

typedef struct repeating_timer
{
  int unused;
} repeating_timer_t;

struct rp23xx_pio_usb_timer_hw
{
  volatile uint32_t timerawl;
};

#define timer_hw \
  ((volatile struct rp23xx_pio_usb_timer_hw *)RP23XX_TIMER_TIMERAWL)

static inline void tight_loop_contents(void)
{
  __asm__ volatile("nop");
}

static inline uint32_t get_time_us_32(void)
{
  return getreg32(RP23XX_TIMER_TIMERAWL);
}

static inline void busy_wait_us(uint32_t usec)
{
  uint32_t start = get_time_us_32();

  while ((uint32_t)(get_time_us_32() - start) < usec)
    {
      tight_loop_contents();
    }
}

static inline void busy_wait_ms(uint32_t msec)
{
  busy_wait_us(msec * 1000u);
}

static inline void busy_wait_at_least_cycles(uint32_t cycles)
{
  while (cycles-- > 0)
    {
      tight_loop_contents();
    }
}

static inline alarm_pool_t *alarm_pool_create(uint32_t alarm_num,
                                              uint32_t max_timers)
{
  (void)alarm_num;
  (void)max_timers;
  return NULL;
}

static inline bool alarm_pool_add_repeating_timer_us(alarm_pool_t *pool,
                                                     int64_t delay_us,
                                                     bool (*callback)
                                                       (repeating_timer_t *),
                                                     void *user_data,
                                                     repeating_timer_t *timer)
{
  (void)pool;
  (void)delay_us;
  (void)callback;
  (void)user_data;
  (void)timer;
  return false;
}

static inline bool cancel_repeating_timer(repeating_timer_t *timer)
{
  (void)timer;
  return true;
}

#endif
