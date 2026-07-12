/****************************************************************************
 * boards/arm/rp23xx/adafruit-fruit-jam-rp2350/src/rp23xx_fruitjam_sdcard.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>

#include <nuttx/debug.h>
#include <nuttx/clock.h>
#include <nuttx/fs/fs.h>
#include <nuttx/mmcsd.h>
#include <nuttx/wqueue.h>

#if defined(CONFIG_MBR_PARTITION) || defined(CONFIG_GPT_PARTITION)
#  include <nuttx/fs/partition.h>
#endif

#include <arch/board/board.h>
#include <arch/board/rp23xx_sdcard.h>

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_SD_SPI
#  include <arch/board/rp23xx_spisd.h>
#  include "rp23xx_gpio.h"
#  include "rp23xx_spi.h"
#endif

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_SD_PIO
#  include "rp23xx_pio_sdio.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FRUITJAM_SD_BLOCKDEV   "/dev/mmcsd0"
#define FRUITJAM_SD_PARTDEV    "/dev/mmcsd0p0"
#define FRUITJAM_SD_MOUNTPT    "/mnt/sd0"
#define FRUITJAM_SD_DEBOUNCE_MS 20

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_SD_SPI
static struct work_s g_fruitjam_spi_cdwork;

static void fruitjam_spi_cd_worker(FAR void *arg)
{
  UNUSED(arg);
  (void)rp23xx_spi0_mediachange();
  rp23xx_gpio_enable_irq(BOARD_SD_DETECT_PIN);
}

static int fruitjam_spi_cd_interrupt(int irq, FAR void *context,
                                     FAR void *arg)
{
  UNUSED(irq);
  UNUSED(context);
  UNUSED(arg);

  rp23xx_gpio_disable_irq(BOARD_SD_DETECT_PIN);
  if (work_available(&g_fruitjam_spi_cdwork))
    {
      work_queue(HPWORK, &g_fruitjam_spi_cdwork, fruitjam_spi_cd_worker,
                 NULL, MSEC2TICK(FRUITJAM_SD_DEBOUNCE_MS));
    }

  return OK;
}
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_SD_PIO
static int fruitjam_sd_mkdir(FAR const char *path)
{
  int ret;

  ret = mkdir(path, 0777);
  if (ret < 0 && errno != EEXIST)
    {
      _err("ERROR: mkdir(%s) failed: %d\n", path, errno);
      return -errno;
    }

  return OK;
}

static int fruitjam_sd_mount_device(FAR const char *devpath)
{
  int ret;

  ret = nx_mount(devpath, FRUITJAM_SD_MOUNTPT, "vfat", 0, NULL);
  if (ret < 0 && ret != -EBUSY)
    {
      _err("ERROR: failed to mount %s at %s: %d\n",
           devpath, FRUITJAM_SD_MOUNTPT, ret);
    }

  return ret;
}

#if defined(CONFIG_MBR_PARTITION) || defined(CONFIG_GPT_PARTITION)
static void fruitjam_sd_partition_handler(FAR struct partition_s *part,
                                           FAR void *arg)
{
  FAR bool *registered = arg;
  char devname[32];
  int ret;

  snprintf(devname, sizeof(devname), "/dev/mmcsd0p%zu", part->index);
  ret = register_blockpartition(devname, 0660, FRUITJAM_SD_BLOCKDEV,
                                part->firstblock, part->nblocks);
  if (ret >= 0 || ret == -EEXIST)
    {
      *registered = true;
    }
  else
    {
      _err("ERROR: failed to register SD partition %s: %d\n",
           devname, ret);
    }
}

static bool fruitjam_sd_register_partitions(FAR int *parse_ret)
{
  bool registered = false;
  int ret;

  ret = parse_block_partition(FRUITJAM_SD_BLOCKDEV,
                              fruitjam_sd_partition_handler,
                              &registered);
  if (parse_ret != NULL)
    {
      *parse_ret = ret;
    }

  return registered;
}
#endif
#endif /* CONFIG_ADAFRUIT_FRUIT_JAM_SD_PIO */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_sdcard_mount(void)
{
#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_SD_SPI
  return board_spisd_mount();
#elif defined(CONFIG_ADAFRUIT_FRUIT_JAM_SD_PIO)
  int ret;
#if defined(CONFIG_MBR_PARTITION) || defined(CONFIG_GPT_PARTITION)
  int parse_ret = OK;
#endif

  fruitjam_sd_mkdir("/mnt");
  fruitjam_sd_mkdir(FRUITJAM_SD_MOUNTPT);

#if defined(CONFIG_MBR_PARTITION) || defined(CONFIG_GPT_PARTITION)
  if (fruitjam_sd_register_partitions(&parse_ret))
    {
      ret = fruitjam_sd_mount_device(FRUITJAM_SD_PARTDEV);
      return ret >= 0 || ret == -EBUSY ? OK : ret;
    }

  if (parse_ret < 0 && parse_ret != -EINVAL)
    {
      return parse_ret;
    }
#endif

  ret = fruitjam_sd_mount_device(FRUITJAM_SD_BLOCKDEV);
  return ret >= 0 || ret == -EBUSY ? OK : ret;
#else
  return -ENOSYS;
#endif
}

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_SD_SPI
int board_sdcard_carddetect_initialize(void)
{
  int ret;

  rp23xx_gpio_init(BOARD_SD_DETECT_PIN);
  rp23xx_gpio_setdir(BOARD_SD_DETECT_PIN, false);
  rp23xx_gpio_set_pulls(BOARD_SD_DETECT_PIN, true, false);
  ret = rp23xx_gpio_irq_attach(BOARD_SD_DETECT_PIN,
                               RP23XX_GPIO_INTR_EDGE_BOTH,
                               fruitjam_spi_cd_interrupt, NULL);
  if (ret >= 0)
    {
      rp23xx_gpio_enable_irq(BOARD_SD_DETECT_PIN);
    }

  return ret;
}
#endif

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_SD_PIO
int board_piosd_initialize(void)
{
  static const struct rp23xx_pio_sdio_config_s config =
  {
    .pio                = 2,
    .clk_pin            = BOARD_SD_SCK_PIN,
    .cmd_pin            = BOARD_SD_MOSI_PIN,
    .dat0_pin           = BOARD_SD_MISO_PIN,
    .cd_pin             = BOARD_SD_DETECT_PIN,
    .cd_active_high     = true,
    .idmode_frequency   = 400000,
    .transfer_frequency = 25000000,
  };

  FAR struct sdio_dev_s *sdio;
  int ret;

  sdio = rp23xx_pio_sdio_initialize(0, &config);
  if (sdio == NULL)
    {
      return -errno;
    }

  ret = mmcsd_slotinitialize(0, sdio);
  if (ret < 0)
    {
      _err("ERROR: failed to bind PIO SD host: %d\n", ret);
      return ret;
    }

#ifdef CONFIG_RP23XX_SDCARD_AUTOMOUNT
  ret = board_sdcard_mount();
  if (ret < 0)
    {
      _err("ERROR: PIO SDCARD is not mounted; persistence unavailable: %d\n",
           ret);
    }
#else
  _info("PIO SDCARD block device ready; automount disabled\n");
#endif

  return OK;
}
#endif /* CONFIG_ADAFRUIT_FRUIT_JAM_SD_PIO */
