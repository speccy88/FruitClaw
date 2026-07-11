/* SPDX-License-Identifier: Apache-2.0 */

#include "fruitclaw.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
#  include <arch/rp23xx/watchdog.h>
#  include <arch/board/board.h>
#  include <nuttx/arch.h>
#  include <nuttx/board.h>
#  include <nuttx/clock.h>
#  include <nuttx/timers/watchdog.h>
#  include <hardware/rp23xx_powman.h>
#  include <hardware/rp23xx_psm.h>
#  include <hardware/rp23xx_ticks.h>
#  include <hardware/rp23xx_watchdog.h>
#endif

#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
#define FC_GUARD_WD_ENABLE RP23XX_WATCHDOG_CTRL_ENABLE
#define FC_GUARD_WD_MASK \
  (RP23XX_WATCHDOG_CTRL_ENABLE | RP23XX_WATCHDOG_CTRL_PAUSE_DBG0 | \
   RP23XX_WATCHDOG_CTRL_PAUSE_DBG1 | RP23XX_WATCHDOG_CTRL_PAUSE_JTAG)
#define FC_GUARD_WD_TRIGGER_MASK \
  (FC_GUARD_WD_MASK | RP23XX_WATCHDOG_CTRL_TRIGGER)
#define FC_GUARD_TICK_CYCLES (BOARD_REF_FREQ / 1000000)

static uint32_t fc_guard_bootguard_magic(void)
{
#ifdef CONFIG_FRUITCLAW_GUARD_BOOTSEL_RECOVERY
  return FC_BOOTGUARD_MAGIC;
#else
  return 0;
#endif
}

static int fc_guard_ioctl(int fd, int cmd, unsigned long arg,
                          const char *name)
{
  int ret;

  ret = ioctl(fd, cmd, arg);
  if (ret < 0)
    {
      int err = -errno;
      FC_LOGE("exec guard %s failed: %d", name, errno);
      return err;
    }

  return 0;
}

static void fc_guard_hw_start_tick(void)
{
  putreg32(FC_GUARD_TICK_CYCLES, RP23XX_TICKS_WATCHDOG_CYCLES);
  putreg32(RP23XX_TICKS_WATCHDOG_CTRL_EN, RP23XX_TICKS_WATCHDOG_CTRL);
}

static void fc_guard_hw_set_scratch(uint32_t timeout_ms, uint32_t stage)
{
  putreg32(fc_guard_bootguard_magic(), RP23XX_POWMAN_SCRATCH0);
  putreg32(timeout_ms, RP23XX_POWMAN_SCRATCH1);
  putreg32(stage, RP23XX_POWMAN_SCRATCH2);
  putreg32(fc_guard_bootguard_magic(), RP23XX_WATCHDOG_SCRATCH(0));
  putreg32(timeout_ms, RP23XX_WATCHDOG_SCRATCH(1));
  putreg32(stage, RP23XX_WATCHDOG_SCRATCH(2));
}

static void fc_guard_hw_clear_scratch(void)
{
  putreg32(0, RP23XX_POWMAN_SCRATCH0);
  putreg32(0, RP23XX_POWMAN_SCRATCH1);
  putreg32(0, RP23XX_POWMAN_SCRATCH2);
  putreg32(0, RP23XX_WATCHDOG_SCRATCH(0));
  putreg32(0, RP23XX_WATCHDOG_SCRATCH(1));
  putreg32(0, RP23XX_WATCHDOG_SCRATCH(2));
}

static void fc_guard_hw_arm(uint32_t timeout_ms, uint32_t stage)
{
  uint32_t timeout_us = timeout_ms * USEC_PER_MSEC;

  if (timeout_us > RP23XX_WATCHDOG_LOAD_MASK)
    {
      timeout_us = RP23XX_WATCHDOG_LOAD_MASK;
    }

  fc_guard_hw_set_scratch(timeout_ms, stage);
  fc_guard_hw_start_tick();
  putreg32(timeout_us, RP23XX_WATCHDOG_LOAD);
  putreg32(RP23XX_PSM_WDSEL_BITS & ~(RP23XX_PSM_XOSC | RP23XX_PSM_ROSC),
           RP23XX_PSM_WDSEL);
  modreg32(FC_GUARD_WD_ENABLE, FC_GUARD_WD_MASK,
           RP23XX_WATCHDOG_CTRL);
}

static void fc_guard_hw_stop(void)
{
  modreg32(0, RP23XX_WATCHDOG_CTRL_ENABLE, RP23XX_WATCHDOG_CTRL);
}

static void fc_guard_hw_reset_now(uint32_t stage)
{
  fc_guard_hw_set_scratch(CONFIG_FRUITCLAW_GUARD_TIMEOUT_MS, stage);
  fc_guard_hw_start_tick();
  putreg32(RP23XX_PSM_WDSEL_BITS & ~(RP23XX_PSM_XOSC | RP23XX_PSM_ROSC),
           RP23XX_PSM_WDSEL);
  modreg32(FC_GUARD_WD_ENABLE | RP23XX_WATCHDOG_CTRL_TRIGGER,
           FC_GUARD_WD_TRIGGER_MASK, RP23XX_WATCHDOG_CTRL);
}

static pthread_mutex_t g_guard_lock = PTHREAD_MUTEX_INITIALIZER;
#ifdef CONFIG_FRUITCLAW_ENABLE_SESSION_GUARD
static pthread_t g_session_thread;
#endif
static pthread_t g_uptime_thread;
static bool g_session_started;
static bool g_session_stop;
static bool g_session_tripped;
static bool g_session_watchdog_armed;
static bool g_uptime_started;
static bool g_uptime_tripped;
static bool g_short_guard_active;
static bool g_short_guard_owner_valid;
static int g_short_guard_fd = -1;
static pthread_t g_short_guard_owner;
static uint32_t g_guard_active_stage;
static uint32_t g_guard_last_stage;
static int64_t g_session_last_ms;
static int64_t g_uptime_start_ms;
static char g_session_last_source[FC_SOURCE_LEN];

static int64_t fc_guard_mono_ms(void);

static void fc_guard_takeover_board_bootguard(void)
{
#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD
  bool active;

  pthread_mutex_lock(&g_guard_lock);
  active = g_short_guard_active;
  pthread_mutex_unlock(&g_guard_lock);

  if (!active)
    {
      board_fruitjam_bootguard_disarm();
    }
#endif
}

static void fc_guard_set_short_active(bool active, uint32_t stage)
{
  pthread_mutex_lock(&g_guard_lock);
  g_short_guard_active = active;
  if (active)
    {
      g_guard_active_stage = stage;
      g_guard_last_stage = stage;
      g_short_guard_fd = -1;
      g_short_guard_owner = pthread_self();
      g_short_guard_owner_valid = true;
    }
  else
    {
      g_guard_active_stage = 0;
      g_short_guard_fd = -1;
      g_short_guard_owner_valid = false;
    }

  pthread_mutex_unlock(&g_guard_lock);
}

static void fc_guard_recover_now(uint32_t stage)
{
  fc_guard_set_short_active(true, stage);
  fc_guard_hw_set_scratch(CONFIG_FRUITCLAW_GUARD_TIMEOUT_MS, stage);

#ifdef CONFIG_FRUITCLAW_GUARD_BOOTSEL_RECOVERY
  /* Use the watchdog path for BOOTSEL recovery.  Direct ROM BOOTSEL reboot can
   * wedge if USB/network state is already unhealthy; watchdog reset plus the
   * bootguard scratch marker is the path designed for unattended recovery.
   */

  fc_guard_hw_reset_now(stage);
#else
  up_systemreset();

  fc_guard_hw_reset_now(stage);
#endif
  for (; ; )
    {
      sleep(60);
    }
}

static int fc_guard_begin_short(uint32_t stage)
{
  int64_t deadline = fc_guard_mono_ms() +
    CONFIG_FRUITCLAW_GUARD_ACQUIRE_TIMEOUT_MS;

  for (; ; )
    {
      int ret = 0;
      uint32_t active_stage = 0;

      pthread_mutex_lock(&g_guard_lock);
      if (g_uptime_tripped)
        {
          ret = -ETIMEDOUT;
        }
      else if (g_short_guard_active)
        {
          if (g_short_guard_owner_valid &&
              pthread_equal(g_short_guard_owner, pthread_self()))
            {
              ret = 1;
            }
          else
            {
              ret = -EBUSY;
              active_stage = g_guard_active_stage;
            }
        }
      else
        {
          g_short_guard_active = true;
          g_guard_active_stage = stage;
          g_guard_last_stage = stage;
          g_short_guard_fd = -1;
          g_short_guard_owner = pthread_self();
          g_short_guard_owner_valid = true;
        }

      pthread_mutex_unlock(&g_guard_lock);

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD
      if (ret == 0)
        {
          board_fruitjam_bootguard_disarm();
        }
#endif

      if (ret == 0 || ret != -EBUSY ||
          CONFIG_FRUITCLAW_GUARD_ACQUIRE_TIMEOUT_MS <= 0 ||
          fc_guard_mono_ms() >= deadline)
        {
          return ret;
        }

      if (active_stage != 0)
        {
          fc_guard_hw_arm(CONFIG_FRUITCLAW_GUARD_TIMEOUT_MS, active_stage);
          fc_guard_session_heartbeat("guard-wait");
        }

      usleep(100 * 1000);
    }
}

static void fc_guard_bind_short_fd(int fd)
{
  pthread_mutex_lock(&g_guard_lock);
  if (g_short_guard_active)
    {
      g_short_guard_fd = fd;
    }

  pthread_mutex_unlock(&g_guard_lock);
}

static bool fc_guard_short_fd_matches(int fd)
{
  bool matches;

  pthread_mutex_lock(&g_guard_lock);
  matches = g_short_guard_active &&
            (g_short_guard_fd < 0 || g_short_guard_fd == fd);
  pthread_mutex_unlock(&g_guard_lock);
  return matches;
}

static void fc_guard_set_session_watchdog(bool armed)
{
  pthread_mutex_lock(&g_guard_lock);
  g_session_watchdog_armed = armed;
  pthread_mutex_unlock(&g_guard_lock);
}

static void fc_guard_attr_set_feeder_priority(pthread_attr_t *attr)
{
  struct sched_param param;
  int max_priority;
  int min_priority;

  if (attr == NULL)
    {
      return;
    }

  max_priority = sched_get_priority_max(SCHED_FIFO);
  min_priority = sched_get_priority_min(SCHED_FIFO);
  if (max_priority < min_priority)
    {
      return;
    }

  memset(&param, 0, sizeof(param));
  param.sched_priority = max_priority > min_priority ?
                         max_priority - 1 : max_priority;
  pthread_attr_setschedpolicy(attr, SCHED_FIFO);
  pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedparam(attr, &param);
}

static int64_t fc_guard_mono_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
    {
      return (int64_t)ts.tv_sec * MSEC_PER_SEC +
             ts.tv_nsec / NSEC_PER_MSEC;
    }

  return (int64_t)TICK2MSEC(clock_systime_ticks());
}

static bool fc_guard_uptime_due_locked(int64_t now)
{
  return CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS > 0 &&
         g_uptime_started && !g_uptime_tripped &&
         g_uptime_start_ms > 0 &&
         now - g_uptime_start_ms >= CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS;
}

static bool fc_guard_uptime_is_tripped(void)
{
  bool tripped;

  pthread_mutex_lock(&g_guard_lock);
  tripped = g_uptime_tripped;
  pthread_mutex_unlock(&g_guard_lock);
  return tripped;
}

static void fc_guard_mark_uptime_tripped(void)
{
  pthread_mutex_lock(&g_guard_lock);
  g_uptime_tripped = true;
  pthread_mutex_unlock(&g_guard_lock);
}

static int fc_guard_program_fd(int fd, uint32_t timeout_ms, uint32_t stage)
{
  int ret;

  ret = fc_guard_ioctl(fd, WDIOC_SET_SCRATCH0,
                       fc_guard_bootguard_magic(),
                       "WDIOC_SET_SCRATCH0");
  if (ret < 0)
    {
      return ret;
    }

  ret = fc_guard_ioctl(fd, WDIOC_SET_SCRATCH1, timeout_ms,
                       "WDIOC_SET_SCRATCH1");
  if (ret < 0)
    {
      return ret;
    }

  ret = fc_guard_ioctl(fd, WDIOC_SET_SCRATCH2, stage,
                       "WDIOC_SET_SCRATCH2");
  if (ret < 0)
    {
      return ret;
    }

  ret = fc_guard_ioctl(fd, WDIOC_SETTIMEOUT, timeout_ms,
                       "WDIOC_SETTIMEOUT");
  if (ret < 0)
    {
      return ret;
    }

  ret = ioctl(fd, WDIOC_START, 0);
  if (ret < 0 && errno != EBUSY)
    {
      int err = -errno;

      FC_LOGE("exec guard WDIOC_START failed: %d", errno);
      return err;
    }

  fc_guard_hw_arm(timeout_ms, stage);
  return 0;
}

static void *fc_guard_uptime_thread(void *arg)
{
  (void)arg;

  for (; ; )
    {
      int64_t now = fc_guard_mono_ms();
      int64_t elapsed;
      int64_t remaining;

      pthread_mutex_lock(&g_guard_lock);
      elapsed = now - g_uptime_start_ms;
      pthread_mutex_unlock(&g_guard_lock);

      remaining = (int64_t)CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS - elapsed;
      if (remaining <= 0)
        {
          break;
        }

      if (remaining > 5000)
        {
          remaining = 5000;
        }

      if (remaining >= MSEC_PER_SEC)
        {
          sleep((unsigned int)((remaining + MSEC_PER_SEC - 1) /
                              MSEC_PER_SEC));
        }
      else
        {
          usleep((useconds_t)remaining * USEC_PER_MSEC);
        }
    }

  pthread_mutex_lock(&g_guard_lock);
  g_uptime_tripped = true;
  pthread_mutex_unlock(&g_guard_lock);

  FC_LOGE("max uptime guard expired after %u ms; arming watchdog recovery",
          (unsigned int)CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS);
  fc_guard_recover_now(FC_GUARD_STAGE_UPTIME);

  return NULL;
}

static void *fc_guard_long_thread(void *arg)
{
  fc_guard_long_t *guard = arg;
  bool keepalive_warned = false;

  while (!guard->stop)
    {
      int64_t now = fc_guard_mono_ms();

      if (now >= guard->deadline_ms)
        {
          guard->expired = true;
          FC_LOGE("long guard expired stage=0x%08lx timeout=%lu ms",
                  (unsigned long)guard->stage,
                  (unsigned long)guard->timeout_ms);
          fc_guard_recover_now(guard->stage);
        }

      /* Feed the RP23xx watchdog directly.  The /dev/watchdog keepalive path
       * is still attempted below for driver state, but a transient ioctl
       * failure must not turn an otherwise healthy long HTTP/TLS operation
       * into an immediate watchdog recovery trip.
       */

      fc_guard_hw_arm(CONFIG_FRUITCLAW_GUARD_TIMEOUT_MS, guard->stage);
      if (fc_guard_ioctl(guard->fd, WDIOC_KEEPALIVE, 0,
                         "WDIOC_KEEPALIVE") < 0)
        {
          if (!keepalive_warned)
            {
              keepalive_warned = true;
              FC_LOGW("long guard ioctl keepalive failed stage=0x%08lx; "
                      "continuing with direct RP23xx feed",
                      (unsigned long)guard->stage);
            }
        }

      fc_guard_session_heartbeat("longguard");
      sleep(2);
    }

  return NULL;
}

#ifdef CONFIG_FRUITCLAW_ENABLE_SESSION_GUARD
static void *fc_guard_session_thread(void *arg)
{
  bool session_armed = false;
  bool arm_warned = false;
  bool keepalive_warned = false;
  int fd;

  (void)arg;

  fd = open(CONFIG_FRUITCLAW_GUARD_DEVPATH, O_RDONLY);
  if (fd < 0)
    {
      FC_LOGE("session guard open %s failed: %d",
              CONFIG_FRUITCLAW_GUARD_DEVPATH, errno);
    }

  for (; ; )
    {
      int64_t now = fc_guard_mono_ms();
      int64_t last;
      bool short_active;
      bool stop;
      bool uptime_due;

      pthread_mutex_lock(&g_guard_lock);
      last = g_session_last_ms;
      short_active = g_short_guard_active;
      stop = g_session_stop;
      uptime_due = fc_guard_uptime_due_locked(now);
      pthread_mutex_unlock(&g_guard_lock);

      if (stop)
        {
          break;
        }

      if (uptime_due)
        {
          fc_guard_mark_uptime_tripped();
          FC_LOGE("max uptime guard expired from session monitor after "
                  "%u ms; arming watchdog recovery",
                  (unsigned int)CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS);
          fc_guard_recover_now(FC_GUARD_STAGE_UPTIME);
        }

      if (short_active)
        {
          session_armed = false;
          fc_guard_set_session_watchdog(false);
          sleep(1);
          continue;
        }

      if (!session_armed)
        {
          int ret = 0;

          if (fd >= 0)
            {
              ret = fc_guard_program_fd(fd,
                        CONFIG_FRUITCLAW_SESSION_GUARD_HW_TIMEOUT_MS,
                        FC_GUARD_STAGE_SESSION);
            }

          if (ret < 0)
            {
              if (!arm_warned)
                {
                  arm_warned = true;
                  FC_LOGW("session guard /dev/watchdog arm failed: %d; "
                          "continuing with direct RP23xx feed", ret);
                }
            }

          session_armed = true;
          fc_guard_set_session_watchdog(true);
        }

      if (now - last > CONFIG_FRUITCLAW_SESSION_GUARD_TIMEOUT_MS)
        {
          pthread_mutex_lock(&g_guard_lock);
          g_session_tripped = true;
          pthread_mutex_unlock(&g_guard_lock);

          fc_guard_set_short_active(true, FC_GUARD_STAGE_SESSION);
          fc_guard_recover_now(FC_GUARD_STAGE_SESSION);
        }

      fc_guard_hw_arm(CONFIG_FRUITCLAW_SESSION_GUARD_HW_TIMEOUT_MS,
                      FC_GUARD_STAGE_SESSION);
      if (fd >= 0 &&
          fc_guard_ioctl(fd, WDIOC_KEEPALIVE, 0, "WDIOC_KEEPALIVE") < 0)
        {
          if (!keepalive_warned)
            {
              keepalive_warned = true;
              FC_LOGW("session guard ioctl keepalive failed; "
                      "continuing with direct RP23xx feed");
            }
        }

      sleep(2);
    }

  fc_guard_hw_clear_scratch();
  fc_guard_hw_stop();
  if (fd >= 0)
    {
      fc_guard_ioctl(fd, WDIOC_STOP, 0, "WDIOC_STOP");
      close(fd);
    }

  fc_guard_set_session_watchdog(false);
  fc_guard_set_short_active(false, 0);
  return NULL;
}
#endif
#endif

int fc_guard_arm(uint32_t stage, int *guard_fd)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
  int fd;
  int ret;

  if (guard_fd == NULL)
    {
      return -EINVAL;
    }

  *guard_fd = -1;
  ret = fc_guard_begin_short(stage);
  if (ret > 0)
    {
      return 0;
    }

  if (ret < 0)
    {
      FC_LOGE("exec guard begin failed stage=0x%08lx ret=%d",
              (unsigned long)stage, ret);
      return ret;
    }

  fd = open(CONFIG_FRUITCLAW_GUARD_DEVPATH, O_RDONLY);
  if (fd < 0)
    {
      int err = -errno;
      FC_LOGE("exec guard open %s failed: %d",
              CONFIG_FRUITCLAW_GUARD_DEVPATH, errno);
      fc_guard_set_short_active(false, 0);
      return err;
    }

  fc_guard_bind_short_fd(fd);
  ret = fc_guard_ioctl(fd, WDIOC_STOP, 0, "WDIOC_STOP");
  if (ret < 0)
    {
      goto errout;
    }

  ret = fc_guard_program_fd(fd, CONFIG_FRUITCLAW_GUARD_TIMEOUT_MS, stage);
  if (ret < 0)
    {
      goto errout;
    }

  *guard_fd = fd;
  FC_LOGD("exec guard armed stage=0x%08lx timeout=%u ms",
          (unsigned long)stage, CONFIG_FRUITCLAW_GUARD_TIMEOUT_MS);
  return 0;

errout:
  fc_guard_hw_clear_scratch();
  fc_guard_hw_stop();
  fc_guard_set_short_active(false, 0);
  close(fd);
  return ret;
#else
  if (guard_fd == NULL)
    {
      return -EINVAL;
    }

  *guard_fd = -1;
  (void)stage;
  return 0;
#endif
}

int fc_guard_disarm(int guard_fd)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
  int ret = 0;

  if (guard_fd < 0)
    {
      return 0;
    }

  if (fc_guard_uptime_is_tripped())
    {
      FC_LOGE("max uptime guard is tripped; leaving watchdog armed");
      close(guard_fd);
      return 0;
    }

  if (!fc_guard_short_fd_matches(guard_fd))
    {
      FC_LOGW("exec guard fd %d is stale; active guard left armed",
              guard_fd);
      close(guard_fd);
      return 0;
    }

  fc_guard_hw_clear_scratch();

  if (fc_guard_ioctl(guard_fd, WDIOC_SET_SCRATCH0, 0,
                     "WDIOC_SET_SCRATCH0") < 0)
    {
      ret = -EIO;
    }

  if (fc_guard_ioctl(guard_fd, WDIOC_SET_SCRATCH1, 0,
                     "WDIOC_SET_SCRATCH1") < 0)
    {
      ret = -EIO;
    }

  if (fc_guard_ioctl(guard_fd, WDIOC_SET_SCRATCH2, 0,
                     "WDIOC_SET_SCRATCH2") < 0)
    {
      ret = -EIO;
    }

  if (fc_guard_ioctl(guard_fd, WDIOC_STOP, 0, "WDIOC_STOP") < 0)
    {
      ret = -EIO;
    }

  fc_guard_hw_stop();
  close(guard_fd);
  fc_guard_set_short_active(false, 0);
  FC_LOGD("exec guard disarmed");
  return ret;
#else
  (void)guard_fd;
  return 0;
#endif
}

int fc_guard_long_start(uint32_t stage, uint32_t timeout_ms,
                        fc_guard_long_t *guard)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
  pthread_attr_t attr;
  int ret;

  if (guard == NULL)
    {
      return -EINVAL;
    }

  memset(guard, 0, sizeof(*guard));
  guard->fd = -1;
  guard->stage = stage;
  guard->timeout_ms = timeout_ms;
  guard->deadline_ms = fc_guard_mono_ms() + timeout_ms;

  ret = fc_guard_begin_short(stage);
  if (ret > 0)
    {
      guard->reentrant = true;
      return 0;
    }

  if (ret < 0)
    {
      FC_LOGE("long guard begin failed stage=0x%08lx ret=%d",
              (unsigned long)stage, ret);
      return ret;
    }

  guard->fd = open(CONFIG_FRUITCLAW_GUARD_DEVPATH, O_RDONLY);
  if (guard->fd < 0)
    {
      int err = -errno;
      FC_LOGE("long guard open %s failed: %d",
              CONFIG_FRUITCLAW_GUARD_DEVPATH, errno);
      fc_guard_set_short_active(false, 0);
      return err;
    }

  fc_guard_bind_short_fd(guard->fd);
  ret = fc_guard_ioctl(guard->fd, WDIOC_STOP, 0, "WDIOC_STOP");
  if (ret < 0)
    {
      fc_guard_hw_clear_scratch();
      fc_guard_hw_stop();
      fc_guard_set_short_active(false, 0);
      close(guard->fd);
      guard->fd = -1;
      return ret;
    }

  /* RP23xx watchdog hardware tops out at roughly 16 seconds.  Long guards
   * keep that real watchdog fed from a high-priority feeder thread and use
   * guard->deadline_ms to decide when to stop feeding for recovery.
   */

  ret = fc_guard_program_fd(guard->fd, CONFIG_FRUITCLAW_GUARD_TIMEOUT_MS,
                            stage);
  if (ret < 0)
    {
      fc_guard_hw_clear_scratch();
      fc_guard_hw_stop();
      fc_guard_set_short_active(false, 0);
      close(guard->fd);
      guard->fd = -1;
      return ret;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_FRUITCLAW_GUARD_STACKSIZE);
  fc_guard_attr_set_feeder_priority(&attr);
  ret = pthread_create(&guard->thread, &attr, fc_guard_long_thread, guard);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      fc_guard_disarm(guard->fd);
      guard->fd = -1;
      return -ret;
    }

  guard->started = true;
  FC_LOGD("long guard armed stage=0x%08lx timeout=%lu ms",
          (unsigned long)stage, (unsigned long)timeout_ms);
  return 0;
#else
  if (guard == NULL)
    {
      return -EINVAL;
    }

  memset(guard, 0, sizeof(*guard));
  guard->fd = -1;
  (void)stage;
  (void)timeout_ms;
  return 0;
#endif
}

void fc_guard_long_set_stage(fc_guard_long_t *guard, uint32_t stage)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
  if (guard == NULL || stage == 0)
    {
      return;
    }

  guard->stage = stage;

  pthread_mutex_lock(&g_guard_lock);
  if (g_short_guard_active)
    {
      g_guard_active_stage = stage;
      g_guard_last_stage = stage;
    }

  pthread_mutex_unlock(&g_guard_lock);

  if (guard->started && guard->fd >= 0)
    {
      fc_guard_hw_arm(CONFIG_FRUITCLAW_GUARD_TIMEOUT_MS, stage);
    }
#else
  (void)guard;
  (void)stage;
#endif
}

int fc_guard_long_stop(fc_guard_long_t *guard)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
  if (guard == NULL)
    {
      return -EINVAL;
    }

  if (!guard->started)
    {
      if (guard->reentrant)
        {
          return 0;
        }

      return fc_guard_disarm(guard->fd);
    }

  guard->stop = true;
  pthread_join(guard->thread, NULL);
  guard->started = false;

  if (guard->expired)
    {
      FC_LOGE("long guard expired; waiting for watchdog reset");
      for (; ; )
        {
          sleep(60);
        }
    }

  return fc_guard_disarm(guard->fd);
#else
  (void)guard;
  return 0;
#endif
}

int fc_guard_uptime_start(void)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
  pthread_attr_t attr;
  int ret;

  if (CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS <= 0)
    {
      return 0;
    }

  pthread_mutex_lock(&g_guard_lock);
  if (g_uptime_started)
    {
      pthread_mutex_unlock(&g_guard_lock);
      return 0;
    }

  g_uptime_start_ms = fc_guard_mono_ms();
  g_uptime_tripped = false;
  pthread_mutex_unlock(&g_guard_lock);

  fc_guard_takeover_board_bootguard();
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_FRUITCLAW_GUARD_STACKSIZE);
  fc_guard_attr_set_feeder_priority(&attr);
  ret = pthread_create(&g_uptime_thread, &attr,
                       fc_guard_uptime_thread, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      pthread_mutex_lock(&g_guard_lock);
      g_uptime_start_ms = 0;
      pthread_mutex_unlock(&g_guard_lock);
      return -ret;
    }

  pthread_mutex_lock(&g_guard_lock);
  g_uptime_started = true;
  pthread_mutex_unlock(&g_guard_lock);
  return 0;
#else
  return 0;
#endif
}

int fc_guard_session_start(void)
{
#if defined(CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD) && \
    defined(CONFIG_FRUITCLAW_ENABLE_SESSION_GUARD)
  pthread_attr_t attr;
  int ret;

  if (g_session_started)
    {
      return 0;
    }

  fc_guard_takeover_board_bootguard();
  fc_guard_session_heartbeat("start");
  g_session_stop = false;
  g_session_tripped = false;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_FRUITCLAW_GUARD_STACKSIZE);
  fc_guard_attr_set_feeder_priority(&attr);
  ret = pthread_create(&g_session_thread, &attr,
                       fc_guard_session_thread, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      return -ret;
    }

  g_session_started = true;
  return 0;
#else
  return 0;
#endif
}

void fc_guard_session_heartbeat(const char *source)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
  pthread_mutex_lock(&g_guard_lock);
  g_session_last_ms = fc_guard_mono_ms();
  fc_strlcpy(g_session_last_source, source ? source : "unknown",
             sizeof(g_session_last_source));
  pthread_mutex_unlock(&g_guard_lock);
#else
  (void)source;
#endif
}

void fc_guard_session_stop(void)
{
#if defined(CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD) && \
    defined(CONFIG_FRUITCLAW_ENABLE_SESSION_GUARD)
  pthread_mutex_lock(&g_guard_lock);
  g_session_stop = true;
  pthread_mutex_unlock(&g_guard_lock);
#endif
}

int fc_guard_prepare_controlled_reboot(void)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
#ifdef CONFIG_FRUITCLAW_ENABLE_SESSION_GUARD
  bool started;

  pthread_mutex_lock(&g_guard_lock);
  started = g_session_started;
  if (started)
    {
      g_session_stop = true;
    }
  pthread_mutex_unlock(&g_guard_lock);

  if (started)
    {
      pthread_join(g_session_thread, NULL);

      pthread_mutex_lock(&g_guard_lock);
      g_session_started = false;
      g_session_stop = false;
      g_session_tripped = false;
      pthread_mutex_unlock(&g_guard_lock);
    }
#endif

  fc_guard_hw_clear_scratch();
  fc_guard_hw_stop();
#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD
  board_fruitjam_bootguard_disarm();
#endif
#endif

  return 0;
}

int fc_guard_force_recovery(uint32_t stage)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
  FC_LOGE("forcing watchdog recovery stage=0x%08" PRIx32, stage);
  fc_guard_recover_now(stage);

  return 0;
#else
  (void)stage;
  return -ENOSYS;
#endif
}

int fc_guard_status(char *out, size_t out_len)
{
  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
    {
      int64_t now = fc_guard_mono_ms();
      int64_t last;
      bool session_started;
      bool session_tripped;
      bool session_watchdog;
      bool uptime_started;
      bool uptime_tripped;
      bool short_active;
      uint32_t active_stage;
      uint32_t last_stage;
      int64_t uptime_start;
      char source[FC_SOURCE_LEN];

      pthread_mutex_lock(&g_guard_lock);
      last = g_session_last_ms;
      session_started = g_session_started;
      session_tripped = g_session_tripped;
      session_watchdog = g_session_watchdog_armed;
      uptime_started = g_uptime_started;
      uptime_tripped = g_uptime_tripped;
      short_active = g_short_guard_active;
      active_stage = g_guard_active_stage;
      last_stage = g_guard_last_stage;
      uptime_start = g_uptime_start_ms;
      fc_strlcpy(source, g_session_last_source, sizeof(source));
      pthread_mutex_unlock(&g_guard_lock);

      int64_t age = last > 0 ? now - last : 0;
      int64_t uptime_remaining = -1;

      if (uptime_started && uptime_start > 0)
        {
          uptime_remaining =
            (int64_t)CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS -
            (now - uptime_start);
          if (uptime_remaining < 0)
            {
              uptime_remaining = 0;
            }
        }

      snprintf(out, out_len,
               "exec=enabled bootsel_recovery=%s short_active=%s "
               "recovery_target=%s active_stage=0x%08lx "
               "last_stage=0x%08lx session=%s "
               "watchdog=%s tripped=%s src=%s age_ms=%lld "
               "uptime_guard=%s "
               "uptime_tripped=%s uptime_remaining_ms=%lld "
               "max_uptime_ms=%u",
#ifdef CONFIG_FRUITCLAW_GUARD_BOOTSEL_RECOVERY
               "enabled",
#else
               "disabled",
#endif
               short_active ? "yes" : "no",
#ifdef CONFIG_FRUITCLAW_GUARD_BOOTSEL_RECOVERY
               "bootsel",
#else
               "reset",
#endif
               (unsigned long)active_stage,
               (unsigned long)last_stage,
               session_started ? "started" : "stopped",
               session_watchdog ? "armed" : "ready",
               session_tripped ? "yes" : "no",
               source[0] ? source : "none",
               (long long)age,
               uptime_started ? "started" :
               CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS > 0 ?
               "ready" : "disabled",
               uptime_tripped ? "yes" : "no",
               (long long)uptime_remaining,
               (unsigned int)CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS);
    }
#else
  snprintf(out, out_len, "exec=disabled session=disabled");
#endif

  return 0;
}
