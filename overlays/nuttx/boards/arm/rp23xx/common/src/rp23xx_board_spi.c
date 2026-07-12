/****************************************************************************
 * boards/arm/rp23xx/common/src/rp23xx_board_spi.c
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

/* The RP23xx controller and common-board sources are both named
 * rp23xx_spi.c.  The Make build puts both objects in libboard.a under the
 * same member name, which drops the board select/status callbacks.  Compile
 * the common-board implementation through a distinct object name.
 */

#include "rp23xx_spi.c"

/****************************************************************************
 * Public Functions
 ****************************************************************************/
