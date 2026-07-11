/****************************************************************************
 * apps/system/neopixels/neopixels_main.c
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <nuttx/leds/ws2812.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_SYSTEM_NEOPIXELS_DEVPATH
#  define CONFIG_SYSTEM_NEOPIXELS_DEVPATH "/dev/leds0"
#endif

#define NEOPIXELS_COUNT CONFIG_WS2812_LED_COUNT
#define NEOPIXELS_DEFAULT_BRIGHTNESS 64
#define NEOPIXELS_DEFAULT_DELAY_MS 40

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct neopixels_color_s
{
  FAR const char *name;
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct neopixels_color_s g_colors[] =
{
  { "red",     255,   0,   0 },
  { "green",     0, 255,   0 },
  { "blue",      0,   0, 255 },
  { "white",   255, 255, 255 },
  { "yellow",  255, 220,   0 },
  { "orange",  255,  80,   0 },
  { "purple",  128,   0, 255 },
  { "cyan",      0, 255, 255 },
  { "pink",    255,  32,  96 }
};

static const struct neopixels_color_s g_fire_colors[] =
{
  { "fire-red",       255,   0,   0 },
  { "fire-orange",    255,  72,   0 },
  { "fire-amber",     255, 140,   0 },
  { "fire-yellow",    255, 220,   0 }
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void neopixels_usage(FAR const char *progname)
{
  fprintf(stderr,
          "Usage:\n"
          "  %s help\n"
          "  %s off\n"
          "  %s <red> <green> <blue> [brightness]\n"
          "  %s rgb <red> <green> <blue> [brightness]\n"
          "  %s <color> [brightness]\n"
          "  %s rainbow [cycles] [delay_ms] [brightness]\n"
          "  %s fire [frames] [delay_ms] [brightness]\n"
          "  %s chase <color> [cycles] [delay_ms] [brightness]\n"
          "  %s pulse <color> [cycles] [delay_ms] [brightness]\n"
          "\n"
          "Colors: red green blue white yellow orange purple cyan pink\n",
          progname, progname, progname, progname, progname, progname,
          progname, progname, progname);
}

static long neopixels_argtol(FAR const char *value, long minval, long maxval,
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

static bool neopixels_is_number(FAR const char *value)
{
  FAR char *endptr;

  if (value == NULL || value[0] == '\0')
    {
      return false;
    }

  strtol(value, &endptr, 0);
  return endptr != value && *endptr == '\0';
}

static bool neopixels_color_lookup(FAR const char *name,
                                   FAR const struct neopixels_color_s **color)
{
  size_t i;

  for (i = 0; i < sizeof(g_colors) / sizeof(g_colors[0]); i++)
    {
      if (strcmp(name, g_colors[i].name) == 0)
        {
          *color = &g_colors[i];
          return true;
        }
    }

  return false;
}

static uint32_t neopixels_rgb_to_pixel(uint8_t r, uint8_t g, uint8_t b,
                                       uint8_t brightness)
{
  uint32_t sr;
  uint32_t sg;
  uint32_t sb;

  sr = ((uint32_t)r * brightness + 127) / 255;
  sg = ((uint32_t)g * brightness + 127) / 255;
  sb = ((uint32_t)b * brightness + 127) / 255;

  return ws2812_gamma_correct((sr << 16) | (sg << 8) | sb);
}

static int neopixels_open(void)
{
  int fd;

  fd = open(CONFIG_SYSTEM_NEOPIXELS_DEVPATH, O_WRONLY);
  if (fd < 0)
    {
      fprintf(stderr, "ERROR: open %s failed: %d\n",
              CONFIG_SYSTEM_NEOPIXELS_DEVPATH, errno);
      return ERROR;
    }

  return fd;
}

static int neopixels_write(int fd, FAR const uint32_t *pixels)
{
  ssize_t expected;
  ssize_t nwritten;

  expected = (ssize_t)(NEOPIXELS_COUNT * sizeof(uint32_t));

  lseek(fd, 0, SEEK_SET);
  nwritten = write(fd, pixels, expected);
  if (nwritten != expected)
    {
      fprintf(stderr, "ERROR: write failed: %ld expected %ld errno %d\n",
              (long)nwritten, (long)expected, errno);
      return ERROR;
    }

  return OK;
}

static int neopixels_fill(int fd, uint8_t r, uint8_t g, uint8_t b,
                          uint8_t brightness)
{
  FAR uint32_t *pixels;
  uint32_t pixel;
  int ret;
  int i;

  pixels = calloc(NEOPIXELS_COUNT, sizeof(uint32_t));
  if (pixels == NULL)
    {
      fprintf(stderr, "ERROR: allocation failed: %d\n", errno);
      return ERROR;
    }

  pixel = neopixels_rgb_to_pixel(r, g, b, brightness);
  for (i = 0; i < NEOPIXELS_COUNT; i++)
    {
      pixels[i] = pixel;
    }

  ret = neopixels_write(fd, pixels);
  free(pixels);
  return ret;
}

static int neopixels_rainbow(int fd, int cycles, int delay_ms,
                             uint8_t brightness)
{
  FAR uint32_t *pixels;
  int cycle;
  int hue;
  int i;
  int ret;

  pixels = calloc(NEOPIXELS_COUNT, sizeof(uint32_t));
  if (pixels == NULL)
    {
      fprintf(stderr, "ERROR: allocation failed: %d\n", errno);
      return ERROR;
    }

  ret = OK;
  for (cycle = 0; cycle < cycles && ret == OK; cycle++)
    {
      for (hue = 0; hue < 256 && ret == OK; hue += 4)
        {
          for (i = 0; i < NEOPIXELS_COUNT; i++)
            {
              pixels[i] = ws2812_gamma_correct(
                          ws2812_hsv_to_rgb((hue + i * 36) & 0xff,
                                            0xff,
                                            brightness));
            }

          ret = neopixels_write(fd, pixels);
          usleep((useconds_t)delay_ms * 1000);
        }
    }

  free(pixels);
  return ret;
}

static int neopixels_fire(int fd, int frames, int delay_ms,
                          uint8_t brightness)
{
  FAR uint32_t *pixels;
  FAR const struct neopixels_color_s *color;
  uint8_t frame_brightness;
  int phase;
  int wave;
  int frame;
  int i;
  int ret;

  pixels = calloc(NEOPIXELS_COUNT, sizeof(uint32_t));
  if (pixels == NULL)
    {
      fprintf(stderr, "ERROR: allocation failed: %d\n", errno);
      return ERROR;
    }

  ret = OK;
  for (frame = 0; frame < frames && ret == OK; frame++)
    {
      for (i = 0; i < NEOPIXELS_COUNT; i++)
        {
          phase = (frame * 19 + i * 47) & 0xff;
          wave = phase < 128 ? phase : 255 - phase;
          frame_brightness = (uint8_t)(((uint32_t)brightness *
                              (96 + wave)) / 223);
          color = &g_fire_colors[(frame + i) %
                    (sizeof(g_fire_colors) / sizeof(g_fire_colors[0]))];
          pixels[i] = neopixels_rgb_to_pixel(color->r, color->g, color->b,
                                             frame_brightness);
        }

      ret = neopixels_write(fd, pixels);
      usleep((useconds_t)delay_ms * 1000);
    }

  free(pixels);
  return ret;
}

static int neopixels_chase(int fd, FAR const struct neopixels_color_s *color,
                           int cycles, int delay_ms, uint8_t brightness)
{
  FAR uint32_t *pixels;
  uint32_t lit;
  uint32_t dim;
  int frame;
  int count;
  int i;
  int ret;

  pixels = calloc(NEOPIXELS_COUNT, sizeof(uint32_t));
  if (pixels == NULL)
    {
      fprintf(stderr, "ERROR: allocation failed: %d\n", errno);
      return ERROR;
    }

  lit = neopixels_rgb_to_pixel(color->r, color->g, color->b, brightness);
  dim = neopixels_rgb_to_pixel(color->r, color->g, color->b, brightness / 8);
  count = cycles * NEOPIXELS_COUNT;
  ret = OK;

  for (frame = 0; frame < count && ret == OK; frame++)
    {
      for (i = 0; i < NEOPIXELS_COUNT; i++)
        {
          pixels[i] = (i == frame % NEOPIXELS_COUNT) ? lit : dim;
        }

      ret = neopixels_write(fd, pixels);
      usleep((useconds_t)delay_ms * 1000);
    }

  free(pixels);
  return ret;
}

static int neopixels_pulse(int fd, FAR const struct neopixels_color_s *color,
                           int cycles, int delay_ms, uint8_t brightness)
{
  int cycle;
  int step;
  int ret;

  ret = OK;
  for (cycle = 0; cycle < cycles && ret == OK; cycle++)
    {
      for (step = 0; step <= 16 && ret == OK; step++)
        {
          ret = neopixels_fill(fd, color->r, color->g, color->b,
                               (uint8_t)(((uint32_t)brightness * step) /
                                         16));
          usleep((useconds_t)delay_ms * 1000);
        }

      for (step = 15; step >= 0 && ret == OK; step--)
        {
          ret = neopixels_fill(fd, color->r, color->g, color->b,
                               (uint8_t)(((uint32_t)brightness * step) /
                                         16));
          usleep((useconds_t)delay_ms * 1000);
        }
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * neopixels_main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const struct neopixels_color_s *color;
  FAR const char *command;
  long value;
  int brightness;
  int delay_ms;
  int cycles;
  int frames;
  int r;
  int g;
  int b;
  int fd;
  int ret;

  if (argc < 2 || strcmp(argv[1], "help") == 0)
    {
      neopixels_usage(argv[0]);
      return argc < 2 ? EXIT_FAILURE : EXIT_SUCCESS;
    }

  command = argv[1];
  ret = ERROR;

  fd = neopixels_open();
  if (fd < 0)
    {
      return EXIT_FAILURE;
    }

  if (strcmp(command, "off") == 0)
    {
      ret = neopixels_fill(fd, 0, 0, 0, 0);
      if (ret == OK)
        {
          printf("neopixels: off\n");
        }
    }
  else if (strcmp(command, "rgb") == 0 || neopixels_is_number(command))
    {
      int argbase;

      argbase = strcmp(command, "rgb") == 0 ? 2 : 1;
      if (argc < argbase + 3)
        {
          neopixels_usage(argv[0]);
          close(fd);
          return EXIT_FAILURE;
        }

      value = neopixels_argtol(argv[argbase], 0, 255, "red");
      if (value == LONG_MIN)
        {
          close(fd);
          return EXIT_FAILURE;
        }

      r = value;
      value = neopixels_argtol(argv[argbase + 1], 0, 255, "green");
      if (value == LONG_MIN)
        {
          close(fd);
          return EXIT_FAILURE;
        }

      g = value;
      value = neopixels_argtol(argv[argbase + 2], 0, 255, "blue");
      if (value == LONG_MIN)
        {
          close(fd);
          return EXIT_FAILURE;
        }

      b = value;
      brightness = NEOPIXELS_DEFAULT_BRIGHTNESS;

      if (argc > argbase + 3)
        {
          value = neopixels_argtol(argv[argbase + 3], 0, 255, "brightness");
          if (value == LONG_MIN)
            {
              close(fd);
              return EXIT_FAILURE;
            }

          brightness = value;
        }

      ret = neopixels_fill(fd, r, g, b, brightness);
      if (ret == OK)
        {
          printf("neopixels: rgb %d %d %d brightness %d\n",
                 r, g, b, brightness);
        }
    }
  else if (strcmp(command, "rainbow") == 0)
    {
      cycles = 2;
      delay_ms = 20;
      brightness = 96;

      if (argc > 2)
        {
          value = neopixels_argtol(argv[2], 1, 1000, "cycles");
          if (value == LONG_MIN)
            {
              close(fd);
              return EXIT_FAILURE;
            }

          cycles = value;
        }

      if (argc > 3)
        {
          value = neopixels_argtol(argv[3], 1, 60000, "delay_ms");
          if (value == LONG_MIN)
            {
              close(fd);
              return EXIT_FAILURE;
            }

          delay_ms = value;
        }

      if (argc > 4)
        {
          value = neopixels_argtol(argv[4], 0, 255, "brightness");
          if (value == LONG_MIN)
            {
              close(fd);
              return EXIT_FAILURE;
            }

          brightness = value;
        }

      printf("neopixels: rainbow cycles %d delay_ms %d brightness %d\n",
             cycles, delay_ms, brightness);
      ret = neopixels_rainbow(fd, cycles, delay_ms, brightness);
    }
  else if (strcmp(command, "fire") == 0)
    {
      frames = 80;
      delay_ms = NEOPIXELS_DEFAULT_DELAY_MS;
      brightness = 128;

      if (argc > 2)
        {
          value = neopixels_argtol(argv[2], 1, 100000, "frames");
          if (value == LONG_MIN)
            {
              close(fd);
              return EXIT_FAILURE;
            }

          frames = value;
        }

      if (argc > 3)
        {
          value = neopixels_argtol(argv[3], 1, 60000, "delay_ms");
          if (value == LONG_MIN)
            {
              close(fd);
              return EXIT_FAILURE;
            }

          delay_ms = value;
        }

      if (argc > 4)
        {
          value = neopixels_argtol(argv[4], 0, 255, "brightness");
          if (value == LONG_MIN)
            {
              close(fd);
              return EXIT_FAILURE;
            }

          brightness = value;
        }

      printf("neopixels: fire frames %d delay_ms %d brightness %d\n",
             frames, delay_ms, brightness);
      ret = neopixels_fire(fd, frames, delay_ms, brightness);
    }
  else if (strcmp(command, "chase") == 0 || strcmp(command, "pulse") == 0)
    {
      if (argc < 3 || !neopixels_color_lookup(argv[2], &color))
        {
          fprintf(stderr, "ERROR: color required for %s\n", command);
          neopixels_usage(argv[0]);
          close(fd);
          return EXIT_FAILURE;
        }

      cycles = strcmp(command, "chase") == 0 ? 8 : 6;
      delay_ms = strcmp(command, "chase") == 0 ? 80 : 35;
      brightness = 96;

      if (argc > 3)
        {
          value = neopixels_argtol(argv[3], 1, 100000, "cycles");
          if (value == LONG_MIN)
            {
              close(fd);
              return EXIT_FAILURE;
            }

          cycles = value;
        }

      if (argc > 4)
        {
          value = neopixels_argtol(argv[4], 1, 60000, "delay_ms");
          if (value == LONG_MIN)
            {
              close(fd);
              return EXIT_FAILURE;
            }

          delay_ms = value;
        }

      if (argc > 5)
        {
          value = neopixels_argtol(argv[5], 0, 255, "brightness");
          if (value == LONG_MIN)
            {
              close(fd);
              return EXIT_FAILURE;
            }

          brightness = value;
        }

      printf("neopixels: %s %s cycles %d delay_ms %d brightness %d\n",
             command, color->name, cycles, delay_ms, brightness);

      if (strcmp(command, "chase") == 0)
        {
          ret = neopixels_chase(fd, color, cycles, delay_ms, brightness);
        }
      else
        {
          ret = neopixels_pulse(fd, color, cycles, delay_ms, brightness);
        }
    }
  else if (neopixels_color_lookup(command, &color))
    {
      brightness = NEOPIXELS_DEFAULT_BRIGHTNESS;

      if (argc > 2)
        {
          value = neopixels_argtol(argv[2], 0, 255, "brightness");
          if (value == LONG_MIN)
            {
              close(fd);
              return EXIT_FAILURE;
            }

          brightness = value;
        }

      ret = neopixels_fill(fd, color->r, color->g, color->b, brightness);
      if (ret == OK)
        {
          printf("neopixels: %s brightness %d\n", color->name, brightness);
        }
    }
  else
    {
      fprintf(stderr, "ERROR: unknown command or color: %s\n", command);
      neopixels_usage(argv[0]);
    }

  close(fd);
  return ret == OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
