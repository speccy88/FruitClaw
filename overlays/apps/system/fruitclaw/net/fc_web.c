/* SPDX-License-Identifier: Apache-2.0 */

#include "fruitclaw.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_MM_IOB
#  include <nuttx/mm/iob.h>
#endif

#ifdef CONFIG_NET_TCP_WRITE_BUFFERS
#  include <nuttx/net/tcp.h>
#endif

#ifdef CONFIG_NETUTILS_HTTPD_CGIPATH
#  include "netutils/httpd.h"
#endif

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED
#  include <nuttx/wireless/esp_hosted.h>
#endif

#ifdef CONFIG_NET_TCP_WRITE_BUFFERS
extern int tcp_wrbuffer_test(void);
#endif

#ifndef CONFIG_FRUITCLAW_WEB_HOME_MAX_BYTES
#  define CONFIG_FRUITCLAW_WEB_HOME_MAX_BYTES 4096
#endif

#define FC_WEB_HOME_LEAF "www/home.md"

static const char g_default_home[] =
  "# FruitClaw\n\n"
  "FruitClaw is running on this Adafruit Fruit Jam RP2350.\n\n"
  "- Open `/docs/index.html#/home` for the full manual.\n"
  "- Use MCP at `/mcp` for tool calls.\n"
  "- Update this page with the `web.home.write` tool or by editing "
  "`www/home.md` under the active FruitClaw data root.\n";

static bool g_web_registered;

static pthread_mutex_t g_httpd_activity_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_httpd_last_phase[32] = "-";
static int64_t g_httpd_last_ms;
static unsigned long g_httpd_activity_count;
static unsigned long g_httpd_handler_count;
static unsigned long g_httpd_recv_count;
static unsigned long g_httpd_recv_wait_count;
static unsigned long g_httpd_recv_ok_count;
static unsigned long g_httpd_recv_timeout_count;
static unsigned long g_httpd_parse_count;
static unsigned long g_httpd_send_count;
static unsigned long g_httpd_sent_count;
static unsigned long g_httpd_send_fail_count;
static unsigned long g_httpd_send_fail_streak;
static unsigned long g_httpd_close_count;
static unsigned long g_httpd_malloc_fail_count;
static int g_httpd_last_send_errno;
static pthread_mutex_t g_transport_progress_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_transport_last_light_pump_ms;
static int64_t g_transport_last_force_pump_ms;

static void fc_web_tcp_pool_status(int *wrb_test, int *wrb_avail,
                                   int *iob_avail,
                                   int *iob_avail_throttled,
                                   struct tcp_sendbuffer_diag_s *send_diag)
{
#ifdef CONFIG_NET_TCP_WRITE_BUFFERS
  if (wrb_test != NULL)
    {
      *wrb_test = tcp_wrbuffer_test();
    }

  if (wrb_avail != NULL)
    {
      *wrb_avail = tcp_wrbuffer_navail();
    }

  if (send_diag != NULL)
    {
      tcp_sendbuffer_diag(send_diag);
    }
#else
  if (wrb_test != NULL)
    {
      *wrb_test = 0;
    }

  if (wrb_avail != NULL)
    {
      *wrb_avail = -1;
    }

  if (send_diag != NULL)
    {
      memset(send_diag, 0, sizeof(*send_diag));
    }
#endif

#ifdef CONFIG_MM_IOB
  if (iob_avail != NULL)
    {
      *iob_avail = iob_navail(false);
    }

  if (iob_avail_throttled != NULL)
    {
      *iob_avail_throttled = iob_navail(true);
    }
#else
  if (iob_avail != NULL)
    {
      *iob_avail = -1;
    }

  if (iob_avail_throttled != NULL)
    {
      *iob_avail_throttled = -1;
    }
#endif
}

static void fc_network_transport_progress(FAR const char *phase)
{
#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED
  bool force = false;
  unsigned int budget = 1;
  int64_t *last_ms;
  int64_t min_interval_ms;
  int64_t now;

  if (phase != NULL &&
      (strstr(phase, "send-wait") != NULL ||
       strstr(phase, "send-eintr") != NULL ||
       strstr(phase, "close-drain") != NULL))
    {
      budget = 1;
      force = true;
    }
  else if (phase != NULL &&
           (strstr(phase, "send") != NULL ||
            strstr(phase, "recv") != NULL ||
            strstr(phase, "poll") != NULL ||
            strstr(phase, "close") != NULL))
    {
      budget = 1;
      force = false;
    }
  else
    {
      return;
    }

  now = fc_mono_ms();
  last_ms = force ? &g_transport_last_force_pump_ms :
                   &g_transport_last_light_pump_ms;
  min_interval_ms = force ? 50 : 250;
  pthread_mutex_lock(&g_transport_progress_lock);
  if (*last_ms > 0 && now >= *last_ms &&
      now - *last_ms < min_interval_ms)
    {
      pthread_mutex_unlock(&g_transport_progress_lock);
      return;
    }

  *last_ms = now;
  pthread_mutex_unlock(&g_transport_progress_lock);

  (void)esp_hosted_spi_pump(budget, force);
#else
  (void)phase;
#endif
}

void ftpd_activity_hook(FAR const char *phase)
{
  fc_guard_session_heartbeat("ftpd");
  fc_operator_progress_mark("ftpd");
  fc_network_transport_progress(phase);
}

void telnet_activity_hook(FAR const char *phase)
{
  fc_guard_session_heartbeat("telnetd");
  fc_operator_progress_mark("telnetd");
  fc_network_transport_progress(phase);
}

#ifdef CONFIG_NETUTILS_HTTPD_CGIPATH
void httpd_activity_hook(FAR const char *phase)
{
  /* Any inbound HTTP traffic should give Telegram polling a chance to yield.
   * The MCP activity window is already used by the Telegram worker for that
   * purpose, and /docs plus /site/home.md share the same constrained network
   * path as /mcp on this board.
   */

  fc_mcp_mark_activity();
  fc_guard_session_heartbeat("httpd");
  fc_operator_progress_mark("httpd");
  fc_network_transport_progress(phase);

  pthread_mutex_lock(&g_httpd_activity_lock);
  g_httpd_activity_count++;
  g_httpd_last_ms = fc_mono_ms();

  if (phase != NULL)
    {
      char previous[sizeof(g_httpd_last_phase)];

      fc_strlcpy(previous, g_httpd_last_phase, sizeof(previous));
      if (strcmp(phase, "handler-start") == 0)
        {
          g_httpd_handler_count++;
        }
      else if (strcmp(phase, "recv-header") == 0 ||
               strcmp(phase, "recv-body") == 0)
        {
          g_httpd_recv_count++;
        }
      else if (strcmp(phase, "recv-header-wait") == 0 ||
               strcmp(phase, "recv-body-wait") == 0)
        {
          g_httpd_recv_wait_count++;
        }
      else if (strcmp(phase, "recv-header-ok") == 0 ||
               strcmp(phase, "recv-body-ok") == 0)
        {
          g_httpd_recv_ok_count++;
        }
      else if (strcmp(phase, "parsed") == 0 ||
               strcmp(phase, "parse-error") == 0)
        {
          g_httpd_parse_count++;
          if (strcmp(phase, "parse-error") == 0 &&
              strncmp(previous, "recv-", 5) == 0)
            {
              g_httpd_recv_timeout_count++;
            }
        }
      else if (strcmp(phase, "send") == 0)
        {
          g_httpd_send_count++;
        }
      else if (strcmp(phase, "sent") == 0)
        {
          g_httpd_sent_count++;
          g_httpd_send_fail_streak = 0;
        }
      else if (strncmp(phase, "send-fail", 9) == 0)
        {
          g_httpd_send_fail_count++;
          g_httpd_send_fail_streak++;
          if (phase[9] == '-')
            {
              g_httpd_last_send_errno = (int)strtol(phase + 10, NULL, 10);
            }
        }
      else if (strcmp(phase, "close") == 0)
        {
          g_httpd_close_count++;
        }
      else if (strcmp(phase, "malloc-fail") == 0)
        {
          g_httpd_malloc_fail_count++;
        }
    }

  fc_strlcpy(g_httpd_last_phase, phase != NULL ? phase : "-",
             sizeof(g_httpd_last_phase));
  pthread_mutex_unlock(&g_httpd_activity_lock);
}
#endif

int fc_web_httpd_status_format(char *out, size_t out_len)
{
  char phase[sizeof(g_httpd_last_phase)];
  int64_t last_ms;
  int64_t now;
  unsigned long activity;
  unsigned long handlers;
  unsigned long recvs;
  unsigned long recv_waits;
  unsigned long recv_ok;
  unsigned long recv_timeouts;
  unsigned long parses;
  unsigned long sends;
  unsigned long sent;
  unsigned long send_fail;
  unsigned long send_fail_streak;
  unsigned long closes;
  unsigned long malloc_fail;
  int send_errno;
  int wrb_test;
  int wrb_avail;
  int iob_avail;
  int iob_avail_throttled;
  struct tcp_sendbuffer_diag_s send_diag;

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  now = fc_mono_ms();
  pthread_mutex_lock(&g_httpd_activity_lock);
  fc_strlcpy(phase, g_httpd_last_phase, sizeof(phase));
  last_ms = g_httpd_last_ms;
  activity = g_httpd_activity_count;
  handlers = g_httpd_handler_count;
  recvs = g_httpd_recv_count;
  recv_waits = g_httpd_recv_wait_count;
  recv_ok = g_httpd_recv_ok_count;
  recv_timeouts = g_httpd_recv_timeout_count;
  parses = g_httpd_parse_count;
  sends = g_httpd_send_count;
  sent = g_httpd_sent_count;
  send_fail = g_httpd_send_fail_count;
  send_fail_streak = g_httpd_send_fail_streak;
  closes = g_httpd_close_count;
  malloc_fail = g_httpd_malloc_fail_count;
  send_errno = g_httpd_last_send_errno;
  pthread_mutex_unlock(&g_httpd_activity_lock);
  fc_web_tcp_pool_status(&wrb_test, &wrb_avail, &iob_avail,
                         &iob_avail_throttled, &send_diag);

  snprintf(out, out_len,
           "httpd_activity: total=%lu handlers=%lu recvs=%lu "
           "recv_waits=%lu recv_ok=%lu recv_timeouts=%lu parses=%lu "
           "sends=%lu sent=%lu send_fail=%lu send_fail_streak=%lu "
           "closes=%lu malloc_fail=%lu "
           "last_send_errno=%d last_phase=%s last_age_ms=%lld "
           "tcp_wrb_test=%d tcp_wrb_avail=%d iob_avail=%d "
           "iob_avail_throttled=%d tcp_send_calls=%lu "
           "tcp_send_ok=%lu tcp_send_eagain=%lu tcp_send_cb_fail=%lu "
           "tcp_send_wrb_fail=%lu tcp_send_iob_fail=%lu "
           "tcp_send_txnotify=%lu tcp_send_queued=%lu "
           "tcp_send_last_ret=%ld tcp_send_nonblock=%lu",
           activity, handlers, recvs, recv_waits, recv_ok, recv_timeouts,
           parses, sends, sent, send_fail, send_fail_streak, closes,
           malloc_fail, send_errno, phase,
           last_ms > 0 ? (long long)(now - last_ms) : -1,
           wrb_test, wrb_avail, iob_avail, iob_avail_throttled,
           (unsigned long)send_diag.send_calls,
           (unsigned long)send_diag.send_ok,
           (unsigned long)send_diag.send_eagain,
           (unsigned long)send_diag.callback_alloc_fail,
           (unsigned long)send_diag.wrbuffer_alloc_fail,
           (unsigned long)send_diag.iob_copy_fail,
           (unsigned long)send_diag.txnotify_calls,
           (unsigned long)send_diag.queued_bytes,
           (long)send_diag.last_ret,
           (unsigned long)send_diag.last_nonblock);
  return 0;
}

int fc_web_httpd_status_json(char *out, size_t out_len)
{
  char phase[sizeof(g_httpd_last_phase)];
  int64_t last_ms;
  int64_t now;
  unsigned long activity;
  unsigned long handlers;
  unsigned long recvs;
  unsigned long recv_waits;
  unsigned long recv_ok;
  unsigned long recv_timeouts;
  unsigned long parses;
  unsigned long sends;
  unsigned long sent;
  unsigned long send_fail;
  unsigned long send_fail_streak;
  unsigned long closes;
  unsigned long malloc_fail;
  int send_errno;
  int wrb_test;
  int wrb_avail;
  int iob_avail;
  int iob_avail_throttled;
  struct tcp_sendbuffer_diag_s send_diag;

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  now = fc_mono_ms();
  pthread_mutex_lock(&g_httpd_activity_lock);
  fc_strlcpy(phase, g_httpd_last_phase, sizeof(phase));
  last_ms = g_httpd_last_ms;
  activity = g_httpd_activity_count;
  handlers = g_httpd_handler_count;
  recvs = g_httpd_recv_count;
  recv_waits = g_httpd_recv_wait_count;
  recv_ok = g_httpd_recv_ok_count;
  recv_timeouts = g_httpd_recv_timeout_count;
  parses = g_httpd_parse_count;
  sends = g_httpd_send_count;
  sent = g_httpd_sent_count;
  send_fail = g_httpd_send_fail_count;
  send_fail_streak = g_httpd_send_fail_streak;
  closes = g_httpd_close_count;
  malloc_fail = g_httpd_malloc_fail_count;
  send_errno = g_httpd_last_send_errno;
  pthread_mutex_unlock(&g_httpd_activity_lock);
  fc_web_tcp_pool_status(&wrb_test, &wrb_avail, &iob_avail,
                         &iob_avail_throttled, &send_diag);

  snprintf(out, out_len,
           "\"httpd_activity\":{\"total\":%lu,\"handlers\":%lu,"
           "\"recvs\":%lu,\"recv_waits\":%lu,\"recv_ok\":%lu,"
           "\"recv_timeouts\":%lu,\"parses\":%lu,\"sends\":%lu,"
           "\"sent\":%lu,"
           "\"send_fail\":%lu,\"send_fail_streak\":%lu,"
           "\"closes\":%lu,\"malloc_fail\":%lu,"
           "\"last_send_errno\":%d,\"last_phase\":\"%s\","
           "\"last_age_ms\":%lld,\"tcp_wrb_test\":%d,"
           "\"tcp_wrb_avail\":%d,\"iob_avail\":%d,"
           "\"iob_avail_throttled\":%d,\"tcp_send_calls\":%lu,"
           "\"tcp_send_ok\":%lu,\"tcp_send_eagain\":%lu,"
           "\"tcp_send_cb_fail\":%lu,\"tcp_send_wrb_fail\":%lu,"
           "\"tcp_send_iob_fail\":%lu,\"tcp_send_txnotify\":%lu,"
           "\"tcp_send_queued\":%lu,\"tcp_send_last_ret\":%ld,"
           "\"tcp_send_nonblock\":%lu}",
           activity, handlers, recvs, recv_waits, recv_ok, recv_timeouts,
           parses, sends, sent, send_fail, send_fail_streak, closes,
           malloc_fail, send_errno, phase,
           last_ms > 0 ? (long long)(now - last_ms) : -1,
           wrb_test, wrb_avail, iob_avail, iob_avail_throttled,
           (unsigned long)send_diag.send_calls,
           (unsigned long)send_diag.send_ok,
           (unsigned long)send_diag.send_eagain,
           (unsigned long)send_diag.callback_alloc_fail,
           (unsigned long)send_diag.wrbuffer_alloc_fail,
           (unsigned long)send_diag.iob_copy_fail,
           (unsigned long)send_diag.txnotify_calls,
           (unsigned long)send_diag.queued_bytes,
           (long)send_diag.last_ret,
           (unsigned long)send_diag.last_nonblock);
  return 0;
}

int fc_web_httpd_activity_snapshot(unsigned long *sends,
                                   unsigned long *sent,
                                   unsigned long *send_fail,
                                   unsigned long *send_fail_streak,
                                   int *last_send_errno,
                                   int64_t *last_age_ms)
{
  int64_t now;
  int64_t last_ms;

  now = fc_mono_ms();
  pthread_mutex_lock(&g_httpd_activity_lock);
  last_ms = g_httpd_last_ms;

  if (sends != NULL)
    {
      *sends = g_httpd_send_count;
    }

  if (sent != NULL)
    {
      *sent = g_httpd_sent_count;
    }

  if (send_fail != NULL)
    {
      *send_fail = g_httpd_send_fail_count;
    }

  if (send_fail_streak != NULL)
    {
      *send_fail_streak = g_httpd_send_fail_streak;
    }

  if (last_send_errno != NULL)
    {
      *last_send_errno = g_httpd_last_send_errno;
    }

  if (last_age_ms != NULL)
    {
      *last_age_ms = last_ms > 0 && now >= last_ms ? now - last_ms : -1;
    }

  pthread_mutex_unlock(&g_httpd_activity_lock);
  return 0;
}

static int fc_web_home_path(char *out, size_t out_len)
{
  return fc_data_path(FC_WEB_HOME_LEAF, out, out_len);
}

int fc_web_home_read(char *out, size_t out_len, bool *custom)
{
  char path[FC_PATH_LEN];
  int ret;

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  if (custom != NULL)
    {
      *custom = false;
    }

  ret = fc_web_home_path(path, sizeof(path));
  if (ret == 0)
    {
      ret = fc_read_text_file(path, out, out_len, false);
      if (ret == 0)
        {
          if (custom != NULL)
            {
              *custom = true;
            }

          return 0;
        }
    }

  fc_strlcpy(out, g_default_home, out_len);
  return 0;
}

int fc_web_home_write(const char *markdown)
{
  char dir[FC_PATH_LEN];
  char path[FC_PATH_LEN];
  size_t len;
  int ret;

  if (markdown == NULL)
    {
      return -EINVAL;
    }

  len = strlen(markdown);
  if (len > CONFIG_FRUITCLAW_WEB_HOME_MAX_BYTES)
    {
      return -EFBIG;
    }

  ret = fc_data_path("www", dir, sizeof(dir));
  if (ret < 0)
    {
      return ret;
    }

  ret = fc_mkdir_p(dir);
  if (ret < 0)
    {
      return ret;
    }

  ret = fc_web_home_path(path, sizeof(path));
  if (ret < 0)
    {
      return ret;
    }

  return fc_write_text_file_atomic(path, markdown);
}

#ifdef CONFIG_NETUTILS_HTTPD_CGIPATH
static void fc_web_home_http_handler(struct httpd_state *pstate, char *path)
{
  static const char extra_headers[] =
    "Allow: GET, OPTIONS\r\n"
    "Cache-Control: no-cache\r\n";
  char body[CONFIG_FRUITCLAW_WEB_HOME_MAX_BYTES + 1];
  int ret;

  (void)path;
  fc_guard_session_heartbeat("web-home");

  if (pstate->ht_method == HTTPD_METHOD_OPTIONS)
    {
      httpd_send_response(pstate, 204, "text/plain", extra_headers,
                          NULL, 0);
      return;
    }

  if (pstate->ht_method != HTTPD_METHOD_GET)
    {
      httpd_send_response(pstate, 405, "text/plain", extra_headers,
                          NULL, 0);
      return;
    }

  ret = fc_web_home_read(body, sizeof(body), NULL);
  if (ret < 0)
    {
      httpd_send_response(pstate, 500, "text/plain", extra_headers,
                          "home read failed\n", 17);
      return;
    }

  httpd_send_response(pstate, 200, "text/markdown; charset=utf-8",
                      extra_headers, body, strlen(body));
}

HTTPD_CGI_CALL(g_fc_web_home_cgi, "/site/home.md",
               fc_web_home_http_handler);
#endif

int fc_web_register_http(void)
{
#ifdef CONFIG_NETUTILS_HTTPD_CGIPATH
  if (g_web_registered)
    {
      return 0;
    }

  httpd_cgi_register(&g_fc_web_home_cgi);
  g_web_registered = true;
  FC_LOGI("web home endpoint registered at /site/home.md");
#endif

  return 0;
}
