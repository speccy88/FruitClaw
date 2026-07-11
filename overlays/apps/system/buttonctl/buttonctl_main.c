/****************************************************************************
 * apps/system/buttonctl/buttonctl_main.c
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

#include <nuttx/input/buttons.h>

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void buttonctl_usage(FAR const char *progname)
{
  fprintf(stderr, "Usage: %s [supported|state|watch [count] [delay_ms]]\n",
          progname);
}

static long buttonctl_argtol(FAR const char *value, long minval, long maxval,
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

static int buttonctl_open(FAR btn_buttonset_t *supported)
{
  int fd;
  int ret;

  fd = open(CONFIG_SYSTEM_BUTTONCTL_DEVPATH, O_RDONLY);
  if (fd < 0)
    {
      fprintf(stderr, "ERROR: open %s failed: %d\n",
              CONFIG_SYSTEM_BUTTONCTL_DEVPATH, errno);
      return ERROR;
    }

  ret = ioctl(fd, BTNIOC_SUPPORTED,
              (unsigned long)((uintptr_t)supported));
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: BTNIOC_SUPPORTED failed: %d\n", errno);
      close(fd);
      return ERROR;
    }

  return fd;
}

static int buttonctl_read(int fd, FAR btn_buttonset_t *state)
{
  ssize_t nread;

  nread = read(fd, state, sizeof(*state));
  if (nread < 0)
    {
      fprintf(stderr, "ERROR: read failed: %d\n", errno);
      return ERROR;
    }

  if (nread != sizeof(*state))
    {
      fprintf(stderr, "ERROR: short read: %ld\n", (long)nread);
      return ERROR;
    }

  return OK;
}

static void buttonctl_print_state(btn_buttonset_t supported,
                                  btn_buttonset_t state)
{
  static FAR const char * const names[] =
  {
    "BUTTON1",
    "BUTTON2",
    "BUTTON3"
  };

  int i;

  printf("buttonctl: supported 0x%08lx state 0x%08lx\n",
         (unsigned long)supported, (unsigned long)state);

  for (i = 0; i < 3; i++)
    {
      if ((supported & (1 << i)) != 0)
        {
          printf("buttonctl: %s %s\n", names[i],
                 (state & (1 << i)) != 0 ? "pressed" : "released");
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * buttonctl_main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  btn_buttonset_t supported;
  btn_buttonset_t state;
  const char *command;
  btn_buttonset_t last_state;
  long value;
  int count;
  int delay_ms;
  int fd;
  int ret;
  int i;
  bool have_last;

  command = argc > 1 ? argv[1] : "state";

  fd = buttonctl_open(&supported);
  if (fd < 0)
    {
      return EXIT_FAILURE;
    }

  if (strcmp(command, "supported") == 0)
    {
      printf("buttonctl: supported 0x%08lx\n", (unsigned long)supported);
      close(fd);
      return EXIT_SUCCESS;
    }

  if (strcmp(command, "state") == 0)
    {
      ret = buttonctl_read(fd, &state);
      if (ret == OK)
        {
          buttonctl_print_state(supported, state);
        }

      close(fd);
      return ret == OK ? EXIT_SUCCESS : EXIT_FAILURE;
    }

  if (strcmp(command, "watch") == 0)
    {
      count = 20;
      delay_ms = 100;

      if (argc > 2)
        {
          value = buttonctl_argtol(argv[2], 1, 1000000, "count");
          if (value == LONG_MIN)
            {
              close(fd);
              return EXIT_FAILURE;
            }

          count = value;
        }

      if (argc > 3)
        {
          value = buttonctl_argtol(argv[3], 1, 60000, "delay_ms");
          if (value == LONG_MIN)
            {
              close(fd);
              return EXIT_FAILURE;
            }

          delay_ms = value;
        }

      ret = OK;
      have_last = false;
      last_state = 0;

      for (i = 0; i < count; i++)
        {
          ret = buttonctl_read(fd, &state);
          if (ret < 0)
            {
              break;
            }

          if (!have_last || state != last_state)
            {
              buttonctl_print_state(supported, state);
              last_state = state;
              have_last = true;
            }

          usleep((useconds_t)delay_ms * 1000);
        }

      close(fd);
      return ret == OK ? EXIT_SUCCESS : EXIT_FAILURE;
    }

  buttonctl_usage(argv[0]);
  close(fd);
  return EXIT_FAILURE;
}
