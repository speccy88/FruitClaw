/****************************************************************************
 * apps/system/userled/userled_main.c
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
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/leds/userled.h>

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void userled_usage(FAR const char *progname)
{
  fprintf(stderr, "Usage: %s [supported|on|off|mask <hex>|blink [count] "
          "[delay_ms]]\n", progname);
}

static int userled_open(FAR userled_set_t *supported)
{
  int fd;
  int ret;

  fd = open(CONFIG_SYSTEM_USERLED_DEVPATH, O_WRONLY);
  if (fd < 0)
    {
      fprintf(stderr, "ERROR: open %s failed: %d\n",
              CONFIG_SYSTEM_USERLED_DEVPATH, errno);
      return ERROR;
    }

  ret = ioctl(fd, ULEDIOC_SUPPORTED,
              (unsigned long)((uintptr_t)supported));
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: ULEDIOC_SUPPORTED failed: %d\n", errno);
      close(fd);
      return ERROR;
    }

  return fd;
}

static int userled_setall(int fd, userled_set_t ledset)
{
  int ret;

  ret = ioctl(fd, ULEDIOC_SETALL, (unsigned long)ledset);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: ULEDIOC_SETALL failed: %d\n", errno);
      return ERROR;
    }

  printf("userled: set 0x%08lx\n", (unsigned long)ledset);
  return OK;
}

static long userled_argtol(FAR const char *value, long minval, long maxval,
                           FAR const char *name)
{
  FAR char *endptr;
  long parsed;

  errno = 0;
  parsed = strtol(value, &endptr, 0);
  if (errno != 0 || endptr == value || *endptr != '\0' ||
      parsed < minval || parsed > maxval)
    {
      fprintf(stderr, "ERROR: invalid %s: %s\n", name, value);
      return LONG_MIN;
    }

  return parsed;
}

static int userled_blink(int fd, userled_set_t supported,
                         int count, useconds_t delay_us)
{
  int i;
  int ret;

  for (i = 0; i < count; i++)
    {
      ret = userled_setall(fd, supported);
      if (ret < 0)
        {
          return ret;
        }

      usleep(delay_us);

      ret = userled_setall(fd, 0);
      if (ret < 0)
        {
          return ret;
        }

      usleep(delay_us);
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * userled_main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  userled_set_t supported;
  userled_set_t ledset;
  long value;
  int count;
  useconds_t delay_us;
  int fd;
  int ret;

  if (argc < 2)
    {
      userled_usage(argv[0]);
      return EXIT_FAILURE;
    }

  fd = userled_open(&supported);
  if (fd < 0)
    {
      return EXIT_FAILURE;
    }

  printf("userled: supported 0x%08lx\n", (unsigned long)supported);

  if (strcmp(argv[1], "supported") == 0)
    {
      ret = OK;
    }
  else if (strcmp(argv[1], "on") == 0)
    {
      ret = userled_setall(fd, supported);
    }
  else if (strcmp(argv[1], "off") == 0)
    {
      ret = userled_setall(fd, 0);
    }
  else if (strcmp(argv[1], "mask") == 0)
    {
      if (argc < 3)
        {
          userled_usage(argv[0]);
          close(fd);
          return EXIT_FAILURE;
        }

      value = userled_argtol(argv[2], 0, INT32_MAX, "mask");
      if (value == LONG_MIN)
        {
          close(fd);
          return EXIT_FAILURE;
        }

      ledset = (userled_set_t)value & supported;
      ret = userled_setall(fd, ledset);
    }
  else if (strcmp(argv[1], "blink") == 0)
    {
      count = 4;
      delay_us = 250000;

      if (argc > 2)
        {
          value = userled_argtol(argv[2], 1, 1000, "count");
          if (value == LONG_MIN)
            {
              close(fd);
              return EXIT_FAILURE;
            }

          count = value;
        }

      if (argc > 3)
        {
          value = userled_argtol(argv[3], 1, 60000, "delay_ms");
          if (value == LONG_MIN)
            {
              close(fd);
              return EXIT_FAILURE;
            }

          delay_us = (useconds_t)value * 1000;
        }

      ret = userled_blink(fd, supported, count, delay_us);
    }
  else
    {
      userled_usage(argv[0]);
      ret = ERROR;
    }

  close(fd);
  return ret == OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
