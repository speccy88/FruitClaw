/****************************************************************************
 * apps/system/irdump/irdump_main.c
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
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <nuttx/lirc.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_SYSTEM_IRDUMP_DEVPATH
#  define CONFIG_SYSTEM_IRDUMP_DEVPATH "/dev/lirc0"
#endif

#define IRDUMP_DEFAULT_MAX_SAMPLES     256
#define IRDUMP_DEFAULT_QUIET_MS        120
#define IRDUMP_DEFAULT_FIRST_MS        15000
#define IRDUMP_MAX_SAMPLES_LIMIT       2048
#define IRDUMP_LEADING_IDLE_US         20000
#define IRDUMP_RC6_UNIT_US             444
#define IRDUMP_RC6_PRE_DATA            0x1bff80
#define IRDUMP_RC6_PRE_TOGGLE_MASK     0x000004
#define IRDUMP_RC6_CODE_BITS           13
#define IRDUMP_RC6_TOTAL_BITS          37
#define IRDUMP_RC6_TRAILER_BIT         4
#define IRDUMP_RC6_MAX_CELLS           128

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct irdump_keycode_s
{
  uint16_t code;
  FAR const char *key;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void irdump_usage(FAR const char *progname)
{
  fprintf(stderr,
          "Usage: %s [-d dev] [-n samples] [-q quiet_ms] [-t first_ms]\n"
          "\n"
          "Defaults: dev=%s samples=%d quiet_ms=%d first_ms=%d\n",
          progname, CONFIG_SYSTEM_IRDUMP_DEVPATH,
          IRDUMP_DEFAULT_MAX_SAMPLES, IRDUMP_DEFAULT_QUIET_MS,
          IRDUMP_DEFAULT_FIRST_MS);
}

static long irdump_argtol(FAR const char *value, long minval, long maxval,
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
      return -1;
    }

  return parsed;
}

static FAR const char *irdump_sample_type(uint32_t sample)
{
  if (LIRC_IS_PULSE(sample))
    {
      return "pulse";
    }
  else if (LIRC_IS_SPACE(sample))
    {
      return "space";
    }
  else if (LIRC_IS_TIMEOUT(sample))
    {
      return "timeout";
    }
  else if (LIRC_IS_FREQUENCY(sample))
    {
      return "freq";
    }

  return "unknown";
}

static uint8_t irdump_reverse8(uint8_t value)
{
  value = (value & 0xf0) >> 4 | (value & 0x0f) << 4;
  value = (value & 0xcc) >> 2 | (value & 0x33) << 2;
  value = (value & 0xaa) >> 1 | (value & 0x55) << 1;
  return value;
}

static bool irdump_in_range(uint32_t value, uint32_t minval, uint32_t maxval)
{
  return value >= minval && value <= maxval;
}

static FAR const char *irdump_apple_a1156_key(uint8_t command)
{
  switch (command)
    {
      case 0x10:
        return "KEY_LEFT";

      case 0x20:
        return "KEY_PLAY";

      case 0x30:
        return "KEY_DOWN";

      case 0x40:
        return "KEY_MENU";

      case 0x50:
        return "KEY_UP";

      case 0x60:
        return "KEY_RIGHT";

      case 0x90:
        return "KEY_LEFT";

      case 0xa0:
        return "KEY_OK";

      case 0xb0:
        return "KEY_DOWN";

      case 0xc0:
        return "KEY_MENU";

      case 0xd0:
        return "KEY_UP";

      case 0xe0:
        return "KEY_RIGHT";

      default:
        return NULL;
    }
}

static FAR const char *irdump_xbox360_key(uint16_t code)
{
  static const struct irdump_keycode_s g_xbox360_keys[] =
  {
    { 0x0bd7, "KEY_EJECTCD" },
    { 0x0b9b, "KEY_VENDOR" },
    { 0x0bf3, "KEY_POWER" },
    { 0x0be6, "KEY_STOP" },
    { 0x0be7, "KEY_PAUSE" },
    { 0x0bea, "KEY_REWIND" },
    { 0x0beb, "KEY_FASTFORWARD" },
    { 0x0be4, "KEY_PREVIOUS" },
    { 0x0be5, "KEY_NEXT" },
    { 0x0be9, "KEY_PLAY" },
    { 0x0bb0, "KEY_ZOOM" },
    { 0x0bae, "KEY_TITLE" },
    { 0x0bdb, "KEY_MENU" },
    { 0x0bdc, "KEY_BACK" },
    { 0x0bf0, "KEY_INFO" },
    { 0x0be1, "KEY_UP" },
    { 0x0be0, "KEY_DOWN" },
    { 0x0bdf, "KEY_LEFT" },
    { 0x0bde, "KEY_RIGHT" },
    { 0x0bdd, "KEY_OK" },
    { 0x0bd9, "KEY_YELLOW" },
    { 0x0b97, "KEY_BLUE" },
    { 0x0b99, "KEY_GREEN" },
    { 0x0bda, "KEY_RED" },
    { 0x0bef, "KEY_VOLUMEUP" },
    { 0x0bee, "KEY_VOLUMEDOWN" },
    { 0x0b93, "KEY_CHANNELUP" },
    { 0x0b92, "KEY_CHANNELDOWN" },
    { 0x0bf1, "KEY_MUTE" },
    { 0x0bf2, "KEY_HOME" },
    { 0x0bf4, "KEY_ENTER" },
    { 0x0bf5, "KEY_DELETE" },
    { 0x0be8, "KEY_RECORD" },
    { 0x0bfe, "KEY_NUMERIC_1" },
    { 0x0bfd, "KEY_NUMERIC_2" },
    { 0x0bfc, "KEY_NUMERIC_3" },
    { 0x0bfb, "KEY_NUMERIC_4" },
    { 0x0bfa, "KEY_NUMERIC_5" },
    { 0x0bf9, "KEY_NUMERIC_6" },
    { 0x0bf8, "KEY_NUMERIC_7" },
    { 0x0bf7, "KEY_NUMERIC_8" },
    { 0x0bf6, "KEY_NUMERIC_9" },
    { 0x0be2, "KEY_NUMERIC_STAR" },
    { 0x0bff, "KEY_NUMERIC_0" },
    { 0x0be3, "KEY_NUMERIC_POUND" }
  };

  size_t i;

  code &= (1 << IRDUMP_RC6_CODE_BITS) - 1;
  for (i = 0; i < sizeof(g_xbox360_keys) / sizeof(g_xbox360_keys[0]); i++)
    {
      if (g_xbox360_keys[i].code == code)
        {
          return g_xbox360_keys[i].key;
        }
    }

  return NULL;
}

static bool irdump_decode_rc6_cells(FAR const uint8_t *cells, int ncells,
                                    int offset, bool one_mark_space,
                                    FAR uint64_t *decoded)
{
  uint64_t value = 0;
  int pos = offset;
  int bit;

  for (bit = 0; bit < IRDUMP_RC6_TOTAL_BITS; bit++)
    {
      bool first_mark;
      bool decoded_bit;

      if (bit == IRDUMP_RC6_TRAILER_BIT)
        {
          if (pos + 4 > ncells ||
              cells[pos] != cells[pos + 1] ||
              cells[pos + 2] != cells[pos + 3] ||
              cells[pos] == cells[pos + 2])
            {
              return false;
            }

          first_mark = cells[pos] != 0;
          pos += 4;
        }
      else
        {
          if (pos + 2 > ncells || cells[pos] == cells[pos + 1])
            {
              return false;
            }

          first_mark = cells[pos] != 0;
          pos += 2;
        }

      decoded_bit = one_mark_space ? first_mark : !first_mark;
      value = (value << 1) | decoded_bit;
    }

  *decoded = value;
  return true;
}

static void irdump_decode_rc6(FAR const uint32_t *samples, int count)
{
  uint8_t cells[IRDUMP_RC6_MAX_CELLS];
  uint32_t pulse;
  uint32_t space;
  int ncells = 0;
  int offset;
  int i;

  if (count < 10 || !LIRC_IS_PULSE(samples[0]) ||
      !LIRC_IS_SPACE(samples[1]))
    {
      return;
    }

  pulse = LIRC_VALUE(samples[0]);
  space = LIRC_VALUE(samples[1]);
  if (!irdump_in_range(pulse, 2200, 3200) ||
      !irdump_in_range(space, 650, 1200))
    {
      return;
    }

  for (i = 2; i < count && ncells < IRDUMP_RC6_MAX_CELLS; i++)
    {
      uint32_t duration = LIRC_VALUE(samples[i]);
      uint8_t level = LIRC_IS_PULSE(samples[i]) ? 1 : 0;
      int units;
      int j;

      if (duration > IRDUMP_LEADING_IDLE_US)
        {
          break;
        }

      units = (duration + IRDUMP_RC6_UNIT_US / 2) / IRDUMP_RC6_UNIT_US;
      if (units < 1)
        {
          units = 1;
        }

      for (j = 0; j < units && ncells < IRDUMP_RC6_MAX_CELLS; j++)
        {
          cells[ncells++] = level;
        }
    }

  for (offset = 0; offset < 4; offset++)
    {
      uint64_t value;
      uint32_t predata;
      uint16_t code;
      FAR const char *key;
      int polarity;

      for (polarity = 0; polarity < 2; polarity++)
        {
          if (!irdump_decode_rc6_cells(cells, ncells, offset,
                                       polarity != 0, &value))
            {
              continue;
            }

          predata = value >> IRDUMP_RC6_CODE_BITS;
          if ((predata & ~IRDUMP_RC6_PRE_TOGGLE_MASK) != IRDUMP_RC6_PRE_DATA)
            {
              continue;
            }

          code = value & ((1 << IRDUMP_RC6_CODE_BITS) - 1);
          key = irdump_xbox360_key(code);

          printf("decoded RC6 pre_data=0x%06" PRIx32
                 " toggle=%" PRIu32
                 " code=0x%04" PRIx16,
                 predata,
                 (uint32_t)((predata & IRDUMP_RC6_PRE_TOGGLE_MASK) ? 1 : 0),
                 code);

          if (key != NULL)
            {
              printf(" key=%s", key);
            }

          printf("\n");

          if (key != NULL)
            {
              printf("decoded Xbox_360 command=0x%04" PRIx16 " key=%s\n",
                     code, key);
            }

          return;
        }
    }
}

static void irdump_decode_nec(FAR const uint32_t *samples, int count)
{
  uint8_t bytes_lsb[4];
  uint8_t bytes_msb[4];
  uint32_t code;
  uint32_t pulse;
  uint32_t space;
  int bit;

  if (count < 67)
    {
      return;
    }

  if (!LIRC_IS_PULSE(samples[0]) || !LIRC_IS_SPACE(samples[1]))
    {
      return;
    }

  pulse = LIRC_VALUE(samples[0]);
  space = LIRC_VALUE(samples[1]);
  if (!irdump_in_range(pulse, 7000, 11000) ||
      !irdump_in_range(space, 3500, 5500))
    {
      return;
    }

  memset(bytes_lsb, 0, sizeof(bytes_lsb));

  for (bit = 0; bit < 32; bit++)
    {
      uint32_t bit_pulse = samples[2 + bit * 2];
      uint32_t bit_space = samples[3 + bit * 2];
      bool one;

      if (!LIRC_IS_PULSE(bit_pulse) || !LIRC_IS_SPACE(bit_space) ||
          !irdump_in_range(LIRC_VALUE(bit_pulse), 300, 900))
        {
          return;
        }

      if (irdump_in_range(LIRC_VALUE(bit_space), 300, 1000))
        {
          one = false;
        }
      else if (irdump_in_range(LIRC_VALUE(bit_space), 1200, 2400))
        {
          one = true;
        }
      else
        {
          return;
        }

      if (one)
        {
          bytes_lsb[bit / 8] |= 1 << (bit % 8);
        }
    }

  for (bit = 0; bit < 4; bit++)
    {
      bytes_msb[bit] = irdump_reverse8(bytes_lsb[bit]);
    }

  code = ((uint32_t)bytes_msb[0] << 24) |
         ((uint32_t)bytes_msb[1] << 16) |
         ((uint32_t)bytes_msb[2] << 8) |
         bytes_msb[3];

  printf("decoded NEC code=0x%08" PRIx32
         " bytes_lsb=%02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8
         " bytes_msb=%02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8
         "\n",
         code,
         bytes_lsb[0], bytes_lsb[1], bytes_lsb[2], bytes_lsb[3],
         bytes_msb[0], bytes_msb[1], bytes_msb[2], bytes_msb[3]);

  if (bytes_msb[0] == 0x77 && bytes_msb[1] == 0xe1)
    {
      FAR const char *key = irdump_apple_a1156_key(bytes_msb[2]);

      printf("decoded Apple_A1156 command=0x%02" PRIx8
             " remote_id=0x%02" PRIx8,
             bytes_msb[2], bytes_msb[3]);

      if (key != NULL)
        {
          printf(" key=%s", key);
        }

      printf("\n");
    }
}

static void irdump_decode(FAR const uint32_t *samples, int count)
{
  irdump_decode_nec(samples, count);
  irdump_decode_rc6(samples, count);
}

static int irdump_print_features(int fd)
{
  unsigned int features;
  unsigned int mode;
  unsigned int resolution;
  int ret;

  ret = ioctl(fd, LIRC_GET_FEATURES, (unsigned long)(uintptr_t)&features);
  if (ret < 0)
    {
      fprintf(stderr, "WARNING: LIRC_GET_FEATURES failed: %d\n", errno);
    }
  else
    {
      printf("features=0x%08x", features);
    }

  ret = ioctl(fd, LIRC_GET_REC_MODE, (unsigned long)(uintptr_t)&mode);
  if (ret < 0)
    {
      printf(" rec_mode=?");
    }
  else
    {
      printf(" rec_mode=0x%02x", mode);
    }

  ret = ioctl(fd, LIRC_GET_REC_RESOLUTION,
              (unsigned long)(uintptr_t)&resolution);
  if (ret < 0)
    {
      printf(" resolution=?\n");
    }
  else
    {
      printf(" resolution=%uus\n", resolution);
    }

  return OK;
}

static int irdump_capture(int fd, int max_samples, int quiet_ms, int first_ms)
{
  struct pollfd pfd;
  FAR uint32_t *samples;
  uint32_t sample;
  int timeout_ms;
  int count;
  int ret;

  samples = calloc(max_samples, sizeof(uint32_t));
  if (samples == NULL)
    {
      fprintf(stderr, "ERROR: failed to allocate sample buffer\n");
      return EXIT_FAILURE;
    }

  pfd.fd = fd;
  pfd.events = POLLIN;
  pfd.revents = 0;

  printf("waiting for IR input; press one remote button now...\n");
  fflush(stdout);

  timeout_ms = first_ms;
  count = 0;

  while (count < max_samples)
    {
      ret = poll(&pfd, 1, timeout_ms);
      if (ret < 0)
        {
          fprintf(stderr, "ERROR: poll failed: %d\n", errno);
          free(samples);
          return EXIT_FAILURE;
        }
      else if (ret == 0)
        {
          if (count == 0)
            {
              fprintf(stderr, "ERROR: timed out waiting for first sample\n");
              free(samples);
              return EXIT_FAILURE;
            }

          break;
        }

      ret = read(fd, &sample, sizeof(sample));
      if (ret < 0)
        {
          if (errno == EAGAIN)
            {
              timeout_ms = quiet_ms;
              continue;
            }

          fprintf(stderr, "ERROR: read failed: %d\n", errno);
          free(samples);
          return EXIT_FAILURE;
        }
      else if (ret != sizeof(sample))
        {
          fprintf(stderr, "ERROR: short read: %d\n", ret);
          free(samples);
          return EXIT_FAILURE;
        }

      if (count == 0 && LIRC_IS_SPACE(sample) &&
          LIRC_VALUE(sample) > IRDUMP_LEADING_IDLE_US)
        {
          printf("ignored leading idle space: %" PRIu32 " us\n",
                 (uint32_t)LIRC_VALUE(sample));
          timeout_ms = quiet_ms;
          continue;
        }

      printf("%03d %-7s %8" PRIu32 " us raw=0x%08" PRIx32 "\n",
             count, irdump_sample_type(sample),
             (uint32_t)LIRC_VALUE(sample), sample);
      samples[count] = sample;
      count++;
      timeout_ms = quiet_ms;
      fflush(stdout);
      usleep(2000);
    }

  printf("captured %d samples\n", count);
  irdump_decode(samples, count);
  free(samples);
  return count > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *devpath = CONFIG_SYSTEM_IRDUMP_DEVPATH;
  int max_samples = IRDUMP_DEFAULT_MAX_SAMPLES;
  int quiet_ms = IRDUMP_DEFAULT_QUIET_MS;
  int first_ms = IRDUMP_DEFAULT_FIRST_MS;
  int opt;
  int fd;
  int ret;

  while ((opt = getopt(argc, argv, "d:n:q:t:h")) != ERROR)
    {
      switch (opt)
        {
          case 'd':
            devpath = optarg;
            break;

          case 'n':
            ret = irdump_argtol(optarg, 1, IRDUMP_MAX_SAMPLES_LIMIT,
                                "samples");
            if (ret < 0)
              {
                return EXIT_FAILURE;
              }

            max_samples = ret;
            break;

          case 'q':
            ret = irdump_argtol(optarg, 1, 5000, "quiet_ms");
            if (ret < 0)
              {
                return EXIT_FAILURE;
              }

            quiet_ms = ret;
            break;

          case 't':
            ret = irdump_argtol(optarg, 1, 60000, "first_ms");
            if (ret < 0)
              {
                return EXIT_FAILURE;
              }

            first_ms = ret;
            break;

          case 'h':
          default:
            irdump_usage(argv[0]);
            return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

  fd = open(devpath, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      fprintf(stderr, "ERROR: failed to open %s: %d\n", devpath, errno);
      return EXIT_FAILURE;
    }

  printf("opened %s\n", devpath);
  irdump_print_features(fd);

  ret = irdump_capture(fd, max_samples, quiet_ms, first_ms);
  close(fd);
  return ret;
}
