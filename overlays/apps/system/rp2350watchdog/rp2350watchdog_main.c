/****************************************************************************
 * apps/system/rp2350watchdog/rp2350watchdog_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <arch/rp23xx/watchdog.h>
#include <nuttx/board.h>
#include <nuttx/timers/watchdog.h>

#include <sys/boardctl.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define RP2350_BOOTSEL_GUARD_MAGIC 0x464a4247 /* "FJBG" */

static int rp2350watchdog_scratch(int fd, unsigned long value)
{
  if (ioctl(fd, WDIOC_SET_SCRATCH0, value) < 0 ||
      ioctl(fd, WDIOC_SET_SCRATCH1,
            CONFIG_SYSTEM_RP2350WATCHDOG_TIMEOUT_MS) < 0 ||
      ioctl(fd, WDIOC_SET_SCRATCH2, 0) < 0)
    {
      fprintf(stderr, "rp2350watchdog: scratch setup failed: %d\n", errno);
      return -1;
    }

  return 0;
}

int main(int argc, FAR char *argv[])
{
#if CONFIG_SYSTEM_RP2350WATCHDOG_DEV_BOOTSEL_MS > 0
  uint64_t elapsed_ms = 0;
#endif
  unsigned long scratch = 0;
  int fd;

  (void)argc;
  (void)argv;

  if (CONFIG_SYSTEM_RP2350WATCHDOG_PING_MS >=
      CONFIG_SYSTEM_RP2350WATCHDOG_TIMEOUT_MS)
    {
      fprintf(stderr, "rp2350watchdog: ping period must be below timeout\n");
      return 1;
    }

#ifdef CONFIG_SYSTEM_RP2350WATCHDOG_BOOTSEL_RECOVERY
  scratch = RP2350_BOOTSEL_GUARD_MAGIC;
#endif

  fd = open(CONFIG_SYSTEM_RP2350WATCHDOG_DEVPATH, O_RDONLY);
  if (fd < 0)
    {
      fprintf(stderr, "rp2350watchdog: open %s failed: %d\n",
              CONFIG_SYSTEM_RP2350WATCHDOG_DEVPATH, errno);
      return 1;
    }

  if (rp2350watchdog_scratch(fd, scratch) < 0 ||
      ioctl(fd, WDIOC_SETTIMEOUT,
            CONFIG_SYSTEM_RP2350WATCHDOG_TIMEOUT_MS) < 0)
    {
      fprintf(stderr, "rp2350watchdog: start failed: %d\n", errno);
      close(fd);
      return 1;
    }

  if (ioctl(fd, WDIOC_START, 0) < 0 && errno != EBUSY)
    {
      fprintf(stderr, "rp2350watchdog: start failed: %d\n", errno);
      close(fd);
      return 1;
    }

  printf("rp2350watchdog: normal-reset watchdog active (%d/%d ms)\n",
         CONFIG_SYSTEM_RP2350WATCHDOG_PING_MS,
         CONFIG_SYSTEM_RP2350WATCHDOG_TIMEOUT_MS);

  for (; ; )
    {
      usleep(CONFIG_SYSTEM_RP2350WATCHDOG_PING_MS * 1000);

#if CONFIG_SYSTEM_RP2350WATCHDOG_DEV_BOOTSEL_MS > 0
      elapsed_ms += CONFIG_SYSTEM_RP2350WATCHDOG_PING_MS;
      if (elapsed_ms >= CONFIG_SYSTEM_RP2350WATCHDOG_DEV_BOOTSEL_MS)
        {
          printf("rp2350watchdog: development BOOTSEL timeout\n");
          boardctl(BOARDIOC_RESET, 3);
        }
#endif

      if (ioctl(fd, WDIOC_KEEPALIVE, 0) < 0)
        {
          fprintf(stderr, "rp2350watchdog: keepalive failed: %d\n", errno);
          close(fd);
          return 1;
        }
    }
}
