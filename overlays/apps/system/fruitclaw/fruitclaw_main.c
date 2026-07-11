/* SPDX-License-Identifier: Apache-2.0 */

#include "fruitclaw.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/boardctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <arch/board/board.h>

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED
#  include <nuttx/wireless/esp_hosted.h>
#endif

#ifdef CONFIG_FRUITCLAW_BOOT_START_WEBSERVER
#  include "netutils/httpd.h"
#endif
#ifdef CONFIG_FRUITCLAW_WIFI_AUTOSTART
#  include "netutils/netlib.h"
#  include "wireless/wapi.h"
#endif

#define FC_WIFI_SSID_LEN 64
#define FC_WIFI_PSK_LEN 96
#define FC_WIFI_REAPPLY_SUPPRESS_MS 300000
#define FC_SERVICE_BOOT_RETRIES 3
#define FC_SERVICE_BOOT_RETRY_DELAY_SEC 3
#define FC_SERVICE_PROBE_FAILURE_LIMIT 3
#define FC_WIFI_CONNECTIVITY_FAILURE_LIMIT 2
#define FC_HTTPD_SEND_FAILURE_RECOVERY_LIMIT 3
#define FC_HTTPD_BIND_BACKOFF_SEC 5

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED_READY_TIMEOUT_MS
#define FC_WIFI_INTERFACE_WAIT_MS \
  CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED_READY_TIMEOUT_MS
#else
#define FC_WIFI_INTERFACE_WAIT_MS 8000
#endif

#ifdef CONFIG_FRUITCLAW_SERVICE_ACTIVE_PROBES
#define FC_SERVICE_ACTIVE_TCP_PROBES 1
#else
#define FC_SERVICE_ACTIVE_TCP_PROBES 0
#endif

#ifndef CONFIG_SYSTEM_TELNETD_PORT
#  define CONFIG_SYSTEM_TELNETD_PORT 23
#endif

#ifndef CONFIG_SYSTEM_TELNETD_PID_PATH
#  define CONFIG_SYSTEM_TELNETD_PID_PATH "/tmp/telnetd.pid"
#endif

#ifndef CONFIG_EXAMPLES_FTPD_PORT
#  define CONFIG_EXAMPLES_FTPD_PORT 21
#endif

static pthread_mutex_t g_bootstrap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_bootstrap_state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_boot_network_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_wifi_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_wifi_last_ok_ms;
static int64_t g_wifi_last_probe_ms;
static int g_wifi_last_gateway_probe;
static int g_wifi_last_internet_probe;
static int64_t g_boot_network_start_ms;
static pthread_t g_boot_network_thread;
static bool g_bootstrapped;
static char g_bootstrap_stage[32] = "idle";
static int g_bootstrap_last_ret;
static bool g_boot_network_started;
static bool g_boot_network_done;
static bool g_boot_network_ok;
static bool g_boot_defer_session_guard;
static int64_t g_runtime_start_ms;
static pthread_mutex_t g_network_recovery_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_network_recovery_thread;
static bool g_network_recovery_running;
static int64_t g_network_recovery_start_ms;
static int64_t g_network_recovery_done_ms;
static int g_network_recovery_last_ret;
static unsigned int g_network_recovery_attempts;
static unsigned int g_network_recovery_failures;
static char g_network_recovery_reason[FC_SOURCE_LEN];
static pthread_mutex_t g_webserver_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_webserver_thread;
static bool g_webserver_started;
static bool g_webserver_listening;
static pid_t g_webserver_owner_pid = -1;
static unsigned long g_webserver_listens;
static unsigned long g_webserver_exits;
static int g_webserver_last_ret;
static int g_webserver_last_errno;
static int64_t g_webserver_last_start_ms;
static int64_t g_webserver_last_exit_ms;
static pthread_mutex_t g_services_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_services_thread;
static bool g_services_supervisor_started;
static unsigned long g_telnetd_attempts;
static unsigned long g_ftpd_attempts;
static unsigned long g_telnetd_restarts;
static unsigned long g_ftpd_restarts;
static bool g_telnetd_started;
static bool g_ftpd_started;
static bool g_telnetd_listening;
static bool g_ftpd_listening;
static int g_telnetd_last_ret;
static int g_ftpd_last_ret;
static int g_telnetd_probe_ret;
static int g_ftpd_probe_ret;
static int g_http_probe_ret;
static unsigned int g_service_bad_probes;
static unsigned int g_wifi_connectivity_bad_probes;
static unsigned long g_httpd_probe_last_recovered_send_fail;
static int64_t g_telnetd_last_ms;
static int64_t g_ftpd_last_ms;
static int64_t g_services_probe_ms;

#ifdef CONFIG_FRUITCLAW_ENABLE_SESSION_GUARD
static void fc_boot_require_network_or_recover(int ret, const char *stage)
{
  if (ret == 0)
    {
      return;
    }

  FC_LOGW("boot network stage %s failed ret=%d; continuing in degraded mode",
          stage ? stage : "unknown", ret);
}
#else
static void fc_boot_require_network_or_recover(int ret, const char *stage)
{
  (void)ret;
  (void)stage;
}
#endif

enum fc_service_id_e
{
  FC_SERVICE_TELNETD = 0,
  FC_SERVICE_FTPD
};

static int fc_bootstrap(void);
static void fc_bootstrap_set_stage(const char *stage, int ret);
static int fc_boot_services_start(void);
static int fc_network_recovery_request(const char *reason, int64_t now_ms,
                                       int64_t poll_age_ms);
static int fc_services_supervisor_start(void);
static void *fc_services_supervisor_main(void *arg);
static int fc_boot_network_guard_start(void);
static bool fc_service_started_snapshot(enum fc_service_id_e id);
static bool fc_service_probe_means_stopped(int ret);
static bool fc_service_autostart_enabled(enum fc_service_id_e id);
static int fc_service_start_id(enum fc_service_id_e id);
static bool fc_httpd_send_errno_means_recover(int err);
static bool fc_webserver_service_name(const char *name);
static void fc_network_transport_pump_window(const char *reason,
                                             unsigned int rounds);
#ifdef CONFIG_FRUITCLAW_BOOT_START_WEBSERVER
static int fc_webserver_supervisor_start(void);
static void *fc_webserver_supervisor_main(void *arg);
#endif

void netlib_server_activity_hook(FAR const char *phase, uint16_t portno)
{
#ifdef CONFIG_FRUITCLAW_BOOT_START_WEBSERVER
  if (ntohs(portno) == 80 && phase != NULL &&
      strcmp(phase, "listening") == 0)
    {
      pthread_mutex_lock(&g_webserver_lock);
      g_webserver_listening = true;
      g_webserver_listens++;
      g_webserver_last_start_ms = fc_mono_ms();
      pthread_mutex_unlock(&g_webserver_lock);
      fc_guard_session_heartbeat("webserver-listening");
    }
#else
  (void)phase;
  (void)portno;
#endif
}

static void fc_ignore_console_interrupts(const char *mode)
{
#ifdef SIGINT
  if (signal(SIGINT, SIG_IGN) == SIG_ERR)
    {
      FC_LOGW("%s: failed to ignore SIGINT: %d",
              mode != NULL ? mode : "runtime", errno);
    }
#else
  (void)mode;
#endif
}

static void fc_network_transport_pump_window(const char *reason,
                                             unsigned int rounds)
{
#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED
  unsigned int i;

  if (rounds == 0)
    {
      rounds = 1;
    }

  for (i = 0; i < rounds; i++)
    {
      (void)esp_hosted_spi_pump(1, false);
      fc_guard_session_heartbeat(reason != NULL ? reason : "net-pump");
      usleep(20 * USEC_PER_MSEC);
    }
#else
  (void)reason;
  (void)rounds;
#endif
}

static void usage(void)
{
  printf("usage:\n");
  printf("  fruitclaw boot\n");
  printf("  fruitclaw start\n");
  printf("  fruitclaw status\n");
  printf("  fruitclaw status-net\n");
  printf("  fruitclaw reboot\n");
  printf("  fruitclaw recover\n");
  printf("  fruitclaw wifi-up [--force]\n");
  printf("  fruitclaw wifi-probe\n");
  printf("  fruitclaw webserver\n");
  printf("  fruitclaw httpd-status\n");
  printf("  fruitclaw service status [telnetd|ftpd|webserver]\n");
  printf("  fruitclaw service <start|stop|restart|enable|disable> "
         "<telnetd|ftpd|webserver>\n");
  printf("  fruitclaw once <message>\n");
  printf("  fruitclaw selftest\n");
  printf("  fruitclaw tools\n");
  printf("  fruitclaw config\n");
  printf("  fruitclaw config set-wifi [ssid password]\n");
  printf("  fruitclaw config set-secret <telegram|deepseek> [value]\n");
  printf("  fruitclaw telegram-discover\n");
  printf("  fruitclaw telegram-test\n");
  printf("  fruitclaw telegram-inject <message>\n");
  printf("  fruitclaw deepseek-test\n");
  printf("  fruitclaw schedule list\n");
  printf("  fruitclaw schedule add-interval <id> <seconds> <prompt>\n");
  printf("  fruitclaw schedule add-once <id> <epoch-seconds> <prompt>\n");
  printf("  fruitclaw schedule add-after <id> <seconds> <prompt>\n");
  printf("  fruitclaw schedule add-cron <id> <expr> <prompt>\n");
  printf("  fruitclaw schedule add-boot <id> <prompt>\n");
  printf("  fruitclaw schedule remove <id>\n");
  printf("  fruitclaw berry-run <path> [json-args]\n");
  printf("  fruitclaw berry-smoke\n");
  printf("  fruitclaw guard-test\n");
  printf("  fruitclaw terminal-run <cmd...>\n");
  printf("  fruitclaw device list\n");
  printf("  fruitclaw device read <path> [max-bytes]\n");
  printf("  fruitclaw device write-text <path> <text...>\n");
  printf("  fruitclaw device write-hex <path> <hex...>\n");
  printf("  fruitclaw neopixels <off|color|effect...>\n");
  printf("  fruitclaw mcp status\n");
}

static int join_args(int argc, char *argv[], int first, char *out,
                     size_t out_len)
{
  int i;
  size_t off = 0;

  if (out == NULL || out_len == 0 || first >= argc)
    {
      return -EINVAL;
    }

  out[0] = '\0';
  for (i = first; i < argc; i++)
    {
      int n = snprintf(out + off, out_len - off, "%s%s",
                       i == first ? "" : " ", argv[i]);
      if (n < 0 || off + n >= out_len)
        {
          return -ENOSPC;
        }

      off += n;
    }

  return 0;
}

static void print_guard_status(char *status)
{
  char *saveptr = NULL;
  char *field;

  printf("  guard_status:\n");
  for (field = strtok_r(status, " ", &saveptr);
       field != NULL;
       field = strtok_r(NULL, " ", &saveptr))
    {
      printf("    %s\n", field);
    }
}

static int boot_run_command_guarded(const char *label, const char *cmd,
                                    bool required, fc_guard_long_t *guard,
                                    uint32_t stage)
{
  int ret;

  if (guard != NULL && stage != 0)
    {
      fc_guard_long_set_stage(guard, stage);
    }

  fc_bootstrap_set_stage(label != NULL ? label : "boot-command", 0);
  fc_guard_session_heartbeat(label != NULL ? label : "boot-command");
  ret = system(cmd);
  if (ret != 0)
    {
      FC_LOGW("boot command failed: %s ret=%d", label, ret);
      return required ? -EIO : 0;
    }

  FC_LOGI("boot command ok: %s", label);
  return 0;
}

static int boot_run_command(const char *label, const char *cmd,
                            bool required)
{
  return boot_run_command_guarded(label, cmd, required, NULL, 0);
}

static void wifi_parse_line(char *line, char *ssid, size_t ssid_len,
                            char *psk, size_t psk_len, int *key_mgmt,
                            int *cipher, int *ordinal)
{
  char *eq;
  char *key;
  char *value;

  fc_trim(line);
  if (line[0] == '\0' || line[0] == '#')
    {
      return;
    }

  eq = strchr(line, '=');
  if (eq == NULL)
    {
      if (*ordinal == 0)
        {
          fc_strlcpy(ssid, line, ssid_len);
        }
      else if (*ordinal == 1)
        {
          fc_strlcpy(psk, line, psk_len);
        }

      (*ordinal)++;
      return;
    }

  *eq = '\0';
  key = line;
  value = eq + 1;
  fc_trim(key);
  fc_trim(value);

  if (strcmp(key, "ssid") == 0)
    {
      fc_strlcpy(ssid, value, ssid_len);
    }
  else if (strcmp(key, "password") == 0 || strcmp(key, "psk") == 0)
    {
      fc_strlcpy(psk, value, psk_len);
    }
  else if (strcmp(key, "key_mgmt") == 0)
    {
      *key_mgmt = atoi(value);
    }
  else if (strcmp(key, "cipher") == 0)
    {
      *cipher = atoi(value);
    }
}

static int wifi_read_config_path(const char *path, char *ssid,
                                 size_t ssid_len, char *psk,
                                 size_t psk_len, int *key_mgmt,
                                 int *cipher)
{
  char buf[384];
  size_t pos = 0;
  int ordinal = 0;
  int ret;

  if (path == NULL || ssid == NULL || psk == NULL || key_mgmt == NULL ||
      cipher == NULL)
    {
      return -EINVAL;
    }

  ssid[0] = '\0';
  psk[0] = '\0';
  *key_mgmt = 3;
  *cipher = 2;

  ret = fc_read_text_file(path, buf, sizeof(buf), false);
  if (ret < 0)
    {
      return ret;
    }

  while (buf[pos] != '\0')
    {
      char line[128];
      size_t off = 0;

      while (buf[pos] != '\0' && buf[pos] != '\n' && off + 1 < sizeof(line))
        {
          line[off++] = buf[pos++];
        }

      while (buf[pos] != '\0' && buf[pos] != '\n')
        {
          pos++;
        }

      if (buf[pos] == '\n')
        {
          pos++;
        }

      line[off] = '\0';
      wifi_parse_line(line, ssid, ssid_len, psk, psk_len, key_mgmt,
                      cipher, &ordinal);
    }

  return ssid[0] == '\0' ? -ENOENT : 0;
}

static int wifi_try_config_path(const char *path, char *ssid,
                                size_t ssid_len, char *psk,
                                size_t psk_len, int *key_mgmt,
                                int *cipher, char *used_path,
                                size_t used_path_len)
{
  int ret;

  if (path == NULL || path[0] == '\0')
    {
      return -ENOENT;
    }

  ret = wifi_read_config_path(path, ssid, ssid_len, psk, psk_len,
                              key_mgmt, cipher);
  if (ret == 0 && used_path != NULL && used_path_len > 0)
    {
      fc_strlcpy(used_path, path, used_path_len);
    }

  return ret;
}

static int wifi_read_config(char *ssid, size_t ssid_len, char *psk,
                            size_t psk_len, int *key_mgmt, int *cipher,
                            char *used_path, size_t used_path_len)
{
  char path[FC_PATH_LEN];
  int ret;

  if (CONFIG_FRUITCLAW_WIFI_CONFIG_PATH[0] == '/')
    {
      ret = wifi_try_config_path(CONFIG_FRUITCLAW_WIFI_CONFIG_PATH,
                                 ssid, ssid_len, psk, psk_len,
                                 key_mgmt, cipher, used_path,
                                 used_path_len);
      if (ret == 0)
        {
          return 0;
        }
    }

  ret = fc_data_path(CONFIG_FRUITCLAW_WIFI_CONFIG_LEAF, path, sizeof(path));
  if (ret == 0)
    {
      ret = wifi_try_config_path(path, ssid, ssid_len, psk, psk_len,
                                 key_mgmt, cipher, used_path,
                                 used_path_len);
      if (ret == 0)
        {
          return 0;
        }
    }

  if (snprintf(path, sizeof(path), "%s/%s", CONFIG_FRUITCLAW_SD_DATA_DIR,
               CONFIG_FRUITCLAW_WIFI_CONFIG_LEAF) < (int)sizeof(path))
    {
      ret = wifi_try_config_path(path, ssid, ssid_len, psk, psk_len,
                                 key_mgmt, cipher, used_path,
                                 used_path_len);
      if (ret == 0)
        {
          return 0;
        }
    }

  if (snprintf(path, sizeof(path), "%s/%s", CONFIG_FRUITCLAW_DATA_DIR,
               CONFIG_FRUITCLAW_WIFI_CONFIG_LEAF) >= (int)sizeof(path))
    {
      return -ENAMETOOLONG;
    }

  return wifi_try_config_path(path, ssid, ssid_len, psk, psk_len,
                              key_mgmt, cipher, used_path, used_path_len);
}

static bool wifi_path_present(const char *path)
{
  return path != NULL && path[0] != '\0' && access(path, F_OK) == 0;
}

static bool wifi_config_present(void)
{
#ifdef CONFIG_FRUITCLAW_WIFI_AUTOSTART
  char path[FC_PATH_LEN];

  if (CONFIG_FRUITCLAW_WIFI_CONFIG_PATH[0] == '/' &&
      wifi_path_present(CONFIG_FRUITCLAW_WIFI_CONFIG_PATH))
    {
      return true;
    }

  if (fc_data_path(CONFIG_FRUITCLAW_WIFI_CONFIG_LEAF, path,
                   sizeof(path)) == 0 && wifi_path_present(path))
    {
      return true;
    }

  if (snprintf(path, sizeof(path), "%s/%s", CONFIG_FRUITCLAW_SD_DATA_DIR,
               CONFIG_FRUITCLAW_WIFI_CONFIG_LEAF) < (int)sizeof(path) &&
      wifi_path_present(path))
    {
      return true;
    }

  if (snprintf(path, sizeof(path), "%s/%s", CONFIG_FRUITCLAW_DATA_DIR,
               CONFIG_FRUITCLAW_WIFI_CONFIG_LEAF) < (int)sizeof(path) &&
      wifi_path_present(path))
    {
      return true;
    }

  return false;
#else
  return false;
#endif
}

static uint32_t fc_wifi_guard_timeout_ms(void)
{
  uint32_t timeout_ms;

#if CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS > 0
  timeout_ms = CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS;
#elif CONFIG_FRUITCLAW_HTTP_GUARD_TIMEOUT_MS > 0
  timeout_ms = CONFIG_FRUITCLAW_HTTP_GUARD_TIMEOUT_MS;
#else
  timeout_ms = CONFIG_FRUITCLAW_CLI_GUARD_TIMEOUT_MS;
#endif

  return timeout_ms;
}

#ifdef CONFIG_FRUITCLAW_WIFI_AUTOSTART
static int wifi_auth_from_version(int version, uint32_t *auth_wpa)
{
  if (auth_wpa == NULL)
    {
      return -EINVAL;
    }

  switch ((enum wpa_ver_e)version)
    {
      case WPA_VER_NONE:
        *auth_wpa = IW_AUTH_WPA_VERSION_DISABLED;
        return 0;

      case WPA_VER_1:
        *auth_wpa = IW_AUTH_WPA_VERSION_WPA;
        return 0;

      case WPA_VER_2:
        *auth_wpa = IW_AUTH_WPA_VERSION_WPA2;
        return 0;

      case WPA_VER_3:
        *auth_wpa = IW_AUTH_WPA_VERSION_WPA3;
        return 0;

      default:
        return -EINVAL;
    }
}

static int wifi_cipher_from_alg(enum wpa_alg_e alg, uint32_t *cipher)
{
  if (cipher == NULL)
    {
      return -EINVAL;
    }

  switch (alg)
    {
      case WPA_ALG_NONE:
        *cipher = IW_AUTH_CIPHER_NONE;
        return 0;

      case WPA_ALG_WEP:
        *cipher = IW_AUTH_CIPHER_WEP40;
        return 0;

      case WPA_ALG_TKIP:
        *cipher = IW_AUTH_CIPHER_TKIP;
        return 0;

      case WPA_ALG_CCMP:
        *cipher = IW_AUTH_CIPHER_CCMP;
        return 0;

      default:
        return -EINVAL;
    }
}

static int wifi_direct_ifdown(fc_guard_long_t *guard)
{
  int sock;
  int ret;

  if (guard != NULL)
    {
      fc_guard_long_set_stage(guard, FC_GUARD_STAGE_WIFI_IFUP);
    }

  fc_bootstrap_set_stage("wifi-ifdown", 0);
  fc_guard_session_heartbeat("wifi-ifdown");

  sock = wapi_make_socket();
  if (sock < 0)
    {
      FC_LOGW("wifi ifdown socket failed: %d", sock);
      return sock;
    }

  ret = wapi_set_ifdown(sock, CONFIG_FRUITCLAW_WIFI_IFNAME);
  close(sock);
  if (ret < 0)
    {
      FC_LOGW("wifi ifdown failed: %d", ret);
    }
  else
    {
      FC_LOGI("wifi ifdown ok");
    }

  return ret;
}

static int wifi_direct_ifup(fc_guard_long_t *guard)
{
  int sock;
  int ret;

  if (guard != NULL)
    {
      fc_guard_long_set_stage(guard, FC_GUARD_STAGE_WIFI_IFUP);
    }

  fc_bootstrap_set_stage("wifi-ifup", 0);
  fc_guard_session_heartbeat("wifi-ifup");

  sock = wapi_make_socket();
  if (sock < 0)
    {
      FC_LOGW("wifi ifup socket failed: %d", sock);
      return sock;
    }

  ret = wapi_set_ifup(sock, CONFIG_FRUITCLAW_WIFI_IFNAME);
  close(sock);
  if (ret < 0)
    {
      FC_LOGW("wifi ifup failed: %d", ret);
    }
  else
    {
      FC_LOGI("wifi ifup ok");
    }

  return ret;
}

static int wifi_direct_psk(int sock, const char *psk,
                           int alg_index, int version)
{
  enum wpa_alg_e alg = (enum wpa_alg_e)alg_index;
  uint32_t auth_wpa;
  uint32_t cipher;
  size_t passlen;
  int ret;

  if (psk == NULL)
    {
      return -EINVAL;
    }

  passlen = strnlen(psk, 64);
  if (passlen < 8 || passlen > 63)
    {
      return -EINVAL;
    }

  ret = wifi_auth_from_version(version, &auth_wpa);
  if (ret < 0)
    {
      return ret;
    }

  ret = wifi_cipher_from_alg(alg, &cipher);
  if (ret < 0)
    {
      return ret;
    }

  ret = wpa_driver_wext_set_auth_param(sock, CONFIG_FRUITCLAW_WIFI_IFNAME,
                                       IW_AUTH_WPA_VERSION, auth_wpa);
  if (ret < 0)
    {
      return ret;
    }

  ret = wpa_driver_wext_set_auth_param(sock, CONFIG_FRUITCLAW_WIFI_IFNAME,
                                       IW_AUTH_CIPHER_PAIRWISE, cipher);
  if (ret < 0)
    {
      return ret;
    }

  return wpa_driver_wext_set_key_ext(sock, CONFIG_FRUITCLAW_WIFI_IFNAME,
                                     alg, psk, passlen);
}

static int wifi_apply_config_once(const char *ssid, const char *psk,
                                  bool use_psk, int key_mgmt, int cipher,
                                  fc_guard_long_t *guard)
{
  int sock;
  int ret;

  (void)wifi_direct_ifup(guard);

  sock = wapi_make_socket();
  if (sock < 0)
    {
      return sock;
    }

  if (use_psk)
    {
      if (guard != NULL)
        {
          fc_guard_long_set_stage(guard, FC_GUARD_STAGE_WIFI_PSK);
        }

      fc_bootstrap_set_stage("wifi-psk", 0);
      fc_guard_session_heartbeat("wifi-psk");

      ret = wifi_direct_psk(sock, psk, key_mgmt, cipher);
      if (ret < 0)
        {
          close(sock);
          FC_LOGW("wifi psk failed: %d", ret);
          return ret;
        }
    }

  if (guard != NULL)
    {
      fc_guard_long_set_stage(guard, FC_GUARD_STAGE_WIFI_ESSID);
    }

  fc_bootstrap_set_stage("wifi-essid", 0);
  fc_guard_session_heartbeat("wifi-essid");

  ret = wapi_set_essid(sock, CONFIG_FRUITCLAW_WIFI_IFNAME, ssid,
                       WAPI_ESSID_ON);
  close(sock);
  if (ret < 0)
    {
      FC_LOGW("wifi essid failed: %d", ret);
      return ret;
    }

  sleep(3);

  if (guard != NULL)
    {
      fc_guard_long_set_stage(guard, FC_GUARD_STAGE_WIFI_DHCP);
    }

  fc_bootstrap_set_stage("wifi-dhcp", 0);
  fc_guard_session_heartbeat("wifi-dhcp");
  fc_network_transport_pump_window("wifi-dhcp-pre", 1);
  ret = netlib_obtain_ipv4addr(CONFIG_FRUITCLAW_WIFI_IFNAME);
  fc_network_transport_pump_window("wifi-dhcp-post", 1);
  if (ret < 0)
    {
      FC_LOGW("wifi dhcp failed: %d", ret);
    }
  else
    {
      FC_LOGI("wifi dhcp ok");
    }

  return ret;
}

static int wifi_apply_config_retry(const char *ssid, const char *psk,
                                   bool use_psk, int key_mgmt, int cipher,
                                   fc_guard_long_t *guard)
{
  int retries = CONFIG_FRUITCLAW_WIFI_COMMAND_RETRIES;
  int delay_sec = CONFIG_FRUITCLAW_WIFI_COMMAND_RETRY_DELAY_SEC;
  int attempt;
  int ret = -EIO;

  if (retries < 1)
    {
      retries = 1;
    }

  if (delay_sec < 0)
    {
      delay_sec = 0;
    }

  for (attempt = 1; attempt <= retries; attempt++)
    {
      ret = wifi_apply_config_once(ssid, psk, use_psk, key_mgmt, cipher,
                                   guard);
      if (ret == 0)
        {
          if (attempt > 1)
            {
              FC_LOGI("wifi config attempt %d/%d ok", attempt, retries);
            }

          return 0;
        }

      FC_LOGW("wifi config attempt %d/%d failed: %d",
              attempt, retries, ret);
      fc_operator_progress_mark("wifi-command-retry");
      if (attempt < retries && delay_sec > 0)
        {
          sleep(delay_sec);
        }
    }

  return ret;
}
#endif

static int wifi_probe_connectivity(bool update_status)
{
  const int probe_timeout_ms = 1500;
  const int probe_retries = 3;
  struct in_addr addr;
  uint8_t flags = 0;
  int flags_ret;
  int ip_ret;
  int gateway_ret = -EAGAIN;
  int internet_ret = -EAGAIN;
  int ret;

  flags_ret = netlib_getifstatus(CONFIG_FRUITCLAW_WIFI_IFNAME, &flags);
  ip_ret = netlib_get_ipv4addr(CONFIG_FRUITCLAW_WIFI_IFNAME, &addr);
  if (flags_ret < 0 || ip_ret < 0 || (flags & IFF_UP) == 0 ||
      (flags & IFF_RUNNING) == 0 || addr.s_addr == 0)
    {
      ret = -ENETDOWN;
      goto out;
    }

  fc_network_transport_pump_window("wifi-probe-drain", 1);
  fc_guard_session_heartbeat("wifi-gateway-probe");
  gateway_ret = netlib_check_ifconnectivity(CONFIG_FRUITCLAW_WIFI_IFNAME,
                                            probe_timeout_ms,
                                            probe_retries);
  if (gateway_ret <= 0)
    {
      ret = gateway_ret < 0 ? gateway_ret : -ETIMEDOUT;
      goto out;
    }

  fc_network_transport_pump_window("wifi-internet-drain", 1);
  fc_guard_session_heartbeat("wifi-internet-probe");
  internet_ret = netlib_check_ipconnectivity("8.8.8.8", probe_timeout_ms,
                                            probe_retries);
  ret = internet_ret > 0 ? internet_ret :
        internet_ret < 0 ? internet_ret : -ETIMEDOUT;

out:
  if (update_status)
    {
      g_wifi_last_probe_ms = fc_mono_ms();
      g_wifi_last_gateway_probe = gateway_ret;
      g_wifi_last_internet_probe = internet_ret;
      if (ret > 0)
        {
          g_wifi_last_ok_ms = g_wifi_last_probe_ms;
        }
    }

  return ret;
}

static int wifi_interface_ready(void)
{
  struct in_addr addr;
  uint8_t flags = 0;
  int flags_ret;
  int ip_ret;

  flags_ret = netlib_getifstatus(CONFIG_FRUITCLAW_WIFI_IFNAME, &flags);
  ip_ret = netlib_get_ipv4addr(CONFIG_FRUITCLAW_WIFI_IFNAME, &addr);
  if (flags_ret < 0)
    {
      return flags_ret;
    }

  if (ip_ret < 0)
    {
      return ip_ret;
    }

  if ((flags & IFF_UP) == 0 || (flags & IFF_RUNNING) == 0 ||
      addr.s_addr == 0)
    {
      return -ENETDOWN;
    }

  return 1;
}

static int wifi_wait_for_interface(fc_guard_long_t *guard)
{
  uint32_t waited_ms = 0;
  int last_ret = -ENODEV;

  while (waited_ms <= FC_WIFI_INTERFACE_WAIT_MS)
    {
      uint8_t flags = 0;

      last_ret = netlib_getifstatus(CONFIG_FRUITCLAW_WIFI_IFNAME, &flags);
      if (last_ret >= 0)
        {
          if (waited_ms > 0)
            {
              FC_LOGI("Wi-Fi interface %s registered after %u ms",
                      CONFIG_FRUITCLAW_WIFI_IFNAME, (unsigned int)waited_ms);
            }

          return 0;
        }

      if (guard != NULL)
        {
          fc_guard_long_set_stage(guard, FC_GUARD_STAGE_WIFI_START);
        }

      fc_network_transport_pump_window("wifi-if-wait", 1);
      usleep(230 * USEC_PER_MSEC);
      waited_ms += 250;
    }

  printf("Wi-Fi interface %s did not register after %u ms: %d\n",
         CONFIG_FRUITCLAW_WIFI_IFNAME, FC_WIFI_INTERFACE_WAIT_MS, last_ret);
  return last_ret < 0 ? last_ret : -ETIMEDOUT;
}

static void wifi_describe_interface(char *buf, size_t buflen)
{
  uint8_t flags = 0;
  int flags_ret;

  if (buf == NULL || buflen == 0)
    {
      return;
    }

  flags_ret = netlib_getifstatus(CONFIG_FRUITCLAW_WIFI_IFNAME, &flags);
  snprintf(buf, buflen,
           "if=%s flags_ret=%d flags=0x%02x up=%s running=%s",
           CONFIG_FRUITCLAW_WIFI_IFNAME, flags_ret, flags,
           (flags & IFF_UP) != 0 ? "yes" : "no",
           (flags & IFF_RUNNING) != 0 ? "yes" : "no");
}

static void wifi_describe_ipv4(char *buf, size_t buflen)
{
  struct in_addr addr;
  struct in_addr mask;
  struct in_addr router;
  char ip[INET_ADDRSTRLEN];
  char netmask[INET_ADDRSTRLEN];
  char gateway[INET_ADDRSTRLEN];
  int ip_ret;
  int mask_ret;
  int router_ret;

  if (buf == NULL || buflen == 0)
    {
      return;
    }

  addr.s_addr = 0;
  mask.s_addr = 0;
  router.s_addr = 0;
  ip_ret = netlib_get_ipv4addr(CONFIG_FRUITCLAW_WIFI_IFNAME, &addr);
  mask_ret = netlib_get_ipv4netmask(CONFIG_FRUITCLAW_WIFI_IFNAME, &mask);
  router_ret = netlib_get_dripv4addr(CONFIG_FRUITCLAW_WIFI_IFNAME, &router);
  if (inet_ntop(AF_INET, &addr, ip, sizeof(ip)) == NULL)
    {
      strlcpy(ip, "0.0.0.0", sizeof(ip));
    }

  if (inet_ntop(AF_INET, &mask, netmask, sizeof(netmask)) == NULL)
    {
      strlcpy(netmask, "0.0.0.0", sizeof(netmask));
    }

  if (inet_ntop(AF_INET, &router, gateway, sizeof(gateway)) == NULL)
    {
      strlcpy(gateway, "0.0.0.0", sizeof(gateway));
    }

  snprintf(buf, buflen,
           "ip_ret=%d mask_ret=%d router_ret=%d ip=%s mask=%s gw=%s",
           ip_ret, mask_ret, router_ret, ip, netmask, gateway);
}

static int cmd_wifi_up_guarded(bool already_guarded, bool force)
{
#ifdef CONFIG_FRUITCLAW_WIFI_AUTOSTART
  char ssid[FC_WIFI_SSID_LEN];
  char psk[FC_WIFI_PSK_LEN];
  char path[FC_PATH_LEN];
  char ifstatus[96];
  char ipstatus[128];
  int key_mgmt;
  int cipher;
  fc_guard_long_t guard;
  bool guarded = false;
  bool wifi_locked = false;
  int ret;

  ret = fc_bootstrap();
  if (ret < 0)
    {
      return 1;
    }

  ret = wifi_read_config(ssid, sizeof(ssid), psk, sizeof(psk), &key_mgmt,
                         &cipher, path, sizeof(path));
  if (ret < 0)
    {
      char data_path[FC_PATH_LEN];
      char sd_path[FC_PATH_LEN];

      fc_data_path(CONFIG_FRUITCLAW_WIFI_CONFIG_LEAF, data_path,
                   sizeof(data_path));
      snprintf(sd_path, sizeof(sd_path), "%s/%s",
               CONFIG_FRUITCLAW_SD_DATA_DIR,
               CONFIG_FRUITCLAW_WIFI_CONFIG_LEAF);
      printf("Wi-Fi config missing at explicit=%s active=%s sd=%s "
             "fallback=%s/%s\n",
             CONFIG_FRUITCLAW_WIFI_CONFIG_PATH[0] != '\0' ?
             CONFIG_FRUITCLAW_WIFI_CONFIG_PATH : "(unset)",
             data_path, sd_path, CONFIG_FRUITCLAW_DATA_DIR,
             CONFIG_FRUITCLAW_WIFI_CONFIG_LEAF);
      return 1;
    }

  FC_LOGD("wifi config loaded from %s", path);

  {
    uint32_t waited_ms = 0;
    uint32_t wait_limit_ms = already_guarded ? 0 : fc_wifi_guard_timeout_ms();

    for (; ; )
      {
        ret = pthread_mutex_trylock(&g_wifi_lock);
        if (ret == 0)
          {
            break;
          }

        if (ret != EBUSY || waited_ms >= wait_limit_ms)
          {
            printf("Wi-Fi bring-up already running\n");
            return 1;
          }

        fc_guard_session_heartbeat("wifi-lock-wait");
        usleep(250 * USEC_PER_MSEC);
        waited_ms += 250;
      }
  }

  wifi_locked = true;

  if (!force && g_wifi_last_ok_ms > 0 &&
      fc_mono_ms() - g_wifi_last_ok_ms < FC_WIFI_REAPPLY_SUPPRESS_MS)
    {
      printf("Wi-Fi bring-up skipped: recent success\n");
      ret = 0;
      goto out;
    }

  if (!already_guarded)
    {
      ret = fc_guard_long_start(FC_GUARD_STAGE_WIFI,
                                fc_wifi_guard_timeout_ms(), &guard);
      if (ret < 0)
        {
          printf("Wi-Fi guard arm failed: %d\n", ret);
          goto out;
        }

      guarded = true;
    }

  if (force)
    {
      printf("Wi-Fi force mode: reapplying interface configuration\n");
      (void)wifi_direct_ifdown(guarded ? &guard : NULL);
      g_wifi_last_ok_ms = 0;
      sleep(1);
    }

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED
  if (guarded)
    {
      fc_guard_long_set_stage(&guard, FC_GUARD_STAGE_WIFI_START);
    }

  fc_bootstrap_set_stage("wifi-esp-start", 0);
  fc_guard_session_heartbeat("wifi-esp-start");
  ret = board_fruitjam_esp_hosted_start();

  if (ret < 0)
    {
      printf("ESP-Hosted start failed: %d\n", ret);
      goto out;
    }

  ret = wifi_wait_for_interface(guarded ? &guard : NULL);
  if (ret < 0)
    {
      goto out;
    }
#endif

  ret = wifi_apply_config_retry(ssid, psk, psk[0] != '\0', key_mgmt,
                                cipher, guarded ? &guard : NULL);
#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED
  if (ret < 0 && force)
    {
      printf("ESP-Hosted recovery: resetting transport after Wi-Fi "
             "reapply failure\n");
      fc_bootstrap_set_stage("wifi-esp-recover", 0);
      fc_guard_session_heartbeat("wifi-esp-recover");
      ret = board_fruitjam_esp_hosted_recover();
      if (ret < 0)
        {
          printf("ESP-Hosted recovery failed: %d\n", ret);
          goto out;
        }

      sleep(1);
      ret = wifi_apply_config_retry(ssid, psk, psk[0] != '\0', key_mgmt,
                                    cipher, guarded ? &guard : NULL);
    }
#endif

  if (ret == 0)
    {
      int probe_ret;

      probe_ret = wifi_probe_connectivity(true);
      if (probe_ret > 0)
        {
          g_wifi_last_ok_ms = fc_mono_ms();
          wifi_describe_interface(ifstatus, sizeof(ifstatus));
          wifi_describe_ipv4(ipstatus, sizeof(ipstatus));
          printf("Wi-Fi connectivity ok: gateway_probe=%d "
                 "internet_probe=%d\n",
                 g_wifi_last_gateway_probe, g_wifi_last_internet_probe);
          printf("Wi-Fi interface: %s\n", ifstatus);
          printf("Wi-Fi IPv4: %s\n", ipstatus);
        }
      else
        {
          wifi_describe_interface(ifstatus, sizeof(ifstatus));
          wifi_describe_ipv4(ipstatus, sizeof(ipstatus));
          printf("Wi-Fi commands ok, but connectivity is not confirmed: "
                 "probe_ret=%d gateway_probe=%d internet_probe=%d\n",
                 probe_ret, g_wifi_last_gateway_probe,
                 g_wifi_last_internet_probe);
          printf("Wi-Fi interface: %s\n", ifstatus);
          printf("Wi-Fi IPv4: %s\n", ipstatus);
          ret = probe_ret < 0 ? probe_ret : -ETIMEDOUT;
        }
    }

out:
  if (guarded)
    {
      int guard_ret = fc_guard_long_stop(&guard);
      if (ret == 0 && guard_ret < 0)
        {
          ret = guard_ret;
        }
    }

  if (wifi_locked)
    {
      pthread_mutex_unlock(&g_wifi_lock);
    }

  return ret < 0 ? 1 : 0;
#else
  (void)already_guarded;
  (void)force;
  printf("Wi-Fi autostart is disabled.\n");
  return 1;
#endif
}

static int cmd_wifi_up(bool force)
{
  return cmd_wifi_up_guarded(false, force);
}

static void *fc_boot_network_main(void *arg)
{
  int network_ret = 0;

  (void)arg;

#ifdef CONFIG_FRUITCLAW_WIFI_AUTOSTART
    {
      int attempt;
      int ret = 1;

      for (attempt = 1; attempt <= 8; attempt++)
        {
          FC_LOGD("boot Wi-Fi attempt %d/8", attempt);
          fc_operator_progress_mark("wifi-attempt");
          ret = cmd_wifi_up_guarded(true, false);
          if (ret == 0)
            {
              fc_operator_progress_mark("wifi-ready");
              break;
            }

          if (wifi_interface_ready() > 0)
            {
              FC_LOGW("boot Wi-Fi has IP but active probes failed ret=%d; "
                      "starting services in degraded network mode", ret);
              network_ret = ret;
              ret = 0;
              fc_operator_progress_mark("wifi-ip-only");
              break;
            }

          fc_operator_progress_mark("wifi-retry");
          sleep(5);
        }

      if (ret != 0)
        {
          FC_LOGW("boot continuing without confirmed Wi-Fi");
          fc_operator_progress_mark("wifi-failed");
          network_ret = -ENETDOWN;
          fc_boot_require_network_or_recover(network_ret, "wifi");
        }
    }
#endif

#ifdef CONFIG_FRUITCLAW_ENABLE_SESSION_GUARD
  if (g_boot_defer_session_guard)
    {
      int ret;

      ret = fc_guard_session_start();
      if (ret < 0)
        {
          printf("session guard start failed: %d\n", ret);
        }
      else
        {
          fc_guard_session_heartbeat(network_ret == 0 ?
                                     "network-ready" :
                                     "network-attempted");
        }

      g_boot_defer_session_guard = false;
    }
#endif

#ifdef CONFIG_FRUITCLAW_BOOT_START_WEBSERVER
  {
    int web_ret;

    web_ret = fc_webserver_supervisor_start();
    if (web_ret < 0)
      {
        FC_LOGW("webserver supervisor start failed ret=%d", web_ret);
        fc_boot_require_network_or_recover(web_ret, "webserver");
      }
    else
      {
        fc_guard_session_heartbeat("webserver-start");
      }
  }
#endif

  {
    int attempt;
    int services_ret = -EIO;

    for (attempt = 1; attempt <= FC_SERVICE_BOOT_RETRIES; attempt++)
      {
        services_ret = fc_boot_services_start();
        if (services_ret == 0)
          {
            break;
          }

        FC_LOGW("boot services attempt %d/%d failed: %d",
                attempt, FC_SERVICE_BOOT_RETRIES, services_ret);
        fc_operator_progress_mark("services-retry");
        fc_guard_session_heartbeat("services-retry");
        if (attempt < FC_SERVICE_BOOT_RETRIES)
          {
            sleep(FC_SERVICE_BOOT_RETRY_DELAY_SEC);
        }
    }

    if (services_ret != 0)
      {
        fc_boot_require_network_or_recover(services_ret, "services");
      }
  }

  fc_guard_session_heartbeat("services");

#ifdef CONFIG_FRUITCLAW_BOOT_START_NTPC
  /* Keep remote operator paths ahead of time sync.  NTP can be slow when
   * esp-hosted is jittery, but Telnet/FTP/MCP must come up promptly.
   */

  boot_run_command("ntpcstart", "ntpcstart &", false);
  fc_guard_session_heartbeat("ntpcstart");
#endif

  pthread_mutex_lock(&g_boot_network_lock);
  g_boot_network_done = true;
  g_boot_network_ok = network_ret == 0;
  pthread_mutex_unlock(&g_boot_network_lock);
  fc_guard_session_heartbeat(network_ret == 0 ? "network-ready" :
                             "network-attempted");
  return NULL;
}

static int fc_boot_network_start(void)
{
  pthread_attr_t attr;
  int ret;

  pthread_mutex_lock(&g_boot_network_lock);
  if (g_boot_network_started)
    {
      pthread_mutex_unlock(&g_boot_network_lock);
      return 0;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_FRUITCLAW_BOOT_NETWORK_STACKSIZE);
  g_boot_network_done = false;
  g_boot_network_start_ms = fc_mono_ms();
  ret = pthread_create(&g_boot_network_thread, &attr, fc_boot_network_main,
                       NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      pthread_mutex_unlock(&g_boot_network_lock);
      return -ret;
    }

  pthread_detach(g_boot_network_thread);
  g_boot_network_started = true;
  pthread_mutex_unlock(&g_boot_network_lock);

  ret = fc_boot_network_guard_start();
  if (ret < 0)
    {
      FC_LOGW("boot network guard start failed ret=%d", ret);
    }

  return 0;
}

static void *fc_boot_network_guard_main(void *arg)
{
#if defined(CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD) && \
    CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS > 0
  int64_t start_ms = (int64_t)(intptr_t)arg;
  int64_t deadline_ms = start_ms + CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS;

  for (; ; )
    {
      int64_t now = fc_mono_ms();
      bool done;

      pthread_mutex_lock(&g_boot_network_lock);
      done = g_boot_network_done || g_boot_network_start_ms != start_ms;
      pthread_mutex_unlock(&g_boot_network_lock);
      if (done)
        {
          return NULL;
        }

      if (now >= deadline_ms)
        {
          FC_LOGE("boot network worker stale after %u ms; forcing recovery",
                  (unsigned int)CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS);
          fc_guard_force_recovery(FC_GUARD_STAGE_NETREC);
          return NULL;
        }

      fc_guard_session_heartbeat("boot-network-guard");
      sleep(1);
    }
#else
  (void)arg;
  return NULL;
#endif
}

static int fc_boot_network_guard_start(void)
{
#if defined(CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD) && \
    CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS > 0
  pthread_attr_t attr;
  pthread_t thread;
  int64_t start_ms;
  int ret;

  pthread_mutex_lock(&g_boot_network_lock);
  start_ms = g_boot_network_start_ms;
  pthread_mutex_unlock(&g_boot_network_lock);
  if (start_ms <= 0)
    {
      return -EINVAL;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_FRUITCLAW_GUARD_STACKSIZE);
  ret = pthread_create(&thread, &attr, fc_boot_network_guard_main,
                       (void *)(intptr_t)start_ms);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      return -ret;
    }

  pthread_detach(thread);
  return 0;
#else
  return 0;
#endif
}

static bool fc_boot_network_active_snapshot(void)
{
  bool active;

  pthread_mutex_lock(&g_boot_network_lock);
  active = g_boot_network_started && !g_boot_network_done;
  pthread_mutex_unlock(&g_boot_network_lock);
  return active;
}

static int64_t fc_boot_network_active_age_ms(int64_t now_ms)
{
  int64_t age = -1;

  pthread_mutex_lock(&g_boot_network_lock);
  if (g_boot_network_started && !g_boot_network_done &&
      g_boot_network_start_ms > 0 && now_ms >= g_boot_network_start_ms)
    {
      age = now_ms - g_boot_network_start_ms;
    }

  pthread_mutex_unlock(&g_boot_network_lock);
  return age;
}

static void fc_boot_network_status_snapshot(bool *started, bool *done,
                                            bool *ok)
{
  pthread_mutex_lock(&g_boot_network_lock);
  if (started != NULL)
    {
      *started = g_boot_network_started;
    }

  if (done != NULL)
    {
      *done = g_boot_network_done;
    }

  if (ok != NULL)
    {
      *ok = g_boot_network_ok;
    }

  pthread_mutex_unlock(&g_boot_network_lock);
}

static int fc_boot_network_wait(uint32_t timeout_ms, bool *ok)
{
  int64_t start_ms = fc_mono_ms();

  for (; ; )
    {
      bool started;
      bool done;
      bool ready;
      int64_t now;

      fc_boot_network_status_snapshot(&started, &done, &ready);
      if (!started)
        {
          return -ENOTCONN;
        }

      if (done)
        {
          if (ok != NULL)
            {
              *ok = ready;
            }

          return ready ? 0 : -ENETDOWN;
        }

      now = fc_mono_ms();
      if (timeout_ms > 0 && now >= start_ms &&
          now - start_ms >= timeout_ms)
        {
          if (ok != NULL)
            {
              *ok = false;
            }

          return -ETIMEDOUT;
        }

      fc_operator_progress_mark("boot-network-wait");
      fc_guard_session_heartbeat("boot-network-wait");
      sleep(1);
    }
}

static void fc_bootstrap_set_stage(const char *stage, int ret)
{
  pthread_mutex_lock(&g_bootstrap_state_lock);
  snprintf(g_bootstrap_stage, sizeof(g_bootstrap_stage), "%s",
           stage != NULL ? stage : "unknown");
  g_bootstrap_last_ret = ret;
  pthread_mutex_unlock(&g_bootstrap_state_lock);
}

void fc_bootstrap_note(const char *stage, int ret)
{
  fc_bootstrap_set_stage(stage, ret);
}

static void fc_bootstrap_get_stage(char *stage, size_t stage_len,
                                   int *ret)
{
  pthread_mutex_lock(&g_bootstrap_state_lock);
  if (stage != NULL && stage_len > 0)
    {
      snprintf(stage, stage_len, "%s", g_bootstrap_stage);
    }

  if (ret != NULL)
    {
      *ret = g_bootstrap_last_ret;
    }

  pthread_mutex_unlock(&g_bootstrap_state_lock);
}

static int fc_bootstrap(void)
{
  int ret;

  pthread_mutex_lock(&g_bootstrap_lock);
  if (g_bootstrapped)
    {
      pthread_mutex_unlock(&g_bootstrap_lock);
      return 0;
    }

  fc_bootstrap_set_stage("uptime-guard", 0);
  ret = fc_guard_uptime_start();
  if (ret < 0)
    {
      printf("max uptime guard start failed: %d\n", ret);
    }

  fc_bootstrap_set_stage("data-dir", 0);
  ret = fc_init_data_dir();
  if (ret < 0)
    {
      fc_bootstrap_set_stage("data-dir-failed", ret);
      printf("data directory init failed: %d\n", ret);
      goto out;
    }

  fc_bootstrap_set_stage("queue-init", 0);
  fc_queue_init(fc_main_queue());
  fc_queue_init(fc_agent_queue());

  fc_bootstrap_set_stage("cap-init", 0);
  ret = fc_cap_init();
  if (ret < 0)
    {
      fc_bootstrap_set_stage("cap-init-failed", ret);
      printf("capability init failed: %d\n", ret);
      goto out;
    }

  fc_bootstrap_set_stage("scheduler-load", 0);
  fc_scheduler_load();

  fc_bootstrap_set_stage("web-register", 0);
  ret = fc_web_register_http();
  if (ret < 0)
    {
      fc_bootstrap_set_stage("web-register-failed", ret);
      printf("web register failed: %d\n", ret);
      goto out;
    }

#ifdef CONFIG_FRUITCLAW_MCP_SERVER
  fc_bootstrap_set_stage("mcp-register", 0);
  ret = fc_mcp_register_http();
  if (ret < 0)
    {
      fc_bootstrap_set_stage("mcp-register-failed", ret);
      printf("MCP register failed: %d\n", ret);
      goto out;
    }
#endif
  g_bootstrapped = true;
  fc_bootstrap_set_stage("ready", 0);

out:
  pthread_mutex_unlock(&g_bootstrap_lock);
  return ret;
}

static void fc_bootstrap_probe(bool *ready, bool *busy)
{
  int ret;

  if (ready != NULL)
    {
      *ready = false;
    }

  if (busy != NULL)
    {
      *busy = false;
    }

  ret = pthread_mutex_trylock(&g_bootstrap_lock);
  if (ret != 0)
    {
      if (busy != NULL)
        {
          *busy = true;
        }

      return;
    }

  if (ready != NULL)
    {
      *ready = g_bootstrapped;
    }

  pthread_mutex_unlock(&g_bootstrap_lock);
}

static int fc_cli_guard_begin(const char *label, fc_guard_long_t *guard)
{
  int ret;

  if (guard == NULL)
    {
      return -EINVAL;
    }

  ret = fc_guard_long_start(FC_GUARD_STAGE_CLI,
                            CONFIG_FRUITCLAW_CLI_GUARD_TIMEOUT_MS,
                            guard);
  if (ret < 0)
    {
      printf("%s guard start failed: %d\n",
             label != NULL ? label : "cli", ret);
    }

  return ret;
}

static int fc_cli_guard_begin_optional_status(const char *label,
                                              fc_guard_long_t *guard)
{
  int ret;

  if (guard == NULL)
    {
      return -EINVAL;
    }

  memset(guard, 0, sizeof(*guard));
  ret = fc_guard_long_start(FC_GUARD_STAGE_CLI,
                            CONFIG_FRUITCLAW_CLI_GUARD_TIMEOUT_MS,
                            guard);
  if (ret < 0)
    {
      printf("%s guard busy/unavailable: %d; printing read-only status "
             "without CLI guard\n", label != NULL ? label : "status", ret);
      guard->reentrant = true;
      return 0;
    }

  return 0;
}

static int fc_cli_guard_end(fc_guard_long_t *guard, int cmd_ret)
{
  int guard_ret;

  guard_ret = fc_guard_long_stop(guard);
  if (guard_ret < 0 && cmd_ret == 0)
    {
      printf("cli guard stop failed: %d\n", guard_ret);
      return 1;
    }

  return cmd_ret;
}

static void fc_webserver_reconcile_locked(void)
{
  if (g_webserver_started && g_webserver_owner_pid > 0 &&
      kill(g_webserver_owner_pid, 0) < 0 && errno == ESRCH)
    {
      g_webserver_started = false;
      g_webserver_listening = false;
      g_webserver_last_ret = -ESRCH;
      g_webserver_last_errno = ESRCH;
      g_webserver_last_exit_ms = fc_mono_ms();
      g_webserver_owner_pid = -1;
    }
}

static bool fc_webserver_running_snapshot(void)
{
  bool started;
  bool listening;

  pthread_mutex_lock(&g_webserver_lock);
  fc_webserver_reconcile_locked();
  started = g_webserver_started;
  listening = g_webserver_listening;
  pthread_mutex_unlock(&g_webserver_lock);

  return started && listening;
}

static int fc_webserver_supervisor_start(void)
{
#ifdef CONFIG_FRUITCLAW_BOOT_START_WEBSERVER
  pthread_attr_t attr;
  int ret;

  pthread_mutex_lock(&g_webserver_lock);
  fc_webserver_reconcile_locked();
  if (g_webserver_started)
    {
      pthread_mutex_unlock(&g_webserver_lock);
      return 0;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_NETUTILS_HTTPDSTACKSIZE);
  ret = pthread_create(&g_webserver_thread, &attr,
                       fc_webserver_supervisor_main, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      pthread_mutex_unlock(&g_webserver_lock);
      return -ret;
    }

  pthread_detach(g_webserver_thread);
  g_webserver_started = true;
  g_webserver_owner_pid = getpid();
  pthread_mutex_unlock(&g_webserver_lock);
  return 0;
#else
  return 0;
#endif
}

static int cmd_wifi_probe(void)
{
#ifdef CONFIG_FRUITCLAW_WIFI_AUTOSTART
  fc_guard_long_t guard;
  char ifstatus[96];
  char ipstatus[128];
  int ret;
  int lock_ret;

  ret = fc_cli_guard_begin("wifi-probe", &guard);
  if (ret < 0)
    {
      return 1;
    }

  ret = fc_bootstrap();
  if (ret < 0)
    {
      return fc_cli_guard_end(&guard, 1);
    }

  lock_ret = pthread_mutex_trylock(&g_wifi_lock);
  if (lock_ret != 0)
    {
      printf("Wi-Fi probe skipped: Wi-Fi bring-up/recovery is running "
             "(lock_ret=%d)\n", lock_ret);
      return fc_cli_guard_end(&guard, 1);
    }

  ret = wifi_probe_connectivity(true);
  pthread_mutex_unlock(&g_wifi_lock);

  wifi_describe_interface(ifstatus, sizeof(ifstatus));
  wifi_describe_ipv4(ipstatus, sizeof(ipstatus));
  printf("Wi-Fi probe: ret=%d gateway_probe=%d internet_probe=%d\n",
         ret, g_wifi_last_gateway_probe, g_wifi_last_internet_probe);
  printf("Wi-Fi interface: %s\n", ifstatus);
  printf("Wi-Fi IPv4: %s\n", ipstatus);
  return fc_cli_guard_end(&guard, ret > 0 ? 0 : 1);
#else
  printf("Wi-Fi autostart disabled\n");
  return 1;
#endif
}

#ifdef CONFIG_FRUITCLAW_BOOT_START_WEBSERVER
static void *fc_webserver_supervisor_main(void *arg)
{
  unsigned int bind_failures = 0;

  (void)arg;

  httpd_init();

  for (; ; )
    {
      bool bound_elsewhere;
      int ret;
      int err;

      pthread_mutex_lock(&g_webserver_lock);
      g_webserver_listening = false;
      g_webserver_last_start_ms = fc_mono_ms();
      pthread_mutex_unlock(&g_webserver_lock);

      fc_operator_progress_mark("webserver");
      fc_guard_session_heartbeat("webserver");
      errno = 0;
      ret = httpd_listen();
      err = errno;
      bound_elsewhere = (err == EADDRINUSE);

      pthread_mutex_lock(&g_webserver_lock);
      g_webserver_listening = false;
      g_webserver_exits++;
      g_webserver_last_ret = ret;
      g_webserver_last_errno = err;
      g_webserver_last_exit_ms = fc_mono_ms();
      pthread_mutex_unlock(&g_webserver_lock);

      if (bound_elsewhere)
        {
          bind_failures++;

          pthread_mutex_lock(&g_webserver_lock);
          g_webserver_listening = true;
          g_webserver_last_ret = 0;
          g_webserver_last_errno = EADDRINUSE;
          pthread_mutex_unlock(&g_webserver_lock);

          FC_LOGW("webserver bind failed ret=%d errno=%d failures=%u",
                  ret, err, bind_failures);
          fc_guard_session_heartbeat("webserver-bound-elsewhere");
          sleep(FC_HTTPD_BIND_BACKOFF_SEC);
        }
      else
        {
          bind_failures = 0;
          FC_LOGW("webserver listener exited ret=%d errno=%d; restarting",
                  ret, err);
          fc_guard_session_heartbeat("webserver-restart");
          sleep(5);
        }
    }

  return NULL;
}
#endif

int fc_webserver_status_format(char *out, size_t out_len)
{
  char httpd_status[1024];
  bool started;
  bool listening;
  unsigned long listens;
  unsigned long exits;
  int last_ret;
  int last_errno;
  int64_t start_ms;
  int64_t exit_ms;
  int64_t now = fc_mono_ms();

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  (void)fc_web_httpd_status_format(httpd_status, sizeof(httpd_status));

  pthread_mutex_lock(&g_webserver_lock);
  fc_webserver_reconcile_locked();
  started = g_webserver_started;
  listening = g_webserver_listening;
  listens = g_webserver_listens;
  exits = g_webserver_exits;
  last_ret = g_webserver_last_ret;
  last_errno = g_webserver_last_errno;
  start_ms = g_webserver_last_start_ms;
  exit_ms = g_webserver_last_exit_ms;
  pthread_mutex_unlock(&g_webserver_lock);

  snprintf(out, out_len,
           "webserver: supervisor=%s listening=%s listens=%lu exits=%lu "
           "last_ret=%d last_errno=%d last_start_age_ms=%lld "
           "last_exit_age_ms=%lld %s",
           started ? "started" : "stopped",
           listening ? "yes" : "no", listens, exits, last_ret, last_errno,
           start_ms > 0 ? (long long)(now - start_ms) : -1,
           exit_ms > 0 ? (long long)(now - exit_ms) : -1,
           httpd_status);
  return 0;
}

static bool fc_webserver_service_name(const char *name)
{
  return name != NULL &&
         (strcmp(name, "web") == 0 ||
          strcmp(name, "http") == 0 ||
          strcmp(name, "httpd") == 0 ||
          strcmp(name, "webserver") == 0 ||
          strcmp(name, "mcp") == 0);
}

static bool fc_webserver_compiled(void)
{
#ifdef CONFIG_FRUITCLAW_BOOT_START_WEBSERVER
  return true;
#else
  return false;
#endif
}

static int fc_webserver_status_json(const char *action, char *out,
                                    size_t out_len)
{
  char httpd_json[384];
  bool started;
  bool listening;
  unsigned long listens;
  unsigned long exits;
  int last_ret;
  int last_errno;
  int64_t start_ms;
  int64_t exit_ms;
  int64_t now = fc_mono_ms();
  int probe_ret;

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  (void)fc_web_httpd_status_json(httpd_json, sizeof(httpd_json));

  pthread_mutex_lock(&g_webserver_lock);
  fc_webserver_reconcile_locked();
  started = g_webserver_started;
  listening = g_webserver_listening;
  listens = g_webserver_listens;
  exits = g_webserver_exits;
  last_ret = g_webserver_last_ret;
  last_errno = g_webserver_last_errno;
  start_ms = g_webserver_last_start_ms;
  exit_ms = g_webserver_last_exit_ms;
  pthread_mutex_unlock(&g_webserver_lock);

  probe_ret = started && listening ? 0 : -ENOTCONN;
  snprintf(out, out_len,
           "{\"ok\":true,\"service\":\"webserver\",\"action\":\"%s\","
           "\"compiled\":%s,\"boot_default\":%s,\"enabled\":%s,"
           "\"autostart\":%s,\"started\":%s,\"listening\":%s,"
           "\"start_supported\":%s,\"stop_supported\":false,"
           "\"listens\":%lu,\"exits\":%lu,\"last_ret\":%d,"
           "\"last_errno\":%d,\"probe_ret\":%d,"
           "\"last_start_age_ms\":%lld,\"last_exit_age_ms\":%lld,%s}",
           action != NULL ? action : "status",
           fc_webserver_compiled() ? "true" : "false",
           fc_webserver_compiled() ? "true" : "false",
           fc_webserver_compiled() ? "true" : "false",
           fc_webserver_compiled() ? "true" : "false",
           started ? "true" : "false",
           listening ? "true" : "false",
           fc_webserver_compiled() ? "true" : "false",
           listens, exits, last_ret, last_errno, probe_ret,
           start_ms > 0 ? (long long)(now - start_ms) : -1,
           exit_ms > 0 ? (long long)(now - exit_ms) : -1,
           httpd_json);

  return 0;
}

static int fc_webserver_service_control(const char *verb, char *out,
                                        size_t out_len)
{
  int ret;

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  if (strcmp(verb, "status") == 0)
    {
      return fc_webserver_status_json(verb, out, out_len);
    }

  if (!fc_webserver_compiled())
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"service\":\"webserver\",\"action\":\"%s\","
               "\"error\":\"service not compiled\",\"code\":%d}",
               verb, -ENOSYS);
      return -ENOSYS;
    }

  if (strcmp(verb, "start") == 0 || strcmp(verb, "restart") == 0)
    {
      if (!fc_webserver_running_snapshot())
        {
          ret = boot_run_command_guarded("webserver",
                                         "fruitclaw webserver &",
                                         false, NULL, 0);
          if (ret < 0)
            {
              snprintf(out, out_len,
                       "{\"ok\":false,\"service\":\"webserver\","
                       "\"action\":\"%s\",\"error\":\"service control "
                       "failed\",\"code\":%d}",
                       verb, ret);
              return ret;
            }
        }

      usleep(500000);
      return fc_webserver_status_json(verb, out, out_len);
    }

  if (strcmp(verb, "stop") == 0)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"service\":\"webserver\",\"action\":\"%s\","
               "\"error\":\"stop is not supported for the embedded "
               "webserver supervisor\",\"code\":%d}",
               verb, -ENOTSUP);
      return -ENOTSUP;
    }

  if (strcmp(verb, "enable") == 0 || strcmp(verb, "disable") == 0)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"service\":\"webserver\",\"action\":\"%s\","
               "\"error\":\"webserver enable/disable is compile-time in "
               "this build\",\"code\":%d}",
               verb, -ENOTSUP);
      return -ENOTSUP;
    }

  snprintf(out, out_len,
           "{\"ok\":false,\"service\":\"webserver\",\"action\":\"%s\","
           "\"error\":\"unknown action\",\"code\":%d}",
           verb, -EINVAL);
  return -EINVAL;
}

#ifdef CONFIG_FRUITCLAW_BOOT_START_WEBSERVER
static int fc_webserver_passive_probe(void)
{
  bool started;
  bool listening;
  unsigned long send_fail = 0;
  unsigned long send_fail_streak = 0;
  int send_errno = 0;
  int64_t last_age_ms = -1;

  pthread_mutex_lock(&g_webserver_lock);
  fc_webserver_reconcile_locked();
  started = g_webserver_started;
  listening = g_webserver_listening;
  pthread_mutex_unlock(&g_webserver_lock);

  if (!started || !listening)
    {
      return -ENOTCONN;
    }

  (void)fc_web_httpd_activity_snapshot(NULL, NULL, &send_fail,
                                       &send_fail_streak, &send_errno,
                                       &last_age_ms);
  if (send_fail_streak >= FC_HTTPD_SEND_FAILURE_RECOVERY_LIMIT &&
      send_fail != g_httpd_probe_last_recovered_send_fail &&
      fc_httpd_send_errno_means_recover(send_errno))
    {
      g_httpd_probe_last_recovered_send_fail = send_fail;
      FC_LOGW("HTTPD send failure streak=%lu total=%lu errno=%d "
              "age_ms=%lld; requesting network recovery",
              send_fail_streak, send_fail, send_errno,
              (long long)last_age_ms);
      return -send_errno;
    }

  return 0;
}
#endif

static int cmd_boot(void)
{
  fc_guard_long_t boot_guard;
  bool boot_guarded = false;
  int ret;

  fc_ignore_console_interrupts("boot");
  g_boot_defer_session_guard = true;

  ret = fc_guard_long_start(FC_GUARD_STAGE_BOOT,
                            CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS > 0 ?
                            CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS :
                            CONFIG_FRUITCLAW_CLI_GUARD_TIMEOUT_MS,
                            &boot_guard);
  if (ret < 0)
    {
      printf("boot guard start failed: %d\n", ret);
    }
  else
    {
      boot_guarded = true;
    }

  ret = fc_bootstrap();
  if (boot_guarded)
    {
      int guard_ret = fc_guard_long_stop(&boot_guard);

      if (ret == 0 && guard_ret < 0)
        {
          ret = guard_ret;
        }
    }

  if (ret < 0)
    {
      return 1;
    }

  fc_guard_session_heartbeat("boot");

  ret = fc_boot_network_start();
  if (ret < 0)
    {
      printf("boot network start failed: %d\n", ret);
      g_boot_defer_session_guard = false;
    }
  else
    {
      bool network_ok = false;
      uint32_t wait_ms =
        CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS > 0 ?
        CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS :
        CONFIG_FRUITCLAW_CLI_GUARD_TIMEOUT_MS;
      int wait_ret;

      wait_ret = fc_boot_network_wait(wait_ms, &network_ok);
      if (wait_ret < 0)
        {
          FC_LOGW("boot network wait finished degraded ret=%d ok=%s",
                  wait_ret, network_ok ? "yes" : "no");
        }
      else
        {
          FC_LOGI("boot network ready");
        }
    }

  return fc_runtime_start();
}

static int cmd_webserver_run(void)
{
#ifdef CONFIG_FRUITCLAW_BOOT_START_WEBSERVER
  int ret;

  ret = fc_bootstrap();
  if (ret < 0)
    {
      return 1;
    }

  ret = fc_webserver_supervisor_start();
  if (ret < 0)
    {
      printf("webserver supervisor start failed: %d\n", ret);
      return 1;
    }

  printf("FruitClaw webserver helper started.\n");
  for (; ; )
    {
      fc_guard_session_heartbeat("webserver-helper");
      fc_operator_progress_mark("webserver-helper");
      sleep(5);
    }
#else
  printf("webserver support is not compiled in this FruitClaw build\n");
  return 1;
#endif
}

static int cmd_httpd_status(void)
{
  char status[1536];

  if (fc_webserver_status_format(status, sizeof(status)) < 0)
    {
      return 1;
    }

  printf("%s\n", status);
  return 0;
}

static uint32_t fc_network_recovery_timeout_ms(void)
{
#if CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS > 0
  return CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS;
#else
  return CONFIG_FRUITCLAW_CLI_GUARD_TIMEOUT_MS;
#endif
}

static uint32_t fc_network_recovery_grace_ms(void)
{
  uint32_t timeout = fc_network_recovery_timeout_ms();
  uint32_t grace = timeout / 2;

  if (grace < 15000)
    {
      grace = 15000;
    }

  return grace;
}

static bool fc_network_recovery_stale_from_start(int64_t now_ms,
                                                 int64_t start_ms)
{
  uint32_t timeout_ms = fc_network_recovery_timeout_ms();

  return start_ms > 0 && timeout_ms > 0 &&
         now_ms >= start_ms &&
         now_ms - start_ms > timeout_ms;
}

static bool fc_network_recovery_active_snapshot(int64_t *age_ms,
                                                bool *stale)
{
  bool running;
  int64_t start_ms;
  int64_t now = fc_mono_ms();

  pthread_mutex_lock(&g_network_recovery_lock);
  running = g_network_recovery_running;
  start_ms = g_network_recovery_start_ms;
  pthread_mutex_unlock(&g_network_recovery_lock);

  if (age_ms != NULL)
    {
      *age_ms = running && start_ms > 0 && now >= start_ms ?
                now - start_ms : -1;
    }

  if (stale != NULL)
    {
      *stale = running && fc_network_recovery_stale_from_start(now, start_ms);
    }

  return running;
}

static void fc_network_recovery_finish(int ret)
{
  pthread_mutex_lock(&g_network_recovery_lock);
  g_network_recovery_running = false;
  g_network_recovery_done_ms = fc_mono_ms();
  g_network_recovery_last_ret = ret;
  if (ret != 0)
    {
      g_network_recovery_failures++;
    }

  pthread_mutex_unlock(&g_network_recovery_lock);
}

static void *fc_network_recovery_main(void *arg)
{
  int ret;
  int probe_ret;

  (void)arg;

  FC_LOGW("operator recovery: probing Wi-Fi before reset");
  fc_operator_progress_mark("net-recover");
  fc_guard_session_heartbeat("net-recover");

  fc_network_transport_pump_window("net-recover-drain", 5);
  probe_ret = wifi_probe_connectivity(true);
  if (probe_ret > 0)
    {
      FC_LOGI("operator recovery: link recovered after pump probe=%d",
              probe_ret);
      ret = 0;
    }
  else
    {
      FC_LOGW("operator recovery: pump probe failed ret=%d; restarting "
              "Wi-Fi", probe_ret);

      /* Recovery runs in a detached background thread after Telegram/network
       * staleness.  Do not nest the Wi-Fi short guard here; the runtime
       * operator loop supervises this worker and escalates with FCNR if
       * recovery itself becomes stale.
       */

      ret = cmd_wifi_up_guarded(true, true);
    }

#ifdef CONFIG_FRUITCLAW_BOOT_START_WEBSERVER
  {
    int web_ret = fc_webserver_supervisor_start();

    if (web_ret < 0)
      {
        FC_LOGW("webserver supervisor restart failed ret=%d", web_ret);
      }

    if (ret == 0 && web_ret < 0)
      {
        ret = web_ret;
      }
  }
#endif

  {
    int services_ret = fc_boot_services_start();

    if (services_ret < 0)
      {
        FC_LOGW("boot services restart failed ret=%d", services_ret);
      }

    if (ret == 0 && services_ret < 0)
      {
        ret = services_ret;
      }
  }

  fc_operator_progress_mark(ret == 0 ? "net-recovered" :
                            "net-recover-fail");
  FC_LOGW("operator recovery finished ret=%d", ret);
  fc_network_recovery_finish(ret);
  return NULL;
}

static int fc_network_recovery_request(const char *reason, int64_t now_ms,
                                       int64_t poll_age_ms)
{
  pthread_attr_t attr;
  int64_t poll_start_ms = poll_age_ms >= 0 ? now_ms - poll_age_ms : now_ms;
  int64_t last_attempt_ms;
  int64_t wait_base_ms;
  uint32_t grace_ms = fc_network_recovery_grace_ms();
  int ret;

  pthread_mutex_lock(&g_network_recovery_lock);
  if (g_network_recovery_running)
    {
      bool stale =
        fc_network_recovery_stale_from_start(now_ms,
                                             g_network_recovery_start_ms);

      pthread_mutex_unlock(&g_network_recovery_lock);
      return stale ? -ETIMEDOUT : 0;
    }

  last_attempt_ms = g_network_recovery_start_ms;
  if (last_attempt_ms > 0 && last_attempt_ms > poll_start_ms)
    {
      wait_base_ms = g_network_recovery_done_ms > 0 ?
                     g_network_recovery_done_ms : last_attempt_ms;
      if (now_ms - wait_base_ms < grace_ms)
        {
          pthread_mutex_unlock(&g_network_recovery_lock);
          return 0;
        }

      pthread_mutex_unlock(&g_network_recovery_lock);
      return -ETIMEDOUT;
    }

  g_network_recovery_running = true;
  g_network_recovery_start_ms = now_ms;
  g_network_recovery_done_ms = 0;
  g_network_recovery_last_ret = 0;
  g_network_recovery_attempts++;
  fc_strlcpy(g_network_recovery_reason, reason ? reason : "operator",
             sizeof(g_network_recovery_reason));
  pthread_mutex_unlock(&g_network_recovery_lock);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_FRUITCLAW_BOOT_NETWORK_STACKSIZE);
  ret = pthread_create(&g_network_recovery_thread, &attr,
                       fc_network_recovery_main, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      pthread_mutex_lock(&g_network_recovery_lock);
      g_network_recovery_running = false;
      g_network_recovery_done_ms = fc_mono_ms();
      g_network_recovery_last_ret = -ret;
      g_network_recovery_failures++;
      pthread_mutex_unlock(&g_network_recovery_lock);
      return -ret;
    }

  pthread_detach(g_network_recovery_thread);
  return 0;
}

int fc_network_recovery_status_format(char *out, size_t out_len)
{
  bool running;
  int64_t start_ms;
  int64_t done_ms;
  int last_ret;
  unsigned int attempts;
  unsigned int failures;
  char reason[FC_SOURCE_LEN];
  bool stale;
  int64_t now = fc_mono_ms();

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_network_recovery_lock);
  running = g_network_recovery_running;
  start_ms = g_network_recovery_start_ms;
  done_ms = g_network_recovery_done_ms;
  last_ret = g_network_recovery_last_ret;
  attempts = g_network_recovery_attempts;
  failures = g_network_recovery_failures;
  fc_strlcpy(reason, g_network_recovery_reason, sizeof(reason));
  pthread_mutex_unlock(&g_network_recovery_lock);

  stale = running && fc_network_recovery_stale_from_start(now, start_ms);

  snprintf(out, out_len,
           "network_recovery: running=%s attempts=%u failures=%u "
           "last_ret=%d last_start_age_ms=%lld last_done_age_ms=%lld "
           "reason=%s stale=%s",
           running ? "yes" : "no", attempts, failures, last_ret,
           start_ms > 0 ? (long long)(now - start_ms) : -1,
           done_ms > 0 ? (long long)(now - done_ms) : -1,
           reason[0] ? reason : "-", stale ? "yes" : "no");
  return 0;
}

#ifdef CONFIG_FRUITCLAW_ENABLE_TELEGRAM
static uint32_t fc_telegram_poll_recovery_ms(void)
{
  uint32_t timeout = CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS;

  if (timeout == 0)
    {
      timeout = CONFIG_FRUITCLAW_HTTP_GUARD_TIMEOUT_MS;
    }

  if (timeout == 0)
    {
      timeout = 90000;
    }

  return timeout;
}

static uint32_t fc_telegram_first_success_grace_ms(void)
{
  uint32_t timeout = CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS;
  uint32_t grace;

  if (timeout == 0)
    {
      timeout = fc_telegram_poll_recovery_ms();
    }

  if (timeout > UINT32_MAX / 4)
    {
      grace = UINT32_MAX;
    }
  else
    {
      grace = timeout * 4;
    }

  if (grace < 300000)
    {
      grace = 300000;
    }

  if (grace > 600000)
    {
      grace = 600000;
    }

  return grace;
}

#if defined(CONFIG_FRUITCLAW_ENABLE_TELEGRAM) && \
    CONFIG_FRUITCLAW_TELEGRAM_HTTP_GUARD_TIMEOUT_MS == 0
static void fc_operator_telegram_degraded(const char *reason,
                                          int64_t age_ms,
                                          uint32_t timeout_ms,
                                          int64_t now_ms)
{
  static int64_t last_log_ms;

  if (last_log_ms > 0 && now_ms - last_log_ms < 60000)
    {
      return;
    }

  last_log_ms = now_ms;
  FC_LOGW("telegram degraded: reason=%s age_ms=%lld timeout_ms=%u; "
          "network recovery suppressed for isolated Telegram transport",
          reason != NULL ? reason : "-",
          (long long)age_ms,
          (unsigned int)timeout_ms);
}
#endif

#if CONFIG_FRUITCLAW_TELEGRAM_HTTP_GUARD_TIMEOUT_MS > 0
static bool fc_operator_network_recovery_handled(const char *source,
                                                 int rec_ret)
{
  if (rec_ret == 0)
    {
      return true;
    }

  if (rec_ret == -ETIMEDOUT)
    {
      FC_LOGE("%s network recovery stale; forcing recovery", source);
      fc_guard_force_recovery(FC_GUARD_STAGE_NETREC);
      return true;
    }

  return false;
}
#endif
#endif

static void fc_operator_guard_check(void)
{
#if defined(CONFIG_FRUITCLAW_ENABLE_SESSION_GUARD) && \
    CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS > 0
  int64_t now = fc_mono_ms();
  char source[FC_SOURCE_LEN];
  int64_t age = fc_operator_progress_age_ms(source, sizeof(source));
  int64_t boot_net_age = fc_boot_network_active_age_ms(now);

  if (boot_net_age > CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS)
    {
      FC_LOGE("boot network guard expired: active_age_ms=%lld "
              "timeout_ms=%u; forcing recovery",
              (long long)boot_net_age,
              (unsigned int)CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS);
      fc_guard_force_recovery(FC_GUARD_STAGE_NETREC);
      return;
    }

#ifdef CONFIG_FRUITCLAW_ENABLE_TELEGRAM
  if (fc_telegram_poll_stale(now, fc_telegram_poll_recovery_ms()))
    {
      int64_t poll_age = fc_telegram_poll_age_ms(NULL);
#if CONFIG_FRUITCLAW_TELEGRAM_HTTP_GUARD_TIMEOUT_MS > 0
      int rec_ret;

      FC_LOGE("telegram poll guard expired: "
              "telegram_poll_age_ms=%lld timeout_ms=%u",
              (long long)poll_age,
              (unsigned int)fc_telegram_poll_recovery_ms());
      rec_ret = fc_network_recovery_request("telegram-poll-stale",
                                            now, poll_age);
      if (fc_operator_network_recovery_handled("telegram-poll-stale",
                                               rec_ret))
        {
          return;
        }

      FC_LOGW("telegram poll recovery unavailable: ret=%d; "
              "leaving board online", rec_ret);
      return;
#else
      fc_operator_telegram_degraded("poll-stale", poll_age,
                                    fc_telegram_poll_recovery_ms(), now);
#endif
    }

  {
    int64_t success_age = fc_telegram_last_success_age_ms();
    int64_t stale_age = success_age;

    if (stale_age < 0 && g_runtime_start_ms > 0 && now >= g_runtime_start_ms)
      {
        stale_age = now - g_runtime_start_ms;
      }

    if (success_age < 0)
      {
        uint32_t first_grace = fc_telegram_first_success_grace_ms();

        if (stale_age <= first_grace)
          {
            return;
          }

        {
#if CONFIG_FRUITCLAW_TELEGRAM_HTTP_GUARD_TIMEOUT_MS > 0
          int rec_ret;

          rec_ret = fc_network_recovery_request("telegram-first-success",
                                                now, stale_age);
          if (fc_operator_network_recovery_handled("telegram-first-success",
                                                   rec_ret))
            {
              return;
            }

          FC_LOGW("telegram has not succeeded yet: runtime_age_ms=%lld "
                  "timeout_ms=%u first_grace_ms=%u ret=%d; "
                  "leaving board online",
                  (long long)stale_age,
                  (unsigned int)CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS,
                  (unsigned int)first_grace, rec_ret);
          return;
#else
          fc_operator_telegram_degraded("first-success", stale_age,
                                        first_grace, now);
#endif
        }
      }

    if (stale_age > CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS)
      {
#if CONFIG_FRUITCLAW_TELEGRAM_HTTP_GUARD_TIMEOUT_MS > 0
        int rec_ret;

        rec_ret = fc_network_recovery_request("telegram-success-stale",
                                              now, stale_age);
        if (fc_operator_network_recovery_handled("telegram-success-stale",
                                                 rec_ret))
          {
            return;
          }

        FC_LOGW("telegram success stale after network recovery: "
                "telegram_success_age_ms=%lld timeout_ms=%u ret=%d; "
                "leaving board online",
                (long long)success_age,
                (unsigned int)CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS,
                rec_ret);
        return;
#else
        fc_operator_telegram_degraded("success-stale", stale_age,
                                      CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS,
                                      now);
#endif
      }
  }
#endif

  if (fc_operator_progress_stale(g_runtime_start_ms, now, age,
                                 CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS))
    {
      FC_LOGE("operator guard expired: progress_age_ms=%lld "
              "progress_source=%s runtime_age_ms=%lld timeout_ms=%u",
              (long long)age,
              source[0] ? source : "-",
              (long long)(g_runtime_start_ms > 0 ?
                now - g_runtime_start_ms : -1),
              (unsigned int)CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS);
      fc_guard_force_recovery(FC_GUARD_STAGE_OPERATOR);
    }
#endif
}

int fc_runtime_start(void)
{
  int ret;

  ret = fc_bootstrap();
  if (ret < 0)
    {
      return ret;
    }

  g_runtime_start_ms = fc_mono_ms();
  fc_operator_progress_mark("runtime-start");

#ifdef CONFIG_FRUITCLAW_ENABLE_SESSION_GUARD
  if (!g_boot_defer_session_guard)
    {
      ret = fc_guard_session_start();
      if (ret < 0)
        {
          printf("session guard start failed: %d\n", ret);
        }
    }
#endif

  fc_guard_session_heartbeat("runtime-init");

#ifdef CONFIG_FRUITCLAW_BOOT_START_WEBSERVER
  ret = fc_webserver_supervisor_start();
  if (ret < 0)
    {
      printf("webserver supervisor start failed: %d\n", ret);
    }
#endif

  ret = fc_router_worker_start();
  if (ret < 0)
    {
      printf("router start failed: %d\n", ret);
      return ret;
    }

  fc_guard_session_heartbeat("router");

  ret = fc_agent_worker_start();
  if (ret < 0)
    {
      printf("agent start failed: %d\n", ret);
      return ret;
    }

  fc_guard_session_heartbeat("agent");

#ifdef CONFIG_FRUITCLAW_ENABLE_TELEGRAM
  fc_operator_progress_mark("network-async");
  if (g_boot_network_started && g_boot_network_done && !g_boot_network_ok &&
      wifi_interface_ready() <= 0)
    {
      FC_LOGW("telegram start deferred: Wi-Fi is not ready");
    }
  else
    {
      ret = fc_telegram_worker_start();
      if (ret < 0)
        {
          printf("telegram start failed: %d\n", ret);
        }
    }
#endif

  fc_guard_session_heartbeat("telegram");

#ifdef CONFIG_FRUITCLAW_ENABLE_SCHEDULER
  ret = fc_scheduler_worker_start();
  if (ret < 0)
    {
      printf("scheduler start failed: %d\n", ret);
    }
#endif

  fc_guard_session_heartbeat("scheduler");

  ret = fc_services_supervisor_start();
  if (ret < 0)
    {
      printf("services supervisor start failed: %d\n", ret);
    }

  fc_guard_session_heartbeat("services-supervisor");

  printf("FruitClaw started. Press reset or stop the task to exit.\n");
  for (; ; )
    {
      fc_guard_session_heartbeat("runtime");
      fc_operator_progress_mark("runtime");
      fc_operator_guard_check();
      sleep(5);
    }

  return 0;
}

struct fc_once_job_s
{
  fc_event_t *ev;
  char *reply;
  size_t reply_len;
  int ret;
};

static void *fc_once_thread_main(void *arg)
{
  struct fc_once_job_s *job = arg;

  job->ret = fc_agent_handle_event(job->ev, job->reply, job->reply_len);
  return NULL;
}

static int cmd_once(int argc, char *argv[])
{
  pthread_attr_t attr;
  pthread_t thread;
  fc_event_t *ev;
  struct fc_once_job_s job;
  char *reply;
  int ret;

  if (argc < 3)
    {
      usage();
      return 1;
    }

  ret = fc_bootstrap();
  if (ret < 0)
    {
      return 1;
    }

  ev = calloc(1, sizeof(*ev));
  reply = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  if (ev == NULL || reply == NULL)
    {
      free(ev);
      free(reply);
      return 1;
    }

  fc_make_id(ev->id, sizeof(ev->id), "cli");
  fc_strlcpy(ev->source, "cli", sizeof(ev->source));
  fc_strlcpy(ev->type, "message.in", sizeof(ev->type));
  fc_strlcpy(ev->channel, "cli", sizeof(ev->channel));
  fc_strlcpy(ev->session_id, "cli", sizeof(ev->session_id));
#ifdef CONFIG_FRUITCLAW_OWNER_MODE
  ev->owner_mode = true;
#endif
  join_args(argc, argv, 2, ev->text, sizeof(ev->text));
  ev->ts_ms = fc_time_ms();

  memset(&job, 0, sizeof(job));
  job.ev = ev;
  job.reply = reply;
  job.reply_len = CONFIG_FRUITCLAW_MAX_JSON;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_FRUITCLAW_WORKER_STACKSIZE);
  ret = pthread_create(&thread, &attr, fc_once_thread_main, &job);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      free(ev);
      free(reply);
      return 1;
    }

  pthread_join(thread, NULL);
  ret = job.ret;
  if (reply[0] != '\0')
    {
      printf("%s\n", reply);
    }

  free(ev);
  free(reply);
  return ret < 0 ? 1 : 0;
}

static int cmd_config(void)
{
  char ca_path[FC_PATH_LEN];
  char bootstrap_stage[32];
  bool bootstrap_ready;
  bool bootstrap_busy;
  int bootstrap_ret;

  fc_bootstrap_probe(&bootstrap_ready, &bootstrap_busy);
  fc_bootstrap_get_stage(bootstrap_stage, sizeof(bootstrap_stage),
                         &bootstrap_ret);

  printf("program: %s\n", CONFIG_FRUITCLAW_PROGNAME);
  printf("bootstrap: %s\n",
         bootstrap_ready ? "ready" :
         bootstrap_busy ? "initializing" : "not ready");
  printf("bootstrap_stage: %s ret=%d\n", bootstrap_stage, bootstrap_ret);
  if (bootstrap_ready)
    {
      printf("data_dir: %s\n", fc_data_dir());
    }
  else
    {
      printf("data_dir: pending bootstrap\n");
    }

  printf("fallback_data_dir: %s\n", CONFIG_FRUITCLAW_DATA_DIR);
  printf("sd_data_dir: %s\n", CONFIG_FRUITCLAW_SD_DATA_DIR);
  printf("deepseek_endpoint: %s\n", CONFIG_FRUITCLAW_DEEPSEEK_ENDPOINT);
  printf("deepseek_model: %s\n", CONFIG_FRUITCLAW_DEEPSEEK_MODEL);
  printf("llm_guard_timeout_ms: %u\n",
         (unsigned int)CONFIG_FRUITCLAW_LLM_GUARD_TIMEOUT_MS);
  printf("agent_guard_timeout_ms: %u\n",
         (unsigned int)CONFIG_FRUITCLAW_AGENT_GUARD_TIMEOUT_MS);
  printf("operator_guard_timeout_ms: %u\n",
         (unsigned int)CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS);
  printf("http_guard_timeout_ms: %u\n",
         (unsigned int)CONFIG_FRUITCLAW_HTTP_GUARD_TIMEOUT_MS);
  printf("cli_guard_timeout_ms: %u\n",
         (unsigned int)CONFIG_FRUITCLAW_CLI_GUARD_TIMEOUT_MS);
  printf("max_uptime_guard_ms: %u\n",
         (unsigned int)CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS);
  printf("telegram_enabled: %s\n",
#ifdef CONFIG_FRUITCLAW_ENABLE_TELEGRAM
         "yes"
#else
         "no"
#endif
         );
  printf("scheduler_enabled: %s\n",
#ifdef CONFIG_FRUITCLAW_ENABLE_SCHEDULER
         "yes"
#else
         "no"
#endif
         );
  printf("berry_enabled: %s\n",
#ifdef CONFIG_FRUITCLAW_ENABLE_BERRY
         "yes"
#else
         "no"
#endif
         );
  printf("tls_unverified_allowed: %s\n",
#ifdef CONFIG_FRUITCLAW_TLS_ALLOW_UNVERIFIED
         "yes"
#else
         "no"
#endif
         );
  if (bootstrap_ready)
    {
      fc_tls_ca_cert_path(ca_path, sizeof(ca_path));
      printf("tls_ca_path: %s\n", ca_path);
    }
  else
    {
      printf("tls_ca_path: pending bootstrap\n");
    }
  printf("owner_mode: %s\n",
#ifdef CONFIG_FRUITCLAW_OWNER_MODE
         "yes"
#else
         "no"
#endif
         );
  printf("boot_autostart: %s\n",
#ifdef CONFIG_FRUITCLAW_BOOT_AUTOSTART
         "yes"
#else
         "no"
#endif
         );
  printf("boot_start_telnetd: %s\n",
#ifdef CONFIG_FRUITCLAW_BOOT_START_TELNETD
         "yes"
#else
         "no"
#endif
         );
  printf("boot_start_ftpd: %s\n",
#ifdef CONFIG_FRUITCLAW_BOOT_START_FTPD
         "yes"
#else
         "no"
#endif
         );
  printf("wifi_autostart: %s",
#ifdef CONFIG_FRUITCLAW_WIFI_AUTOSTART
         "yes"
#else
         "no"
#endif
         );
#ifdef CONFIG_FRUITCLAW_WIFI_AUTOSTART
  printf(" (%s, %s, fallback leaf %s)", CONFIG_FRUITCLAW_WIFI_IFNAME,
         CONFIG_FRUITCLAW_WIFI_CONFIG_PATH,
         CONFIG_FRUITCLAW_WIFI_CONFIG_LEAF);
#endif
  printf("\n");
  printf("exec_guard: %s",
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
         "yes"
#else
         "no"
#endif
         );
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
  printf(" (%s, %u ms)",
         CONFIG_FRUITCLAW_GUARD_DEVPATH,
         CONFIG_FRUITCLAW_GUARD_TIMEOUT_MS);
#endif
  printf("\n");
  printf("llm_guard: %s",
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
         "yes"
#else
         "no"
#endif
         );
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
  printf(" (%u ms)", (unsigned int)CONFIG_FRUITCLAW_LLM_GUARD_TIMEOUT_MS);
#endif
  printf("\n");
  printf("agent_guard: %s",
#if defined(CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD) && \
    CONFIG_FRUITCLAW_AGENT_GUARD_TIMEOUT_MS > 0
         "yes"
#else
         "no"
#endif
         );
#if defined(CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD) && \
    CONFIG_FRUITCLAW_AGENT_GUARD_TIMEOUT_MS > 0
  printf(" (%u ms)",
         (unsigned int)CONFIG_FRUITCLAW_AGENT_GUARD_TIMEOUT_MS);
#endif
  printf("\n");
  printf("operator_guard: %s",
#if defined(CONFIG_FRUITCLAW_ENABLE_SESSION_GUARD) && \
    CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS > 0
         "yes"
#else
         "no"
#endif
         );
#if defined(CONFIG_FRUITCLAW_ENABLE_SESSION_GUARD) && \
    CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS > 0
  printf(" (%u ms)",
         (unsigned int)CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS);
#endif
  printf("\n");
  printf("http_guard: %s",
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
         "yes"
#else
         "no"
#endif
         );
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
  printf(" (%u ms)", (unsigned int)CONFIG_FRUITCLAW_HTTP_GUARD_TIMEOUT_MS);
#endif
  printf("\n");
  printf("cli_guard: %s",
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
         "yes"
#else
         "no"
#endif
         );
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
  printf(" (%u ms)", (unsigned int)CONFIG_FRUITCLAW_CLI_GUARD_TIMEOUT_MS);
#endif
  printf("\n");
  printf("max_uptime_guard: %s",
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
         CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS > 0 ? "yes" : "no"
#else
         "no"
#endif
         );
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
  printf(" (%u ms)",
         (unsigned int)CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS);
#endif
  printf("\n");
  printf("mcp_server: %s\n",
#ifdef CONFIG_FRUITCLAW_MCP_SERVER
         "enabled"
#else
         "disabled"
#endif
         );
#ifdef CONFIG_FRUITCLAW_MCP_SERVER
  printf("mcp_yolo_mode: %s\n",
#  ifdef CONFIG_FRUITCLAW_MCP_YOLO_MODE
         "yes"
#  else
         "no"
#  endif
         );
#endif
  return 0;
}

static int read_prompt_line(const char *prompt, char *out, size_t out_len,
                            bool secret)
{
  struct termios oldt;
  struct termios newt;
  size_t pos = 0;
  bool changed = false;
  bool overflow = false;
  bool tty;

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  tty = isatty(STDIN_FILENO);

  if (prompt != NULL)
    {
      printf("%s", prompt);
      fflush(stdout);
    }

  if (tty && tcgetattr(STDIN_FILENO, &oldt) == 0)
    {
      newt = oldt;
      newt.c_lflag &= ~(ECHO | ICANON);
      newt.c_cc[VMIN] = 1;
      newt.c_cc[VTIME] = 0;
      if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) == 0)
        {
          changed = true;
        }
    }

  for (; ; )
    {
      char ch;
      ssize_t nread = read(STDIN_FILENO, &ch, 1);

      if (nread == 0)
        {
          out[pos] = '\0';
          if (changed)
            {
              tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            }

          return -EIO;
        }

      if (nread < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          out[pos] = '\0';
          if (changed)
            {
              tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            }

          return -errno;
        }

      if (ch == '\r' || ch == '\n')
        {
          if (tty)
            {
              write(STDOUT_FILENO, "\n", 1);
            }

          break;
        }

      if (ch == 0x03)
        {
          out[0] = '\0';
          if (tty)
            {
              write(STDOUT_FILENO, "\n", 1);
            }

          if (changed)
            {
              tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            }

          return -EINTR;
        }

      if (ch == '\b' || ch == 0x7f)
        {
          if (pos > 0)
            {
              pos--;
              if (tty && !secret)
                {
                  write(STDOUT_FILENO, "\b \b", 3);
                }
            }

          continue;
        }

      if (pos + 1 < out_len)
        {
          out[pos++] = ch;
          if (tty && !secret)
            {
              write(STDOUT_FILENO, &ch, 1);
            }
        }
      else
        {
          overflow = true;
        }
    }

  if (changed)
    {
      tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    }

  out[pos] = '\0';
  fc_trim(out);
  if (overflow)
    {
      return -ENOSPC;
    }

  return out[0] == '\0' ? -EINVAL : 0;
}

static int cmd_config_set_secret(int argc, char *argv[])
{
  char value[256];
  char path[FC_PATH_LEN];
  char text[260];
  const char *leaf = NULL;
  int ret;

  if (argc != 4 && argc != 5)
    {
      usage();
      return 1;
    }

  if (strcmp(argv[3], "telegram") == 0 ||
      strcmp(argv[3], "telegram_token") == 0)
    {
      leaf = "telegram_token";
    }
  else if (strcmp(argv[3], "deepseek") == 0 ||
           strcmp(argv[3], "deepseek_api_key") == 0)
    {
      leaf = "deepseek_api_key";
    }
  else
    {
      printf("unknown secret: %s\n", argv[3]);
      return 1;
    }

  ret = fc_init_data_dir();
  if (ret < 0)
    {
      printf("data directory init failed: %d\n", ret);
      return 1;
    }

  if (argc == 5)
    {
      ret = fc_strlcpy(value, argv[4], sizeof(value));
      fc_trim(value);
      if (ret < 0 || value[0] == '\0')
        {
          printf("secret not written: %d\n", ret < 0 ? ret : -EINVAL);
          return 1;
        }
    }
  else
    {
      ret = read_prompt_line("secret value: ", value, sizeof(value), true);
      if (ret < 0)
        {
          printf("secret not written: %d\n", ret);
          return 1;
        }
    }

  if (fc_secret_path(leaf, path, sizeof(path)) < 0 ||
      snprintf(text, sizeof(text), "%s\n", value) >= (int)sizeof(text) ||
      fc_write_text_file_atomic(path, text) < 0)
    {
      printf("secret write failed\n");
      return 1;
    }

  printf("stored %s\n", leaf);
  return 0;
}

static int cmd_config_set_wifi(int argc, char *argv[])
{
  char ssid[FC_WIFI_SSID_LEN];
  char psk[FC_WIFI_PSK_LEN];
  char path[FC_PATH_LEN];
  char text[FC_WIFI_SSID_LEN + FC_WIFI_PSK_LEN + 32];
  int ret;

  if (argc != 3 && argc != 5)
    {
      usage();
      return 1;
    }

  ret = fc_init_data_dir();
  if (ret < 0)
    {
      printf("data directory init failed: %d\n", ret);
      return 1;
    }

  if (argc == 5)
    {
      ret = fc_strlcpy(ssid, argv[3], sizeof(ssid));
      fc_trim(ssid);
      if (ret < 0 || ssid[0] == '\0')
        {
          printf("wifi config not written: %d\n",
                 ret < 0 ? ret : -EINVAL);
          return 1;
        }

      ret = fc_strlcpy(psk, argv[4], sizeof(psk));
      fc_trim(psk);
      if (ret < 0 || psk[0] == '\0')
        {
          printf("wifi config not written: %d\n",
                 ret < 0 ? ret : -EINVAL);
          return 1;
        }
    }
  else
    {
      ret = read_prompt_line("ssid: ", ssid, sizeof(ssid), false);
      if (ret < 0)
        {
          printf("wifi config not written: %d\n", ret);
          return 1;
        }

      ret = read_prompt_line("password: ", psk, sizeof(psk), true);
      if (ret < 0)
        {
          printf("wifi config not written: %d\n", ret);
          return 1;
        }
    }

  if (fc_data_path(CONFIG_FRUITCLAW_WIFI_CONFIG_LEAF, path,
                   sizeof(path)) < 0 ||
      snprintf(text, sizeof(text), "ssid=%s\npassword=%s\n", ssid, psk) >=
      (int)sizeof(text) ||
      fc_write_text_file_atomic(path, text) < 0)
    {
      printf("wifi config write failed\n");
      return 1;
    }

  printf("stored %s\n", CONFIG_FRUITCLAW_WIFI_CONFIG_LEAF);
  return 0;
}

static int cmd_config_cli(int argc, char *argv[])
{
  if (argc == 2)
    {
      return cmd_config();
    }

  if (argc >= 3 && strcmp(argv[2], "set-secret") == 0)
    {
      return cmd_config_set_secret(argc, argv);
    }

  if (argc >= 3 && strcmp(argv[2], "set-wifi") == 0)
    {
      return cmd_config_set_wifi(argc, argv);
    }

  usage();
  return 1;
}

static void fc_service_record(bool telnetd, int ret)
{
  pthread_mutex_lock(&g_services_lock);
  if (telnetd)
    {
      g_telnetd_attempts++;
      g_telnetd_last_ret = ret;
      g_telnetd_last_ms = fc_mono_ms();
      if (ret == 0)
        {
          g_telnetd_started = true;
          g_telnetd_listening = true;
        }
      else
        {
          g_telnetd_started = false;
          g_telnetd_listening = false;
        }
    }
  else
    {
      g_ftpd_attempts++;
      g_ftpd_last_ret = ret;
      g_ftpd_last_ms = fc_mono_ms();
      if (ret == 0)
        {
          g_ftpd_started = true;
          g_ftpd_listening = true;
        }
      else
        {
          g_ftpd_started = false;
          g_ftpd_listening = false;
        }
    }

  pthread_mutex_unlock(&g_services_lock);
}

static void fc_service_probe_record(int http_ret, int telnet_ret,
                                    int ftp_ret)
{
  pthread_mutex_lock(&g_services_lock);
  g_services_probe_ms = fc_mono_ms();
  g_http_probe_ret = http_ret;
  g_telnetd_probe_ret = telnet_ret;
  g_ftpd_probe_ret = ftp_ret;
  if (fc_service_probe_means_stopped(telnet_ret))
    {
      g_telnetd_started = false;
    }

  if (g_telnetd_started)
    {
      g_telnetd_listening = telnet_ret == 0;
    }
  else
    {
      g_telnetd_listening = false;
    }

  if (g_ftpd_started)
    {
      g_ftpd_listening = ftp_ret == 0;
    }
  else
    {
      g_ftpd_listening = false;
    }

  pthread_mutex_unlock(&g_services_lock);
}

static int fc_telnetd_pid_probe(void)
{
  char buf[24];
  char *end;
  long value;
  int fd;
  ssize_t n;

  fd = open(CONFIG_SYSTEM_TELNETD_PID_PATH, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    {
      return n < 0 ? -errno : -EINVAL;
    }

  buf[n] = '\0';
  value = strtol(buf, &end, 10);
  if (end == buf || value <= 0)
    {
      return -EINVAL;
    }

  if (kill((pid_t)value, 0) < 0)
    {
      int ret = -errno;

      if (ret == -ESRCH)
        {
          unlink(CONFIG_SYSTEM_TELNETD_PID_PATH);
        }

      return ret;
    }

  return 0;
}

static bool fc_service_probe_means_stopped(int ret)
{
  return ret == -ENOENT || ret == -ESRCH || ret == -ECONNREFUSED;
}

static bool fc_httpd_send_errno_means_recover(int err)
{
  return err == ETIMEDOUT || err == EPIPE || err == ECONNRESET ||
         err == ECONNABORTED || err == ENETDOWN || err == ENETUNREACH ||
         err == ENOTCONN;
}

static void fc_httpd_probe_recover_if_needed(int http_ret)
{
  if (http_ret < 0 && fc_httpd_send_errno_means_recover(-http_ret))
    {
      int probe_ret;

      FC_LOGW("HTTP transport probe failed ret=%d; recovering network",
              http_ret);
      fc_network_transport_pump_window("http-send-drain", 5);
      probe_ret = wifi_probe_connectivity(true);
      if (probe_ret > 0)
        {
          FC_LOGI("HTTP transport recovered after pump probe=%d",
                  probe_ret);
          return;
        }

      if (wifi_interface_ready() > 0)
        {
          FC_LOGW("HTTP transport probe failed but Wi-Fi interface remains "
                  "ready; suppressing ESP-Hosted recovery");
          return;
        }

      (void)fc_network_recovery_request("http-send-fail",
                                        fc_mono_ms(), -1);
    }
}

static void fc_service_note_restart(enum fc_service_id_e id)
{
  pthread_mutex_lock(&g_services_lock);
  if (id == FC_SERVICE_TELNETD)
    {
      g_telnetd_restarts++;
    }
  else
    {
      g_ftpd_restarts++;
    }

  pthread_mutex_unlock(&g_services_lock);
}

static int fc_service_probe_live(enum fc_service_id_e id)
{
#if FC_SERVICE_ACTIVE_TCP_PROBES
  int ret;

  if (!fc_service_started_snapshot(id))
    {
      return -ENOTCONN;
    }

  if (id == FC_SERVICE_TELNETD)
    {
      ret = fc_telnetd_pid_probe();
    }
  else
    {
      /* NuttX/esp-hosted can refuse board-to-self TCP connects to the FTP
       * listener while external hosts can connect normally.  Treat the FTP
       * daemon start result as authoritative and use host-side checks for
       * real reachability.
       */

      ret = 0;
    }

  pthread_mutex_lock(&g_services_lock);
  g_services_probe_ms = fc_mono_ms();
  if (id == FC_SERVICE_TELNETD)
    {
      g_telnetd_probe_ret = ret;
      if (fc_service_probe_means_stopped(ret))
        {
          g_telnetd_started = false;
        }

      if (g_telnetd_started)
        {
          g_telnetd_listening = ret == 0;
        }
      else
        {
          g_telnetd_listening = false;
        }
    }
  else
    {
      g_ftpd_probe_ret = ret;
      if (g_ftpd_started)
        {
          g_ftpd_listening = ret == 0;
        }
      else
        {
          g_ftpd_listening = false;
        }
    }

  pthread_mutex_unlock(&g_services_lock);
  return ret;
#else
  (void)id;
  return 0;
#endif
}

static void fc_services_probe_live_started(void)
{
#ifdef CONFIG_FRUITCLAW_BOOT_START_WEBSERVER
  int http_ret = fc_webserver_passive_probe();

  pthread_mutex_lock(&g_services_lock);
  g_services_probe_ms = fc_mono_ms();
  g_http_probe_ret = http_ret;
  pthread_mutex_unlock(&g_services_lock);
  fc_httpd_probe_recover_if_needed(http_ret);
#endif

#if FC_SERVICE_ACTIVE_TCP_PROBES
  if (fc_service_started_snapshot(FC_SERVICE_TELNETD))
    {
      (void)fc_service_probe_live(FC_SERVICE_TELNETD);
    }

  if (fc_service_started_snapshot(FC_SERVICE_FTPD))
    {
      (void)fc_service_probe_live(FC_SERVICE_FTPD);
    }
#endif
}

static unsigned int fc_service_probe_bad_count_update(int http_ret,
                                                      int telnet_ret,
                                                      int ftp_ret)
{
  unsigned int bad;

  pthread_mutex_lock(&g_services_lock);
  if (http_ret < 0 || telnet_ret < 0 || ftp_ret < 0)
    {
      g_service_bad_probes++;
    }
  else
    {
      g_service_bad_probes = 0;
    }

  bad = g_service_bad_probes;
  pthread_mutex_unlock(&g_services_lock);
  return bad;
}

#ifdef CONFIG_FRUITCLAW_WIFI_AUTOSTART
static void fc_wifi_connectivity_supervisor_tick(void)
{
  int64_t now = fc_mono_ms();
  fc_guard_long_t guard;
  bool guarded = false;
  int ret;
  int lock_ret;

  if (!wifi_config_present())
    {
      return;
    }

#ifdef CONFIG_FRUITCLAW_ENABLE_TELEGRAM
    {
      bool telegram_active = false;

      (void)fc_telegram_poll_age_ms(&telegram_active);
      if (telegram_active)
        {
          return;
        }
    }
#endif

  if (fc_boot_network_active_snapshot() ||
      fc_network_recovery_active_snapshot(NULL, NULL))
    {
      return;
    }

  lock_ret = pthread_mutex_trylock(&g_wifi_lock);
  if (lock_ret != 0)
    {
      return;
    }

  ret = fc_guard_long_start(FC_GUARD_STAGE_WIFI,
                            fc_wifi_guard_timeout_ms(), &guard);
  if (ret == 0)
    {
      guarded = true;
      fc_guard_long_set_stage(&guard, FC_GUARD_STAGE_WIFI_DHCP);
      ret = wifi_probe_connectivity(true);
    }
  else
    {
      FC_LOGW("Wi-Fi active probe guard unavailable ret=%d; using "
              "passive interface check", ret);
      ret = wifi_interface_ready();
    }

  if (guarded)
    {
      int guard_ret = fc_guard_long_stop(&guard);

      if (ret > 0 && guard_ret < 0)
        {
          ret = guard_ret;
        }
    }

  pthread_mutex_unlock(&g_wifi_lock);
  if (ret > 0)
    {
      if (g_wifi_connectivity_bad_probes > 0)
        {
          FC_LOGI("Wi-Fi connectivity recovered after %u bad probes",
                  g_wifi_connectivity_bad_probes);
        }

      g_wifi_connectivity_bad_probes = 0;
      return;
    }

  g_wifi_connectivity_bad_probes++;
  FC_LOGW("Wi-Fi interface probe failed count=%u ret=%d gateway=%d "
          "internet=%d",
          g_wifi_connectivity_bad_probes, ret, g_wifi_last_gateway_probe,
          g_wifi_last_internet_probe);

  if (wifi_interface_ready() > 0)
    {
      FC_LOGW("Wi-Fi has a valid interface/IP; suppressing automatic "
              "ESP-Hosted recovery for active probe failure");
      return;
    }

  if (g_wifi_connectivity_bad_probes >= FC_WIFI_CONNECTIVITY_FAILURE_LIMIT)
    {
      int rec_ret = fc_network_recovery_request("wifi-connectivity", now, -1);

      if (rec_ret == 0)
        {
          g_wifi_connectivity_bad_probes = 0;
        }
      else
        {
          FC_LOGW("Wi-Fi connectivity recovery request failed ret=%d",
                  rec_ret);
        }
    }
}
#endif

static void *fc_services_supervisor_main(void *arg)
{
  (void)arg;

  sleep(12);
  for (; ; )
    {
      int http_ret = 0;
      int telnet_ret = 0;
      int ftp_ret = 0;
      unsigned int bad;

      fc_guard_session_heartbeat("services-idle");
#if FC_SERVICE_ACTIVE_TCP_PROBES
#ifdef CONFIG_FRUITCLAW_BOOT_START_WEBSERVER
      http_ret = fc_webserver_passive_probe();
#else
      http_ret = 0;
#endif

      if (fc_service_started_snapshot(FC_SERVICE_TELNETD))
        {
          telnet_ret = fc_telnetd_pid_probe();
        }

      if (fc_service_started_snapshot(FC_SERVICE_FTPD))
        {
          ftp_ret = 0;
        }
#else
#ifdef CONFIG_FRUITCLAW_BOOT_START_WEBSERVER
      http_ret = fc_webserver_passive_probe();
#endif

      if (fc_service_started_snapshot(FC_SERVICE_TELNETD))
        {
          telnet_ret = 0;
        }

      if (fc_service_started_snapshot(FC_SERVICE_FTPD))
        {
          ftp_ret = 0;
        }
#endif

      fc_service_probe_record(http_ret, telnet_ret, ftp_ret);
      if (fc_service_autostart_enabled(FC_SERVICE_TELNETD) &&
          !fc_service_started_snapshot(FC_SERVICE_TELNETD))
        {
          int rec_ret;

          FC_LOGW("telnetd autostart enabled but not running; restarting");
          rec_ret = fc_service_start_id(FC_SERVICE_TELNETD);
          if (rec_ret == 0)
            {
              fc_service_note_restart(FC_SERVICE_TELNETD);
            }
          else
            {
              FC_LOGW("telnetd restart failed ret=%d", rec_ret);
            }
        }

      if (fc_service_autostart_enabled(FC_SERVICE_FTPD) &&
          !fc_service_started_snapshot(FC_SERVICE_FTPD))
        {
          int rec_ret;

          FC_LOGW("ftpd autostart enabled but not running; restarting");
          rec_ret = fc_service_start_id(FC_SERVICE_FTPD);
          if (rec_ret == 0)
            {
              fc_service_note_restart(FC_SERVICE_FTPD);
            }
          else
            {
              FC_LOGW("ftpd restart failed ret=%d", rec_ret);
            }
        }

      bad = fc_service_probe_bad_count_update(http_ret, telnet_ret,
                                              ftp_ret);
      fc_httpd_probe_recover_if_needed(http_ret);
      if (http_ret >= 0 && bad >= FC_SERVICE_PROBE_FAILURE_LIMIT)
        {
          FC_LOGW("service probe failures=%u http=%d telnet=%d ftp=%d",
                  bad, http_ret, telnet_ret, ftp_ret);
          if (g_wifi_last_ok_ms > 0)
            {
              (void)fc_network_recovery_request("service-probe",
                                                fc_mono_ms(), -1);
            }
        }

#ifdef CONFIG_FRUITCLAW_WIFI_AUTOSTART
      fc_wifi_connectivity_supervisor_tick();
#endif

      sleep(30);
    }

  return NULL;
}

static int fc_services_supervisor_start(void)
{
#if defined(CONFIG_FRUITCLAW_BOOT_START_TELNETD) || \
    defined(CONFIG_FRUITCLAW_BOOT_START_FTPD)
  pthread_attr_t attr;
  int ret;

  pthread_mutex_lock(&g_services_lock);
  if (g_services_supervisor_started)
    {
      pthread_mutex_unlock(&g_services_lock);
      return 0;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_FRUITCLAW_SMALL_STACKSIZE);
  ret = pthread_create(&g_services_thread, &attr,
                       fc_services_supervisor_main, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      pthread_mutex_unlock(&g_services_lock);
      return -ret;
    }

  pthread_detach(g_services_thread);
  g_services_supervisor_started = true;
  pthread_mutex_unlock(&g_services_lock);
#endif

  return 0;
}

static const char *fc_service_name(enum fc_service_id_e id)
{
  return id == FC_SERVICE_TELNETD ? "telnetd" : "ftpd";
}

static int fc_service_resolve(const char *name, enum fc_service_id_e *id)
{
  if (name == NULL || id == NULL)
    {
      return -EINVAL;
    }

  if (strcmp(name, "telnet") == 0 || strcmp(name, "telnetd") == 0)
    {
      *id = FC_SERVICE_TELNETD;
      return 0;
    }

  if (strcmp(name, "ftp") == 0 || strcmp(name, "ftpd") == 0)
    {
      *id = FC_SERVICE_FTPD;
      return 0;
    }

  return -EINVAL;
}

static bool fc_service_compiled(enum fc_service_id_e id)
{
  if (id == FC_SERVICE_TELNETD)
    {
#ifdef CONFIG_SYSTEM_TELNETD
      return true;
#else
      return false;
#endif
    }

#if defined(CONFIG_EXAMPLES_FTPD) && defined(CONFIG_NETUTILS_FTPD)
  return true;
#else
  return false;
#endif
}

static bool fc_service_boot_default(enum fc_service_id_e id)
{
  if (id == FC_SERVICE_TELNETD)
    {
#ifdef CONFIG_FRUITCLAW_BOOT_START_TELNETD
      return true;
#else
      return false;
#endif
    }

#ifdef CONFIG_FRUITCLAW_BOOT_START_FTPD
  return true;
#else
  return false;
#endif
}

static bool fc_service_stop_supported(enum fc_service_id_e id)
{
  return fc_service_compiled(id);
}

static int fc_service_disabled_path(enum fc_service_id_e id, char *path,
                                    size_t path_len)
{
  char leaf[48];

  if (path == NULL || path_len == 0)
    {
      return -EINVAL;
    }

  snprintf(leaf, sizeof(leaf), "services/%s.disabled",
           fc_service_name(id));
  return fc_data_path(leaf, path, path_len);
}

static bool fc_service_runtime_enabled(enum fc_service_id_e id)
{
  char path[FC_PATH_LEN];

  if (!fc_service_compiled(id))
    {
      return false;
    }

  if (fc_service_disabled_path(id, path, sizeof(path)) < 0)
    {
      return false;
    }

  return access(path, F_OK) != 0;
}

static bool fc_service_autostart_enabled(enum fc_service_id_e id)
{
  return fc_service_compiled(id) && fc_service_boot_default(id) &&
         fc_service_runtime_enabled(id);
}

static int fc_service_set_enabled(enum fc_service_id_e id, bool enabled)
{
  char dir[FC_PATH_LEN];
  char path[FC_PATH_LEN];
  int fd;
  int ret;
  ssize_t n;
  static const char disabled_text[] = "disabled\n";

  if (!fc_service_compiled(id))
    {
      return -ENOSYS;
    }

  if (fc_data_path("services", dir, sizeof(dir)) < 0 ||
      fc_service_disabled_path(id, path, sizeof(path)) < 0)
    {
      return -EINVAL;
    }

  if (enabled)
    {
      if (unlink(path) < 0 && errno != ENOENT)
        {
          return -errno;
        }

      return 0;
    }

  ret = fc_mkdir_p(dir);
  if (ret < 0)
    {
      return ret;
    }

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0664);
  if (fd < 0)
    {
      return -errno;
    }

  n = write(fd, disabled_text, sizeof(disabled_text) - 1);
  close(fd);
  return n == (ssize_t)(sizeof(disabled_text) - 1) ? 0 : -EIO;
}

static void fc_service_mark_stopped(enum fc_service_id_e id, int ret)
{
  pthread_mutex_lock(&g_services_lock);
  if (id == FC_SERVICE_TELNETD)
    {
      g_telnetd_last_ret = ret;
      g_telnetd_last_ms = fc_mono_ms();
      if (ret == 0)
        {
          g_telnetd_started = false;
          g_telnetd_listening = false;
        }
    }
  else
    {
      g_ftpd_last_ret = ret;
      g_ftpd_last_ms = fc_mono_ms();
      if (ret == 0)
        {
          g_ftpd_started = false;
          g_ftpd_listening = false;
        }
    }

  pthread_mutex_unlock(&g_services_lock);
}

static bool fc_service_started_snapshot(enum fc_service_id_e id)
{
  bool started;

  pthread_mutex_lock(&g_services_lock);
  started = id == FC_SERVICE_TELNETD ? g_telnetd_started :
                                      g_ftpd_started;
  pthread_mutex_unlock(&g_services_lock);
  return started;
}

static void fc_service_state_snapshot(enum fc_service_id_e id,
                                      bool *started, bool *listening,
                                      int *probe_ret)
{
  pthread_mutex_lock(&g_services_lock);
  if (id == FC_SERVICE_TELNETD)
    {
      if (started != NULL)
        {
          *started = g_telnetd_started;
        }

      if (listening != NULL)
        {
          *listening = g_telnetd_listening;
        }

      if (probe_ret != NULL)
        {
          *probe_ret = g_telnetd_probe_ret;
        }
    }
  else
    {
      if (started != NULL)
        {
          *started = g_ftpd_started;
        }

      if (listening != NULL)
        {
          *listening = g_ftpd_listening;
        }

      if (probe_ret != NULL)
        {
          *probe_ret = g_ftpd_probe_ret;
        }
    }

  pthread_mutex_unlock(&g_services_lock);
}

static int fc_service_start_id(enum fc_service_id_e id)
{
  bool started;
  int ret;

  if (!fc_service_compiled(id))
    {
      return -ENOSYS;
    }

  pthread_mutex_lock(&g_services_lock);
  started = id == FC_SERVICE_TELNETD ? g_telnetd_started :
                                     g_ftpd_started;
  pthread_mutex_unlock(&g_services_lock);

  if (started)
    {
      return 0;
    }

  if (id == FC_SERVICE_TELNETD)
    {
      ret = boot_run_command_guarded("telnetd", "telnetd -4 &", true,
                                     NULL, 0);
      fc_service_record(true, ret);
    }
  else
    {
      ret = boot_run_command_guarded("ftpd", "ftpd_start -4 &", true,
                                     NULL, 0);
      fc_service_record(false, ret);
    }

  return ret;
}

static int fc_service_stop_id(enum fc_service_id_e id)
{
  int ret;

  if (!fc_service_compiled(id))
    {
      return -ENOSYS;
    }

  if (!fc_service_started_snapshot(id))
    {
      fc_service_mark_stopped(id, 0);
      return 0;
    }

  if (id == FC_SERVICE_TELNETD)
    {
      fc_bootstrap_set_stage("telnetd-stop", 0);
      fc_guard_session_heartbeat("telnetd-stop");
      ret = system("telnetd -k");
      ret = ret == 0 ? 0 : -EIO;
      if (ret < 0 && fc_service_probe_means_stopped(fc_telnetd_pid_probe()))
        {
          ret = 0;
        }

      fc_service_mark_stopped(id, ret);
      return ret;
    }

  fc_bootstrap_set_stage("ftpd-stop", 0);
  fc_guard_session_heartbeat("ftpd-stop");
  ret = system("ftpd_stop");
  ret = ret == 0 ? 0 : -EIO;
  fc_service_mark_stopped(id, ret);
  return ret;
}

static int fc_service_restart_id(enum fc_service_id_e id)
{
  int ret;

  if (!fc_service_compiled(id))
    {
      return -ENOSYS;
    }

  if (!fc_service_stop_supported(id))
    {
      return -ENOTSUP;
    }

  ret = fc_service_stop_id(id);
  if (ret < 0)
    {
      return ret;
    }

  usleep(200000);
  ret = fc_service_start_id(id);
  if (ret == 0)
    {
      fc_service_note_restart(id);
    }

  return ret;
}

static int fc_boot_services_start(void)
{
  bool telnetd_started;
  bool ftpd_started;
  int final_ret = 0;

  pthread_mutex_lock(&g_services_lock);
  telnetd_started = g_telnetd_started;
  ftpd_started = g_ftpd_started;
  pthread_mutex_unlock(&g_services_lock);

#ifdef CONFIG_FRUITCLAW_BOOT_START_TELNETD
  if (!telnetd_started && fc_service_autostart_enabled(FC_SERVICE_TELNETD))
    {
      unsigned int attempt;
      int ret = -EIO;

      for (attempt = 0; attempt < FC_SERVICE_BOOT_RETRIES; attempt++)
        {
          int probe_ret;

          ret = fc_service_start_id(FC_SERVICE_TELNETD);
          if (ret == 0)
            {
              usleep(500 * USEC_PER_MSEC);
              probe_ret = fc_service_probe_live(FC_SERVICE_TELNETD);
              if (probe_ret == 0)
                {
                  break;
                }

              FC_LOGW("telnetd boot probe failed attempt=%u ret=%d",
                      attempt + 1, probe_ret);
              fc_service_mark_stopped(FC_SERVICE_TELNETD, probe_ret);
              ret = probe_ret;
            }

          if (attempt + 1 < FC_SERVICE_BOOT_RETRIES)
            {
              sleep(FC_SERVICE_BOOT_RETRY_DELAY_SEC);
            }
        }

      if (ret < 0 && final_ret == 0)
        {
          final_ret = ret;
        }
    }
#endif

#ifdef CONFIG_FRUITCLAW_BOOT_START_FTPD
  if (!ftpd_started && fc_service_autostart_enabled(FC_SERVICE_FTPD))
    {
      int ret = boot_run_command("ftpd", "ftpd_start -4 &", false);

      fc_service_record(false, ret);
      if (ret < 0 && final_ret == 0)
        {
          final_ret = ret;
        }
    }
#endif

  return final_ret;
}

int fc_services_status_format(char *out, size_t out_len)
{
  unsigned long telnetd_attempts;
  unsigned long ftpd_attempts;
  unsigned long telnetd_restarts;
  unsigned long ftpd_restarts;
  unsigned int bad_probes;
  bool telnetd_started;
  bool ftpd_started;
  bool telnetd_listening;
  bool ftpd_listening;
  int telnetd_ret;
  int ftpd_ret;
  int telnetd_probe_ret;
  int ftpd_probe_ret;
  int http_probe_ret;
  int64_t telnetd_ms;
  int64_t ftpd_ms;
  int64_t probe_ms;
  int64_t now = fc_mono_ms();

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  fc_services_probe_live_started();

  pthread_mutex_lock(&g_services_lock);
  telnetd_attempts = g_telnetd_attempts;
  ftpd_attempts = g_ftpd_attempts;
  telnetd_restarts = g_telnetd_restarts;
  ftpd_restarts = g_ftpd_restarts;
  bad_probes = g_service_bad_probes;
  telnetd_started = g_telnetd_started;
  ftpd_started = g_ftpd_started;
  telnetd_listening = g_telnetd_listening;
  ftpd_listening = g_ftpd_listening;
  telnetd_ret = g_telnetd_last_ret;
  ftpd_ret = g_ftpd_last_ret;
  telnetd_probe_ret = g_telnetd_probe_ret;
  ftpd_probe_ret = g_ftpd_probe_ret;
  http_probe_ret = g_http_probe_ret;
  telnetd_ms = g_telnetd_last_ms;
  ftpd_ms = g_ftpd_last_ms;
  probe_ms = g_services_probe_ms;
  pthread_mutex_unlock(&g_services_lock);

  snprintf(out, out_len,
           "services: telnetd=%s started=%s attempts=%lu last_ret=%d "
           "last_age_ms=%lld ftpd=%s started=%s attempts=%lu last_ret=%d "
           "last_age_ms=%lld telnet_listening=%s telnet_probe=%d "
           "telnet_restarts=%lu ftpd_listening=%s ftpd_probe=%d "
           "ftpd_restarts=%lu http_probe=%d bad_probes=%u "
           "probe_age_ms=%lld",
           fc_service_autostart_enabled(FC_SERVICE_TELNETD) ? "enabled" :
                                                              "disabled",
           telnetd_started ? "yes" : "no", telnetd_attempts,
           telnetd_ret,
           telnetd_ms > 0 ? (long long)(now - telnetd_ms) : -1,
           fc_service_autostart_enabled(FC_SERVICE_FTPD) ? "enabled" :
                                                           "disabled",
           ftpd_started ? "yes" : "no", ftpd_attempts, ftpd_ret,
           ftpd_ms > 0 ? (long long)(now - ftpd_ms) : -1,
           telnetd_listening ? "yes" : "no", telnetd_probe_ret,
           telnetd_restarts,
           ftpd_listening ? "yes" : "no", ftpd_probe_ret, ftpd_restarts,
           http_probe_ret, bad_probes,
           probe_ms > 0 ? (long long)(now - probe_ms) : -1);
  return 0;
}

int fc_services_status_json(char *out, size_t out_len)
{
  char httpd_json[384];
  unsigned long telnetd_attempts;
  unsigned long ftpd_attempts;
  unsigned long telnetd_restarts;
  unsigned long ftpd_restarts;
  unsigned int bad_probes;
  bool telnetd_started;
  bool ftpd_started;
  bool telnetd_listening;
  bool ftpd_listening;
  int telnetd_ret;
  int ftpd_ret;
  int telnetd_probe_ret;
  int ftpd_probe_ret;
  int http_probe_ret;
  bool web_started;
  bool web_listening;
  unsigned long web_listens;
  unsigned long web_exits;
  int web_last_ret;
  int web_last_errno;
  int web_probe_ret;
  int64_t telnetd_ms;
  int64_t ftpd_ms;
  int64_t web_start_ms;
  int64_t web_exit_ms;
  int64_t probe_ms;
  int64_t now = fc_mono_ms();

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  fc_services_probe_live_started();

  (void)fc_web_httpd_status_json(httpd_json, sizeof(httpd_json));

  pthread_mutex_lock(&g_services_lock);
  telnetd_attempts = g_telnetd_attempts;
  ftpd_attempts = g_ftpd_attempts;
  telnetd_restarts = g_telnetd_restarts;
  ftpd_restarts = g_ftpd_restarts;
  bad_probes = g_service_bad_probes;
  telnetd_started = g_telnetd_started;
  ftpd_started = g_ftpd_started;
  telnetd_listening = g_telnetd_listening;
  ftpd_listening = g_ftpd_listening;
  telnetd_ret = g_telnetd_last_ret;
  ftpd_ret = g_ftpd_last_ret;
  telnetd_probe_ret = g_telnetd_probe_ret;
  ftpd_probe_ret = g_ftpd_probe_ret;
  http_probe_ret = g_http_probe_ret;
  telnetd_ms = g_telnetd_last_ms;
  ftpd_ms = g_ftpd_last_ms;
  probe_ms = g_services_probe_ms;
  pthread_mutex_unlock(&g_services_lock);

  pthread_mutex_lock(&g_webserver_lock);
  fc_webserver_reconcile_locked();
  web_started = g_webserver_started;
  web_listening = g_webserver_listening;
  web_listens = g_webserver_listens;
  web_exits = g_webserver_exits;
  web_last_ret = g_webserver_last_ret;
  web_last_errno = g_webserver_last_errno;
  web_start_ms = g_webserver_last_start_ms;
  web_exit_ms = g_webserver_last_exit_ms;
  pthread_mutex_unlock(&g_webserver_lock);
  web_probe_ret = web_started && web_listening ? 0 : -ENOTCONN;

  snprintf(out, out_len,
           "{\"ok\":true,"
           "\"telnetd\":{\"compiled\":%s,\"boot_default\":%s,"
           "\"enabled\":%s,\"autostart\":%s,\"started\":%s,"
           "\"listening\":%s,\"start_supported\":%s,"
           "\"stop_supported\":%s,\"attempts\":%lu,"
           "\"restarts\":%lu,\"last_ret\":%d,\"probe_ret\":%d,"
           "\"last_age_ms\":%lld},"
           "\"ftpd\":{\"compiled\":%s,\"boot_default\":%s,"
           "\"enabled\":%s,\"autostart\":%s,\"started\":%s,"
           "\"listening\":%s,\"start_supported\":%s,"
           "\"stop_supported\":%s,\"attempts\":%lu,"
           "\"restarts\":%lu,\"last_ret\":%d,\"probe_ret\":%d,"
           "\"last_age_ms\":%lld},"
           "\"webserver\":{\"compiled\":%s,\"boot_default\":%s,"
           "\"enabled\":%s,\"autostart\":%s,\"started\":%s,"
           "\"listening\":%s,\"start_supported\":%s,"
           "\"stop_supported\":false,\"listens\":%lu,\"exits\":%lu,"
           "\"last_ret\":%d,\"last_errno\":%d,\"probe_ret\":%d,"
           "\"last_start_age_ms\":%lld,\"last_exit_age_ms\":%lld,%s},"
           "\"http_probe_ret\":%d,\"bad_probes\":%u,"
           "\"probe_age_ms\":%lld}",
           fc_service_compiled(FC_SERVICE_TELNETD) ? "true" : "false",
           fc_service_boot_default(FC_SERVICE_TELNETD) ? "true" : "false",
           fc_service_runtime_enabled(FC_SERVICE_TELNETD) ? "true" :
                                                            "false",
           fc_service_autostart_enabled(FC_SERVICE_TELNETD) ? "true" :
                                                              "false",
           telnetd_started ? "true" : "false",
           telnetd_listening ? "true" : "false",
           fc_service_compiled(FC_SERVICE_TELNETD) ? "true" : "false",
           fc_service_stop_supported(FC_SERVICE_TELNETD) ? "true" : "false",
           telnetd_attempts, telnetd_restarts, telnetd_ret,
           telnetd_probe_ret,
           telnetd_ms > 0 ? (long long)(now - telnetd_ms) : -1,
           fc_service_compiled(FC_SERVICE_FTPD) ? "true" : "false",
           fc_service_boot_default(FC_SERVICE_FTPD) ? "true" : "false",
           fc_service_runtime_enabled(FC_SERVICE_FTPD) ? "true" : "false",
           fc_service_autostart_enabled(FC_SERVICE_FTPD) ? "true" :
                                                           "false",
           ftpd_started ? "true" : "false",
           ftpd_listening ? "true" : "false",
           fc_service_compiled(FC_SERVICE_FTPD) ? "true" : "false",
           fc_service_stop_supported(FC_SERVICE_FTPD) ? "true" : "false",
           ftpd_attempts, ftpd_restarts, ftpd_ret, ftpd_probe_ret,
           ftpd_ms > 0 ? (long long)(now - ftpd_ms) : -1,
           fc_webserver_compiled() ? "true" : "false",
           fc_webserver_compiled() ? "true" : "false",
           fc_webserver_compiled() ? "true" : "false",
           fc_webserver_compiled() ? "true" : "false",
           web_started ? "true" : "false",
           web_listening ? "true" : "false",
           fc_webserver_compiled() ? "true" : "false",
           web_listens, web_exits, web_last_ret, web_last_errno,
           web_probe_ret,
           web_start_ms > 0 ? (long long)(now - web_start_ms) : -1,
           web_exit_ms > 0 ? (long long)(now - web_exit_ms) : -1,
           httpd_json,
           http_probe_ret, bad_probes,
           probe_ms > 0 ? (long long)(now - probe_ms) : -1);

  return 0;
}

static int fc_service_status_id_json(enum fc_service_id_e id,
                                     char *out, size_t out_len)
{
  unsigned long attempts;
  unsigned long restarts;
  bool started;
  bool listening;
  int last_ret;
  int probe_ret;
  int64_t last_ms;
  int64_t now;

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  now = fc_mono_ms();
  (void)fc_service_probe_live(id);

  pthread_mutex_lock(&g_services_lock);
  if (id == FC_SERVICE_TELNETD)
    {
      attempts = g_telnetd_attempts;
      restarts = g_telnetd_restarts;
      started = g_telnetd_started;
      listening = g_telnetd_listening;
      last_ret = g_telnetd_last_ret;
      probe_ret = g_telnetd_probe_ret;
      last_ms = g_telnetd_last_ms;
    }
  else
    {
      attempts = g_ftpd_attempts;
      restarts = g_ftpd_restarts;
      started = g_ftpd_started;
      listening = g_ftpd_listening;
      last_ret = g_ftpd_last_ret;
      probe_ret = g_ftpd_probe_ret;
      last_ms = g_ftpd_last_ms;
    }

  pthread_mutex_unlock(&g_services_lock);

  snprintf(out, out_len,
           "{\"ok\":true,\"service\":\"%s\",\"compiled\":%s,"
           "\"boot_default\":%s,\"enabled\":%s,\"autostart\":%s,"
           "\"started\":%s,\"listening\":%s,\"start_supported\":%s,"
           "\"stop_supported\":%s,\"attempts\":%lu,\"restarts\":%lu,"
           "\"last_ret\":%d,\"probe_ret\":%d,\"last_age_ms\":%lld}",
           fc_service_name(id),
           fc_service_compiled(id) ? "true" : "false",
           fc_service_boot_default(id) ? "true" : "false",
           fc_service_runtime_enabled(id) ? "true" : "false",
           fc_service_autostart_enabled(id) ? "true" : "false",
           started ? "true" : "false",
           listening ? "true" : "false",
           fc_service_compiled(id) ? "true" : "false",
           fc_service_stop_supported(id) ? "true" : "false",
           attempts, restarts, last_ret, probe_ret,
           last_ms > 0 ? (long long)(now - last_ms) : -1);

  return 0;
}

int fc_service_control(const char *service_name, const char *action,
                       char *out, size_t out_len)
{
  enum fc_service_id_e id;
  const char *service;
  const char *verb;
  const char *note = NULL;
  bool started;
  bool listening;
  int probe_ret;
  int ret;

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  service = service_name != NULL ? service_name : "all";
  verb = action != NULL ? action : "status";

  if (fc_webserver_service_name(service))
    {
      return fc_webserver_service_control(verb, out, out_len);
    }

  if (strcmp(verb, "status") == 0)
    {
      if (strcmp(service, "all") == 0 || service[0] == '\0')
        {
          return fc_services_status_json(out, out_len);
        }

      ret = fc_service_resolve(service, &id);
      if (ret < 0)
        {
          snprintf(out, out_len,
                   "{\"ok\":false,\"service\":\"%s\",\"action\":\"%s\","
                   "\"error\":\"unknown service\",\"code\":%d}",
                   service, verb, ret);
          return ret;
        }

      return fc_service_status_id_json(id, out, out_len);
    }

  if (strcmp(service, "all") == 0 || service[0] == '\0')
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"service\":\"%s\",\"action\":\"%s\","
               "\"error\":\"service required\",\"code\":%d}",
               service, verb, -EINVAL);
      return -EINVAL;
    }

  ret = fc_service_resolve(service, &id);
  if (ret < 0)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"service\":\"%s\",\"action\":\"%s\","
               "\"error\":\"unknown service\",\"code\":%d}",
               service, verb, ret);
      return ret;
    }

  if (!fc_service_compiled(id))
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"service\":\"%s\",\"action\":\"%s\","
               "\"error\":\"service not compiled\",\"code\":%d}",
               fc_service_name(id), verb, -ENOSYS);
      return -ENOSYS;
    }

  if (strcmp(verb, "start") == 0)
    {
      ret = fc_service_start_id(id);
    }
  else if (strcmp(verb, "stop") == 0)
    {
      ret = fc_service_stop_id(id);
      if (ret < 0 && id == FC_SERVICE_TELNETD)
        {
          note = "telnetd stop uses telnetd -k and requires a live "
                 "PID file.";
        }
    }
  else if (strcmp(verb, "restart") == 0)
    {
      ret = fc_service_restart_id(id);
      if (ret < 0 && id == FC_SERVICE_TELNETD)
        {
          note = "telnetd restart uses telnetd -k followed by telnetd -4.";
        }
    }
  else if (strcmp(verb, "enable") == 0)
    {
      ret = fc_service_set_enabled(id, true);
      note = "autostart enabled for future FruitClaw boots";
    }
  else if (strcmp(verb, "disable") == 0)
    {
      ret = fc_service_set_enabled(id, false);
      note = "autostart disabled for future FruitClaw boots";
    }
  else
    {
      ret = -EINVAL;
    }

  if (ret == -EINVAL)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"service\":\"%s\",\"action\":\"%s\","
               "\"error\":\"unknown action\",\"code\":%d}",
               fc_service_name(id), verb, ret);
      return ret;
    }

  if (ret < 0)
    {
      (void)fc_service_probe_live(id);
      fc_service_state_snapshot(id, &started, &listening, &probe_ret);
      snprintf(out, out_len,
               "{\"ok\":false,\"service\":\"%s\",\"action\":\"%s\","
               "\"enabled\":%s,\"autostart\":%s,\"started\":%s,"
               "\"listening\":%s,\"probe_ret\":%d,\"error\":\"service "
               "control failed\",\"code\":%d%s%s%s}",
               fc_service_name(id), verb,
               fc_service_runtime_enabled(id) ? "true" : "false",
               fc_service_autostart_enabled(id) ? "true" : "false",
               started ? "true" : "false",
               listening ? "true" : "false",
               probe_ret,
               ret,
               note != NULL ? ",\"note\":\"" : "",
               note != NULL ? note : "",
               note != NULL ? "\"" : "");
      return ret;
    }

  (void)fc_service_probe_live(id);
  fc_service_state_snapshot(id, &started, &listening, &probe_ret);
  snprintf(out, out_len,
           "{\"ok\":true,\"service\":\"%s\",\"action\":\"%s\","
           "\"enabled\":%s,\"autostart\":%s,\"started\":%s,"
           "\"listening\":%s,\"probe_ret\":%d,\"stop_supported\":%s%s%s%s}",
           fc_service_name(id), verb,
           fc_service_runtime_enabled(id) ? "true" : "false",
           fc_service_autostart_enabled(id) ? "true" : "false",
           started ? "true" : "false",
           listening ? "true" : "false",
           probe_ret,
           fc_service_stop_supported(id) ? "true" : "false",
           note != NULL ? ",\"note\":\"" : "",
           note != NULL ? note : "",
           note != NULL ? "\"" : "");

  return 0;
}

static int file_present(const char *leaf)
{
  char path[FC_PATH_LEN];

  if (fc_data_path(leaf, path, sizeof(path)) < 0)
    {
      return 0;
    }

  return access(path, F_OK) == 0;
}

static int secret_present(const char *leaf)
{
  char path[FC_PATH_LEN];

  if (fc_secret_path(leaf, path, sizeof(path)) < 0)
    {
      return 0;
    }

  return access(path, F_OK) == 0;
}

#ifdef CONFIG_FRUITCLAW_WIFI_AUTOSTART
static void print_wifi_runtime_status(void)
{
  char ifstatus[96];
  char ipstatus[128];
  int gateway_probe;
  int internet_probe;
  int64_t probe_ms;
  int64_t ok_ms;
  int64_t recovery_age;
  int64_t telegram_age = -1;
  bool boot_started;
  bool boot_done;
  bool boot_ok;
  bool recovery_running;
  bool recovery_stale;
  bool telegram_active = false;
  int64_t now = fc_mono_ms();

#ifdef CONFIG_FRUITCLAW_ENABLE_TELEGRAM
  telegram_age = fc_telegram_poll_age_ms(&telegram_active);
#endif

  gateway_probe = g_wifi_last_gateway_probe;
  internet_probe = g_wifi_last_internet_probe;
  probe_ms = g_wifi_last_probe_ms;
  ok_ms = g_wifi_last_ok_ms;
  fc_boot_network_status_snapshot(&boot_started, &boot_done, &boot_ok);
  recovery_running =
      fc_network_recovery_active_snapshot(&recovery_age, &recovery_stale);
  wifi_describe_interface(ifstatus, sizeof(ifstatus));
  wifi_describe_ipv4(ipstatus, sizeof(ipstatus));

  printf("  wifi_status: if=%s live_netlib=disabled "
         "detail=\"use wifi-probe for active checks\"\n",
         CONFIG_FRUITCLAW_WIFI_IFNAME);
  printf("  wifi_boot: started=%s done=%s ok=%s\n",
         boot_started ? "yes" : "no",
         boot_done ? "yes" : "no",
         boot_ok ? "yes" : "no");
  printf("  wifi_recovery: running=%s age_ms=%lld stale=%s\n",
         recovery_running ? "yes" : "no",
         recovery_running ? (long long)recovery_age : -1,
         recovery_running && recovery_stale ? "yes" : "no");
  printf("  wifi_telegram_poll: active=%s age_ms=%lld\n",
         telegram_active ? "yes" : "no", (long long)telegram_age);
  printf("  wifi_probe_cache: last_ok_age_ms=%lld gateway=%d internet=%d "
         "probe_age_ms=%lld\n",
         ok_ms > 0 ? (long long)(now - ok_ms) : -1,
         gateway_probe, internet_probe,
         probe_ms > 0 ? (long long)(now - probe_ms) : -1);
  printf("  wifi_interface: %s\n", ifstatus);
  printf("  wifi_ipv4: %s\n", ipstatus);
}
#endif

static void print_status_net_services(void)
{
  bool telnetd_started = false;
  bool telnetd_listening = false;
  bool ftpd_started = false;
  bool ftpd_listening = false;
  int telnetd_probe = 0;
  int ftpd_probe = 0;

  fc_service_state_snapshot(FC_SERVICE_TELNETD, &telnetd_started,
                            &telnetd_listening, &telnetd_probe);
  fc_service_state_snapshot(FC_SERVICE_FTPD, &ftpd_started,
                            &ftpd_listening, &ftpd_probe);

  printf("  telnetd: autostart=%s started=%s listening=%s probe=%d\n",
         fc_service_autostart_enabled(FC_SERVICE_TELNETD) ? "yes" : "no",
         telnetd_started ? "yes" : "no",
         telnetd_listening ? "yes" : "no", telnetd_probe);
  printf("  ftpd: autostart=%s started=%s listening=%s probe=%d\n",
         fc_service_autostart_enabled(FC_SERVICE_FTPD) ? "yes" : "no",
         ftpd_started ? "yes" : "no",
         ftpd_listening ? "yes" : "no", ftpd_probe);
  printf("  webserver: compiled=%s running=%s\n",
#ifdef CONFIG_FRUITCLAW_BOOT_START_WEBSERVER
         "yes", fc_webserver_running_snapshot() ? "yes" : "no"
#else
         "no", "no"
#endif
         );
}

static int cmd_status(bool include_net)
{
  char progress[128];
  char bootstrap_stage[32];
  bool bootstrap_ready;
  bool bootstrap_busy;
  fc_guard_long_t guard;
  int bootstrap_ret;
  int ret;

  ret = fc_cli_guard_begin_optional_status(include_net ? "status-net" :
                                           "status", &guard);
  if (ret < 0)
    {
      return 1;
    }

  fc_bootstrap_probe(&bootstrap_ready, &bootstrap_busy);
  fc_bootstrap_get_stage(bootstrap_stage, sizeof(bootstrap_stage),
                         &bootstrap_ret);

  printf("FruitClaw status\n");
  printf("  bootstrap: %s\n",
         bootstrap_ready ? "ready" :
         bootstrap_busy ? "initializing" : "not ready");
  printf("  bootstrap_stage: %s ret=%d\n", bootstrap_stage, bootstrap_ret);
  if (bootstrap_ready)
    {
      printf("  data_root: %s\n", fc_data_dir());
    }
  else
    {
      printf("  data_root: pending bootstrap "
             "(sd=%s fallback=%s)\n",
             CONFIG_FRUITCLAW_SD_DATA_DIR, CONFIG_FRUITCLAW_DATA_DIR);
    }

  printf("  boot_autostart: %s\n",
#ifdef CONFIG_FRUITCLAW_BOOT_AUTOSTART
         "enabled"
#else
         "disabled"
#endif
         );
    {
      bool boot_started;
      bool boot_done;
      bool boot_ok;

      fc_boot_network_status_snapshot(&boot_started, &boot_done, &boot_ok);
      printf("  boot_network: started=%s done=%s ok=%s\n",
             boot_started ? "yes" : "no",
             boot_done ? "yes" : "no",
             boot_ok ? "yes" : "no");
    }
  if (!bootstrap_ready)
    {
      printf("  wifi_autostart: %s (startup pending)\n",
#ifdef CONFIG_FRUITCLAW_WIFI_AUTOSTART
             "enabled"
#else
             "disabled"
#endif
             );
        {
          char status_guard[768];

          if (fc_guard_status(status_guard, sizeof(status_guard)) == 0)
            {
              print_guard_status(status_guard);
            }
        }

      return fc_cli_guard_end(&guard, 0);
    }

  printf("  wifi_autostart: %s",
#ifdef CONFIG_FRUITCLAW_WIFI_AUTOSTART
         "enabled"
#else
         "disabled"
#endif
         );
#ifdef CONFIG_FRUITCLAW_WIFI_AUTOSTART
  printf(" (%s, %s, %s)", CONFIG_FRUITCLAW_WIFI_IFNAME,
         CONFIG_FRUITCLAW_WIFI_CONFIG_PATH,
         wifi_config_present() ?
         "config present" : "config missing");
#endif
  printf("\n");
#ifdef CONFIG_FRUITCLAW_WIFI_AUTOSTART
  if (include_net)
    {
      print_wifi_runtime_status();
    }
  else
    {
      printf("  wifi_status: skipped in plain status; use status-net for "
             "live netlib status\n");
    }
#endif
  if (include_net)
    {
      char recovery[256];

      print_status_net_services();
      if (fc_network_recovery_status_format(recovery, sizeof(recovery)) == 0)
        {
          printf("  %s\n", recovery);
        }

      fc_guard_session_heartbeat("status-net");
      printf("  mcp: %s\n",
#ifdef CONFIG_FRUITCLAW_MCP_SERVER
             "enabled"
#else
             "disabled"
#endif
             );
      printf("  detail_commands: wifi-probe, esphostedctl, httpd-status, "
             "service status, mcp status, status\n");
      return fc_cli_guard_end(&guard, 0);
    }

  printf("  telegram_token: %s\n",
         secret_present("telegram_token") ? "present" : "missing");
  printf("  deepseek_key: %s\n",
         secret_present("deepseek_api_key") ? "present" : "missing");
  printf("  allowed_chats: %s\n",
         file_present("telegram_allowed_chats.txt") ? "present" :
         "missing");
  printf("  schedules_file: %s\n",
         file_present("schedules.json") ? "present" : "missing");
  fc_operator_progress_mark("status");
  if (fc_operator_progress_status_format(progress, sizeof(progress)) == 0)
    {
      printf("  %s\n", progress);
    }

  printf("  queues: main=%u agent=%u\n",
         fc_queue_count(fc_main_queue()), fc_queue_count(fc_agent_queue()));
  fc_router_status(stdout);
  fc_agent_status(stdout);
    {
      char recovery[256];
      char webserver[1536];
      char services[256];

      if (fc_network_recovery_status_format(recovery, sizeof(recovery)) == 0)
        {
          printf("  %s\n", recovery);
        }

      if (fc_webserver_status_format(webserver, sizeof(webserver)) == 0)
        {
          printf("  %s\n", webserver);
        }

      if (fc_services_status_format(services, sizeof(services)) == 0)
        {
          printf("  %s\n", services);
        }
    }
  fc_guard_session_heartbeat("status");

  printf("  telegram: %s\n",
#ifdef CONFIG_FRUITCLAW_ENABLE_TELEGRAM
         "enabled"
#else
         "disabled"
#endif
         );
  fc_telegram_status(stdout);
  printf("  deepseek: %s\n",
#ifdef CONFIG_FRUITCLAW_ENABLE_DEEPSEEK
         "enabled"
#else
         "disabled"
#endif
         );
  fc_deepseek_status(stdout);
  printf("  scheduler: %s\n",
#ifdef CONFIG_FRUITCLAW_ENABLE_SCHEDULER
         "enabled"
#else
         "disabled"
#endif
         );
  fc_scheduler_status(stdout);
  printf("  berry: %s\n",
#ifdef CONFIG_FRUITCLAW_ENABLE_BERRY
         "enabled"
#else
         "disabled"
#endif
         );
  fc_berry_status(stdout);
  printf("  terminal_tool: %s\n",
#ifdef CONFIG_FRUITCLAW_ENABLE_TERMINAL_TOOL
         "enabled"
#else
         "disabled"
#endif
         );
  printf("  neopixels: %s (%s)\n",
#ifdef CONFIG_FRUITCLAW_ENABLE_NEOPIXELS_TOOL
         "enabled",
#else
         "disabled",
#endif
         CONFIG_SYSTEM_NEOPIXELS_DEVPATH);
  printf("  device_tools: %s\n",
#ifdef CONFIG_FRUITCLAW_ENABLE_DEVICE_TOOL
         "enabled"
#else
         "disabled"
#endif
         );
  printf("  exec_guard: %s",
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
         "enabled"
#else
         "disabled"
#endif
         );
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
  printf(" (%s, %u ms)",
         CONFIG_FRUITCLAW_GUARD_DEVPATH,
         CONFIG_FRUITCLAW_GUARD_TIMEOUT_MS);
#endif
  printf("\n");
  printf("  llm_guard: %s",
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
         "enabled"
#else
         "disabled"
#endif
         );
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
  printf(" (%u ms)", (unsigned int)CONFIG_FRUITCLAW_LLM_GUARD_TIMEOUT_MS);
#endif
  printf("\n");
  printf("  agent_guard: %s",
#if defined(CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD) && \
    CONFIG_FRUITCLAW_AGENT_GUARD_TIMEOUT_MS > 0
         "enabled"
#else
         "disabled"
#endif
         );
#if defined(CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD) && \
    CONFIG_FRUITCLAW_AGENT_GUARD_TIMEOUT_MS > 0
  printf(" (%u ms)",
         (unsigned int)CONFIG_FRUITCLAW_AGENT_GUARD_TIMEOUT_MS);
#endif
  printf("\n");
  printf("  operator_guard: %s",
#if defined(CONFIG_FRUITCLAW_ENABLE_SESSION_GUARD) && \
    CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS > 0
         "enabled"
#else
         "disabled"
#endif
         );
#if defined(CONFIG_FRUITCLAW_ENABLE_SESSION_GUARD) && \
    CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS > 0
  printf(" (%u ms)",
         (unsigned int)CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS);
#endif
  printf("\n");
  printf("  http_guard: %s",
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
         "enabled"
#else
         "disabled"
#endif
         );
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
  printf(" (%u ms)", (unsigned int)CONFIG_FRUITCLAW_HTTP_GUARD_TIMEOUT_MS);
#endif
  printf("\n");
  printf("  cli_guard: %s",
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
         "enabled"
#else
         "disabled"
#endif
         );
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
  printf(" (%u ms)", (unsigned int)CONFIG_FRUITCLAW_CLI_GUARD_TIMEOUT_MS);
#endif
  printf("\n");
  printf("  max_uptime_guard: %s",
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
         CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS > 0 ? "enabled" : "disabled"
#else
         "disabled"
#endif
         );
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
  printf(" (%u ms)",
         (unsigned int)CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS);
#endif
  printf("\n");
    {
      char status_guard[768];

      if (fc_guard_status(status_guard, sizeof(status_guard)) == 0)
        {
          print_guard_status(status_guard);
        }
    }
#ifdef CONFIG_FRUITCLAW_MCP_SERVER
    {
      char mcp_status[512];

      printf("  mcp: enabled\n");
      if (fc_mcp_status_format(mcp_status, sizeof(mcp_status)) == 0)
        {
          printf("  %s\n", mcp_status);
        }
    }
#else
  printf("  mcp: disabled\n");
#endif
  fc_cap_status(stdout);
  printf("  detail_commands: tools, schedule list, mcp status\n");

  return fc_cli_guard_end(&guard, 0);
}

static int cmd_telegram_test(void)
{
  char chat_id[FC_CHAT_ID_LEN];
  int ret;

  fc_bootstrap();
  ret = fc_telegram_first_allowed_chat(chat_id, sizeof(chat_id));
  if (ret < 0)
    {
      printf("No allowed Telegram chat configured.\n");
      return 1;
    }

  ret = fc_telegram_send_message(chat_id, "FruitClaw telegram-test OK");
  if (ret < 0)
    {
      printf("Telegram test failed: %d\n", ret);
      return 1;
    }

  printf("Telegram test sent.\n");
  return 0;
}

static int cmd_telegram_discover(void)
{
  char *out;
  int ret;

  fc_bootstrap();
  out = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  if (out == NULL)
    {
      return 1;
    }

  ret = fc_telegram_discover_chats(out, CONFIG_FRUITCLAW_MAX_JSON);
  printf("%s", out);
  free(out);
  return ret < 0 ? 1 : 0;
}

static int cmd_telegram_inject(int argc, char *argv[])
{
  char text[CONFIG_FRUITCLAW_MAX_EVENT_TEXT];
  int ret;

  if (argc < 3)
    {
      usage();
      return 1;
    }

  fc_bootstrap();
  if (join_args(argc, argv, 2, text, sizeof(text)) < 0)
    {
      printf("telegram inject text too long\n");
      return 1;
    }

  ret = fc_telegram_inject_text(NULL, text);
  if (ret < 0)
    {
      printf("Telegram inject failed: %d\n", ret);
      return 1;
    }

  printf("Telegram inject queued.\n");
  return 0;
}

static int cmd_deepseek_test(void)
{
  char *reply;
  int ret;

  fc_bootstrap();
  reply = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  if (reply == NULL)
    {
      return 1;
    }

  ret = fc_deepseek_test(reply, CONFIG_FRUITCLAW_MAX_JSON);
  printf("%s\n", reply);
  free(reply);
  return ret < 0 ? 1 : 0;
}

static int cmd_schedule(int argc, char *argv[])
{
  fc_guard_long_t guard;
  char prompt[CONFIG_FRUITCLAW_MAX_EVENT_TEXT];
  char list[CONFIG_FRUITCLAW_MAX_JSON];
  char *endptr;
  long long value;
  int ret;

  ret = fc_cli_guard_begin("schedule", &guard);
  if (ret < 0)
    {
      return 1;
    }

  ret = fc_bootstrap();
  if (ret < 0)
    {
      return fc_cli_guard_end(&guard, 1);
    }

  if (argc < 3)
    {
      usage();
      return fc_cli_guard_end(&guard, 1);
    }

  if (strcmp(argv[2], "list") == 0)
    {
      ret = fc_scheduler_list(list, sizeof(list));
      printf("%s", list);
      return fc_cli_guard_end(&guard, ret < 0 ? 1 : 0);
    }

  if (strcmp(argv[2], "add-interval") == 0 && argc >= 6)
    {
      value = strtoll(argv[4], &endptr, 10);
      if (*argv[4] == '\0' || *endptr != '\0' || value <= 0 ||
          value > UINT32_MAX)
        {
          printf("invalid interval seconds\n");
          return fc_cli_guard_end(&guard, 1);
        }

      join_args(argc, argv, 5, prompt, sizeof(prompt));
      ret = fc_scheduler_add_interval(argv[3], (uint32_t)value,
                                      prompt);
      printf("%s\n", ret == 0 ? "schedule added" : "schedule add failed");
      return fc_cli_guard_end(&guard, ret < 0 ? 1 : 0);
    }

  if (strcmp(argv[2], "add-once") == 0 && argc >= 6)
    {
      value = strtoll(argv[4], &endptr, 10);
      if (*argv[4] == '\0' || *endptr != '\0' || value <= 0)
        {
          printf("invalid epoch seconds\n");
          return fc_cli_guard_end(&guard, 1);
        }

      join_args(argc, argv, 5, prompt, sizeof(prompt));
      ret = fc_scheduler_add_once(argv[3], (int64_t)value, prompt);
      printf("%s\n", ret == 0 ? "schedule added" : "schedule add failed");
      return fc_cli_guard_end(&guard, ret < 0 ? 1 : 0);
    }

  if (strcmp(argv[2], "add-after") == 0 && argc >= 6)
    {
      time_t now;

      value = strtoll(argv[4], &endptr, 10);
      if (*argv[4] == '\0' || *endptr != '\0' || value <= 0)
        {
          printf("invalid delay seconds\n");
          return fc_cli_guard_end(&guard, 1);
        }

      now = time(NULL);
      if (now <= 0)
        {
          printf("system time is not set\n");
          return fc_cli_guard_end(&guard, 1);
        }

      join_args(argc, argv, 5, prompt, sizeof(prompt));
      ret = fc_scheduler_add_once(argv[3], (int64_t)now + value, prompt);
      printf("%s\n", ret == 0 ? "schedule added" : "schedule add failed");
      return fc_cli_guard_end(&guard, ret < 0 ? 1 : 0);
    }

  if (strcmp(argv[2], "add-cron") == 0 && argc >= 6)
    {
      join_args(argc, argv, 5, prompt, sizeof(prompt));
      ret = fc_scheduler_add_cron(argv[3], argv[4], prompt);
      printf("%s\n", ret == 0 ? "schedule added" : "schedule add failed");
      return fc_cli_guard_end(&guard, ret < 0 ? 1 : 0);
    }

  if (strcmp(argv[2], "add-boot") == 0 && argc >= 5)
    {
      join_args(argc, argv, 4, prompt, sizeof(prompt));
      ret = fc_scheduler_add_boot(argv[3], prompt);
      printf("%s\n", ret == 0 ? "schedule added" : "schedule add failed");
      return fc_cli_guard_end(&guard, ret < 0 ? 1 : 0);
    }

  if (strcmp(argv[2], "remove") == 0 && argc >= 4)
    {
      ret = fc_scheduler_remove(argv[3]);
      printf("%s\n", ret == 0 ? "schedule removed" :
             "schedule remove failed");
      return fc_cli_guard_end(&guard, ret < 0 ? 1 : 0);
    }

  usage();
  return fc_cli_guard_end(&guard, 1);
}

static int cmd_berry_run(int argc, char *argv[])
{
  char *out;
  const char *args = "{}";
  fc_tool_context_t ctx;
  int ret;

  if (argc < 3)
    {
      usage();
      return 1;
    }

  fc_bootstrap();
  if (argc >= 4)
    {
      args = argv[3];
    }

  out = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  if (out == NULL)
    {
      return 1;
    }

  fc_tool_context_local(&ctx);
  ret = fc_berry_run_file(&ctx, argv[2], args, out,
                          CONFIG_FRUITCLAW_MAX_JSON);
  printf("%s\n", out);
  free(out);
  return ret < 0 ? 1 : 0;
}

static int cmd_berry_smoke(void)
{
  char path[FC_PATH_LEN];
  char *out;
  fc_tool_context_t ctx;
  int ret;

  ret = fc_bootstrap();
  if (ret < 0)
    {
      return 1;
    }

  ret = fc_data_path("scripts/hello.be", path, sizeof(path));
  if (ret < 0)
    {
      printf("Berry smoke path failed: %d\n", ret);
      return 1;
    }

  ret = fc_write_text_file_atomic(path, "claw.reply(\"hello\")\n");
  if (ret < 0)
    {
      printf("Berry smoke write failed: %d\n", ret);
      return 1;
    }

  out = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  if (out == NULL)
    {
      return 1;
    }

  fc_tool_context_local(&ctx);
  ret = fc_berry_run_file(&ctx, "hello.be", "{}", out,
                          CONFIG_FRUITCLAW_MAX_JSON);
  printf("%s\n", out);
  free(out);
  return ret < 0 ? 1 : 0;
}

static int cmd_guard_test(void)
{
  int guard_fd = -1;
  int ret;
  unsigned int sleep_sec;

  ret = fc_bootstrap();
  if (ret < 0)
    {
      return 1;
    }

  ret = fc_guard_arm(FC_GUARD_STAGE_TEST, &guard_fd);
  if (ret < 0)
    {
      printf("guard-test arm failed: %d\n", ret);
      return 1;
    }

  if (guard_fd < 0)
    {
      printf("guard-test unavailable: exec guard disabled\n");
      return 1;
    }

  sleep_sec = (CONFIG_FRUITCLAW_GUARD_TIMEOUT_MS + 1999) / 1000;
  printf("guard-test armed; board should watchdog-reset to %s in about "
         "%u s\n",
#ifdef CONFIG_FRUITCLAW_GUARD_BOOTSEL_RECOVERY
         "BOOTSEL",
#else
         "the app",
#endif
         sleep_sec);
  sleep(sleep_sec + 2);
  fc_guard_disarm(guard_fd);
  printf("guard-test failed: watchdog did not reset the board\n");
  return 1;
}

static int cmd_reboot(void)
{
  int ret;

  ret = fc_guard_prepare_controlled_reboot();
  if (ret < 0)
    {
      printf("controlled reboot prep failed: %d\n", ret);
      return 1;
    }

  boardctl(BOARDIOC_RESET, 0);
  printf("controlled reboot failed\n");
  return 1;
}

static int cmd_recover(void)
{
  int ret;

  printf("forcing FruitClaw watchdog recovery...\n");
  fflush(stdout);
  ret = fc_guard_force_recovery(FC_GUARD_STAGE_CLI);
  printf("FruitClaw recovery failed: %d\n", ret);
  return 1;
}

static int cmd_terminal_run(int argc, char *argv[])
{
  char command[FC_TERMINAL_MAX_COMMAND + 1];
  char esc[FC_TERMINAL_MAX_COMMAND * 6 + 1];
  char args[FC_TERMINAL_MAX_COMMAND * 6 + 32];
  char *out;
  fc_tool_context_t ctx;
  int ret;

  if (argc < 3)
    {
      usage();
      return 1;
    }

  fc_bootstrap();
  if (join_args(argc, argv, 2, command, sizeof(command)) < 0 ||
      fc_json_escape(command, esc, sizeof(esc)) < 0)
    {
      printf("command too long\n");
      return 1;
    }

  snprintf(args, sizeof(args), "{\"command\":\"%s\"}", esc);
  out = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  if (out == NULL)
    {
      return 1;
    }

  fc_tool_context_local(&ctx);
  ret = fc_builtin_terminal_run(&ctx, args, out, CONFIG_FRUITCLAW_MAX_JSON);
  printf("%s\n", out);
  free(out);
  return ret < 0 ? 1 : 0;
}

static int cmd_neopixels(int argc, char *argv[])
{
  char args[256];
  char *out;
  fc_tool_context_t ctx;
  const char *cmd;
  int ret;

  if (argc < 3)
    {
      usage();
      return 1;
    }

  fc_bootstrap();
  cmd = argv[2];
  if (strcmp(cmd, "off") == 0)
    {
      snprintf(args, sizeof(args), "{\"effect\":\"off\"}");
    }
  else if (strcmp(cmd, "rainbow") == 0 || strcmp(cmd, "chase") == 0 ||
           strcmp(cmd, "pulse") == 0)
    {
      const char *color = argc >= 4 ? argv[3] : "blue";
      snprintf(args, sizeof(args),
               "{\"effect\":\"%s\",\"color\":\"%s\",\"cycles\":%d}",
               cmd, color, argc >= 5 ? atoi(argv[4]) : 2);
    }
  else if (argc >= 5)
    {
      snprintf(args, sizeof(args),
               "{\"effect\":\"fill\",\"r\":%d,\"g\":%d,\"b\":%d}",
               atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    }
  else
    {
      snprintf(args, sizeof(args),
               "{\"effect\":\"fill\",\"color\":\"%s\"}", cmd);
    }

  out = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  if (out == NULL)
    {
      return 1;
    }

  fc_tool_context_local(&ctx);
  ret = fc_builtin_neopixels_set(&ctx, args, out,
                                 CONFIG_FRUITCLAW_MAX_JSON);
  printf("%s\n", out);
  free(out);
  return ret < 0 ? 1 : 0;
}

static int cmd_device(int argc, char *argv[])
{
  char *args;
  char *out;
  char *esc_path;
  char *esc_data;
  char data[512];
  fc_tool_context_t ctx;
  const char *tool = NULL;
  int ret;

  if (argc < 3)
    {
      usage();
      return 1;
    }

  ret = fc_bootstrap();
  if (ret < 0)
    {
      return 1;
    }

  args = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  out = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  esc_path = calloc(1, FC_PATH_LEN * 6 + 1);
  esc_data = calloc(1, sizeof(data) * 6 + 1);
  if (args == NULL || out == NULL || esc_path == NULL || esc_data == NULL)
    {
      free(args);
      free(out);
      free(esc_path);
      free(esc_data);
      return 1;
    }

  if (strcmp(argv[2], "list") == 0)
    {
      tool = "device.list";
      snprintf(args, CONFIG_FRUITCLAW_MAX_JSON, "{}");
    }
  else if (strcmp(argv[2], "read") == 0 && argc >= 4)
    {
      long max_bytes = 128;
      char *endptr;

      if (fc_json_escape(argv[3], esc_path, FC_PATH_LEN * 6 + 1) < 0)
        {
          printf("path too long\n");
          ret = 1;
          goto out;
        }

      if (argc >= 5)
        {
          max_bytes = strtol(argv[4], &endptr, 10);
          if (*argv[4] == '\0' || *endptr != '\0' || max_bytes <= 0 ||
              max_bytes > 256)
            {
              printf("invalid max-bytes\n");
              ret = 1;
              goto out;
            }
        }

      tool = "device.read";
      snprintf(args, CONFIG_FRUITCLAW_MAX_JSON,
               "{\"path\":\"%s\",\"max_bytes\":%ld}",
               esc_path, max_bytes);
    }
  else if (strcmp(argv[2], "write-text") == 0 && argc >= 5)
    {
      if (fc_json_escape(argv[3], esc_path, FC_PATH_LEN * 6 + 1) < 0 ||
          join_args(argc, argv, 4, data, sizeof(data)) < 0 ||
          fc_json_escape(data, esc_data, sizeof(data) * 6 + 1) < 0)
        {
          printf("device write data too long\n");
          ret = 1;
          goto out;
        }

      tool = "device.write";
      snprintf(args, CONFIG_FRUITCLAW_MAX_JSON,
               "{\"path\":\"%s\",\"mode\":\"text\",\"data\":\"%s\"}",
               esc_path, esc_data);
    }
  else if (strcmp(argv[2], "write-hex") == 0 && argc >= 5)
    {
      if (fc_json_escape(argv[3], esc_path, FC_PATH_LEN * 6 + 1) < 0 ||
          join_args(argc, argv, 4, data, sizeof(data)) < 0 ||
          fc_json_escape(data, esc_data, sizeof(data) * 6 + 1) < 0)
        {
          printf("device hex data too long\n");
          ret = 1;
          goto out;
        }

      tool = "device.write";
      snprintf(args, CONFIG_FRUITCLAW_MAX_JSON,
               "{\"path\":\"%s\",\"mode\":\"hex\",\"data\":\"%s\"}",
               esc_path, esc_data);
    }
  else
    {
      usage();
      ret = 1;
      goto out;
    }

  fc_tool_context_local(&ctx);
  ret = fc_cap_execute_ctx(&ctx, tool, args, out, CONFIG_FRUITCLAW_MAX_JSON);
  printf("%s\n", out);
  ret = ret < 0 ? 1 : 0;

out:
  free(args);
  free(out);
  free(esc_path);
  free(esc_data);
  return ret;
}

static int cmd_service(int argc, char *argv[])
{
  fc_guard_long_t guard;
  const char *action;
  const char *service;
  char *out;
  int ret;

  if (argc < 3)
    {
      usage();
      return 1;
    }

  if (strcmp(argv[2], "status") == 0)
    {
      action = "status";
      service = argc >= 4 ? argv[3] : "all";
    }
  else if (argc >= 4)
    {
      action = argv[2];
      service = argv[3];
    }
  else
    {
      usage();
      return 1;
    }

  if (strcmp(action, "status") == 0)
    {
      ret = fc_cli_guard_begin_optional_status("service", &guard);
    }
  else
    {
      ret = fc_cli_guard_begin("service", &guard);
    }

  if (ret < 0)
    {
      return 1;
    }

  ret = fc_bootstrap();
  if (ret < 0)
    {
      return fc_cli_guard_end(&guard, 1);
    }

  out = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  if (out == NULL)
    {
      return fc_cli_guard_end(&guard, 1);
    }

  ret = fc_service_control(service, action, out, CONFIG_FRUITCLAW_MAX_JSON);
  printf("%s\n", out);
  free(out);

  return fc_cli_guard_end(&guard, ret < 0 ? 1 : 0);
}

static int cmd_tools(void)
{
  fc_guard_long_t guard;
  int ret;

  ret = fc_cli_guard_begin("tools", &guard);
  if (ret < 0)
    {
      return 1;
    }

  ret = fc_bootstrap();
  if (ret < 0)
    {
      return fc_cli_guard_end(&guard, 1);
    }

  fc_cap_list(stdout);
  return fc_cli_guard_end(&guard, 0);
}

static int cmd_mcp_status_cli(void)
{
  char bootstrap_stage[32];
  bool bootstrap_ready;
  bool bootstrap_busy;
  int bootstrap_ret;
  int ret;

  fc_bootstrap_probe(&bootstrap_ready, &bootstrap_busy);
  fc_bootstrap_get_stage(bootstrap_stage, sizeof(bootstrap_stage),
                         &bootstrap_ret);
  printf("bootstrap: %s stage=%s ret=%d\n",
         bootstrap_ready ? "ready" :
         bootstrap_busy ? "initializing" : "not ready",
         bootstrap_stage, bootstrap_ret);

#ifdef CONFIG_FRUITCLAW_MCP_SERVER
  ret = fc_mcp_status(stdout);
#else
  printf("mcp: disabled\n");
  ret = 0;
#endif
  return ret < 0 ? 1 : 0;
}

int main(int argc, FAR char *argv[])
{
  if (argc < 2)
    {
      usage();
      return 1;
    }

  if (strcmp(argv[1], "boot") == 0)
    {
      return cmd_boot();
    }
  else if (strcmp(argv[1], "start") == 0)
    {
      fc_ignore_console_interrupts("start");
      return fc_runtime_start();
    }
  else if (strcmp(argv[1], "status") == 0)
    {
      return cmd_status(false);
    }
  else if (strcmp(argv[1], "status-net") == 0)
    {
      return cmd_status(true);
    }
  else if (strcmp(argv[1], "reboot") == 0)
    {
      return cmd_reboot();
    }
  else if (strcmp(argv[1], "recover") == 0)
    {
      return cmd_recover();
    }
  else if (strcmp(argv[1], "wifi-up") == 0)
    {
      bool force = argc >= 3 && strcmp(argv[2], "--force") == 0;

      if (argc >= 3 && !force)
        {
          usage();
          return 1;
        }

      return cmd_wifi_up(force);
    }
  else if (strcmp(argv[1], "wifi-probe") == 0)
    {
      return cmd_wifi_probe();
    }
  else if (strcmp(argv[1], "webserver") == 0)
    {
      return cmd_webserver_run();
    }
  else if (strcmp(argv[1], "httpd-status") == 0)
    {
      return cmd_httpd_status();
    }
  else if (strcmp(argv[1], "service") == 0)
    {
      return cmd_service(argc, argv);
    }
  else if (strcmp(argv[1], "once") == 0)
    {
      return cmd_once(argc, argv);
    }
  else if (strcmp(argv[1], "selftest") == 0)
    {
      fc_bootstrap();
      return fc_selftest_main();
    }
  else if (strcmp(argv[1], "tools") == 0)
    {
      return cmd_tools();
    }
  else if (strcmp(argv[1], "config") == 0)
    {
      return cmd_config_cli(argc, argv);
    }
  else if (strcmp(argv[1], "telegram-test") == 0)
    {
      return cmd_telegram_test();
    }
  else if (strcmp(argv[1], "telegram-discover") == 0)
    {
      return cmd_telegram_discover();
    }
  else if (strcmp(argv[1], "telegram-inject") == 0)
    {
      return cmd_telegram_inject(argc, argv);
    }
  else if (strcmp(argv[1], "deepseek-test") == 0)
    {
      return cmd_deepseek_test();
    }
  else if (strcmp(argv[1], "schedule") == 0)
    {
      return cmd_schedule(argc, argv);
    }
  else if (strcmp(argv[1], "berry-run") == 0)
    {
      return cmd_berry_run(argc, argv);
    }
  else if (strcmp(argv[1], "berry-smoke") == 0)
    {
      return cmd_berry_smoke();
    }
  else if (strcmp(argv[1], "guard-test") == 0)
    {
      return cmd_guard_test();
    }
  else if (strcmp(argv[1], "terminal-run") == 0)
    {
      return cmd_terminal_run(argc, argv);
    }
  else if (strcmp(argv[1], "neopixels") == 0)
    {
      return cmd_neopixels(argc, argv);
    }
  else if (strcmp(argv[1], "device") == 0)
    {
      return cmd_device(argc, argv);
    }
  else if (strcmp(argv[1], "mcp") == 0)
    {
      if (argc >= 3 && strcmp(argv[2], "status") == 0)
        {
          return cmd_mcp_status_cli();
        }

      usage();
      return 1;
    }

  usage();
  return 1;
}
