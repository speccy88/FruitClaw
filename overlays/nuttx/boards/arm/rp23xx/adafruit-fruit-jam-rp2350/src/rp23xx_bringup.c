/****************************************************************************
 * boards/arm/rp23xx/adafruit-fruit-jam-rp2350/src/rp23xx_bringup.c
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

#include <debug.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nuttx/clock.h>
#include <nuttx/fs/fs.h>
#include <nuttx/mutex.h>
#include <nuttx/usb/usbhost.h>
#include <nuttx/wqueue.h>

#include <arch/board/board.h>

#include "rp23xx_pico.h"

#if defined(CONFIG_RP23XX_PIO_USBHOST) && \
    defined(CONFIG_RP23XX_PIO_USBHOST_AUTOSTART)
#  include "rp23xx_pio_usbhost.h"
#endif

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED
#  include <nuttx/wireless/esp_hosted.h>
#  include "rp23xx_gpio.h"
#  include "rp23xx_spi.h"
#endif

#ifdef CONFIG_ARCH_BOARD_COMMON
#include "rp23xx_common_bringup.h"
#endif /* CONFIG_ARCH_BOARD_COMMON */

#ifdef CONFIG_USERLED
#  include <nuttx/leds/userled.h>
#endif

#ifdef CONFIG_INPUT_BUTTONS
#  include <nuttx/input/buttons.h>
#endif

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_IR_RX
int fruitjam_irrx_initialize(void);
#endif

#ifdef CONFIG_RP23XX_I2S
int fruitjam_audio_codec_initialize(void);
#endif

int board_fruitjam_shared_peripherals_recover(void);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_board_bringup_done;

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED_AUTOSTART
static struct work_s g_fruitjam_esp_hosted_work;
#endif

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED
static bool g_fruitjam_esp_hosted_started;
static mutex_t g_fruitjam_esp_hosted_lock = NXMUTEX_INITIALIZER;
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: fruitjam_esp_hosted_initialize
 ****************************************************************************/

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED
static int fruitjam_esp_hosted_reset(FAR void *arg, bool asserted)
{
  UNUSED(arg);

  /* ESP32-C6 reset is active-low on the Fruit Jam NINA wiring. */

  rp23xx_gpio_put(BOARD_NINA_RESET_PIN, !asserted);
  return OK;
}

static bool fruitjam_esp_hosted_handshake_ready(FAR void *arg)
{
  UNUSED(arg);
  return rp23xx_gpio_get(BOARD_NINA_READY_PIN);
}

static bool fruitjam_esp_hosted_data_ready(FAR void *arg)
{
  UNUSED(arg);
  return rp23xx_gpio_get(BOARD_NINA_IRQ_PIN);
}

static int fruitjam_esp_hosted_attach_dataready(FAR void *arg,
                                                xcpt_t handler,
                                                FAR void *israrg)
{
  UNUSED(arg);

  return rp23xx_gpio_irq_attach(BOARD_NINA_IRQ_PIN,
                                RP23XX_GPIO_INTR_EDGE_HIGH,
                                handler,
                                israrg);
}

static int fruitjam_esp_hosted_enable_dataready_irq(FAR void *arg,
                                                    bool enable)
{
  UNUSED(arg);

  if (enable)
    {
      rp23xx_gpio_enable_irq(BOARD_NINA_IRQ_PIN);
    }
  else
    {
      rp23xx_gpio_disable_irq(BOARD_NINA_IRQ_PIN);
    }

  return OK;
}

static const struct esp_hosted_gpio_ops_s g_fruitjam_esp_hosted_gpio_ops =
{
  .reset                = fruitjam_esp_hosted_reset,
  .handshake_ready      = fruitjam_esp_hosted_handshake_ready,
  .data_ready           = fruitjam_esp_hosted_data_ready,
  .attach_dataready     = fruitjam_esp_hosted_attach_dataready,
  .enable_dataready_irq = fruitjam_esp_hosted_enable_dataready_irq,
};

static int fruitjam_esp_hosted_initialize(void)
{
  struct esp_hosted_config_s config;
  FAR struct spi_dev_s *spi;

  spi = rp23xx_spibus_initialize(BOARD_NINA_SPI_BUS);
  if (spi == NULL)
    {
      syslog(LOG_ERR, "ERROR: failed to initialize ESP-Hosted SPI%d\n",
             BOARD_NINA_SPI_BUS);
      return -ENODEV;
    }

  memset(&config, 0, sizeof(config));
  config.spi           = spi;
  config.spi_devid     = SPIDEV_WIRELESS(0);
  config.spi_frequency = CONFIG_ESP_HOSTED_SPI_FREQUENCY;
  config.spi_mode      = CONFIG_ESP_HOSTED_SPI_MODE;
  config.spi_bits      = 8;
  config.gpio          = &g_fruitjam_esp_hosted_gpio_ops;

  return esp_hosted_spi_initialize(&config);
}

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED_AUTOSTART
static void fruitjam_esp_hosted_worker(FAR void *arg)
{
  int ret;

  UNUSED(arg);

  ret = board_fruitjam_esp_hosted_start();
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "WARNING: ESP-Hosted wlan0 bring-up not ready: %d\n", ret);
    }
}
#endif
#endif

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED
int board_fruitjam_esp_hosted_start(void)
{
  int ret;

  ret = nxmutex_lock(&g_fruitjam_esp_hosted_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_fruitjam_esp_hosted_started)
    {
      nxmutex_unlock(&g_fruitjam_esp_hosted_lock);
      return OK;
    }

  ret = fruitjam_esp_hosted_initialize();
  if (ret >= 0)
    {
      g_fruitjam_esp_hosted_started = true;
      (void)board_fruitjam_shared_peripherals_recover();
    }

  nxmutex_unlock(&g_fruitjam_esp_hosted_lock);
  return ret;
}

int board_fruitjam_esp_hosted_recover(void)
{
  int ret;

  ret = nxmutex_lock(&g_fruitjam_esp_hosted_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_fruitjam_esp_hosted_started)
    {
      ret = fruitjam_esp_hosted_initialize();
      if (ret >= 0)
        {
          g_fruitjam_esp_hosted_started = true;
        }

      nxmutex_unlock(&g_fruitjam_esp_hosted_lock);
      return ret;
    }

  ret = esp_hosted_spi_recover();
  if (ret >= 0)
    {
      (void)board_fruitjam_shared_peripherals_recover();
    }

  nxmutex_unlock(&g_fruitjam_esp_hosted_lock);
  return ret;
}
#endif

int board_fruitjam_shared_peripherals_recover(void)
{
  int final_ret = OK;

#ifdef CONFIG_RP23XX_I2S
  int ret;

  ret = board_fruitjam_audio_codec_recover();
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "WARNING: Fruit Jam audio codec recovery failed: %d\n", ret);
      final_ret = ret;
    }
#endif

#if defined(CONFIG_RP23XX_BOARD_HAS_WS2812) && defined(CONFIG_WS2812)
  {
    int fd = open("/dev/leds0", O_WRONLY);

    if (fd >= 0)
      {
        close(fd);
      }
    else if (final_ret == OK)
      {
        final_ret = -errno;
      }
  }
#endif

  return final_ret;
}

/****************************************************************************
 * Name: rp23xx_mount_tmpfs
 ****************************************************************************/

#ifdef CONFIG_FS_TMPFS
static void rp23xx_mount_tmpfs(FAR const char *path)
{
  int ret;

  ret = mkdir(path, 0777);
  if (ret < 0 && errno != EEXIST)
    {
      syslog(LOG_WARNING, "WARNING: mkdir(%s) failed: %d\n", path, errno);
      return;
    }

  ret = nx_mount(NULL, path, "tmpfs", 0, NULL);
  if (ret < 0 && ret != -EBUSY)
    {
      syslog(LOG_WARNING, "WARNING: nx_mount(tmpfs,%s) failed: %d\n",
             path, ret);
    }
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rp23xx_bringup
 ****************************************************************************/

int rp23xx_bringup(uintptr_t arg)
{
  int ret = OK;

  UNUSED(arg);

  if (g_board_bringup_done)
    {
      return OK;
    }

#ifdef CONFIG_ARCH_BOARD_COMMON

  ret = rp23xx_common_bringup();
  if (ret < 0)
    {
      return ret;
    }

#endif /* CONFIG_ARCH_BOARD_COMMON */

  /* --- Place any board specific bringup code here --- */

#ifdef CONFIG_USBHOST_XBOXCONTROLLER
  ret = usbhost_xboxcontroller_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: usbhost_xboxcontroller_init failed: %d\n",
             ret);
    }
#endif

#if defined(CONFIG_RP23XX_PIO_USBHOST) && \
    defined(CONFIG_RP23XX_PIO_USBHOST_AUTOSTART)
  FAR struct usbhost_connection_s *usbhost_conn;

  ret = rp23xx_pio_usbhost_initialize(&usbhost_conn);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: PIO USB host initialize failed: %d\n", ret);
    }
  else
    {
      ret = usbhost_waiter_initialize(usbhost_conn);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: usbhost_waiter_initialize failed: %d\n",
                 ret);
        }
    }
#endif

#ifdef CONFIG_RP23XX_I2S
  ret = fruitjam_audio_codec_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: failed to initialize Fruit Jam audio codec: %d\n",
             ret);
    }
#endif

#ifdef CONFIG_FS_TMPFS
  rp23xx_mount_tmpfs("/tmp");
  rp23xx_mount_tmpfs("/scripts");
  rp23xx_mount_tmpfs("/data");
#endif

#ifdef CONFIG_USERLED
  /* Register the LED driver */

  ret = userled_lower_initialize("/dev/userleds");
  if (ret < 0)
    {
      syslog(LOG_ERR, \
      "ERROR: userled_lower_initialize() failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_INPUT_BUTTONS
  /* Register the BUTTON driver */

  ret = btn_lower_initialize("/dev/buttons");
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: btn_lower_initialize() failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_IR_RX
  ret = fruitjam_irrx_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: fruitjam_irrx_initialize() failed: %d\n",
             ret);
    }
#endif

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED_AUTOSTART
#ifdef CONFIG_SCHED_LPWORK
  clock_t delay;

  delay = MSEC2TICK(
    CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED_START_DELAY_MS);
  ret = work_queue(LPWORK, &g_fruitjam_esp_hosted_work,
                   fruitjam_esp_hosted_worker, NULL, delay);
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "WARNING: failed to defer ESP-Hosted wlan0 bring-up: %d\n",
             ret);
    }
#else
  fruitjam_esp_hosted_worker(NULL);
#endif
#endif

  g_board_bringup_done = true;

  return OK;
}
