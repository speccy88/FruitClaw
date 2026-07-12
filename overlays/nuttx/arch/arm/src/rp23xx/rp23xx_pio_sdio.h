/****************************************************************************
 * arch/arm/src/rp23xx/rp23xx_pio_sdio.h
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

#ifndef __ARCH_ARM_SRC_RP23XX_RP23XX_PIO_SDIO_H
#define __ARCH_ARM_SRC_RP23XX_RP23XX_PIO_SDIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/sdio.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct rp23xx_pio_sdio_config_s
{
  uint8_t  pio;
  uint8_t  clk_pin;
  uint8_t  cmd_pin;
  uint8_t  dat0_pin;
  uint8_t  cd_pin;
  bool     cd_active_high;
  uint32_t idmode_frequency;
  uint32_t transfer_frequency;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__
#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: rp23xx_pio_sdio_initialize
 *
 * Description:
 *   Initialize one four-bit SD-memory slot backed by an RP2350 PIO block.
 *   DAT1 through DAT3 must immediately follow DAT0.  The returned lower-half
 *   is registered with mmcsd_slotinitialize() by board logic.
 *
 ****************************************************************************/

FAR struct sdio_dev_s *
rp23xx_pio_sdio_initialize(
  unsigned int slotno,
  FAR const struct rp23xx_pio_sdio_config_s *config);

#ifdef __cplusplus
}
#endif
#endif /* __ASSEMBLY__ */
#endif /* __ARCH_ARM_SRC_RP23XX_RP23XX_PIO_SDIO_H */
