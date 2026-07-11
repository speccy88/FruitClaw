/****************************************************************************
 * boards/arm/rp23xx/raspberrypi-pico-2-w/include/rp23xx_extra_gpio.h
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

#ifndef __BOARDS_ARM_RP23XX_RASPBERRYPI_PICO_2_W_INCLUDE_RP23XX_EXTRA_GPIO_H
#define __BOARDS_ARM_RP23XX_RASPBERRYPI_PICO_2_W_INCLUDE_RP23XX_EXTRA_GPIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/wireless/ieee80211/bcmf_gpio.h>
#include <nuttx/wireless/ieee80211/bcmf_gspi.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* These are GPIOs on the CYW43439, not pins on the RP2350 package. */

#define RP23XX_EXTRA_GPIO_LED   0
#define RP23XX_EXTRA_GPIO_NUM   3

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifndef __ASSEMBLY__

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

extern FAR gspi_dev_t *g_cyw43439;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rp23xx_extra_gpio_put
 ****************************************************************************/

static inline int rp23xx_extra_gpio_put(uint32_t gpio, bool value)
{
  if (gpio >= RP23XX_EXTRA_GPIO_NUM)
    {
      return -EINVAL;
    }

  if (g_cyw43439 == NULL || g_cyw43439->priv == NULL)
    {
      return -ENODEV;
    }

  return bcmf_set_gpio(g_cyw43439->priv, gpio, value);
}

/****************************************************************************
 * Name: rp23xx_extra_gpio_get
 ****************************************************************************/

static inline int rp23xx_extra_gpio_get(uint32_t gpio, FAR bool *value)
{
  if (gpio >= RP23XX_EXTRA_GPIO_NUM || value == NULL)
    {
      return -EINVAL;
    }

  if (g_cyw43439 == NULL || g_cyw43439->priv == NULL)
    {
      return -ENODEV;
    }

  return bcmf_get_gpio(g_cyw43439->priv, gpio, value);
}

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* __BOARDS_ARM_RP23XX_RASPBERRYPI_PICO_2_W_INCLUDE_RP23XX_EXTRA_GPIO_H */
