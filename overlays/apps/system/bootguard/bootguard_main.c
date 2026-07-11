/****************************************************************************
 * apps/system/bootguard/bootguard_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <arch/rp23xx/watchdog.h>
#if defined(CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD)
#  include <arch/board/board.h>
#endif
#include <nuttx/timers/watchdog.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BOOTGUARD_MAGIC 0x464a4247 /* "FJBG" */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void bootguard_usage(FAR const char *progname)
{
  fprintf(stderr,
          "Usage: %s [status|off|on [timeout-ms]|kick]\n",
          progname);
}

static int bootguard_open(void)
{
  int fd;

  fd = open(CONFIG_SYSTEM_BOOTGUARD_DEVPATH, O_RDONLY);
  if (fd < 0)
    {
      fprintf(stderr, "ERROR: open(%s) failed: %d\n",
              CONFIG_SYSTEM_BOOTGUARD_DEVPATH, errno);
    }

  return fd;
}

static int bootguard_getscratch(int fd, FAR uint32_t *scratch)
{
  int ret;

  ret = ioctl(fd, WDIOC_GET_SCRATCH0, (unsigned long)((uintptr_t)scratch));
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: WDIOC_GET_SCRATCH0 failed: %d\n", errno);
    }

  return ret;
}

static int bootguard_status(int fd)
{
  struct watchdog_status_s status;
  uint32_t scratch0 = 0;
  uint32_t scratch1 = 0;
  uint32_t scratch2 = 0;
  int ret;

  ret = bootguard_getscratch(fd, &scratch0);
  if (ret < 0)
    {
      return EXIT_FAILURE;
    }

  ret = ioctl(fd, WDIOC_GET_SCRATCH1, (unsigned long)((uintptr_t)&scratch1));
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: WDIOC_GET_SCRATCH1 failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  ret = ioctl(fd, WDIOC_GET_SCRATCH2, (unsigned long)((uintptr_t)&scratch2));
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: WDIOC_GET_SCRATCH2 failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  ret = ioctl(fd, WDIOC_GETSTATUS, (unsigned long)((uintptr_t)&status));
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: WDIOC_GETSTATUS failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  printf("bootguard: %s\n",
         scratch0 == BOOTGUARD_MAGIC ? "armed" : "disarmed");
  printf("scratch0: 0x%08" PRIx32 "\n", scratch0);
  printf("timeout-ms: %" PRIu32 "\n", scratch1);
  printf("diag: 0x%08" PRIx32 "\n", scratch2);
  printf("watchdog-flags: 0x%08" PRIx32 "\n", status.flags);
  printf("watchdog-timeout-ms: %" PRIu32 "\n", status.timeout);
  printf("watchdog-timeleft-ms: %" PRIu32 "\n", status.timeleft);

  return EXIT_SUCCESS;
}

static int bootguard_off(int fd)
{
#if defined(CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD)
  board_fruitjam_bootguard_disarm();
  printf("bootguard: disarmed\n");
  return EXIT_SUCCESS;
#else
  int ret;

  ret = ioctl(fd, WDIOC_SET_SCRATCH0, 0);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: WDIOC_SET_SCRATCH0 failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  ret = ioctl(fd, WDIOC_SET_SCRATCH1, 0);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: WDIOC_SET_SCRATCH1 failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  ret = ioctl(fd, WDIOC_SET_SCRATCH2, 0);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: WDIOC_SET_SCRATCH2 failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  ret = ioctl(fd, WDIOC_STOP, 0);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: WDIOC_STOP failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  printf("bootguard: disarmed\n");
  return EXIT_SUCCESS;
#endif
}

static int bootguard_on(int fd, int argc, FAR char *argv[])
{
  uint32_t timeout =
#if defined(CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD)
    CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD_TIMEOUT_MS;
#else
    15000;
#endif
#if !defined(CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD)
  int ret;
#endif

  if (argc > 2)
    {
      timeout = strtoul(argv[2], NULL, 0);
      if (timeout == 0)
        {
          bootguard_usage(argv[0]);
          return EXIT_FAILURE;
        }
    }

#if defined(CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD)
  (void)fd;

  board_fruitjam_bootguard_arm_timeout(timeout);
  printf("bootguard: armed for %" PRIu32 " ms\n", timeout);
  return EXIT_SUCCESS;
#else
  ret = ioctl(fd, WDIOC_SET_SCRATCH0, BOOTGUARD_MAGIC);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: WDIOC_SET_SCRATCH0 failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  ret = ioctl(fd, WDIOC_SET_SCRATCH1, timeout);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: WDIOC_SET_SCRATCH1 failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  ret = ioctl(fd, WDIOC_SET_SCRATCH2, 0);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: WDIOC_SET_SCRATCH2 failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  ret = ioctl(fd, WDIOC_SETTIMEOUT, timeout);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: WDIOC_SETTIMEOUT failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  ret = ioctl(fd, WDIOC_START, 0);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: WDIOC_START failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  printf("bootguard: armed for %" PRIu32 " ms\n", timeout);
  return EXIT_SUCCESS;
#endif
}

static int bootguard_kick(int fd)
{
  int ret;

  ret = ioctl(fd, WDIOC_KEEPALIVE, 0);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: WDIOC_KEEPALIVE failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  printf("bootguard: kicked\n");
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *cmd = "status";
  int exitcode;
  int fd;

  if (argc > 1)
    {
      cmd = argv[1];
    }

  fd = bootguard_open();
  if (fd < 0)
    {
      return EXIT_FAILURE;
    }

  if (strcmp(cmd, "status") == 0)
    {
      exitcode = bootguard_status(fd);
    }
  else if (strcmp(cmd, "off") == 0 || strcmp(cmd, "disarm") == 0)
    {
      exitcode = bootguard_off(fd);
    }
  else if (strcmp(cmd, "on") == 0 || strcmp(cmd, "arm") == 0)
    {
      exitcode = bootguard_on(fd, argc, argv);
    }
  else if (strcmp(cmd, "kick") == 0)
    {
      exitcode = bootguard_kick(fd);
    }
  else
    {
      bootguard_usage(argv[0]);
      exitcode = EXIT_FAILURE;
    }

  close(fd);
  return exitcode;
}
