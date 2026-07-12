/****************************************************************************
 * apps/system/sdmount/sdmount_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/arch.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arch/board/rp23xx_sdcard.h>

#ifndef CONFIG_SYSTEM_SDMOUNT_DEVPATH
#  define CONFIG_SYSTEM_SDMOUNT_DEVPATH "/dev/mmcsd0"
#endif

#ifndef CONFIG_SYSTEM_SDMOUNT_MOUNTPOINT
#  define CONFIG_SYSTEM_SDMOUNT_MOUNTPOINT "/mnt/sd0"
#endif

#ifndef CONFIG_SYSTEM_SDMOUNT_TIMEOUT_SEC
#  define CONFIG_SYSTEM_SDMOUNT_TIMEOUT_SEC 12
#endif

#define SDCHECK_FILE CONFIG_SYSTEM_SDMOUNT_MOUNTPOINT "/.sdmount-check"
#define SDREADY_FILE CONFIG_SYSTEM_SDMOUNT_MOUNTPOINT "/.fruitclaw-mounted"

static volatile bool g_sdmount_done;

static void *sdmount_timeout_main(void *arg)
{
  (void)arg;

  sleep(CONFIG_SYSTEM_SDMOUNT_TIMEOUT_SEC);
  if (!g_sdmount_done)
    {
      printf("sdmount: timed out; resetting board\n");
      fflush(stdout);
      up_systemreset();
    }

  return NULL;
}

static int sdmount_write_text(FAR const char *path, FAR const char *text)
{
  size_t len;
  ssize_t nwritten;
  int fd;

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0664);
  if (fd < 0)
    {
      return -errno;
    }

  len = strlen(text);
  nwritten = write(fd, text, len);
  if (nwritten < 0)
    {
      int err = errno;
      close(fd);
      return -err;
    }

  close(fd);
  return (size_t)nwritten == len ? 0 : -EIO;
}

static int sdmount_healthcheck(void)
{
  char buf[16];
  DIR *dir;
  int fd;
  int ret;
  int i;

  dir = opendir(CONFIG_SYSTEM_SDMOUNT_MOUNTPOINT);
  if (dir == NULL)
    {
      return -errno;
    }

  for (i = 0; i < 4; i++)
    {
      if (readdir(dir) == NULL)
        {
          break;
        }
    }

  closedir(dir);

  ret = sdmount_write_text(SDCHECK_FILE, "ok\n");
  if (ret < 0)
    {
      return ret;
    }

  fd = open(SDCHECK_FILE, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  ret = read(fd, buf, sizeof(buf));
  if (ret < 0)
    {
      int err = errno;
      close(fd);
      return -err;
    }

  close(fd);

  if (ret < 3 || memcmp(buf, "ok\n", 3) != 0)
    {
      return -EIO;
    }

  ret = sdmount_write_text(SDREADY_FILE, "mounted\n");
  if (ret < 0)
    {
      return ret;
    }

  unlink(SDCHECK_FILE);
  return 0;
}

int main(int argc, FAR char *argv[])
{
#if defined(CONFIG_ADAFRUIT_FRUIT_JAM_SD_SPI) || \
    defined(CONFIG_ADAFRUIT_FRUIT_JAM_SD_PIO)
  pthread_t timeout_thread;
  int ret;

  g_sdmount_done = false;
  ret = pthread_create(&timeout_thread, NULL, sdmount_timeout_main, NULL);
  if (ret == 0)
    {
      pthread_detach(timeout_thread);
    }

  printf("sdmount: mounting %s at %s\n",
         CONFIG_SYSTEM_SDMOUNT_DEVPATH,
         CONFIG_SYSTEM_SDMOUNT_MOUNTPOINT);
  fflush(stdout);

  ret = board_sdcard_mount();

  if (ret < 0)
    {
      g_sdmount_done = true;
      printf("sdmount: mount failed: %d\n", ret);
      return 1;
    }

  printf("sdmount: checking filesystem\n");
  fflush(stdout);

  ret = sdmount_healthcheck();
  g_sdmount_done = true;
  if (ret < 0)
    {
      printf("sdmount: health check failed: %d\n", ret);
      return 1;
    }

  printf("sdmount: mounted %s at %s\n",
         CONFIG_SYSTEM_SDMOUNT_DEVPATH,
         CONFIG_SYSTEM_SDMOUNT_MOUNTPOINT);
  return 0;
#else
  printf("sdmount: Fruit Jam SD support is disabled\n");
  return 1;
#endif
}
