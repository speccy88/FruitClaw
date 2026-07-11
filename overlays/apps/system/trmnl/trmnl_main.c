/* SPDX-License-Identifier: Apache-2.0 */

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/video/fb.h>

#if (defined(CONFIG_SYSTEM_TRMNL_BOOT_GUARD) && \
     defined(CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD)) || \
    defined(CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED)
#  include <arch/board/board.h>
#endif

#include "netutils/cJSON.h"
#include "netutils/webclient.h"
#include "zlib.h"

#ifndef CONFIG_SYSTEM_TRMNL_PROGNAME
#  define CONFIG_SYSTEM_TRMNL_PROGNAME "trmnl"
#endif

#ifndef CONFIG_SYSTEM_TRMNL_FB_DEVPATH
#  define CONFIG_SYSTEM_TRMNL_FB_DEVPATH "/dev/fb0"
#endif

#ifndef CONFIG_SYSTEM_TRMNL_SD_DATA_DIR
#  define CONFIG_SYSTEM_TRMNL_SD_DATA_DIR "/mnt/sd0/trmnl"
#endif

#ifndef CONFIG_SYSTEM_TRMNL_FLASH_DATA_DIR
#  define CONFIG_SYSTEM_TRMNL_FLASH_DATA_DIR "/flash/trmnl"
#endif

#ifndef CONFIG_SYSTEM_TRMNL_DATA_DIR
#  define CONFIG_SYSTEM_TRMNL_DATA_DIR "/data/trmnl"
#endif

#ifndef CONFIG_SYSTEM_TRMNL_MAX_JSON
#  define CONFIG_SYSTEM_TRMNL_MAX_JSON 8192
#endif

#ifndef CONFIG_SYSTEM_TRMNL_MAX_PNG
#  define CONFIG_SYSTEM_TRMNL_MAX_PNG 1048576
#endif

#ifndef CONFIG_SYSTEM_TRMNL_MAX_URL
#  define CONFIG_SYSTEM_TRMNL_MAX_URL 2048
#endif

#ifndef CONFIG_SYSTEM_TRMNL_DEFAULT_API_URL
#  define CONFIG_SYSTEM_TRMNL_DEFAULT_API_URL "https://usetrmnl.com/api"
#endif

#ifndef CONFIG_SYSTEM_TRMNL_DEFAULT_ID
#  define CONFIG_SYSTEM_TRMNL_DEFAULT_ID ""
#endif

#ifndef CONFIG_SYSTEM_TRMNL_DEFAULT_TOKEN
#  define CONFIG_SYSTEM_TRMNL_DEFAULT_TOKEN ""
#endif

#ifndef CONFIG_SYSTEM_TRMNL_FW_VERSION
#  define CONFIG_SYSTEM_TRMNL_FW_VERSION "fruitjam-nuttx-0.1"
#endif

#ifndef CONFIG_SYSTEM_TRMNL_MODEL
#  define CONFIG_SYSTEM_TRMNL_MODEL "fruitjam-nuttx"
#endif

#ifndef CONFIG_SYSTEM_TRMNL_DEFAULT_REFRESH_SEC
#  define CONFIG_SYSTEM_TRMNL_DEFAULT_REFRESH_SEC 900
#endif

#ifndef CONFIG_SYSTEM_TRMNL_HTTP_TIMEOUT_SEC
#  define CONFIG_SYSTEM_TRMNL_HTTP_TIMEOUT_SEC 30
#endif

#ifndef CONFIG_SYSTEM_TRMNL_WIFI_IFNAME
#  define CONFIG_SYSTEM_TRMNL_WIFI_IFNAME "wlan0"
#endif

#ifndef CONFIG_SYSTEM_TRMNL_WIFI_CONFIG_PATH
#  define CONFIG_SYSTEM_TRMNL_WIFI_CONFIG_PATH "/mnt/sd0/trmnl_wifi.conf"
#endif

#ifndef CONFIG_SYSTEM_TRMNL_WIFI_CONFIG_LEAF
#  define CONFIG_SYSTEM_TRMNL_WIFI_CONFIG_LEAF "wifi.conf"
#endif

#ifndef CONFIG_SYSTEM_TRMNL_WIFI_DEFAULT_SSID
#  define CONFIG_SYSTEM_TRMNL_WIFI_DEFAULT_SSID ""
#endif

#ifndef CONFIG_SYSTEM_TRMNL_WIFI_DEFAULT_PSK
#  define CONFIG_SYSTEM_TRMNL_WIFI_DEFAULT_PSK ""
#endif

#ifndef CONFIG_SYSTEM_TRMNL_BOOT_WIFI_RETRIES
#  define CONFIG_SYSTEM_TRMNL_BOOT_WIFI_RETRIES 8
#endif

#ifndef CONFIG_SYSTEM_TRMNL_BOOT_WIFI_RETRY_DELAY
#  define CONFIG_SYSTEM_TRMNL_BOOT_WIFI_RETRY_DELAY 2
#endif

#ifndef CONFIG_SYSTEM_TRMNL_TLS_CA_CERT_PATH
#  define CONFIG_SYSTEM_TRMNL_TLS_CA_CERT_PATH "/mnt/sd0/trmnl/certs/roots.pem"
#endif

#define TRMNL_WIDTH 800
#define TRMNL_HEIGHT 480
#define TRMNL_Y2_STRIDE (TRMNL_WIDTH / 4)
#define TRMNL_Y2_BYTES (TRMNL_Y2_STRIDE * TRMNL_HEIGHT)
#define TRMNL_PATH_LEN 160
#define TRMNL_ID_LEN 32
#define TRMNL_TOKEN_LEN 96
#define TRMNL_API_LEN 160
#define TRMNL_FILENAME_LEN 160
#define TRMNL_HEADER_MAX 14
#define TRMNL_WIFI_SSID_LEN 64
#define TRMNL_WIFI_PSK_LEN 96

struct trmnl_config
{
  char root[TRMNL_PATH_LEN];
  char id[TRMNL_ID_LEN];
  char token[TRMNL_TOKEN_LEN];
  char api_url[TRMNL_API_LEN];
  char image_format[16];
};

struct trmnl_display_response
{
  char image_url[CONFIG_SYSTEM_TRMNL_MAX_URL];
  char filename[TRMNL_FILENAME_LEN];
  uint32_t refresh_rate;
  int status;
};

struct trmnl_state
{
  long http_status;
  uint32_t refresh_rate;
  bool image_cached;
  char filename[TRMNL_FILENAME_LEN];
  char error[128];
};

struct trmnl_header
{
  const char *name;
  const char *value;
};

struct trmnl_headers
{
  struct trmnl_header items[TRMNL_HEADER_MAX];
  unsigned int count;
};

struct trmnl_http_buf
{
  uint8_t *data;
  size_t len;
  size_t cap;
};

struct trmnl_png
{
  unsigned int width;
  unsigned int height;
  uint8_t *gray;
};

static pthread_mutex_t g_http_lock = PTHREAD_MUTEX_INITIALIZER;

typedef int (*trmnl_cmd_fn_t)(void);

static uint64_t trmnl_now_ms(void);
static int trmnl_load_compiled_config(struct trmnl_config *cfg);

#if defined(CONFIG_SYSTEM_TRMNL_ENABLE_TLS) && defined(CONFIG_CRYPTO_MBEDTLS)
#  include <mbedtls/ctr_drbg.h>
#  include <mbedtls/debug.h>
#  include <mbedtls/entropy.h>
#  include <mbedtls/error.h>
#  include <mbedtls/net_sockets.h>
#  include <mbedtls/ssl.h>
#  include <mbedtls/x509_crt.h>

struct trmnl_tls_connection
{
  mbedtls_net_context net;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_x509_crt ca;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
};

static const int g_trmnl_tls12_ciphersuites[] =
{
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
  MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
  MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
  MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
  MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
  MBEDTLS_TLS_RSA_WITH_AES_128_GCM_SHA256,
  MBEDTLS_TLS_RSA_WITH_AES_256_GCM_SHA384,
  0
};

static const uint16_t g_trmnl_tls_groups[] =
{
  MBEDTLS_SSL_IANA_TLS_GROUP_X25519,
  MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1,
  0
};

#ifdef CONFIG_MBEDTLS_DEBUG_C
static void trmnl_tls_debug(void *ctx, int level, const char *file, int line,
                            const char *str)
{
  const char *base;

  (void)ctx;
  base = strrchr(file, '/');
  fprintf(stderr, "trmnl: tls dbg%d %s:%d: %s", level,
          base != NULL ? base + 1 : file, line, str);
}
#endif

static const char *trmnl_tls_error(int ret, char *buf, size_t buflen)
{
#ifdef CONFIG_MBEDTLS_ERROR_C
  mbedtls_strerror(ret, buf, buflen);
  return buf;
#else
  snprintf(buf, buflen, "mbedtls error %d", ret);
  return buf;
#endif
}

static const char *trmnl_tls_state_name(int state)
{
  switch (state)
    {
      case MBEDTLS_SSL_HELLO_REQUEST:
        return "HELLO_REQUEST";
      case MBEDTLS_SSL_CLIENT_HELLO:
        return "CLIENT_HELLO";
      case MBEDTLS_SSL_SERVER_HELLO:
        return "SERVER_HELLO";
      case MBEDTLS_SSL_SERVER_CERTIFICATE:
        return "SERVER_CERTIFICATE";
      case MBEDTLS_SSL_SERVER_KEY_EXCHANGE:
        return "SERVER_KEY_EXCHANGE";
      case MBEDTLS_SSL_CERTIFICATE_REQUEST:
        return "CERTIFICATE_REQUEST";
      case MBEDTLS_SSL_SERVER_HELLO_DONE:
        return "SERVER_HELLO_DONE";
      case MBEDTLS_SSL_CLIENT_CERTIFICATE:
        return "CLIENT_CERTIFICATE";
      case MBEDTLS_SSL_CLIENT_KEY_EXCHANGE:
        return "CLIENT_KEY_EXCHANGE";
      case MBEDTLS_SSL_CERTIFICATE_VERIFY:
        return "CERTIFICATE_VERIFY";
      case MBEDTLS_SSL_CLIENT_CHANGE_CIPHER_SPEC:
        return "CLIENT_CHANGE_CIPHER_SPEC";
      case MBEDTLS_SSL_CLIENT_FINISHED:
        return "CLIENT_FINISHED";
      case MBEDTLS_SSL_SERVER_CHANGE_CIPHER_SPEC:
        return "SERVER_CHANGE_CIPHER_SPEC";
      case MBEDTLS_SSL_SERVER_FINISHED:
        return "SERVER_FINISHED";
      case MBEDTLS_SSL_FLUSH_BUFFERS:
        return "FLUSH_BUFFERS";
      case MBEDTLS_SSL_HANDSHAKE_WRAPUP:
        return "HANDSHAKE_WRAPUP";
      case MBEDTLS_SSL_NEW_SESSION_TICKET:
        return "NEW_SESSION_TICKET";
      case MBEDTLS_SSL_SERVER_HELLO_VERIFY_REQUEST_SENT:
        return "SERVER_HELLO_VERIFY_REQUEST_SENT";
      case MBEDTLS_SSL_HELLO_RETRY_REQUEST:
        return "HELLO_RETRY_REQUEST";
      case MBEDTLS_SSL_ENCRYPTED_EXTENSIONS:
        return "ENCRYPTED_EXTENSIONS";
      case MBEDTLS_SSL_END_OF_EARLY_DATA:
        return "END_OF_EARLY_DATA";
      case MBEDTLS_SSL_CLIENT_CERTIFICATE_VERIFY:
        return "CLIENT_CERTIFICATE_VERIFY";
      case MBEDTLS_SSL_CLIENT_CCS_AFTER_SERVER_FINISHED:
        return "CLIENT_CCS_AFTER_SERVER_FINISHED";
      case MBEDTLS_SSL_CLIENT_CCS_BEFORE_2ND_CLIENT_HELLO:
        return "CLIENT_CCS_BEFORE_2ND_CLIENT_HELLO";
      case MBEDTLS_SSL_SERVER_CCS_AFTER_SERVER_HELLO:
        return "SERVER_CCS_AFTER_SERVER_HELLO";
      case MBEDTLS_SSL_CLIENT_CCS_AFTER_CLIENT_HELLO:
        return "CLIENT_CCS_AFTER_CLIENT_HELLO";
      default:
        return "UNKNOWN";
    }
}

static unsigned int trmnl_tls_timeout_ms(unsigned int timeout_second)
{
  if (timeout_second > 0 && timeout_second < 3600)
    {
      return timeout_second * 1000;
    }

  return 30000;
}

static bool trmnl_tls_deadline_expired(uint64_t deadline_ms)
{
  int64_t remaining = (int64_t)(deadline_ms - trmnl_now_ms());

  return remaining <= 0;
}

#endif

static uint64_t trmnl_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
    {
      return 0;
    }

  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool trmnl_bootguard_arm(const char *command)
{
#if defined(CONFIG_SYSTEM_TRMNL_BOOT_GUARD) && \
    defined(CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD)
  board_fruitjam_bootguard_arm();
  printf("trmnl: bootguard armed for %s\n", command);
  return true;
#else
  (void)command;
  return false;
#endif
}

static void trmnl_bootguard_disarm(bool armed)
{
#if defined(CONFIG_SYSTEM_TRMNL_BOOT_GUARD) && \
    defined(CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_BOOT_GUARD)
  if (armed)
    {
      board_fruitjam_bootguard_disarm();
    }
#else
  (void)armed;
#endif
}

static int trmnl_run_guarded(const char *command, trmnl_cmd_fn_t fn)
{
  bool armed = trmnl_bootguard_arm(command);
  int ret = fn();

  trmnl_bootguard_disarm(armed);
  return ret;
}

static void trmnl_path(char *out, size_t out_len, const char *root,
                       const char *leaf)
{
  snprintf(out, out_len, "%s/%s", root, leaf);
}

static int trmnl_mkdirs(const char *path)
{
  char tmp[TRMNL_PATH_LEN];
  char *p;

  if (path == NULL || path[0] == '\0')
    {
      return -EINVAL;
    }

  strlcpy(tmp, path, sizeof(tmp));
  for (p = tmp + 1; *p != '\0'; p++)
    {
      if (*p == '/')
        {
          *p = '\0';
          if (mkdir(tmp, 0777) < 0 && errno != EEXIST)
            {
              return -errno;
            }

          *p = '/';
        }
    }

  if (mkdir(tmp, 0777) < 0 && errno != EEXIST)
    {
      return -errno;
    }

  return 0;
}

static bool trmnl_parent_dir_ready(const char *path)
{
  char parent[TRMNL_PATH_LEN];
  struct stat st;
  char *slash;

  if (path == NULL || path[0] == '\0')
    {
      return false;
    }

  strlcpy(parent, path, sizeof(parent));
  slash = strrchr(parent, '/');
  if (slash == NULL || slash == parent)
    {
      return true;
    }

  *slash = '\0';
  return stat(parent, &st) == 0 && S_ISDIR(st.st_mode);
}

static int trmnl_select_root(char *root, size_t root_len)
{
  int ret;

  if (trmnl_parent_dir_ready(CONFIG_SYSTEM_TRMNL_SD_DATA_DIR))
    {
      ret = trmnl_mkdirs(CONFIG_SYSTEM_TRMNL_SD_DATA_DIR);
      if (ret == 0)
        {
          strlcpy(root, CONFIG_SYSTEM_TRMNL_SD_DATA_DIR, root_len);
          return 0;
        }
    }

  if (trmnl_parent_dir_ready(CONFIG_SYSTEM_TRMNL_FLASH_DATA_DIR))
    {
      ret = trmnl_mkdirs(CONFIG_SYSTEM_TRMNL_FLASH_DATA_DIR);
      if (ret == 0)
        {
          strlcpy(root, CONFIG_SYSTEM_TRMNL_FLASH_DATA_DIR, root_len);
          return 0;
        }
    }

  ret = trmnl_mkdirs(CONFIG_SYSTEM_TRMNL_DATA_DIR);
  if (ret < 0)
    {
      return ret;
    }

  strlcpy(root, CONFIG_SYSTEM_TRMNL_DATA_DIR, root_len);
  return 0;
}

static const char *trmnl_root_kind(const char *root)
{
  if (strcmp(root, CONFIG_SYSTEM_TRMNL_SD_DATA_DIR) == 0)
    {
      return "sd";
    }

  if (strcmp(root, CONFIG_SYSTEM_TRMNL_FLASH_DATA_DIR) == 0)
    {
      return "flash";
    }

  if (strcmp(root, CONFIG_SYSTEM_TRMNL_DATA_DIR) == 0)
    {
      return "volatile";
    }

  return "custom";
}

static bool trmnl_root_is_volatile(const char *root)
{
  return strcmp(root, CONFIG_SYSTEM_TRMNL_DATA_DIR) == 0;
}

static void trmnl_log(const char *root, const char *fmt, ...)
{
  char logdir[TRMNL_PATH_LEN];
  char path[TRMNL_PATH_LEN];
  FILE *fp;
  va_list ap;

  trmnl_path(logdir, sizeof(logdir), root, "logs");
  trmnl_mkdirs(logdir);
  trmnl_path(path, sizeof(path), root, "logs/trmnl.log");

  fp = fopen(path, "a");
  if (fp == NULL)
    {
      return;
    }

  fprintf(fp, "%" PRIu64 " ", trmnl_now_ms());
  va_start(ap, fmt);
  vfprintf(fp, fmt, ap);
  va_end(ap);
  fputc('\n', fp);
  fclose(fp);
}

static char *trmnl_read_file(const char *path, size_t max_len, size_t *out_len)
{
  FILE *fp;
  char *buf;
  size_t len;

  fp = fopen(path, "rb");
  if (fp == NULL)
    {
      return NULL;
    }

  buf = malloc(max_len + 1);
  if (buf == NULL)
    {
      fclose(fp);
      return NULL;
    }

  len = fread(buf, 1, max_len, fp);
  if (!feof(fp))
    {
      free(buf);
      fclose(fp);
      errno = EFBIG;
      return NULL;
    }

  buf[len] = '\0';
  fclose(fp);
  if (out_len != NULL)
    {
      *out_len = len;
    }

  return buf;
}

static int trmnl_write_file(const char *path, const void *data, size_t len)
{
  FILE *fp;

  fp = fopen(path, "wb");
  if (fp == NULL)
    {
      return -errno;
    }

  if (fwrite(data, 1, len, fp) != len)
    {
      int ret = -errno;
      fclose(fp);
      return ret < 0 ? ret : -EIO;
    }

  fclose(fp);
  return 0;
}

static int trmnl_write_file_atomic(const char *path, const void *data,
                                   size_t len)
{
  char tmp[TRMNL_PATH_LEN];
  int ret;

  if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp))
    {
      return -ENAMETOOLONG;
    }

  ret = trmnl_write_file(tmp, data, len);
  if (ret < 0)
    {
      unlink(tmp);
      return ret;
    }

  if (rename(tmp, path) < 0)
    {
      ret = -errno;
      unlink(tmp);
      return ret;
    }

  return 0;
}

static void trmnl_trim(char *s)
{
  char *start = s;
  char *end;

  while (*start == ' ' || *start == '\t' || *start == '\r' ||
         *start == '\n')
    {
      start++;
    }

  if (start != s)
    {
      memmove(s, start, strlen(start) + 1);
    }

  end = s + strlen(s);
  while (end > s)
    {
      char ch = end[-1];

      if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
        {
          break;
        }

      *--end = '\0';
    }
}

static int trmnl_shell_quote_arg(const char *in, char *out, size_t out_len)
{
  size_t off = 0;
  size_t i;

  if (in == NULL || out == NULL || out_len < 3)
    {
      return -EINVAL;
    }

  out[off++] = '"';
  for (i = 0; in[i] != '\0'; i++)
    {
      unsigned char ch = (unsigned char)in[i];

      if (ch < 0x20)
        {
          return -EINVAL;
        }

      if (in[i] == '"' || in[i] == '\\')
        {
          if (off + 2 >= out_len)
            {
              return -ENOSPC;
            }

          out[off++] = '\\';
        }
      else if (off + 1 >= out_len)
        {
          return -ENOSPC;
        }

      out[off++] = in[i];
    }

  if (off + 2 > out_len)
    {
      return -ENOSPC;
    }

  out[off++] = '"';
  out[off] = '\0';
  return 0;
}

static int trmnl_run_command(const char *label, const char *cmd,
                             bool required)
{
  int ret = system(cmd);

  if (ret != 0)
    {
      fprintf(stderr, "trmnl: %s failed: %d\n", label, ret);
      return required ? -EIO : 0;
    }

  printf("trmnl: %s ok\n", label);
  return 0;
}

static void trmnl_wifi_parse_line(char *line, char *ssid, size_t ssid_len,
                                  char *psk, size_t psk_len, int *key_mgmt,
                                  int *cipher, int *ordinal)
{
  char *eq;
  char *key;
  char *value;

  trmnl_trim(line);
  if (line[0] == '\0' || line[0] == '#')
    {
      return;
    }

  eq = strchr(line, '=');
  if (eq == NULL)
    {
      if (*ordinal == 0)
        {
          strlcpy(ssid, line, ssid_len);
        }
      else if (*ordinal == 1)
        {
          strlcpy(psk, line, psk_len);
        }

      (*ordinal)++;
      return;
    }

  *eq = '\0';
  key = line;
  value = eq + 1;
  trmnl_trim(key);
  trmnl_trim(value);

  if (strcmp(key, "ssid") == 0)
    {
      strlcpy(ssid, value, ssid_len);
    }
  else if (strcmp(key, "password") == 0 || strcmp(key, "psk") == 0)
    {
      strlcpy(psk, value, psk_len);
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

static int trmnl_wifi_read_config_path(const char *path, char *ssid,
                                       size_t ssid_len, char *psk,
                                       size_t psk_len, int *key_mgmt,
                                       int *cipher)
{
  char *text;
  size_t pos = 0;
  int ordinal = 0;

  if (path == NULL || ssid == NULL || psk == NULL || key_mgmt == NULL ||
      cipher == NULL)
    {
      return -EINVAL;
    }

  ssid[0] = '\0';
  psk[0] = '\0';
  *key_mgmt = 3;
  *cipher = 2;

  text = trmnl_read_file(path, 512, NULL);
  if (text == NULL)
    {
      return -errno;
    }

  while (text[pos] != '\0')
    {
      char line[160];
      size_t off = 0;

      while (text[pos] != '\0' && text[pos] != '\n' &&
             off + 1 < sizeof(line))
        {
          line[off++] = text[pos++];
        }

      while (text[pos] != '\0' && text[pos] != '\n')
        {
          pos++;
        }

      if (text[pos] == '\n')
        {
          pos++;
        }

      line[off] = '\0';
      trmnl_wifi_parse_line(line, ssid, ssid_len, psk, psk_len,
                            key_mgmt, cipher, &ordinal);
    }

  free(text);
  return ssid[0] == '\0' ? -ENOENT : 0;
}

static int trmnl_wifi_read_config(char *ssid, size_t ssid_len, char *psk,
                                  size_t psk_len, int *key_mgmt,
                                  int *cipher, char *used_path,
                                  size_t used_path_len)
{
  char root[TRMNL_PATH_LEN];
  char path[TRMNL_PATH_LEN];
  int ret;

  if (CONFIG_SYSTEM_TRMNL_WIFI_CONFIG_PATH[0] == '/' &&
      access(CONFIG_SYSTEM_TRMNL_WIFI_CONFIG_PATH, F_OK) == 0)
    {
      ret = trmnl_wifi_read_config_path(CONFIG_SYSTEM_TRMNL_WIFI_CONFIG_PATH,
                                        ssid, ssid_len, psk, psk_len,
                                        key_mgmt, cipher);
      if (ret == 0 && used_path != NULL)
        {
          strlcpy(used_path, CONFIG_SYSTEM_TRMNL_WIFI_CONFIG_PATH,
                  used_path_len);
        }

      return ret;
    }

  ret = trmnl_select_root(root, sizeof(root));
  if (ret < 0)
    {
      return ret;
    }

  trmnl_path(path, sizeof(path), root, CONFIG_SYSTEM_TRMNL_WIFI_CONFIG_LEAF);
  ret = trmnl_wifi_read_config_path(path, ssid, ssid_len, psk, psk_len,
                                    key_mgmt, cipher);
  if (ret == 0 && used_path != NULL)
    {
      strlcpy(used_path, path, used_path_len);
    }

  if (ret < 0 && CONFIG_SYSTEM_TRMNL_WIFI_DEFAULT_SSID[0] != '\0')
    {
      if (strlcpy(ssid, CONFIG_SYSTEM_TRMNL_WIFI_DEFAULT_SSID, ssid_len) >=
          ssid_len ||
          strlcpy(psk, CONFIG_SYSTEM_TRMNL_WIFI_DEFAULT_PSK, psk_len) >=
          psk_len)
        {
          return -ENAMETOOLONG;
        }

      *key_mgmt = 3;
      *cipher = 2;
      if (used_path != NULL)
        {
          strlcpy(used_path, "compiled defaults", used_path_len);
        }

      return 0;
    }

  return ret;
}

static int trmnl_wifi_up(void)
{
#ifdef CONFIG_SYSTEM_TRMNL_WIFI_AUTOSTART
  char ssid[TRMNL_WIFI_SSID_LEN];
  char psk[TRMNL_WIFI_PSK_LEN];
  char qssid[TRMNL_WIFI_SSID_LEN * 2 + 4];
  char qpsk[TRMNL_WIFI_PSK_LEN * 2 + 4];
  char path[TRMNL_PATH_LEN];
  char cmd[256];
  int key_mgmt;
  int cipher;
  int ret;

  ret = trmnl_wifi_read_config(ssid, sizeof(ssid), psk, sizeof(psk),
                               &key_mgmt, &cipher, path, sizeof(path));
  if (ret < 0)
    {
      fprintf(stderr, "trmnl: Wi-Fi config missing at %s or data %s\n",
              CONFIG_SYSTEM_TRMNL_WIFI_CONFIG_PATH,
              CONFIG_SYSTEM_TRMNL_WIFI_CONFIG_LEAF);
      return ret;
    }

  if (trmnl_shell_quote_arg(ssid, qssid, sizeof(qssid)) < 0 ||
      trmnl_shell_quote_arg(psk, qpsk, sizeof(qpsk)) < 0)
    {
      fprintf(stderr, "trmnl: Wi-Fi config contains unsupported chars\n");
      return -EINVAL;
    }

  printf("trmnl: Wi-Fi config loaded from %s\n", path);

#ifdef CONFIG_ADAFRUIT_FRUIT_JAM_RP2350_ESP_HOSTED
  ret = board_fruitjam_esp_hosted_start();
  if (ret < 0)
    {
      fprintf(stderr, "trmnl: ESP-Hosted start failed: %d\n", ret);
      return ret;
    }
#endif

  snprintf(cmd, sizeof(cmd), "ifup %s", CONFIG_SYSTEM_TRMNL_WIFI_IFNAME);
  trmnl_run_command("wifi ifup", cmd, false);

  if (psk[0] != '\0')
    {
      snprintf(cmd, sizeof(cmd), "wapi psk %s %s %d %d",
               CONFIG_SYSTEM_TRMNL_WIFI_IFNAME, qpsk, key_mgmt, cipher);
      ret = trmnl_run_command("wifi psk", cmd, true);
      if (ret < 0)
        {
          return ret;
        }
    }

  snprintf(cmd, sizeof(cmd), "wapi essid %s %s 1",
           CONFIG_SYSTEM_TRMNL_WIFI_IFNAME, qssid);
  ret = trmnl_run_command("wifi essid", cmd, true);
  if (ret < 0)
    {
      return ret;
    }

  sleep(3);
  snprintf(cmd, sizeof(cmd), "renew %s", CONFIG_SYSTEM_TRMNL_WIFI_IFNAME);
  return trmnl_run_command("wifi dhcp", cmd, true);
#else
  return 0;
#endif
}

static const char *trmnl_json_string(cJSON *root, const char *key)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

  if (!cJSON_IsString(item) || item->valuestring == NULL)
    {
      return NULL;
    }

  return item->valuestring;
}

static int trmnl_json_escape(char *out, size_t out_len, const char *in)
{
  size_t pos = 0;

  if (out_len == 0)
    {
      return -ENOSPC;
    }

  while (*in != '\0')
    {
      unsigned char ch = (unsigned char)*in++;

      if (ch == '"' || ch == '\\')
        {
          if (pos + 2 >= out_len)
            {
              return -ENOSPC;
            }

          out[pos++] = '\\';
          out[pos++] = (char)ch;
        }
      else if (ch < 0x20)
        {
          if (pos + 6 >= out_len)
            {
              return -ENOSPC;
            }

          snprintf(&out[pos], out_len - pos, "\\u%04x", ch);
          pos += 6;
        }
      else
        {
          if (pos + 1 >= out_len)
            {
              return -ENOSPC;
            }

          out[pos++] = (char)ch;
        }
    }

  out[pos] = '\0';
  return 0;
}

static int trmnl_load_config(struct trmnl_config *cfg)
{
  char path[TRMNL_PATH_LEN];
  char *text;
  cJSON *json;
  const char *value;
  int ret = 0;

  memset(cfg, 0, sizeof(*cfg));
  ret = trmnl_select_root(cfg->root, sizeof(cfg->root));
  if (ret < 0)
    {
      return ret;
    }

  trmnl_path(path, sizeof(path), cfg->root, "device.json");
  text = trmnl_read_file(path, 4096, NULL);
  if (text == NULL)
    {
      int saved_errno = errno;

      ret = trmnl_load_compiled_config(cfg);
      if (ret == 0)
        {
          return 0;
        }

      return -saved_errno;
    }

  json = cJSON_Parse(text);
  free(text);
  if (json == NULL)
    {
      return -EINVAL;
    }

  value = trmnl_json_string(json, "TrmnlId");
  if (value == NULL || strlcpy(cfg->id, value, sizeof(cfg->id)) >=
      sizeof(cfg->id))
    {
      ret = -EINVAL;
      goto out;
    }

  value = trmnl_json_string(json, "TrmnlToken");
  if (value == NULL || strlcpy(cfg->token, value, sizeof(cfg->token)) >=
      sizeof(cfg->token))
    {
      ret = -EINVAL;
      goto out;
    }

  value = trmnl_json_string(json, "TrmnlApiUrl");
  strlcpy(cfg->api_url, value != NULL ? value :
          CONFIG_SYSTEM_TRMNL_DEFAULT_API_URL, sizeof(cfg->api_url));

  value = trmnl_json_string(json, "ImageFormat");
  strlcpy(cfg->image_format, value != NULL ? value : "png",
          sizeof(cfg->image_format));

out:
  cJSON_Delete(json);
  return ret;
}

static int trmnl_save_config(const struct trmnl_config *cfg)
{
  char path[TRMNL_PATH_LEN];
  char id[TRMNL_ID_LEN * 6];
  char token[TRMNL_TOKEN_LEN * 6];
  char api[TRMNL_API_LEN * 6];
  char image[32];
  char body[1800];
  int len;

  if (trmnl_json_escape(id, sizeof(id), cfg->id) < 0 ||
      trmnl_json_escape(token, sizeof(token), cfg->token) < 0 ||
      trmnl_json_escape(api, sizeof(api), cfg->api_url) < 0 ||
      trmnl_json_escape(image, sizeof(image), cfg->image_format) < 0)
    {
      return -ENOSPC;
    }

  len = snprintf(body, sizeof(body),
                 "{\n"
                 "  \"TrmnlId\": \"%s\",\n"
                 "  \"TrmnlToken\": \"%s\",\n"
                 "  \"TrmnlApiUrl\": \"%s\",\n"
                 "  \"ImageFormat\": \"%s\"\n"
                 "}\n",
                 id, token, api, image);
  if (len < 0 || (size_t)len >= sizeof(body))
    {
      return -ENOSPC;
    }

  trmnl_path(path, sizeof(path), cfg->root, "device.json");
  return trmnl_write_file_atomic(path, body, len);
}

static int trmnl_save_state(const struct trmnl_config *cfg,
                            const struct trmnl_state *state)
{
  char path[TRMNL_PATH_LEN];
  char filename[TRMNL_FILENAME_LEN * 2];
  char error[sizeof(state->error) * 2];
  char body[512];
  int len;

  if (trmnl_json_escape(filename, sizeof(filename), state->filename) < 0 ||
      trmnl_json_escape(error, sizeof(error), state->error) < 0)
    {
      return -ENOSPC;
    }

  len = snprintf(body, sizeof(body),
                 "{\n"
                 "  \"last_ms\": %" PRIu64 ",\n"
                 "  \"http_status\": %ld,\n"
                 "  \"refresh_rate\": %" PRIu32 ",\n"
                 "  \"image_cached\": %s,\n"
                 "  \"filename\": \"%s\",\n"
                 "  \"last_error\": \"%s\"\n"
                 "}\n",
                 trmnl_now_ms(), state->http_status, state->refresh_rate,
                 state->image_cached ? "true" : "false", filename, error);

  if (len < 0 || (size_t)len >= sizeof(body))
    {
      return -ENOSPC;
    }

  trmnl_path(path, sizeof(path), cfg->root, "state.json");
  return trmnl_write_file(path, body, len);
}

#if defined(CONFIG_SYSTEM_TRMNL_ENABLE_TLS) && defined(CONFIG_CRYPTO_MBEDTLS)
static int trmnl_tls_entropy(void *data, unsigned char *output, size_t len)
{
  struct timespec ts;
  uintptr_t mix[4];
  size_t i;

  arc4random_buf(output, len);
  clock_gettime(CLOCK_REALTIME, &ts);

  mix[0] = (uintptr_t)data;
  mix[1] = (uintptr_t)output;
  mix[2] = (uintptr_t)ts.tv_sec ^ ((uintptr_t)ts.tv_nsec << 8);
  mix[3] = (uintptr_t)trmnl_now_ms();

  for (i = 0; i < len; i++)
    {
      output[i] ^= ((unsigned char *)mix)[i % sizeof(mix)];
    }

  return 0;
}

static int trmnl_tls_connect(void *ctx, const char *hostname,
                             const char *port, unsigned int timeout_second,
                             struct webclient_tls_connection **connp)
{
  struct trmnl_tls_connection *conn;
  unsigned int timeout_ms;
  uint64_t deadline_ms;
  struct timeval tv;
  uint32_t poll_timeout;
  uint64_t now_ms;
  const char *version;
  const char *cipher;
  int last_state = -1;
  int state;
  int ret;

  (void)ctx;

  conn = calloc(1, sizeof(*conn));
  if (conn == NULL)
    {
      return -ENOMEM;
    }

  fprintf(stderr, "trmnl: tls begin host=%s port=%s timeout=%u\n",
          hostname, port, timeout_second);

  mbedtls_net_init(&conn->net);
  mbedtls_ssl_init(&conn->ssl);
  mbedtls_ssl_config_init(&conn->conf);
  mbedtls_x509_crt_init(&conn->ca);
  mbedtls_entropy_init(&conn->entropy);
  mbedtls_ctr_drbg_init(&conn->ctr_drbg);

  ret = mbedtls_ctr_drbg_seed(&conn->ctr_drbg, mbedtls_entropy_func,
                              &conn->entropy,
                              (const unsigned char *)"trmnl", 5);
  if (ret != 0)
    {
      fprintf(stderr, "trmnl: tls platform seed failed host=%s ret=%d\n",
              hostname, ret);
      ret = mbedtls_ctr_drbg_seed(&conn->ctr_drbg, trmnl_tls_entropy, conn,
                                  (const unsigned char *)"trmnl", 5);
      if (ret != 0)
        {
          fprintf(stderr, "trmnl: tls fallback seed failed host=%s ret=%d\n",
                  hostname, ret);
          goto fail;
        }
    }

  ret = mbedtls_ssl_config_defaults(&conn->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0)
    {
      fprintf(stderr, "trmnl: tls config failed host=%s ret=%d\n",
              hostname, ret);
      goto fail;
    }

#if defined(MBEDTLS_SSL_SESSION_TICKETS) && defined(MBEDTLS_SSL_CLI_C)
  mbedtls_ssl_conf_session_tickets(&conn->conf,
                                   MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
#endif

#ifdef CONFIG_MBEDTLS_DEBUG_C
  mbedtls_ssl_conf_dbg(&conn->conf, trmnl_tls_debug, NULL);
#endif

#ifdef MBEDTLS_SSL_PROTO_TLS1_2
  mbedtls_ssl_conf_min_tls_version(&conn->conf,
                                   MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_max_tls_version(&conn->conf,
                                   MBEDTLS_SSL_VERSION_TLS1_2);
#endif

  mbedtls_ssl_conf_ciphersuites(&conn->conf, g_trmnl_tls12_ciphersuites);
  mbedtls_ssl_conf_groups(&conn->conf, g_trmnl_tls_groups);

#ifdef CONFIG_SYSTEM_TRMNL_TLS_ALLOW_UNVERIFIED
  mbedtls_ssl_conf_authmode(&conn->conf, MBEDTLS_SSL_VERIFY_NONE);
#else
  ret = mbedtls_x509_crt_parse_file(&conn->ca,
                                    CONFIG_SYSTEM_TRMNL_TLS_CA_CERT_PATH);
  if (ret != 0)
    {
      fprintf(stderr, "trmnl: tls ca failed host=%s ret=%d\n",
              hostname, ret);
      ret = -ENOENT;
      goto fail;
    }

  mbedtls_ssl_conf_ca_chain(&conn->conf, &conn->ca, NULL);
  mbedtls_ssl_conf_authmode(&conn->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
#endif

  mbedtls_ssl_conf_rng(&conn->conf, mbedtls_ctr_drbg_random,
                       &conn->ctr_drbg);

  timeout_ms = trmnl_tls_timeout_ms(timeout_second);
  mbedtls_ssl_conf_read_timeout(&conn->conf, timeout_ms);

  fprintf(stderr, "trmnl: tls tcp connect host=%s\n", hostname);
  ret = mbedtls_net_connect(&conn->net, hostname, port,
                            MBEDTLS_NET_PROTO_TCP);
  if (ret != 0)
    {
      fprintf(stderr, "trmnl: tls tcp failed host=%s ret=%d\n",
              hostname, ret);
      goto fail;
    }

  fprintf(stderr, "trmnl: tls tcp ok host=%s fd=%d\n",
          hostname, conn->net.fd);

  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  ret = setsockopt(conn->net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if (ret < 0)
    {
      fprintf(stderr, "trmnl: tls sndtimeo failed errno=%d\n", errno);
    }

  ret = setsockopt(conn->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  if (ret < 0)
    {
      fprintf(stderr, "trmnl: tls rcvtimeo failed errno=%d\n", errno);
    }

  ret = mbedtls_ssl_setup(&conn->ssl, &conn->conf);
  if (ret != 0)
    {
      fprintf(stderr, "trmnl: tls setup failed host=%s ret=%d\n",
              hostname, ret);
      goto fail;
    }

  ret = mbedtls_ssl_set_hostname(&conn->ssl, hostname);
  if (ret != 0)
    {
      fprintf(stderr, "trmnl: tls hostname failed host=%s ret=%d\n",
              hostname, ret);
      goto fail;
    }

  mbedtls_ssl_set_bio(&conn->ssl, &conn->net, mbedtls_net_send,
                      mbedtls_net_recv, mbedtls_net_recv_timeout);

  fprintf(stderr, "trmnl: tls handshake start host=%s\n", hostname);
  deadline_ms = trmnl_now_ms() + timeout_ms;
  for (; ; )
    {
      state = conn->ssl.MBEDTLS_PRIVATE(state);
      if (state != last_state)
        {
          fprintf(stderr, "trmnl: tls state %d %s\n", state,
                  trmnl_tls_state_name(state));
          last_state = state;
        }

      ret = mbedtls_ssl_handshake(&conn->ssl);

      state = conn->ssl.MBEDTLS_PRIVATE(state);
      if (state != last_state)
        {
          fprintf(stderr, "trmnl: tls state %d %s\n", state,
                  trmnl_tls_state_name(state));
          last_state = state;
        }

      if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
          ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          break;
        }

      if (trmnl_tls_deadline_expired(deadline_ms))
        {
          ret = -ETIMEDOUT;
          break;
        }

      now_ms = trmnl_now_ms();
      poll_timeout = (uint32_t)(deadline_ms - now_ms);
      ret = mbedtls_net_poll(&conn->net,
                             ret == MBEDTLS_ERR_SSL_WANT_READ ?
                             MBEDTLS_NET_POLL_READ :
                             MBEDTLS_NET_POLL_WRITE,
                             poll_timeout);
      if (ret < 0)
        {
          fprintf(stderr, "trmnl: tls poll failed host=%s ret=%d\n",
                  hostname, ret);
          goto fail;
        }
    }

  if (ret != 0)
    {
      char errbuf[96];

      state = conn->ssl.MBEDTLS_PRIVATE(state);
      version = mbedtls_ssl_get_version(&conn->ssl);
      cipher = mbedtls_ssl_get_ciphersuite(&conn->ssl);
      fprintf(stderr,
              "trmnl: tls handshake failed host=%s ret=%d (%s) "
              "state=%d %s version=%s cipher=%s\n",
              hostname, ret, trmnl_tls_error(ret, errbuf, sizeof(errbuf)),
              state, trmnl_tls_state_name(state),
              version != NULL ? version : "(none)",
              cipher != NULL ? cipher : "(none)");
      goto fail;
    }

  fprintf(stderr, "trmnl: tls handshake ok host=%s\n", hostname);

#ifndef CONFIG_SYSTEM_TRMNL_TLS_ALLOW_UNVERIFIED
  if (mbedtls_ssl_get_verify_result(&conn->ssl) != 0)
    {
      ret = -EACCES;
      goto fail;
    }
#endif

  *connp = (struct webclient_tls_connection *)conn;
  return 0;

fail:
  mbedtls_net_free(&conn->net);
  mbedtls_ssl_free(&conn->ssl);
  mbedtls_ssl_config_free(&conn->conf);
  mbedtls_x509_crt_free(&conn->ca);
  mbedtls_ctr_drbg_free(&conn->ctr_drbg);
  mbedtls_entropy_free(&conn->entropy);
  free(conn);
  return ret < 0 ? ret : -EIO;
}

static ssize_t trmnl_tls_send(void *ctx, struct webclient_tls_connection *c,
                              const void *buf, size_t len)
{
  struct trmnl_tls_connection *conn = (struct trmnl_tls_connection *)c;
  uint64_t deadline_ms;
  int ret;

  (void)ctx;
  deadline_ms = trmnl_now_ms() +
                (CONFIG_SYSTEM_TRMNL_HTTP_TIMEOUT_SEC * 1000ull);
  do
    {
      ret = mbedtls_ssl_write(&conn->ssl, buf, len);
      if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
          ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          if (trmnl_tls_deadline_expired(deadline_ms))
            {
              return -ETIMEDOUT;
            }

          usleep(10000);
        }
    }
  while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
         ret == MBEDTLS_ERR_SSL_WANT_WRITE);

  return ret < 0 ? -EIO : ret;
}

static ssize_t trmnl_tls_recv(void *ctx, struct webclient_tls_connection *c,
                              void *buf, size_t len)
{
  struct trmnl_tls_connection *conn = (struct trmnl_tls_connection *)c;
  uint64_t deadline_ms;
  int ret;

  (void)ctx;
  deadline_ms = trmnl_now_ms() +
                (CONFIG_SYSTEM_TRMNL_HTTP_TIMEOUT_SEC * 1000ull);
  do
    {
      ret = mbedtls_ssl_read(&conn->ssl, buf, len);
      if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
          ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          if (trmnl_tls_deadline_expired(deadline_ms))
            {
              return -ETIMEDOUT;
            }

          usleep(10000);
        }
    }
  while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
         ret == MBEDTLS_ERR_SSL_WANT_WRITE);

  if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
    {
      return 0;
    }

  return ret < 0 ? -EIO : ret;
}

static int trmnl_tls_close(void *ctx, struct webclient_tls_connection *c)
{
  struct trmnl_tls_connection *conn = (struct trmnl_tls_connection *)c;

  (void)ctx;
  if (conn == NULL)
    {
      return 0;
    }

  mbedtls_ssl_close_notify(&conn->ssl);
  mbedtls_net_free(&conn->net);
  mbedtls_ssl_free(&conn->ssl);
  mbedtls_ssl_config_free(&conn->conf);
  mbedtls_x509_crt_free(&conn->ca);
  mbedtls_ctr_drbg_free(&conn->ctr_drbg);
  mbedtls_entropy_free(&conn->entropy);
  free(conn);
  return 0;
}

static int trmnl_tls_get_poll_info(void *ctx,
                                   struct webclient_tls_connection *c,
                                   struct webclient_poll_info *info)
{
  struct trmnl_tls_connection *conn = (struct trmnl_tls_connection *)c;

  (void)ctx;
  if (conn == NULL || info == NULL)
    {
      return -EINVAL;
    }

  info->fd = conn->net.fd;
  info->flags = WEBCLIENT_POLL_INFO_WANT_READ |
                WEBCLIENT_POLL_INFO_WANT_WRITE;
  return 0;
}

static const struct webclient_tls_ops g_trmnl_tls_ops =
{
  .connect = trmnl_tls_connect,
  .send = trmnl_tls_send,
  .recv = trmnl_tls_recv,
  .close = trmnl_tls_close,
  .get_poll_info = trmnl_tls_get_poll_info,
  .init_connection = NULL,
};
#endif

static int trmnl_http_sink(char **buffer, int offset, int datend,
                           int *buflen, void *arg)
{
  struct trmnl_http_buf *out = arg;
  int len = datend - offset;

  (void)buflen;
  if (len <= 0)
    {
      return 0;
    }

  if (out->len + len > out->cap)
    {
      return -ENOSPC;
    }

  memcpy(out->data + out->len, &(*buffer)[offset], len);
  out->len += len;
  return 0;
}

static void trmnl_headers_add(struct trmnl_headers *headers,
                              const char *name, const char *value)
{
  if (headers->count < TRMNL_HEADER_MAX)
    {
      headers->items[headers->count].name = name;
      headers->items[headers->count].value = value;
      headers->count++;
    }
}

static int trmnl_http_get(const char *url, const struct trmnl_headers *headers,
                          uint8_t *response, size_t response_cap,
                          size_t *response_len, long *status_code)
{
  struct webclient_context ctx;
  struct trmnl_http_buf out;
  char iobuf[768];
  char reason[64];
  char header_lines[TRMNL_HEADER_MAX][192];
  const char *header_ptrs[TRMNL_HEADER_MAX];
  unsigned int i;
  int ret;

  if (strncmp(url, "https://", 8) == 0)
    {
#if !defined(CONFIG_SYSTEM_TRMNL_ENABLE_TLS) || !defined(CONFIG_CRYPTO_MBEDTLS)
      return -ENOTSUP;
#endif
    }

  ret = pthread_mutex_lock(&g_http_lock);
  if (ret != 0)
    {
      return -ret;
    }

  memset(&ctx, 0, sizeof(ctx));
  webclient_set_defaults(&ctx);
  reason[0] = '\0';

  out.data = response;
  out.len = 0;
  out.cap = response_cap;

  ctx.method = "GET";
  ctx.url = url;
  ctx.buffer = iobuf;
  ctx.buflen = sizeof(iobuf);
  ctx.sink_callback = trmnl_http_sink;
  ctx.sink_callback_arg = &out;
  ctx.http_reason = reason;
  ctx.http_reason_len = sizeof(reason);
  ctx.timeout_sec = CONFIG_SYSTEM_TRMNL_HTTP_TIMEOUT_SEC;
  ctx.protocol_version = WEBCLIENT_PROTOCOL_VERSION_HTTP_1_1;

#if defined(CONFIG_SYSTEM_TRMNL_ENABLE_TLS) && defined(CONFIG_CRYPTO_MBEDTLS)
  if (strncmp(url, "https://", 8) == 0)
    {
      ctx.tls_ops = &g_trmnl_tls_ops;
    }
#endif

  if (headers != NULL)
    {
      for (i = 0; i < headers->count && i < TRMNL_HEADER_MAX; i++)
        {
          snprintf(header_lines[i], sizeof(header_lines[i]), "%s: %s",
                   headers->items[i].name, headers->items[i].value);
          header_ptrs[i] = header_lines[i];
        }

      ctx.headers = header_ptrs;
      ctx.nheaders = i;
    }

  ret = webclient_perform(&ctx);
  if (status_code != NULL)
    {
      *status_code = ctx.http_status;
    }

  if (response_len != NULL)
    {
      *response_len = out.len;
    }

  pthread_mutex_unlock(&g_http_lock);

  if (ret < 0)
    {
      fprintf(stderr, "trmnl: http get failed ret=%d http=%u\n",
              ret, ctx.http_status);
      return ret;
    }

  if (ctx.http_status >= 400)
    {
      fprintf(stderr, "trmnl: http status %u %s\n",
              ctx.http_status, reason);
      return -EIO;
    }

  return 0;
}

static void trmnl_display_headers(const struct trmnl_config *cfg,
                                  const struct trmnl_state *state,
                                  struct trmnl_headers *headers)
{
  static char refresh[16];

  memset(headers, 0, sizeof(*headers));
  snprintf(refresh, sizeof(refresh), "%" PRIu32,
           state->refresh_rate > 0 ? state->refresh_rate :
           CONFIG_SYSTEM_TRMNL_DEFAULT_REFRESH_SEC);
  trmnl_headers_add(headers, "ID", cfg->id);
  trmnl_headers_add(headers, "Access-Token", cfg->token);
  trmnl_headers_add(headers, "Content-Type", "application/json");
  trmnl_headers_add(headers, "Refresh-Rate", refresh);
  trmnl_headers_add(headers, "FW-Version", CONFIG_SYSTEM_TRMNL_FW_VERSION);
  trmnl_headers_add(headers, "Model", CONFIG_SYSTEM_TRMNL_MODEL);
  trmnl_headers_add(headers, "Width", "800");
  trmnl_headers_add(headers, "Height", "480");
  trmnl_headers_add(headers, "Image-Cached",
                    state->image_cached ? "true" : "false");
  trmnl_headers_add(headers, "Update-Source", "scheduled");
}

static void trmnl_image_headers(const struct trmnl_config *cfg,
                                struct trmnl_headers *headers)
{
  memset(headers, 0, sizeof(*headers));
  trmnl_headers_add(headers, "ID", cfg->id);
  trmnl_headers_add(headers, "Access-Token", cfg->token);
}

static uint32_t trmnl_json_u32(cJSON *root, const char *key)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

  if (cJSON_IsNumber(item))
    {
      return (uint32_t)item->valuedouble;
    }

  if (cJSON_IsString(item) && item->valuestring != NULL)
    {
      return (uint32_t)strtoul(item->valuestring, NULL, 10);
    }

  return 0;
}

static int trmnl_load_compiled_config(struct trmnl_config *cfg)
{
  if (CONFIG_SYSTEM_TRMNL_DEFAULT_ID[0] == '\0' ||
      CONFIG_SYSTEM_TRMNL_DEFAULT_TOKEN[0] == '\0')
    {
      return -ENOENT;
    }

  if (strlcpy(cfg->id, CONFIG_SYSTEM_TRMNL_DEFAULT_ID, sizeof(cfg->id)) >=
      sizeof(cfg->id) ||
      strlcpy(cfg->token, CONFIG_SYSTEM_TRMNL_DEFAULT_TOKEN,
              sizeof(cfg->token)) >= sizeof(cfg->token) ||
      strlcpy(cfg->api_url, CONFIG_SYSTEM_TRMNL_DEFAULT_API_URL,
              sizeof(cfg->api_url)) >= sizeof(cfg->api_url))
    {
      return -ENAMETOOLONG;
    }

  strlcpy(cfg->image_format, "png", sizeof(cfg->image_format));
  return 0;
}

static int trmnl_parse_display(const char *json_text,
                               struct trmnl_display_response *resp)
{
  cJSON *json;
  const char *value;
  int ret = 0;

  memset(resp, 0, sizeof(*resp));
  json = cJSON_Parse(json_text);
  if (json == NULL)
    {
      return -EINVAL;
    }

  resp->status = (int)trmnl_json_u32(json, "status");
  value = trmnl_json_string(json, "image_url");
  if (value != NULL && strlcpy(resp->image_url, value,
      sizeof(resp->image_url)) >= sizeof(resp->image_url))
    {
      ret = -ENOSPC;
      goto out;
    }

  value = trmnl_json_string(json, "filename");
  if (value != NULL)
    {
      strlcpy(resp->filename, value, sizeof(resp->filename));
    }

  resp->refresh_rate = trmnl_json_u32(json, "refresh_rate");

out:
  cJSON_Delete(json);
  return ret;
}

static int trmnl_fetch_display(const struct trmnl_config *cfg,
                               const struct trmnl_state *state,
                               struct trmnl_display_response *resp,
                               long *http_status)
{
  struct trmnl_headers headers;
  uint8_t *body;
  size_t len = 0;
  char url[TRMNL_API_LEN + 16];
  int ret;

  body = malloc(CONFIG_SYSTEM_TRMNL_MAX_JSON + 1);
  if (body == NULL)
    {
      return -ENOMEM;
    }

  snprintf(url, sizeof(url), "%s/display", cfg->api_url);
  trmnl_display_headers(cfg, state, &headers);
  ret = trmnl_http_get(url, &headers, body, CONFIG_SYSTEM_TRMNL_MAX_JSON,
                       &len, http_status);
  if (ret < 0)
    {
      if (len > 0)
        {
          body[len < 160 ? len : 160] = '\0';
          fprintf(stderr, "trmnl: display http body: %s\n", body);
        }

      free(body);
      return ret;
    }

  body[len] = '\0';
  ret = trmnl_parse_display((char *)body, resp);
  if (ret < 0)
    {
      body[len < 160 ? len : 160] = '\0';
      fprintf(stderr, "trmnl: display json parse failed len=%zu body=%s\n",
              len, body);
    }

  free(body);
  return ret;
}

static int trmnl_fetch_image(const struct trmnl_config *cfg, const char *url,
                             uint8_t **out, size_t *out_len,
                             long *http_status)
{
  struct trmnl_headers headers;
  uint8_t *body;
  size_t len = 0;
  int ret;

  body = malloc(CONFIG_SYSTEM_TRMNL_MAX_PNG);
  if (body == NULL)
    {
      return -ENOMEM;
    }

  trmnl_image_headers(cfg, &headers);
  ret = trmnl_http_get(url, &headers, body, CONFIG_SYSTEM_TRMNL_MAX_PNG,
                       &len, http_status);
  if (ret < 0)
    {
      free(body);
      return ret;
    }

  *out = body;
  *out_len = len;
  return 0;
}

static uint32_t trmnl_be32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

static unsigned int trmnl_png_channels(unsigned int color_type)
{
  switch (color_type)
    {
      case 0:
      case 3:
        return 1;
      case 2:
        return 3;
      case 4:
        return 2;
      case 6:
        return 4;
      default:
        return 0;
    }
}

static unsigned int trmnl_png_get_bits(const uint8_t *row, unsigned int x,
                                       unsigned int bits)
{
  unsigned int bit = x * bits;
  unsigned int byte = bit >> 3;
  unsigned int shift = 8 - bits - (bit & 7);

  return (row[byte] >> shift) & ((1u << bits) - 1);
}

static uint8_t trmnl_scale_sample(unsigned int value, unsigned int bits)
{
  if (bits == 8)
    {
      return value;
    }

  return (uint8_t)((value * 255u) / ((1u << bits) - 1u));
}

static int trmnl_png_unfilter(uint8_t *raw, const uint8_t *filtered,
                              unsigned int width, unsigned int height,
                              unsigned int stride, unsigned int bpp)
{
  unsigned int y;
  unsigned int x;

  for (y = 0; y < height; y++)
    {
      const uint8_t *src = filtered + y * (stride + 1);
      uint8_t *dst = raw + y * stride;
      const uint8_t *prev = y > 0 ? raw + (y - 1) * stride : NULL;
      unsigned int filter = src[0];

      src++;
      for (x = 0; x < stride; x++)
        {
          uint8_t a = x >= bpp ? dst[x - bpp] : 0;
          uint8_t b = prev != NULL ? prev[x] : 0;
          uint8_t c = (prev != NULL && x >= bpp) ? prev[x - bpp] : 0;
          int p;
          int pa;
          int pb;
          int pc;

          switch (filter)
            {
              case 0:
                dst[x] = src[x];
                break;
              case 1:
                dst[x] = src[x] + a;
                break;
              case 2:
                dst[x] = src[x] + b;
                break;
              case 3:
                dst[x] = src[x] + ((uint16_t)a + b) / 2;
                break;
              case 4:
                p = (int)a + (int)b - (int)c;
                pa = abs(p - (int)a);
                pb = abs(p - (int)b);
                pc = abs(p - (int)c);
                dst[x] = src[x] + (pa <= pb && pa <= pc ? a :
                         (pb <= pc ? b : c));
                break;
              default:
                return -EINVAL;
            }
        }
    }

  return 0;
}

static int trmnl_png_decode_gray(const uint8_t *png, size_t png_len,
                                 struct trmnl_png *out)
{
  static const uint8_t sig[8] =
  {
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
  };

  uint8_t palette[256][3];
  uint8_t *idat = NULL;
  uint8_t *filtered = NULL;
  uint8_t *raw = NULL;
  uint8_t *gray = NULL;
  size_t idat_len = 0;
  size_t idat_cap = 0;
  size_t pos = 8;
  unsigned int width = 0;
  unsigned int height = 0;
  unsigned int bit_depth = 0;
  unsigned int color_type = 0;
  unsigned int channels;
  unsigned int bitspp;
  unsigned int stride;
  unsigned int bpp_filter;
  unsigned int x;
  unsigned int y;
  int palette_entries = 0;
  int ret = 0;
  uLongf filtered_len;

  memset(out, 0, sizeof(*out));
  memset(palette, 0, sizeof(palette));

  if (png_len < 33 || memcmp(png, sig, sizeof(sig)) != 0)
    {
      return -EINVAL;
    }

  while (pos + 12 <= png_len)
    {
      uint32_t len = trmnl_be32(&png[pos]);
      const uint8_t *type = &png[pos + 4];
      const uint8_t *data = &png[pos + 8];

      if (pos + 12u + len > png_len)
        {
          ret = -EINVAL;
          goto out;
        }

      if (memcmp(type, "IHDR", 4) == 0)
        {
          if (len != 13)
            {
              ret = -EINVAL;
              goto out;
            }

          width = trmnl_be32(&data[0]);
          height = trmnl_be32(&data[4]);
          bit_depth = data[8];
          color_type = data[9];
          if (data[10] != 0 || data[11] != 0 || data[12] != 0)
            {
              ret = -ENOTSUP;
              goto out;
            }
        }
      else if (memcmp(type, "PLTE", 4) == 0)
        {
          unsigned int i;

          if (len % 3 != 0 || len / 3 > 256)
            {
              ret = -EINVAL;
              goto out;
            }

          palette_entries = len / 3;
          for (i = 0; i < (unsigned int)palette_entries; i++)
            {
              palette[i][0] = data[i * 3];
              palette[i][1] = data[i * 3 + 1];
              palette[i][2] = data[i * 3 + 2];
            }
        }
      else if (memcmp(type, "IDAT", 4) == 0)
        {
          uint8_t *tmp;

          if (idat_len + len > CONFIG_SYSTEM_TRMNL_MAX_PNG)
            {
              ret = -ENOSPC;
              goto out;
            }

          if (idat_len + len > idat_cap)
            {
              idat_cap = idat_len + len + 4096;
              tmp = realloc(idat, idat_cap);
              if (tmp == NULL)
                {
                  ret = -ENOMEM;
                  goto out;
                }

              idat = tmp;
            }

          memcpy(idat + idat_len, data, len);
          idat_len += len;
        }
      else if (memcmp(type, "IEND", 4) == 0)
        {
          break;
        }

      pos += 12u + len;
    }

  channels = trmnl_png_channels(color_type);
  if (width == 0 || height == 0 || channels == 0 || idat_len == 0)
    {
      ret = -EINVAL;
      goto out;
    }

  if (color_type == 2 || color_type == 4 || color_type == 6)
    {
      if (bit_depth != 8)
        {
          ret = -ENOTSUP;
          goto out;
        }
    }
  else if (!(bit_depth == 1 || bit_depth == 2 || bit_depth == 4 ||
             bit_depth == 8))
    {
      ret = -ENOTSUP;
      goto out;
    }

  if (color_type == 3 && palette_entries == 0)
    {
      ret = -EINVAL;
      goto out;
    }

  bitspp = channels * bit_depth;
  stride = (width * bitspp + 7) / 8;
  bpp_filter = bitspp >= 8 ? bitspp / 8 : 1;
  filtered_len = (uLongf)(height * (stride + 1));

  filtered = malloc(filtered_len);
  raw = malloc(height * stride);
  gray = malloc(width * height);
  if (filtered == NULL || raw == NULL || gray == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  ret = uncompress(filtered, &filtered_len, idat, idat_len);
  if (ret != Z_OK || filtered_len != height * (stride + 1))
    {
      ret = -EINVAL;
      goto out;
    }

  ret = trmnl_png_unfilter(raw, filtered, width, height, stride, bpp_filter);
  if (ret < 0)
    {
      goto out;
    }

  for (y = 0; y < height; y++)
    {
      const uint8_t *row = raw + y * stride;

      for (x = 0; x < width; x++)
        {
          uint8_t value;

          if (color_type == 0)
            {
              value = trmnl_scale_sample(
                bit_depth == 8 ? row[x] :
                trmnl_png_get_bits(row, x, bit_depth), bit_depth);
            }
          else if (color_type == 3)
            {
              unsigned int idx = bit_depth == 8 ? row[x] :
                                 trmnl_png_get_bits(row, x, bit_depth);
              uint32_t r;
              uint32_t g;
              uint32_t b;

              if (idx >= (unsigned int)palette_entries)
                {
                  ret = -EINVAL;
                  goto out;
                }

              r = palette[idx][0];
              g = palette[idx][1];
              b = palette[idx][2];
              value = (uint8_t)((r * 30 + g * 59 + b * 11) / 100);
            }
          else if (color_type == 2)
            {
              const uint8_t *p = row + x * 3;

              value = (uint8_t)((p[0] * 30u + p[1] * 59u + p[2] * 11u) /
                                100u);
            }
          else if (color_type == 4)
            {
              const uint8_t *p = row + x * 2;

              value = (uint8_t)((p[0] * p[1] + 255u * (255u - p[1])) /
                                255u);
            }
          else
            {
              const uint8_t *p = row + x * 4;
              uint32_t lum = (p[0] * 30u + p[1] * 59u + p[2] * 11u) / 100u;

              value = (uint8_t)((lum * p[3] + 255u * (255u - p[3])) /
                                255u);
            }

          gray[y * width + x] = value;
        }
    }

  out->width = width;
  out->height = height;
  out->gray = gray;
  gray = NULL;
  ret = 0;

out:
  free(idat);
  free(filtered);
  free(raw);
  free(gray);
  return ret;
}

static void trmnl_pack_y2(uint8_t *dst, const uint8_t *gray)
{
  unsigned int x;
  unsigned int y;

  memset(dst, 0, TRMNL_Y2_BYTES);
  for (y = 0; y < TRMNL_HEIGHT; y++)
    {
      for (x = 0; x < TRMNL_WIDTH; x++)
        {
          uint8_t level = (uint8_t)((gray[y * TRMNL_WIDTH + x] * 3u + 127u) /
                                    255u);
          unsigned int shift = (3 - (x & 3)) * 2;

          dst[y * TRMNL_Y2_STRIDE + (x >> 2)] |= level << shift;
        }
    }
}

static int trmnl_open_fb(int *fd_out, struct fb_videoinfo_s *vinfo,
                         struct fb_planeinfo_s *pinfo)
{
  int fd;

  fd = open(CONFIG_SYSTEM_TRMNL_FB_DEVPATH, O_RDWR);
  if (fd < 0)
    {
      return -errno;
    }

  if (ioctl(fd, FBIOGET_VIDEOINFO, (unsigned long)vinfo) < 0 ||
      ioctl(fd, FBIOGET_PLANEINFO, (unsigned long)pinfo) < 0)
    {
      int ret = -errno;
      close(fd);
      return ret;
    }

  if (vinfo->xres != TRMNL_WIDTH || vinfo->yres != TRMNL_HEIGHT ||
      vinfo->fmt != FB_FMT_Y2 || pinfo->bpp != 2 ||
      pinfo->stride < TRMNL_Y2_STRIDE || pinfo->fblen < TRMNL_Y2_BYTES)
    {
      close(fd);
      return -ENOTSUP;
    }

  *fd_out = fd;
  return 0;
}

static int trmnl_render_gray(const uint8_t *gray)
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  uint8_t *packed;
  int fd = -1;
  int ret;

  ret = trmnl_open_fb(&fd, &vinfo, &pinfo);
  if (ret < 0)
    {
      return ret;
    }

  packed = malloc(TRMNL_Y2_BYTES);
  if (packed == NULL)
    {
      close(fd);
      return -ENOMEM;
    }

  trmnl_pack_y2(packed, gray);
  if (pinfo.stride == TRMNL_Y2_STRIDE)
    {
      ssize_t nwritten;

      if (lseek(fd, 0, SEEK_SET) < 0)
        {
          ret = -errno;
          goto out;
        }

      nwritten = write(fd, packed, TRMNL_Y2_BYTES);
      if (nwritten < 0)
        {
          ret = -errno;
          goto out;
        }

      if (nwritten != TRMNL_Y2_BYTES)
        {
          ret = -EIO;
          goto out;
        }
    }
  else
    {
      unsigned int y;

      for (y = 0; y < TRMNL_HEIGHT; y++)
        {
          ssize_t nwritten;

          if (lseek(fd, y * pinfo.stride, SEEK_SET) < 0)
            {
              ret = -errno;
              goto out;
            }

          nwritten = write(fd, packed + y * TRMNL_Y2_STRIDE,
                           TRMNL_Y2_STRIDE);
          if (nwritten < 0)
            {
              ret = -errno;
              goto out;
            }

          if (nwritten != TRMNL_Y2_STRIDE)
            {
              ret = -EIO;
              goto out;
            }
        }
    }

#ifdef CONFIG_FB_UPDATE
  {
    struct fb_area_s area;

    area.x = 0;
    area.y = 0;
    area.w = TRMNL_WIDTH;
    area.h = TRMNL_HEIGHT;

    if (ioctl(fd, FBIO_UPDATE, (uintptr_t)&area) < 0)
      {
        ret = -errno;
        goto out;
      }
  }
#endif

  ret = 0;

out:
  free(packed);
  close(fd);
  return ret;
}

static int trmnl_test_pattern(void)
{
  uint8_t *gray;
  unsigned int x;
  unsigned int y;
  int ret;

  gray = malloc(TRMNL_WIDTH * TRMNL_HEIGHT);
  if (gray == NULL)
    {
      return -ENOMEM;
    }

  for (y = 0; y < TRMNL_HEIGHT; y++)
    {
      for (x = 0; x < TRMNL_WIDTH; x++)
        {
          gray[y * TRMNL_WIDTH + x] =
            (uint8_t)(((x / 100) % 4) * 85u);
        }
    }

  ret = trmnl_render_gray(gray);
  free(gray);
  if (ret == 0)
    {
      printf("trmnl: test pattern displayed\n");
    }

  return ret;
}

static int trmnl_once(void)
{
  struct trmnl_config cfg;
  struct trmnl_state state;
  struct trmnl_display_response display;
  uint8_t *png = NULL;
  size_t png_len = 0;
  struct trmnl_png decoded;
  char curpath[TRMNL_PATH_LEN];
  char lastpath[TRMNL_PATH_LEN];
  long http_status = 0;
  bool guard_armed;
  int ret;

  guard_armed = trmnl_bootguard_arm("once");
  memset(&state, 0, sizeof(state));
  state.refresh_rate = CONFIG_SYSTEM_TRMNL_DEFAULT_REFRESH_SEC;

  ret = trmnl_load_config(&cfg);
  if (ret < 0)
    {
      fprintf(stderr, "trmnl: config load failed: %d\n", ret);
      trmnl_bootguard_disarm(guard_armed);
      return ret;
    }

  trmnl_log(cfg.root, "once begin");
  ret = trmnl_fetch_display(&cfg, &state, &display, &http_status);
  state.http_status = http_status;
  if (ret < 0)
    {
      snprintf(state.error, sizeof(state.error),
               "display fetch failed %d http=%ld", ret, http_status);
      goto fail;
    }

  if (display.image_url[0] == '\0')
    {
      ret = -ENOENT;
      snprintf(state.error, sizeof(state.error), "display response no image");
      goto fail;
    }

  state.refresh_rate = display.refresh_rate > 0 ? display.refresh_rate :
                       CONFIG_SYSTEM_TRMNL_DEFAULT_REFRESH_SEC;
  strlcpy(state.filename, display.filename, sizeof(state.filename));

  ret = trmnl_fetch_image(&cfg, display.image_url, &png, &png_len,
                          &http_status);
  state.http_status = http_status;
  if (ret < 0)
    {
      snprintf(state.error, sizeof(state.error),
               "image fetch failed %d http=%ld", ret, http_status);
      goto fail;
    }

  trmnl_path(lastpath, sizeof(lastpath), cfg.root, "last.png");
  trmnl_path(curpath, sizeof(curpath), cfg.root, "current.png");
  unlink(lastpath);
  rename(curpath, lastpath);
  ret = trmnl_write_file(curpath, png, png_len);
  if (ret < 0)
    {
      snprintf(state.error, sizeof(state.error), "cache write failed %d",
               ret);
      goto fail;
    }

  ret = trmnl_png_decode_gray(png, png_len, &decoded);
  if (ret < 0)
    {
      snprintf(state.error, sizeof(state.error), "png decode failed %d",
               ret);
      goto fail;
    }

  if (decoded.width != TRMNL_WIDTH || decoded.height != TRMNL_HEIGHT)
    {
      free(decoded.gray);
      ret = -EINVAL;
      snprintf(state.error, sizeof(state.error), "png size %ux%u",
               decoded.width, decoded.height);
      goto fail;
    }

  ret = trmnl_render_gray(decoded.gray);
  free(decoded.gray);
  if (ret < 0)
    {
      snprintf(state.error, sizeof(state.error), "render failed %d", ret);
      goto fail;
    }

  state.image_cached = true;
  state.error[0] = '\0';
  trmnl_save_state(&cfg, &state);
  trmnl_log(cfg.root, "once ok http=%ld refresh=%" PRIu32 " filename=%s",
            state.http_status, state.refresh_rate, state.filename);
  free(png);
  printf("trmnl: displayed %s refresh=%" PRIu32 "s\n",
         state.filename[0] != '\0' ? state.filename : "(unnamed)",
         state.refresh_rate);
  trmnl_bootguard_disarm(guard_armed);
  return 0;

fail:
  trmnl_save_state(&cfg, &state);
  trmnl_log(cfg.root, "once failed: %s", state.error);
  free(png);
  fprintf(stderr, "trmnl: %s\n", state.error);
  trmnl_bootguard_disarm(guard_armed);
  return ret;
}

static int trmnl_start(void)
{
  uint32_t delay_sec = CONFIG_SYSTEM_TRMNL_DEFAULT_REFRESH_SEC;

  for (; ; )
    {
      if (trmnl_once() == 0)
        {
          struct trmnl_config cfg;
          char path[TRMNL_PATH_LEN];
          char *text;

          if (trmnl_load_config(&cfg) == 0)
            {
              trmnl_path(path, sizeof(path), cfg.root, "state.json");
              text = trmnl_read_file(path, 1024, NULL);
              if (text != NULL)
                {
                  cJSON *json = cJSON_Parse(text);

                  if (json != NULL)
                    {
                      uint32_t parsed = trmnl_json_u32(json, "refresh_rate");

                      if (parsed > 0)
                        {
                          delay_sec = parsed;
                        }

                      cJSON_Delete(json);
                    }

                  free(text);
                }
            }
        }

      sleep(delay_sec > 0 ? delay_sec : CONFIG_SYSTEM_TRMNL_DEFAULT_REFRESH_SEC);
    }

  return 0;
}

static int trmnl_boot(void)
{
  int attempt;
  int delay_sec = CONFIG_SYSTEM_TRMNL_BOOT_WIFI_RETRY_DELAY;
  int ret;
  int retries = CONFIG_SYSTEM_TRMNL_BOOT_WIFI_RETRIES;

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
      ret = trmnl_wifi_up();

      if (ret >= 0)
        {
          break;
        }

      fprintf(stderr, "trmnl: boot wifi attempt %d/%d failed: %d\n",
              attempt, retries, ret);
      if (attempt < retries && delay_sec > 0)
        {
          sleep(delay_sec);
        }
    }

  if (ret < 0)
    {
      return ret;
    }

  return trmnl_start();
}

static int trmnl_config_cmd(void)
{
  struct trmnl_config cfg;
  int ret = trmnl_load_config(&cfg);

  if (ret < 0)
    {
      fprintf(stderr, "trmnl: config invalid: %d\n", ret);
      return ret;
    }

  printf("root: %s\n", cfg.root);
  printf("storage: %s\n", trmnl_root_kind(cfg.root));
  printf("id: %s\n", cfg.id);
  printf("api: %s\n", cfg.api_url);
  printf("image_format: %s\n", cfg.image_format);
  printf("token: configured\n");
  return 0;
}

static void trmnl_print_storage_warning(const char *root)
{
  if (trmnl_root_is_volatile(root))
    {
      printf("trmnl: warning: %s is volatile; mount SD or /flash for "
             "reset-persistent credentials\n", root);
    }
}

static int trmnl_config_for_edit(struct trmnl_config *cfg)
{
  int ret;

  ret = trmnl_load_config(cfg);
  if (ret == 0)
    {
      return 0;
    }

  memset(cfg, 0, sizeof(*cfg));
  ret = trmnl_select_root(cfg->root, sizeof(cfg->root));
  if (ret < 0)
    {
      return ret;
    }

  trmnl_load_compiled_config(cfg);
  if (cfg->api_url[0] == '\0')
    {
      strlcpy(cfg->api_url, CONFIG_SYSTEM_TRMNL_DEFAULT_API_URL,
              sizeof(cfg->api_url));
    }

  if (cfg->image_format[0] == '\0')
    {
      strlcpy(cfg->image_format, "png", sizeof(cfg->image_format));
    }

  return 0;
}

static int trmnl_validate_config_save(const struct trmnl_config *cfg)
{
  if (cfg->id[0] == '\0' || cfg->token[0] == '\0' ||
      cfg->api_url[0] == '\0' || cfg->image_format[0] == '\0')
    {
      fprintf(stderr, "trmnl: device config needs id, token, api, and "
              "image format\n");
      return -EINVAL;
    }

  if (strcmp(cfg->image_format, "png") != 0)
    {
      fprintf(stderr, "trmnl: only png image format is supported\n");
      return -EINVAL;
    }

  return 0;
}

static int trmnl_set_device(int argc, char *argv[])
{
  struct trmnl_config cfg;
  char path[TRMNL_PATH_LEN];
  int ret;

  if (argc < 4 || argc > 6)
    {
      fprintf(stderr, "Usage: %s set-device <id> <token> [api-url] "
              "[image-format]\n", argv[0]);
      return -EINVAL;
    }

  memset(&cfg, 0, sizeof(cfg));
  ret = trmnl_select_root(cfg.root, sizeof(cfg.root));
  if (ret < 0)
    {
      return ret;
    }

  if (strlcpy(cfg.id, argv[2], sizeof(cfg.id)) >= sizeof(cfg.id) ||
      strlcpy(cfg.token, argv[3], sizeof(cfg.token)) >= sizeof(cfg.token) ||
      strlcpy(cfg.api_url, argc >= 5 ? argv[4] :
              CONFIG_SYSTEM_TRMNL_DEFAULT_API_URL,
              sizeof(cfg.api_url)) >= sizeof(cfg.api_url) ||
      strlcpy(cfg.image_format, argc >= 6 ? argv[5] : "png",
              sizeof(cfg.image_format)) >= sizeof(cfg.image_format))
    {
      return -ENAMETOOLONG;
    }

  ret = trmnl_validate_config_save(&cfg);
  if (ret < 0)
    {
      return ret;
    }

  ret = trmnl_save_config(&cfg);
  if (ret < 0)
    {
      return ret;
    }

  trmnl_path(path, sizeof(path), cfg.root, "device.json");
  printf("trmnl: device config written to %s\n", path);
  trmnl_print_storage_warning(cfg.root);
  return 0;
}

static int trmnl_set_config_field(int argc, char *argv[], const char *field)
{
  struct trmnl_config cfg;
  char path[TRMNL_PATH_LEN];
  int ret;

  if (argc != 3)
    {
      fprintf(stderr, "Usage: %s set-%s <value>\n", argv[0], field);
      return -EINVAL;
    }

  ret = trmnl_config_for_edit(&cfg);
  if (ret < 0)
    {
      return ret;
    }

  if (strcmp(field, "id") == 0)
    {
      ret = strlcpy(cfg.id, argv[2], sizeof(cfg.id)) >= sizeof(cfg.id) ?
            -ENAMETOOLONG : 0;
    }
  else if (strcmp(field, "token") == 0)
    {
      ret = strlcpy(cfg.token, argv[2], sizeof(cfg.token)) >=
            sizeof(cfg.token) ? -ENAMETOOLONG : 0;
    }
  else if (strcmp(field, "api") == 0)
    {
      ret = strlcpy(cfg.api_url, argv[2], sizeof(cfg.api_url)) >=
            sizeof(cfg.api_url) ? -ENAMETOOLONG : 0;
    }
  else if (strcmp(field, "image-format") == 0)
    {
      ret = strlcpy(cfg.image_format, argv[2], sizeof(cfg.image_format)) >=
            sizeof(cfg.image_format) ? -ENAMETOOLONG : 0;
    }
  else
    {
      ret = -EINVAL;
    }

  if (ret < 0)
    {
      return ret;
    }

  ret = trmnl_validate_config_save(&cfg);
  if (ret < 0)
    {
      return ret;
    }

  ret = trmnl_save_config(&cfg);
  if (ret < 0)
    {
      return ret;
    }

  trmnl_path(path, sizeof(path), cfg.root, "device.json");
  printf("trmnl: %s updated in %s\n", field, path);
  trmnl_print_storage_warning(cfg.root);
  return 0;
}

static bool trmnl_config_text_ok(const char *value)
{
  size_t i;

  if (value == NULL || value[0] == '\0')
    {
      return false;
    }

  for (i = 0; value[i] != '\0'; i++)
    {
      unsigned char ch = (unsigned char)value[i];

      if (ch < 0x20 || ch == 0x7f)
        {
          return false;
        }
    }

  return true;
}

static int trmnl_set_wifi(int argc, char *argv[])
{
  char root[TRMNL_PATH_LEN];
  char path[TRMNL_PATH_LEN];
  char body[320];
  int key_mgmt = 3;
  int cipher = 2;
  int len;
  int ret;

  if (argc < 4 || argc > 6)
    {
      fprintf(stderr, "Usage: %s set-wifi <ssid> <password> [key_mgmt] "
              "[cipher]\n", argv[0]);
      return -EINVAL;
    }

  if (!trmnl_config_text_ok(argv[2]) || !trmnl_config_text_ok(argv[3]))
    {
      fprintf(stderr, "trmnl: ssid/password cannot be empty or contain "
              "control characters\n");
      return -EINVAL;
    }

  if (argc >= 5)
    {
      key_mgmt = atoi(argv[4]);
    }

  if (argc >= 6)
    {
      cipher = atoi(argv[5]);
    }

  ret = trmnl_select_root(root, sizeof(root));
  if (ret < 0)
    {
      return ret;
    }

  len = snprintf(body, sizeof(body),
                 "ssid=%s\n"
                 "password=%s\n"
                 "key_mgmt=%d\n"
                 "cipher=%d\n",
                 argv[2], argv[3], key_mgmt, cipher);
  if (len < 0 || (size_t)len >= sizeof(body))
    {
      return -ENOSPC;
    }

  trmnl_path(path, sizeof(path), root, CONFIG_SYSTEM_TRMNL_WIFI_CONFIG_LEAF);
  ret = trmnl_write_file_atomic(path, body, len);
  if (ret < 0)
    {
      return ret;
    }

  printf("trmnl: wifi config written to %s\n", path);
  trmnl_print_storage_warning(root);
  return 0;
}

static int trmnl_storage_cmd(void)
{
  char root[TRMNL_PATH_LEN];
  int ret;

  ret = trmnl_select_root(root, sizeof(root));
  if (ret < 0)
    {
      return ret;
    }

  printf("active: %s (%s)\n", root, trmnl_root_kind(root));
  printf("sd: %s\n", CONFIG_SYSTEM_TRMNL_SD_DATA_DIR);
  printf("flash: %s\n", CONFIG_SYSTEM_TRMNL_FLASH_DATA_DIR);
  printf("fallback: %s\n", CONFIG_SYSTEM_TRMNL_DATA_DIR);
  trmnl_print_storage_warning(root);
  return 0;
}

static int trmnl_status(void)
{
  struct trmnl_config cfg;
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  char path[TRMNL_PATH_LEN];
  char *state;
  int fd;
  int ret;

  ret = trmnl_load_config(&cfg);
  if (ret < 0)
    {
      trmnl_select_root(cfg.root, sizeof(cfg.root));
      printf("config: invalid (%d)\n", ret);
    }
  else
    {
      printf("config: ok\n");
    }

  printf("root: %s\n", cfg.root);
  printf("storage: %s\n", trmnl_root_kind(cfg.root));
  fd = open(CONFIG_SYSTEM_TRMNL_FB_DEVPATH, O_RDWR);
  if (fd < 0)
    {
      printf("framebuffer: open failed %d\n", errno);
    }
  else if (ioctl(fd, FBIOGET_VIDEOINFO, (unsigned long)&vinfo) < 0 ||
           ioctl(fd, FBIOGET_PLANEINFO, (unsigned long)&pinfo) < 0)
    {
      printf("framebuffer: info failed %d\n", errno);
      close(fd);
    }
  else
    {
      printf("framebuffer: %ux%u fmt=%u bpp=%u stride=%u len=%lu\n",
             vinfo.xres, vinfo.yres, vinfo.fmt, pinfo.bpp, pinfo.stride,
             (unsigned long)pinfo.fblen);
      close(fd);
    }

  trmnl_path(path, sizeof(path), cfg.root, "state.json");
  state = trmnl_read_file(path, 1024, NULL);
  if (state != NULL)
    {
      printf("%s", state);
      free(state);
    }
  else
    {
      printf("state: missing\n");
    }

  return 0;
}

static int trmnl_clear_cache(void)
{
  struct trmnl_config cfg;
  char path[TRMNL_PATH_LEN];
  int ret;

  ret = trmnl_load_config(&cfg);
  if (ret < 0)
    {
      return ret;
    }

  trmnl_path(path, sizeof(path), cfg.root, "current.png");
  unlink(path);
  trmnl_path(path, sizeof(path), cfg.root, "last.png");
  unlink(path);
  trmnl_path(path, sizeof(path), cfg.root, "state.json");
  unlink(path);
  printf("trmnl: cache cleared\n");
  return 0;
}

static int trmnl_selftest(void)
{
  static const char json[] =
    "{\"status\":200,\"image_url\":\"https://example.com/a.png\","
    "\"filename\":\"a.png\",\"refresh_rate\":\"123\"}";
  struct trmnl_display_response resp;
  uint8_t gray[4] = {0, 85, 170, 255};
  uint8_t packed;
  int ret;

  ret = trmnl_parse_display(json, &resp);
  if (ret < 0 || strcmp(resp.image_url, "https://example.com/a.png") != 0 ||
      resp.refresh_rate != 123)
    {
      fprintf(stderr, "trmnl: json selftest failed\n");
      return -EINVAL;
    }

  packed = (uint8_t)(((gray[0] * 3u + 127u) / 255u) << 6 |
                     ((gray[1] * 3u + 127u) / 255u) << 4 |
                     ((gray[2] * 3u + 127u) / 255u) << 2 |
                     ((gray[3] * 3u + 127u) / 255u));
  if (packed != 0x1b)
    {
      fprintf(stderr, "trmnl: y2 selftest failed\n");
      return -EINVAL;
    }

  printf("trmnl: selftest ok\n");
  return 0;
}

static void trmnl_usage(const char *progname)
{
  fprintf(stderr,
          "Usage: %s <config|status|storage|set-device|set-id|set-token|"
          "set-api|set-image-format|set-wifi|test-pattern|once|start|boot|"
          "clear-cache|selftest>\n",
          progname);
}

int main(int argc, char *argv[])
{
  int ret;

  if (argc < 2)
    {
      trmnl_usage(argv[0]);
      return EXIT_FAILURE;
    }

  if (strcmp(argv[1], "config") == 0)
    {
      ret = trmnl_run_guarded("config", trmnl_config_cmd);
    }
  else if (strcmp(argv[1], "status") == 0)
    {
      ret = trmnl_run_guarded("status", trmnl_status);
    }
  else if (strcmp(argv[1], "storage") == 0)
    {
      ret = trmnl_run_guarded("storage", trmnl_storage_cmd);
    }
  else if (strcmp(argv[1], "set-device") == 0)
    {
      ret = trmnl_set_device(argc, argv);
    }
  else if (strcmp(argv[1], "set-id") == 0)
    {
      ret = trmnl_set_config_field(argc, argv, "id");
    }
  else if (strcmp(argv[1], "set-token") == 0)
    {
      ret = trmnl_set_config_field(argc, argv, "token");
    }
  else if (strcmp(argv[1], "set-api") == 0)
    {
      ret = trmnl_set_config_field(argc, argv, "api");
    }
  else if (strcmp(argv[1], "set-image-format") == 0)
    {
      ret = trmnl_set_config_field(argc, argv, "image-format");
    }
  else if (strcmp(argv[1], "set-wifi") == 0)
    {
      ret = trmnl_set_wifi(argc, argv);
    }
  else if (strcmp(argv[1], "test-pattern") == 0)
    {
      ret = trmnl_run_guarded("test-pattern", trmnl_test_pattern);
    }
  else if (strcmp(argv[1], "once") == 0)
    {
      ret = trmnl_once();
    }
  else if (strcmp(argv[1], "start") == 0)
    {
      ret = trmnl_start();
    }
  else if (strcmp(argv[1], "boot") == 0)
    {
      ret = trmnl_boot();
    }
  else if (strcmp(argv[1], "clear-cache") == 0)
    {
      ret = trmnl_run_guarded("clear-cache", trmnl_clear_cache);
    }
  else if (strcmp(argv[1], "selftest") == 0)
    {
      ret = trmnl_selftest();
    }
  else
    {
      trmnl_usage(argv[0]);
      return EXIT_FAILURE;
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
