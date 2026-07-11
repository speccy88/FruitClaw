/****************************************************************************
 * apps/system/piousbhost/piousbhost_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/param.h>
#include <sys/stat.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arch/board/board.h>
#include <nuttx/input/keyboard.h>
#include <nuttx/input/mouse.h>
#include <nuttx/usb/rp23xx_pio_usbhost.h>
#include <nuttx/usb/hub.h>
#include <nuttx/usb/usbhost.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_SYSTEM_PIOUSBHOST_GUARD_REFRESH_MS
#  define CONFIG_SYSTEM_PIOUSBHOST_GUARD_REFRESH_MS 2000
#endif

#ifndef CONFIG_SYSTEM_PIOUSBHOST_GUARD_WINDOW_MS
#  define CONFIG_SYSTEM_PIOUSBHOST_GUARD_WINDOW_MS 60000
#endif

#ifndef CONFIG_SYSTEM_PIOUSBHOST_EXPECT_KEYBOARDS
#  define CONFIG_SYSTEM_PIOUSBHOST_EXPECT_KEYBOARDS 0
#endif

#ifndef CONFIG_SYSTEM_PIOUSBHOST_EXPECT_MICE
#  define CONFIG_SYSTEM_PIOUSBHOST_EXPECT_MICE 0
#endif

#ifndef CONFIG_SYSTEM_PIOUSBHOST_HID_WAIT_SEC
#  define CONFIG_SYSTEM_PIOUSBHOST_HID_WAIT_SEC 12
#endif

#ifndef CONFIG_SYSTEM_PIOUSBHOST_HUB_SETTLE_SEC
#  define CONFIG_SYSTEM_PIOUSBHOST_HUB_SETTLE_SEC 16
#endif

#define PIOUSBHOST_HID_MAX  54
#define PIOUSBHOST_PATH_MAX 16

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR struct usbhost_connection_s *g_piousbhost_conn;
static bool g_piousbhost_initialized;
static bool g_piousbhost_waiter_started;
static bool g_piousbhost_post_start_done;
static volatile bool g_piousbhost_guard_active;
#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD
static volatile bool g_piousbhost_guard_feed_running;
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct piousbhost_hiddev_s
{
  char path[PIOUSBHOST_PATH_MAX];
  bool mouse;
  bool keyboard;
  bool rawkbd;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int piousbhost_guardon(void);
static void piousbhost_guardrefresh(void);
#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD
static int piousbhost_guardfeed_start(void);
#endif
static int piousbhost_hidwait_counts(unsigned int keyboards,
                                     unsigned int mice,
                                     unsigned int seconds,
                                     bool verbose);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void piousbhost_usage(FAR const char *progname)
{
  fprintf(stderr,
          "Usage: %s [init|waiter|start|start-guarded|recover|scan|"
          "vbuscycle|hubstatus|hubpower|guardon|guardoff|status|info|"
          "hidstatus|hidwait|hidwatch]\n",
          progname);
}

static int piousbhost_status(void)
{
  printf("piousbhost: init=%s waiter=%s\n",
         g_piousbhost_initialized ? "yes" : "no",
         g_piousbhost_waiter_started ? "yes" : "no");
  return EXIT_SUCCESS;
}

static int piousbhost_info(void)
{
  struct rp23xx_pio_usbhost_info_s info;
  int i;
  int ret;

  ret = rp23xx_pio_usbhost_info(&info);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: rp23xx_pio_usbhost_info failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  printf("piousbhost: init=%u timer=%u frame-thread=%u pending=%u "
         "active=%u frame=%" PRIu32 "\n",
         info.initialized, info.timer_started, info.frame_thread_started,
         info.frame_pending, info.frame_active, info.frame_number);
  printf("root: init=%u connected=%u fullspeed=%u suspended=%u event=%u "
         "line=%u dp=%u dm=%u\n",
         info.root_initialized, info.root_connected, info.root_fullspeed,
         info.root_suspended, info.root_event, info.root_line_state,
         info.root_pin_dp, info.root_pin_dm);
  printf("root-ints: ints=0x%08" PRIx32 " complete=0x%08" PRIx32
         " error=0x%08" PRIx32 " stalled=0x%08" PRIx32 "\n",
         info.root_ints, info.root_ep_complete, info.root_ep_error,
         info.root_ep_stalled);
  printf("hport: change=%u connected=%u class=%u funcaddr=%u speed=%u"
         " enum-count=%" PRIu32 " enum-ret=%d\n",
         info.change_pending, info.hport_connected, info.hport_has_class,
         info.hport_funcaddr, info.hport_speed, info.enum_count,
         info.last_enum_ret);
  printf("ctrl: dir=%s type=0x%02x req=0x%02x value=0x%04x index=%u "
         "len=%u phase=%u ret=%d addr=%u speed=%u parent=%u llep=%u\n",
         info.last_ctrl_dirin ? "in" : "out", info.last_ctrl_type,
         info.last_ctrl_req, info.last_ctrl_value, info.last_ctrl_index,
         info.last_ctrl_len, info.last_ctrl_phase, info.last_ctrl_ret,
         info.last_ctrl_funcaddr, info.last_ctrl_speed,
         info.last_ctrl_parent, info.last_ctrl_ll_ep);
  printf("prev-ctrl: dir=%s type=0x%02x req=0x%02x value=0x%04x index=%u "
         "len=%u phase=%u ret=%d\n",
         info.prev_ctrl_dirin ? "in" : "out", info.prev_ctrl_type,
         info.prev_ctrl_req, info.prev_ctrl_value, info.prev_ctrl_index,
         info.prev_ctrl_len, info.prev_ctrl_phase, info.prev_ctrl_ret);
  printf("addr-recover: setaddr0=%d retry1=%d probe0=%d len=%u",
         info.addr0_setaddr_ret, info.addr1_retry_ret,
         info.addr0_probe_ret, info.addr0_probe_len);
  for (i = 0; i < info.addr0_probe_len; i++)
    {
      printf(" %02x", info.addr0_probe_data[i]);
    }

  printf("\n");
  printf("setaddr-status: ret=%d in-result=%" PRId32
         " pid=0x%02" PRIx32 " expect=0x%02" PRIx32
         " setup-handshake=0x%02" PRIx32
         " tx-len=%" PRIu32 " tx-stage=%" PRIu32
         " tx-timeouts=%" PRIu32 " tx-irq=0x%08" PRIx32
         " tx-fdebug=0x%08" PRIx32 " tx-flevel=0x%08" PRIx32
         " tx-pc=%" PRIu32 "\n",
         info.setaddr_status_ret, info.setaddr_in_result,
         info.setaddr_in_pid, info.setaddr_in_expected_pid,
         info.setaddr_setup_handshake, info.setaddr_tx_len,
         info.setaddr_tx_wait_stage, info.setaddr_tx_timeout_count,
         info.setaddr_tx_irq, info.setaddr_tx_fdebug,
         info.setaddr_tx_flevel, info.setaddr_tx_pc);
  printf("ctrl-data: len=%u", info.last_ctrl_data_len);
  for (i = 0; i < info.last_ctrl_data_len; i++)
    {
      printf(" %02x", info.last_ctrl_data[i]);
    }

  printf("\n");
  printf("hub-desc: char=0x%04x lpsm=%u compound=%u oc=%u indicator=%u "
         "pwrdelay=%ums current=%u removable=0x%02x pwrmask=0x%02x\n",
         info.hub_characteristics, info.hub_lpsm, info.hub_compound,
         info.hub_ocmode, info.hub_indicator, info.hub_pwrondelay_ms,
         info.hub_ctrlcurrent, info.hub_devattached, info.hub_pwrctrlmask);
  printf("hub-ports: count=%u valid=0x%02x\n",
         info.hub_nports, info.hub_port_valid);
  for (i = 0; i < RP23XX_PIO_USBHOST_INFO_PORTS; i++)
    {
      uint16_t status;
      uint16_t change;
      int powerret;

      if ((info.hub_port_valid & (1u << i)) == 0)
        {
          continue;
        }

      status = info.hub_port_status[i];
      change = info.hub_port_change[i];
      powerret = (info.hub_port_power_valid & (1u << i)) != 0 ?
                 info.hub_port_power_ret[i] : -ENODATA;
      printf("  port%u: status=0x%04x change=0x%04x conn=%u en=%u "
             "pwr=%u ls=%u hs=%u oc=%u pwrret=%d\n",
             i + 1, status, change,
             (status & USBHUB_PORT_STAT_CONNECTION) != 0,
             (status & USBHUB_PORT_STAT_ENABLE) != 0,
             (status & USBHUB_PORT_STAT_POWER) != 0,
             (status & USBHUB_PORT_STAT_LOW_SPEED) != 0,
             (status & USBHUB_PORT_STAT_HIGH_SPEED) != 0,
             (status & USBHUB_PORT_STAT_OVERCURRENT) != 0,
             powerret);
    }

  printf("endpoints: count=%u\n", info.ep_count);
  for (i = 0; i < info.ep_count; i++)
    {
      int j;

      if (!info.ep[i].allocated)
        {
          continue;
        }

      printf("  ep%u: addr=%u ep=0x%02x type=0x%02x ll=%u speed=%u "
             "mps=%u int=%u in=%u pending=%u async=%u buflen=%zu "
             "result=%zd last-in-len=%u",
             i, info.ep[i].funcaddr, info.ep[i].epaddr,
             info.ep[i].xfrtype, info.ep[i].ll_ep, info.ep[i].speed,
             info.ep[i].maxpacket, info.ep[i].interval,
             info.ep[i].in, info.ep[i].pending, info.ep[i].async,
             info.ep[i].buflen, info.ep[i].result,
             info.ep[i].last_in_len);

      for (j = 0; j < info.ep[i].last_in_len; j++)
        {
          printf(" %02x", info.ep[i].last_in_data[j]);
        }

      printf("\n");
      if (info.ep[i].ll_valid)
        {
          printf("       ll: addr=%u ep=0x%02x size=%u attr=0x%02x "
                 "data=%u is-tx=%u has=%u actual=%u total=%u fail=%u\n",
                 info.ep[i].ll_dev_addr, info.ep[i].ll_ep_num,
                 info.ep[i].ll_size, info.ep[i].ll_attr,
                 info.ep[i].ll_data_id, info.ep[i].ll_is_tx,
                 info.ep[i].ll_has_transfer, info.ep[i].ll_actual_len,
                 info.ep[i].ll_total_len, info.ep[i].ll_failed_count);
        }
    }

  printf("ll-setup: fail=%" PRIu32 " handshake=0x%02" PRIx32
         " rx0=0x%02" PRIx32 " rx1=0x%02" PRIx32
         " failed-count=%" PRIu32 " rx-start-ok=%" PRIu32
         " rx-timeouts=%" PRIu32 " rx-irq=0x%08" PRIx32 "\n",
         info.ll_last_setup_fail, info.ll_last_setup_handshake,
         info.ll_last_setup_rx0, info.ll_last_setup_rx1,
         info.ll_last_setup_failed_count, info.ll_last_rx_start_ok,
         info.ll_rx_start_timeout_count, info.ll_last_rx_start_irq);
  printf("ll-tx: len=%" PRIu32 " stage=%" PRIu32 " timeouts=%" PRIu32
         " irq=0x%08" PRIx32 " fdebug=0x%08" PRIx32
         " flevel=0x%08" PRIx32 " pc=%" PRIu32 "\n",
         info.ll_last_tx_len, info.ll_last_tx_wait_stage,
         info.ll_tx_timeout_count, info.ll_last_tx_irq,
         info.ll_last_tx_fdebug, info.ll_last_tx_flevel,
         info.ll_last_tx_pc);
  printf("ll-in: result=%" PRId32 " pid=0x%02" PRIx32
         " expect=0x%02" PRIx32 " rx0=0x%02" PRIx32
         " rx1=0x%02" PRIx32 " failed-count=%" PRIu32 "\n",
         info.ll_last_in_result, info.ll_last_in_pid,
         info.ll_last_in_expected_pid, info.ll_last_in_rx0,
         info.ll_last_in_rx1, info.ll_last_in_failed_count);

  return EXIT_SUCCESS;
}

static int piousbhost_init(void)
{
  int ret;

  if (g_piousbhost_initialized)
    {
      printf("piousbhost: already initialized\n");
      return EXIT_SUCCESS;
    }

  ret = rp23xx_pio_usbhost_initialize(&g_piousbhost_conn);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: rp23xx_pio_usbhost_initialize failed: %d\n",
              ret);
      return EXIT_FAILURE;
    }

  g_piousbhost_initialized = true;
  printf("piousbhost: initialized\n");
  return EXIT_SUCCESS;
}

static int piousbhost_waiter(void)
{
  int ret;

  if (!g_piousbhost_initialized)
    {
      ret = piousbhost_init();
      if (ret != EXIT_SUCCESS)
        {
          return ret;
        }
    }

  if (g_piousbhost_waiter_started)
    {
      printf("piousbhost: waiter already started\n");
      return EXIT_SUCCESS;
    }

  ret = usbhost_waiter_initialize(g_piousbhost_conn);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: usbhost_waiter_initialize failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  g_piousbhost_waiter_started = true;
  printf("piousbhost: waiter started\n");
  return EXIT_SUCCESS;
}

#ifdef CONFIG_USBHOST_HUB
static int piousbhost_wait_for_root_hub(FAR const char *label, int attempts)
{
  struct rp23xx_pio_usbhost_info_s info;
  int attempt;
  int ret;

  for (attempt = 1; attempt <= attempts; attempt++)
    {
      ret = rp23xx_pio_usbhost_info(&info);
      if (ret < 0)
        {
          fprintf(stderr, "ERROR: rp23xx_pio_usbhost_info failed: %d\n",
                  ret);
          return EXIT_FAILURE;
        }

      if (info.hport_has_class)
        {
          printf("piousbhost: %s root hub ready addr=%u enum-count=%" PRIu32
                 " enum-ret=%d\n",
                 label, info.hport_funcaddr, info.enum_count,
                 info.last_enum_ret);
          return EXIT_SUCCESS;
        }

      printf("piousbhost: %s waiting for root hub (%d/%d enum-count=%" PRIu32
             " enum-ret=%d change=%u)\n",
             label, attempt, attempts, info.enum_count, info.last_enum_ret,
             info.change_pending);
      usleep(1000 * 1000);
      piousbhost_guardrefresh();
    }

  fprintf(stderr, "ERROR: %s root hub not ready\n", label);
  return EXIT_FAILURE;
}

static int piousbhost_startup_rescan(FAR const char *label, int attempts)
{
  int attempt;
  int ret = -ENODEV;

  for (attempt = 1; attempt <= attempts; attempt++)
    {
      ret = usbhost_hub_rescan();
      if (ret >= 0)
        {
          printf("piousbhost: %s queued=%d\n", label, ret);
          return EXIT_SUCCESS;
        }

      if (ret != -ENODEV || attempt == attempts)
        {
          break;
        }

      printf("piousbhost: %s waiting for hub (%d/%d)\n",
             label, attempt, attempts);
      usleep(1000 * 1000);
      piousbhost_guardrefresh();
    }

  fprintf(stderr, "ERROR: %s failed: %d\n", label, ret);
  return EXIT_FAILURE;
}

static int piousbhost_wait_hub_settled(FAR const char *label,
                                       unsigned int seconds)
{
  struct rp23xx_pio_usbhost_info_s info;
  unsigned int elapsed;
  int ret;

  for (elapsed = 0; elapsed <= seconds; elapsed++)
    {
      unsigned int connected = 0;
      unsigned int disabled = 0;
      unsigned int unpowered = 0;
      unsigned int ports = 0;
      int i;

      ret = rp23xx_pio_usbhost_info(&info);
      if (ret < 0)
        {
          fprintf(stderr, "ERROR: rp23xx_pio_usbhost_info failed: %d\n",
                  ret);
          return EXIT_FAILURE;
        }

      for (i = 0; i < RP23XX_PIO_USBHOST_INFO_PORTS; i++)
        {
          uint16_t status;

          if ((info.hub_port_valid & (1u << i)) == 0)
            {
              continue;
            }

          ports++;
          status = info.hub_port_status[i];
          if ((status & USBHUB_PORT_STAT_POWER) == 0)
            {
              unpowered++;
            }

          if ((status & USBHUB_PORT_STAT_CONNECTION) != 0)
            {
              connected++;
              if ((status & USBHUB_PORT_STAT_ENABLE) == 0)
                {
                  disabled++;
                }
            }
        }

      if (ports > 0 && disabled == 0 && unpowered < ports)
        {
          printf("piousbhost: %s settled ports=%u connected=%u "
                 "unpowered=%u\n", label, ports, connected, unpowered);
          return EXIT_SUCCESS;
        }

      if (ports > 0 && unpowered == ports && elapsed >= 3)
        {
          fprintf(stderr, "ERROR: %s all downstream ports unpowered\n",
                  label);
          return EXIT_FAILURE;
        }

      if ((elapsed % 3) == 0)
        {
          printf("piousbhost: %s settling ports=%u connected=%u "
                 "disabled=%u unpowered=%u (%u/%u)\n",
                 label, ports, connected, disabled, unpowered, elapsed,
                 seconds);
        }

      ret = usbhost_hub_rescan();
      if (ret < 0 && ret != -ENODEV)
        {
          fprintf(stderr, "ERROR: %s settle rescan failed: %d\n",
                  label, ret);
          return EXIT_FAILURE;
        }

      usleep(1000 * 1000);
      piousbhost_guardrefresh();
    }

  fprintf(stderr, "ERROR: %s did not settle within %u seconds\n",
          label, seconds);
  return EXIT_FAILURE;
}

static int piousbhost_startup_sequence(FAR const char *label)
{
  char phase[48];
  int ret;

  snprintf(phase, sizeof(phase), "%s", label);
  ret = piousbhost_wait_for_root_hub(phase, 12);
  if (ret != EXIT_SUCCESS)
    {
      return ret;
    }

  snprintf(phase, sizeof(phase), "%s hub scan", label);
  ret = piousbhost_startup_rescan(phase, 6);
  if (ret != EXIT_SUCCESS)
    {
      return ret;
    }

  usleep(2000 * 1000);
  piousbhost_guardrefresh();

  snprintf(phase, sizeof(phase), "%s downstream scan", label);
  ret = piousbhost_startup_rescan(phase, 6);
  if (ret != EXIT_SUCCESS)
    {
      return ret;
    }

  usleep(2000 * 1000);
  piousbhost_guardrefresh();

  snprintf(phase, sizeof(phase), "%s hub ports", label);
  ret = piousbhost_wait_hub_settled(
          phase, CONFIG_SYSTEM_PIOUSBHOST_HUB_SETTLE_SEC);
  if (ret != EXIT_SUCCESS)
    {
      return ret;
    }

  if (CONFIG_SYSTEM_PIOUSBHOST_EXPECT_KEYBOARDS > 0 ||
      CONFIG_SYSTEM_PIOUSBHOST_EXPECT_MICE > 0)
    {
      ret = piousbhost_hidwait_counts(
              CONFIG_SYSTEM_PIOUSBHOST_EXPECT_KEYBOARDS,
              CONFIG_SYSTEM_PIOUSBHOST_EXPECT_MICE,
              CONFIG_SYSTEM_PIOUSBHOST_HID_WAIT_SEC, true);
      if (ret != EXIT_SUCCESS)
        {
          return ret;
        }
    }

  return EXIT_SUCCESS;
}
#endif

static int piousbhost_start(void)
{
  int ret;

  ret = piousbhost_init();
  if (ret != EXIT_SUCCESS)
    {
      return ret;
    }

  piousbhost_guardrefresh();

  if (!g_piousbhost_post_start_done)
    {
      if (!g_piousbhost_waiter_started)
        {
          usleep(250 * 1000);
          printf("piousbhost: VBUS enabled\n");
          piousbhost_guardrefresh();
        }

      g_piousbhost_post_start_done = true;
    }

  ret = piousbhost_waiter();
  if (ret != EXIT_SUCCESS)
    {
      return ret;
    }

  printf("piousbhost: started\n");
  piousbhost_guardrefresh();

  return EXIT_SUCCESS;
}

static int piousbhost_recover(void)
{
#ifdef CONFIG_USBHOST_HUB
  int ret;

  ret = piousbhost_start();
  if (ret != EXIT_SUCCESS)
    {
      return ret;
    }

  usleep(3000 * 1000);
  piousbhost_guardrefresh();

  ret = piousbhost_startup_sequence("manual");
  if (ret == EXIT_SUCCESS)
    {
      return EXIT_SUCCESS;
    }

#if CONFIG_RP23XX_PIO_USBHOST_VBUS_GPIO >= 0
  printf("piousbhost: manual recovery needs VBUS cycle: %d\n", ret);
  ret = rp23xx_pio_usbhost_vbus_cycle();
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: recovery VBUS cycle failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  g_piousbhost_post_start_done = false;
  printf("piousbhost: recovery VBUS cycle complete\n");
  piousbhost_guardrefresh();

  usleep(3000 * 1000);
  piousbhost_guardrefresh();

  ret = piousbhost_startup_sequence("recovery");
  if (ret != EXIT_SUCCESS)
    {
      return ret;
    }

  return EXIT_SUCCESS;
#else
  return ret;
#endif
#else
  fprintf(stderr, "ERROR: USB hub support is not enabled\n");
  return EXIT_FAILURE;
#endif
}

static int piousbhost_start_guarded(void)
{
#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD
  int ret;

  ret = piousbhost_guardon();
  if (ret != EXIT_SUCCESS)
    {
      return ret;
    }

  g_piousbhost_guard_active = true;
  ret = piousbhost_guardfeed_start();
  if (ret < 0)
    {
      g_piousbhost_guard_active = false;
      board_fruitjam_bootguard_disarm();
      fprintf(stderr, "ERROR: bootguard refresher failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  ret = piousbhost_start();

  /* If control returned to NSH, CDC is still usable.  Keep the guard only
   * for actual hangs/resets during bring-up.
   */

  board_fruitjam_bootguard_disarm();
  g_piousbhost_guard_active = false;
  if (ret == EXIT_SUCCESS)
    {
      printf("piousbhost: bootguard disarmed after successful start\n");
    }
  else
    {
      printf("piousbhost: bootguard disarmed after failed start\n");
    }

  return ret;
#else
  fprintf(stderr, "ERROR: Fruit Jam bootguard is not enabled\n");
  return EXIT_FAILURE;
#endif
}

static void piousbhost_guardrefresh(void)
{
#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD
  if (g_piousbhost_guard_active)
    {
      board_fruitjam_bootguard_arm();
    }
#endif
}

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD
static FAR void *piousbhost_guardfeed(FAR void *arg)
{
  uint32_t elapsed = 0;

  UNUSED(arg);
  g_piousbhost_guard_feed_running = true;

  while (g_piousbhost_guard_active &&
         elapsed < CONFIG_SYSTEM_PIOUSBHOST_GUARD_WINDOW_MS)
    {
      piousbhost_guardrefresh();
      usleep(CONFIG_SYSTEM_PIOUSBHOST_GUARD_REFRESH_MS * 1000);
      elapsed += CONFIG_SYSTEM_PIOUSBHOST_GUARD_REFRESH_MS;
    }

  g_piousbhost_guard_feed_running = false;

  return NULL;
}

static int piousbhost_guardfeed_start(void)
{
  pthread_attr_t attr;
  pthread_t thread;
  int ret;

  if (g_piousbhost_guard_feed_running)
    {
      return OK;
    }

  ret = pthread_attr_init(&attr);
  if (ret != 0)
    {
      return -ret;
    }

  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr, CONFIG_SYSTEM_PIOUSBHOST_STACKSIZE);

  ret = pthread_create(&thread, &attr, piousbhost_guardfeed, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      return -ret;
    }

  printf("piousbhost: bootguard refresher started\n");
  return OK;
}
#endif

static int piousbhost_vbuscycle(void)
{
  int ret;

  ret = rp23xx_pio_usbhost_vbus_cycle();
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: VBUS cycle failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  g_piousbhost_post_start_done = false;
  printf("piousbhost: VBUS cycled\n");
  return EXIT_SUCCESS;
}

static void piousbhost_print_port(unsigned int port, uint16_t status,
                                  uint16_t change, int ret)
{
  if (ret < 0)
    {
      printf("  port%u: ret=%d\n", port, ret);
      return;
    }

  printf("  port%u: status=0x%04x change=0x%04x conn=%u en=%u "
         "pwr=%u ls=%u hs=%u oc=%u cconn=%u cen=%u crst=%u\n",
         port, status, change,
         (status & USBHUB_PORT_STAT_CONNECTION) != 0,
         (status & USBHUB_PORT_STAT_ENABLE) != 0,
         (status & USBHUB_PORT_STAT_POWER) != 0,
         (status & USBHUB_PORT_STAT_LOW_SPEED) != 0,
         (status & USBHUB_PORT_STAT_HIGH_SPEED) != 0,
         (status & USBHUB_PORT_STAT_OVERCURRENT) != 0,
         (change & USBHUB_PORT_STAT_CCONNECTION) != 0,
         (change & USBHUB_PORT_STAT_CENABLE) != 0,
         (change & USBHUB_PORT_STAT_CRESET) != 0);
}

static int piousbhost_hubstatus(int argc, FAR char *argv[], bool setpower)
{
  struct rp23xx_pio_usbhost_info_s info;
  unsigned int first;
  unsigned int last;
  unsigned int port;
  bool failed = false;
  int ret;

  ret = rp23xx_pio_usbhost_info(&info);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: rp23xx_pio_usbhost_info failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  if (info.hub_nports == 0)
    {
      fprintf(stderr, "ERROR: no enumerated hub ports yet\n");
      return EXIT_FAILURE;
    }

  first = 1;
  last = info.hub_nports;

  if (argc > 2)
    {
      FAR char *endptr;
      unsigned long value;

      value = strtoul(argv[2], &endptr, 0);
      if (*endptr != '\0' || value < 1 || value > info.hub_nports)
        {
          fprintf(stderr, "ERROR: port must be 1..%u\n", info.hub_nports);
          return EXIT_FAILURE;
        }

      first = value;
      last = value;
    }

  if (setpower)
    {
      printf("hubpower: requesting PORT_POWER before reading status\n");
    }

  printf("hub: ports=%u\n", info.hub_nports);
  for (port = first; port <= last; port++)
    {
      uint16_t status = 0;
      uint16_t change = 0;

      ret = rp23xx_pio_usbhost_hub_portstatus(port, setpower,
                                              &status, &change);
      if (ret < 0)
        {
          failed = true;
        }

      piousbhost_print_port(port, status, change, ret);
    }

  return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int piousbhost_scan(void)
{
#ifdef CONFIG_USBHOST_HUB
  int ret;

  ret = usbhost_hub_rescan();
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: usbhost_hub_rescan failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  printf("piousbhost: hub scan queued=%d\n", ret);
  return EXIT_SUCCESS;
#else
  fprintf(stderr, "ERROR: USB hub support is not enabled\n");
  return EXIT_FAILURE;
#endif
}

static void piousbhost_print_hex(FAR const uint8_t *buffer, ssize_t nbytes)
{
  ssize_t i;

  for (i = 0; i < nbytes; i++)
    {
      printf(" %02x", buffer[i]);
    }
}

static void piousbhost_print_text(FAR const uint8_t *buffer, ssize_t nbytes)
{
  ssize_t i;

  printf(" text=\"");
  for (i = 0; i < nbytes; i++)
    {
      uint8_t ch = buffer[i];

      if (ch == '\r')
        {
          printf("\\r");
        }
      else if (ch == '\n')
        {
          printf("\\n");
        }
      else if (isprint(ch))
        {
          putchar(ch);
        }
      else
        {
          putchar('.');
        }
    }

  printf("\"");
}

static bool piousbhost_path_exists(FAR const char *path)
{
  struct stat st;

  return stat(path, &st) == 0;
}

static unsigned int piousbhost_add_hiddev(
    FAR struct piousbhost_hiddev_s *hiddevs, unsigned int ndevs,
    unsigned int maxdevs, FAR const char *path, bool mouse, bool keyboard,
    bool rawkbd)
{
  if (ndevs >= maxdevs || !piousbhost_path_exists(path))
    {
      return ndevs;
    }

  strlcpy(hiddevs[ndevs].path, path, sizeof(hiddevs[ndevs].path));
  hiddevs[ndevs].mouse = mouse;
  hiddevs[ndevs].keyboard = keyboard;
  hiddevs[ndevs].rawkbd = rawkbd;
  return ndevs + 1;
}

static unsigned int piousbhost_collect_hiddevs(
    FAR struct piousbhost_hiddev_s *hiddevs, unsigned int maxdevs)
{
  unsigned int ndevs = 0;
  char path[PIOUSBHOST_PATH_MAX];
  int i;

  ndevs = piousbhost_add_hiddev(hiddevs, ndevs, maxdevs, "/dev/kbd0",
                                false, true, false);

  for (i = 0; i < 26; i++)
    {
      snprintf(path, sizeof(path), "/dev/kbd%c", 'a' + i);
      ndevs = piousbhost_add_hiddev(hiddevs, ndevs, maxdevs, path,
                                    false, false, true);
    }

  for (i = 0; i < 26; i++)
    {
      snprintf(path, sizeof(path), "/dev/mouse%d", i);
      ndevs = piousbhost_add_hiddev(hiddevs, ndevs, maxdevs, path,
                                    true, false, false);
    }

  return ndevs;
}

static void piousbhost_count_hiddevs(
    FAR const struct piousbhost_hiddev_s *hiddevs, unsigned int ndevs,
    FAR unsigned int *keyboards, FAR unsigned int *mice,
    FAR bool *aggregate)
{
  unsigned int i;

  *keyboards = 0;
  *mice = 0;
  *aggregate = false;

  for (i = 0; i < ndevs; i++)
    {
      if (hiddevs[i].rawkbd)
        {
          (*keyboards)++;
        }

      if (hiddevs[i].mouse)
        {
          (*mice)++;
        }

      if (hiddevs[i].keyboard)
        {
          *aggregate = true;
        }
    }
}

static int piousbhost_hidstatus(void)
{
  struct piousbhost_hiddev_s hiddevs[PIOUSBHOST_HID_MAX];
  unsigned int keyboards;
  unsigned int mice;
  unsigned int ndevs;
  unsigned int i;
  bool aggregate;

  ndevs = piousbhost_collect_hiddevs(hiddevs, nitems(hiddevs));
  piousbhost_count_hiddevs(hiddevs, ndevs, &keyboards, &mice,
                           &aggregate);

  printf("hid: aggregate-kbd=%u raw-keyboards=%u mice=%u nodes=%u\n",
         aggregate, keyboards, mice, ndevs);
  for (i = 0; i < ndevs; i++)
    {
      printf("  %s%s%s%s\n", hiddevs[i].path,
             hiddevs[i].keyboard ? " aggregate-keyboard" : "",
             hiddevs[i].rawkbd ? " raw-keyboard" : "",
             hiddevs[i].mouse ? " mouse" : "");
    }

  return EXIT_SUCCESS;
}

static int piousbhost_hidwait_counts(unsigned int keyboards,
                                     unsigned int mice,
                                     unsigned int seconds,
                                     bool verbose)
{
  struct piousbhost_hiddev_s hiddevs[PIOUSBHOST_HID_MAX];
  unsigned int got_keyboards;
  unsigned int got_mice;
  unsigned int ndevs;
  unsigned int elapsed;
  bool aggregate;

  for (elapsed = 0; elapsed <= seconds * 5; elapsed++)
    {
      ndevs = piousbhost_collect_hiddevs(hiddevs, nitems(hiddevs));
      piousbhost_count_hiddevs(hiddevs, ndevs, &got_keyboards,
                               &got_mice, &aggregate);

      if (got_keyboards >= keyboards && got_mice >= mice &&
          (keyboards == 0 || aggregate))
        {
          if (verbose)
            {
              printf("hidwait: ready aggregate-kbd=%u raw-keyboards=%u/%u "
                     "mice=%u/%u\n",
                     aggregate, got_keyboards, keyboards, got_mice, mice);
            }

          return EXIT_SUCCESS;
        }

      if (verbose && (elapsed % 5) == 0)
        {
          printf("hidwait: waiting aggregate-kbd=%u raw-keyboards=%u/%u "
                 "mice=%u/%u\n",
                 aggregate, got_keyboards, keyboards, got_mice, mice);
        }

      usleep(200 * 1000);
      piousbhost_guardrefresh();
    }

  fprintf(stderr, "ERROR: hidwait timed out waiting for keyboards=%u "
          "mice=%u\n", keyboards, mice);
  return EXIT_FAILURE;
}

static int piousbhost_hidwait(int argc, FAR char *argv[])
{
  unsigned long keyboards = CONFIG_SYSTEM_PIOUSBHOST_EXPECT_KEYBOARDS;
  unsigned long mice = CONFIG_SYSTEM_PIOUSBHOST_EXPECT_MICE;
  unsigned long seconds = CONFIG_SYSTEM_PIOUSBHOST_HID_WAIT_SEC;
  FAR char *endptr;

  if (argc > 2)
    {
      keyboards = strtoul(argv[2], &endptr, 0);
      if (*endptr != '\0' || keyboards > 26)
        {
          fprintf(stderr, "ERROR: keyboards must be 0..26\n");
          return EXIT_FAILURE;
        }
    }

  if (argc > 3)
    {
      mice = strtoul(argv[3], &endptr, 0);
      if (*endptr != '\0' || mice > 26)
        {
          fprintf(stderr, "ERROR: mice must be 0..26\n");
          return EXIT_FAILURE;
        }
    }

  if (argc > 4)
    {
      seconds = strtoul(argv[4], &endptr, 0);
      if (*endptr != '\0' || seconds > 300)
        {
          fprintf(stderr, "ERROR: seconds must be 0..300\n");
          return EXIT_FAILURE;
        }
    }

  return piousbhost_hidwait_counts(keyboards, mice, seconds, true);
}

static void piousbhost_print_hidread(FAR const char *path,
                                     FAR const uint8_t *buffer,
                                     ssize_t nbytes, bool mouse,
                                     bool keyboard)
{
  if (mouse && nbytes % sizeof(struct mouse_report_s) == 0)
    {
      ssize_t offset;

      for (offset = 0; offset < nbytes;
           offset += sizeof(struct mouse_report_s))
        {
          struct mouse_report_s report;

          memcpy(&report, &buffer[offset], sizeof(report));

          printf("%s: mouse buttons=0x%02x x=%d y=%d wheel=%d\n",
                 path, report.buttons, report.x, report.y, report.wheel);
        }

      return;
    }

  if (keyboard && nbytes % sizeof(struct keyboard_event_s) == 0)
    {
      ssize_t offset;

      for (offset = 0; offset < nbytes;
           offset += sizeof(struct keyboard_event_s))
        {
          struct keyboard_event_s event;

          memcpy(&event, &buffer[offset], sizeof(event));

          printf("%s: keyboard %s code=%" PRIu32 "\n",
                 path, event.type == KEYBOARD_PRESS ? "press" : "release",
                 event.code);
        }

      return;
    }

  printf("%s: %zd bytes", path, nbytes);
  piousbhost_print_hex(buffer, nbytes);
  piousbhost_print_text(buffer, nbytes);
  printf("\n");
}

static int piousbhost_hidwatch(int argc, FAR char *argv[])
{
  struct piousbhost_hiddev_s hiddevs[PIOUSBHOST_HID_MAX];
  struct pollfd pfds[nitems(hiddevs)];
  int map[nitems(hiddevs)];
  unsigned long seconds = 15;
  unsigned int loops;
  unsigned int i;
  unsigned int ndevs;
  unsigned int nopen = 0;
  bool saw_input = false;

  if (argc > 2)
    {
      FAR char *endptr;

      seconds = strtoul(argv[2], &endptr, 0);
      if (*endptr != '\0' || seconds == 0 || seconds > 300)
        {
          fprintf(stderr, "ERROR: seconds must be 1..300\n");
          return EXIT_FAILURE;
        }
    }

  memset(pfds, 0, sizeof(pfds));
  ndevs = piousbhost_collect_hiddevs(hiddevs, nitems(hiddevs));

  for (i = 0; i < ndevs; i++)
    {
      int fd = open(hiddevs[i].path, O_RDONLY | O_NONBLOCK);

      if (fd >= 0)
        {
          map[nopen] = i;
          pfds[nopen].fd = fd;
          pfds[nopen].events = POLLIN;
          nopen++;
          printf("hidwatch: opened %s\n", hiddevs[i].path);
        }
      else if (errno != ENOENT)
        {
          printf("hidwatch: skipped %s: errno=%d\n", hiddevs[i].path, errno);
        }
    }

  if (nopen == 0)
    {
      fprintf(stderr, "ERROR: no HID input devices are available\n");
      return EXIT_FAILURE;
    }

  printf("hidwatch: watching for %lu seconds\n", seconds);
  fflush(stdout);

  for (loops = seconds * 5; loops > 0; loops--)
    {
      int ret = poll(pfds, nopen, 200);

      if (ret < 0)
        {
          fprintf(stderr, "ERROR: poll failed: %d\n", errno);
          break;
        }

      if (ret == 0)
        {
          continue;
        }

      for (i = 0; i < nopen; i++)
        {
          if ((pfds[i].revents & POLLIN) != 0)
            {
              uint8_t buffer[32];
              ssize_t nbytes;
              FAR const struct piousbhost_hiddev_s *dev =
                &hiddevs[map[i]];

              nbytes = read(pfds[i].fd, buffer, sizeof(buffer));
              if (nbytes > 0)
                {
                  saw_input = true;
                  piousbhost_print_hidread(dev->path, buffer, nbytes,
                                           dev->mouse, dev->keyboard);
                  fflush(stdout);
                }
            }
        }
    }

  for (i = 0; i < nopen; i++)
    {
      close(pfds[i].fd);
    }

  if (!saw_input)
    {
      printf("hidwatch: no input before timeout\n");
    }

  return EXIT_SUCCESS;
}

static int piousbhost_guardon(void)
{
#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD
  board_fruitjam_bootguard_arm();
  printf("piousbhost: bootguard armed\n");
  return EXIT_SUCCESS;
#else
  fprintf(stderr, "ERROR: Fruit Jam bootguard is not enabled\n");
  return EXIT_FAILURE;
#endif
}

static int piousbhost_guardoff(void)
{
#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD
  board_fruitjam_bootguard_disarm();
  g_piousbhost_guard_active = false;
  printf("piousbhost: bootguard disarmed\n");
  return EXIT_SUCCESS;
#else
  fprintf(stderr, "ERROR: Fruit Jam bootguard is not enabled\n");
  return EXIT_FAILURE;
#endif
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *cmd = "start";

  if (argc > 1)
    {
      cmd = argv[1];
    }

  if (strcmp(cmd, "start") == 0)
    {
      return piousbhost_start();
    }

  if (strcmp(cmd, "init") == 0)
    {
      return piousbhost_init();
    }

  if (strcmp(cmd, "waiter") == 0)
    {
      return piousbhost_waiter();
    }

  if (strcmp(cmd, "start-guarded") == 0)
    {
      return piousbhost_start_guarded();
    }

  if (strcmp(cmd, "recover") == 0)
    {
      return piousbhost_recover();
    }

  if (strcmp(cmd, "scan") == 0)
    {
      return piousbhost_scan();
    }

  if (strcmp(cmd, "vbuscycle") == 0)
    {
      return piousbhost_vbuscycle();
    }

  if (strcmp(cmd, "hubstatus") == 0)
    {
      return piousbhost_hubstatus(argc, argv, false);
    }

  if (strcmp(cmd, "hubpower") == 0)
    {
      return piousbhost_hubstatus(argc, argv, true);
    }

  if (strcmp(cmd, "guardon") == 0)
    {
      return piousbhost_guardon();
    }

  if (strcmp(cmd, "guardoff") == 0)
    {
      return piousbhost_guardoff();
    }

  if (strcmp(cmd, "status") == 0)
    {
      return piousbhost_status();
    }

  if (strcmp(cmd, "info") == 0)
    {
      return piousbhost_info();
    }

  if (strcmp(cmd, "hidstatus") == 0)
    {
      return piousbhost_hidstatus();
    }

  if (strcmp(cmd, "hidwait") == 0)
    {
      return piousbhost_hidwait(argc, argv);
    }

  if (strcmp(cmd, "hidwatch") == 0)
    {
      return piousbhost_hidwatch(argc, argv);
    }

  piousbhost_usage(argv[0]);
  return EXIT_FAILURE;
}
