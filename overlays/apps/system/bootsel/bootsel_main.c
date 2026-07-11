/****************************************************************************
 * apps/system/bootsel/bootsel_main.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/boardctl.h>
#include <unistd.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * bootsel_main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int ret;

  printf("Rebooting to BOOTSEL...\n");
  fflush(stdout);
  usleep(100000);

  ret = boardctl(BOARDIOC_RESET, CONFIG_SYSTEM_BOOTSEL_RESET_STATUS);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: BOARDIOC_RESET failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}
