/* SPDX-License-Identifier: Apache-2.0 */

#include "fruitclaw.h"

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "netutils/webclient.h"

static pthread_mutex_t g_http_lock = PTHREAD_MUTEX_INITIALIZER;

static void fc_http_add_ms(struct timespec *ts, int64_t ms)
{
  ts->tv_sec += ms / 1000;
  ts->tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L)
    {
      ts->tv_sec++;
      ts->tv_nsec -= 1000000000L;
    }
}

static int fc_http_lock_timed(uint32_t request_timeout_sec,
                              uint32_t guard_timeout_ms)
{
  struct timespec deadline;
  int64_t wait_ms;
  int ret;

  wait_ms = request_timeout_sec > 0 ?
            ((int64_t)request_timeout_sec + 5) * 1000 : 35000;
  if (guard_timeout_ms > 0 && guard_timeout_ms < (uint32_t)wait_ms)
    {
      wait_ms = guard_timeout_ms;
    }

  if (wait_ms < 5000)
    {
      wait_ms = 5000;
    }
  else if (wait_ms > 60000)
    {
      wait_ms = 60000;
    }

  if (clock_gettime(CLOCK_REALTIME, &deadline) < 0)
    {
      ret = pthread_mutex_lock(&g_http_lock);
      return ret == 0 ? 0 : -ret;
    }

  fc_http_add_ms(&deadline, wait_ms);
  ret = pthread_mutex_timedlock(&g_http_lock, &deadline);
  if (ret == ETIMEDOUT)
    {
      FC_LOGW("HTTP lane busy for %lld ms", (long long)wait_ms);
      return -ETIMEDOUT;
    }

  return ret == 0 ? 0 : -ret;
}

#if defined(CONFIG_FRUITCLAW_ENABLE_TLS) && defined(CONFIG_CRYPTO_MBEDTLS)
#  include <mbedtls/ctr_drbg.h>
#  include <mbedtls/entropy.h>
#  include <mbedtls/error.h>
#  include <mbedtls/net_sockets.h>
#  include <mbedtls/ssl.h>
#  include <mbedtls/x509_crt.h>

struct fc_tls_connection
{
  mbedtls_net_context net;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_x509_crt ca;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  uint32_t timeout_ms;
};

static const int g_fc_tls12_ciphersuites[] =
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

static const uint16_t g_fc_tls_groups[] =
{
  MBEDTLS_SSL_IANA_TLS_GROUP_X25519,
  MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1,
  0
};

static uint32_t fc_tls_timeout_ms(unsigned int timeout_second)
{
  if (timeout_second > 0 && timeout_second < 3600)
    {
      return timeout_second * 1000;
    }

  return 30000;
}

static bool fc_tls_deadline_expired(int64_t deadline_ms)
{
  return deadline_ms - fc_mono_ms() <= 0;
}

static uint32_t fc_tls_remaining_ms(int64_t deadline_ms)
{
  int64_t remaining = deadline_ms - fc_mono_ms();

  if (remaining <= 0)
    {
      return 0;
    }

  if (remaining > INT32_MAX)
    {
      return INT32_MAX;
    }

  return (uint32_t)remaining;
}

static int fc_tls_wait(struct fc_tls_connection *conn, int ret,
                       int64_t deadline_ms)
{
  int flags;
  uint32_t timeout_ms;

  if (ret == MBEDTLS_ERR_SSL_WANT_READ)
    {
      flags = MBEDTLS_NET_POLL_READ;
    }
  else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    {
      flags = MBEDTLS_NET_POLL_WRITE;
    }
  else
    {
      return ret;
    }

  if (fc_tls_deadline_expired(deadline_ms))
    {
      return -ETIMEDOUT;
    }

  timeout_ms = fc_tls_remaining_ms(deadline_ms);
  if (timeout_ms > 1000)
    {
      timeout_ms = 1000;
    }

  ret = mbedtls_net_poll(&conn->net, flags, timeout_ms);
  if (ret == 0)
    {
      return fc_tls_deadline_expired(deadline_ms) ? -ETIMEDOUT : 0;
    }

  return ret < 0 ? ret : 0;
}

static ssize_t fc_tls_io_ret(int ret)
{
  if (ret == -ETIMEDOUT)
    {
      return -ETIMEDOUT;
    }

#ifdef MBEDTLS_ERR_SSL_TIMEOUT
  if (ret == MBEDTLS_ERR_SSL_TIMEOUT)
    {
      return -ETIMEDOUT;
    }
#endif

  return ret < 0 ? -EIO : ret;
}

static const char *fc_tls_error(int ret, char *buf, size_t buflen)
{
#ifdef CONFIG_MBEDTLS_ERROR_C
  mbedtls_strerror(ret, buf, buflen);
  return buf;
#else
  snprintf(buf, buflen, "mbedtls error %d", ret);
  return buf;
#endif
}

static int fc_tls_entropy(void *data, unsigned char *output, size_t len,
                          size_t *olen)
{
  static uint32_t counter;
  struct timespec ts;
  uintptr_t mix[4];
  unsigned char *mix_bytes = (unsigned char *)mix;
  uint32_t local_counter;
  size_t i;

  if (output == NULL || olen == NULL)
    {
      return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }

  local_counter = ++counter;
  arc4random_buf(output, len);
  clock_gettime(CLOCK_REALTIME, &ts);

  mix[0] = (uintptr_t)data;
  mix[1] = (uintptr_t)output;
  mix[2] = ((uintptr_t)ts.tv_sec << 16) ^ (uintptr_t)ts.tv_nsec;
  mix[3] = (uintptr_t)getpid() ^ (uintptr_t)local_counter ^
           (uintptr_t)fc_time_ms();

  for (i = 0; i < len; i++)
    {
      output[i] ^= mix_bytes[i % sizeof(mix)] ^
                   (unsigned char)(local_counter + i * 33u);
    }

  *olen = len;
  return 0;
}

static int fc_tls_drbg_entropy(void *data, unsigned char *output, size_t len)
{
  size_t olen = 0;
  int ret;

  ret = fc_tls_entropy(data, output, len, &olen);
  if (ret < 0 || olen != len)
    {
      return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }

  return 0;
}

static int fc_tls_connect(void *ctx, const char *hostname, const char *port,
                          unsigned int timeout_second,
                          struct webclient_tls_connection **connp)
{
  struct fc_tls_connection *conn;
  struct timeval tv;
  uint32_t timeout_ms;
  int64_t deadline_ms;
  int ret;
  const char *pers = "fruitclaw";

  (void)ctx;

  if (hostname == NULL || port == NULL || connp == NULL)
    {
      return -EINVAL;
    }

  conn = calloc(1, sizeof(*conn));
  if (conn == NULL)
    {
      return -ENOMEM;
    }

  mbedtls_net_init(&conn->net);
  mbedtls_ssl_init(&conn->ssl);
  mbedtls_ssl_config_init(&conn->conf);
  mbedtls_x509_crt_init(&conn->ca);
  mbedtls_entropy_init(&conn->entropy);
  mbedtls_ctr_drbg_init(&conn->ctr_drbg);

  ret = mbedtls_ctr_drbg_seed(&conn->ctr_drbg, fc_tls_drbg_entropy,
                              conn, (const unsigned char *)pers,
                              strlen(pers));
  if (ret != 0)
    {
      FC_LOGW("tls ctr_drbg_seed failed host=%s ret=%d", hostname, ret);
      goto fail;
    }

  ret = mbedtls_ssl_config_defaults(&conn->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0)
    {
      FC_LOGW("tls config_defaults failed host=%s ret=%d", hostname, ret);
      goto fail;
    }

#if defined(MBEDTLS_SSL_SESSION_TICKETS) && defined(MBEDTLS_SSL_CLI_C)
  mbedtls_ssl_conf_session_tickets(&conn->conf,
                                   MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
#endif

#ifdef MBEDTLS_SSL_PROTO_TLS1_2
  mbedtls_ssl_conf_min_tls_version(&conn->conf,
                                   MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_max_tls_version(&conn->conf,
                                   MBEDTLS_SSL_VERSION_TLS1_2);
#endif

  mbedtls_ssl_conf_ciphersuites(&conn->conf, g_fc_tls12_ciphersuites);
  mbedtls_ssl_conf_groups(&conn->conf, g_fc_tls_groups);

#ifdef CONFIG_FRUITCLAW_TLS_ALLOW_UNVERIFIED
  mbedtls_ssl_conf_authmode(&conn->conf, MBEDTLS_SSL_VERIFY_NONE);
#else
  {
    char ca_path[FC_PATH_LEN];

    ret = fc_tls_ca_cert_path(ca_path, sizeof(ca_path));
    if (ret < 0)
      {
        goto fail;
      }

    ret = mbedtls_x509_crt_parse_file(&conn->ca, ca_path);
  }
  if (ret != 0)
    {
      FC_LOGW("tls ca_parse failed host=%s ret=%d", hostname, ret);
      ret = -ENOENT;
      goto fail;
    }

  mbedtls_ssl_conf_ca_chain(&conn->conf, &conn->ca, NULL);
  mbedtls_ssl_conf_authmode(&conn->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
#endif

  mbedtls_ssl_conf_rng(&conn->conf, mbedtls_ctr_drbg_random,
                       &conn->ctr_drbg);

  timeout_ms = fc_tls_timeout_ms(timeout_second);
  conn->timeout_ms = timeout_ms;
  mbedtls_ssl_conf_read_timeout(&conn->conf, timeout_ms);

  ret = mbedtls_net_connect(&conn->net, hostname, port,
                            MBEDTLS_NET_PROTO_TCP);
  if (ret != 0)
    {
      FC_LOGW("tls net_connect failed host=%s ret=%d", hostname, ret);
      goto fail;
    }

  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  ret = setsockopt(conn->net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if (ret < 0)
    {
      FC_LOGW("tls sndtimeo failed host=%s errno=%d", hostname, errno);
    }

  ret = setsockopt(conn->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  if (ret < 0)
    {
      FC_LOGW("tls rcvtimeo failed host=%s errno=%d", hostname, errno);
    }

  ret = mbedtls_ssl_setup(&conn->ssl, &conn->conf);
  if (ret != 0)
    {
      FC_LOGW("tls ssl_setup failed host=%s ret=%d", hostname, ret);
      goto fail;
    }

  ret = mbedtls_ssl_set_hostname(&conn->ssl, hostname);
  if (ret != 0)
    {
      FC_LOGW("tls set_hostname failed host=%s ret=%d", hostname, ret);
      goto fail;
    }

  mbedtls_ssl_set_bio(&conn->ssl, &conn->net, mbedtls_net_send,
                      mbedtls_net_recv, mbedtls_net_recv_timeout);

  deadline_ms = fc_mono_ms() + timeout_ms;
  for (; ; )
    {
      ret = mbedtls_ssl_handshake(&conn->ssl);
      if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
          ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          break;
        }

      ret = fc_tls_wait(conn, ret, deadline_ms);
      if (ret != 0)
        {
          break;
        }
    }

  if (ret != 0)
    {
      char errbuf[96];

      FC_LOGW("tls handshake failed host=%s ret=%d (%s)", hostname, ret,
              fc_tls_error(ret, errbuf, sizeof(errbuf)));
      goto fail;
    }

#ifndef CONFIG_FRUITCLAW_TLS_ALLOW_UNVERIFIED
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

static ssize_t fc_tls_send(void *ctx, struct webclient_tls_connection *c,
                           const void *buf, size_t len)
{
  struct fc_tls_connection *conn = (struct fc_tls_connection *)c;
  int64_t deadline_ms;
  int ret;

  (void)ctx;
  if (conn == NULL)
    {
      return -EINVAL;
    }

  deadline_ms = fc_mono_ms() + conn->timeout_ms;
  for (; ; )
    {
      ret = mbedtls_ssl_write(&conn->ssl, buf, len);
      if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
          ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          break;
        }

      ret = fc_tls_wait(conn, ret, deadline_ms);
      if (ret != 0)
        {
          break;
        }
    }

  return fc_tls_io_ret(ret);
}

static ssize_t fc_tls_recv(void *ctx, struct webclient_tls_connection *c,
                           void *buf, size_t len)
{
  struct fc_tls_connection *conn = (struct fc_tls_connection *)c;
  int64_t deadline_ms;
  int ret;

  (void)ctx;
  if (conn == NULL)
    {
      return -EINVAL;
    }

  deadline_ms = fc_mono_ms() + conn->timeout_ms;
  for (; ; )
    {
      ret = mbedtls_ssl_read(&conn->ssl, buf, len);
      if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
          ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          break;
        }

      ret = fc_tls_wait(conn, ret, deadline_ms);
      if (ret != 0)
        {
          break;
        }
    }

  if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
    {
      return 0;
    }

  return fc_tls_io_ret(ret);
}

static int fc_tls_close(void *ctx, struct webclient_tls_connection *c)
{
  struct fc_tls_connection *conn = (struct fc_tls_connection *)c;

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

static int fc_tls_get_poll_info(void *ctx, struct webclient_tls_connection *c,
                                struct webclient_poll_info *info)
{
  struct fc_tls_connection *conn = (struct fc_tls_connection *)c;

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

static const struct webclient_tls_ops g_fc_tls_ops =
{
  fc_tls_connect,
  fc_tls_send,
  fc_tls_recv,
  fc_tls_close,
  fc_tls_get_poll_info,
  NULL
};
#endif

struct fc_http_accum
{
  char *buf;
  size_t len;
  size_t cap;
};

struct fc_http_watchdog
{
  pthread_t thread;
  volatile bool stop;
  bool started;
  int64_t deadline_ms;
};

static void *fc_http_watchdog_main(void *arg)
{
  struct fc_http_watchdog *wd = arg;

  while (!wd->stop && fc_mono_ms() < wd->deadline_ms)
    {
      fc_guard_session_heartbeat("http");
      sleep(1);
    }

  return NULL;
}

static void fc_http_watchdog_start(struct fc_http_watchdog *wd,
                                   uint32_t timeout_ms)
{
  int ret;
  int64_t runtime_ms = timeout_ms > 0 ? (int64_t)timeout_ms + 5000 :
                                        45000;

  memset(wd, 0, sizeof(*wd));
  wd->deadline_ms = fc_mono_ms() + runtime_ms;
  ret = pthread_create(&wd->thread, NULL, fc_http_watchdog_main, wd);
  if (ret == 0)
    {
      wd->started = true;
    }
}

static void fc_http_watchdog_stop(struct fc_http_watchdog *wd)
{
  if (wd->started)
    {
      wd->stop = true;
      pthread_join(wd->thread, NULL);
      wd->started = false;
    }
}

static int fc_http_sink(char **buffer, int offset, int datend, int *buflen,
                        void *arg)
{
  struct fc_http_accum *acc = arg;
  int len = datend - offset;

  (void)buflen;
  if (len <= 0)
    {
      return 0;
    }

  if (acc->len + len >= acc->cap)
    {
      size_t avail = acc->cap > acc->len ? acc->cap - acc->len - 1 : 0;
      if (avail > 0)
        {
          memcpy(acc->buf + acc->len, &(*buffer)[offset], avail);
          acc->len += avail;
          acc->buf[acc->len] = '\0';
        }

      return -ENOSPC;
    }

  memcpy(acc->buf + acc->len, &(*buffer)[offset], len);
  acc->len += len;
  acc->buf[acc->len] = '\0';
  return 0;
}

static int fc_http_perform(const char *method, const char *url,
                           const fc_http_headers_t *headers,
                           const char *body, char *response,
                           size_t response_len, long *status_code,
                           uint32_t request_timeout_sec,
                           uint32_t guard_stage, uint32_t guard_timeout_ms,
                           bool lock_http)
{
  struct webclient_context ctx;
  struct fc_http_accum acc;
  struct fc_http_watchdog wd;
  fc_guard_long_t guard;
  char iobuf[768];
  char header_lines[FC_HTTP_HEADER_MAX][160];
  const char *header_ptrs[FC_HTTP_HEADER_MAX];
  unsigned int i;
  int ret;
  int guard_ret = 0;
  bool locked = false;
  bool guarded = false;

  if (method == NULL || url == NULL || response == NULL || response_len == 0)
    {
      return -EINVAL;
    }

  if (!fc_url_host_allowed(url))
    {
      return -EACCES;
    }

  if (strncmp(url, "https://", 8) == 0)
    {
#if !defined(CONFIG_FRUITCLAW_ENABLE_TLS) || !defined(CONFIG_CRYPTO_MBEDTLS)
      return -ENOTSUP;
#endif
    }

  if (lock_http)
    {
      ret = fc_http_lock_timed(request_timeout_sec, guard_timeout_ms);
      if (ret < 0)
        {
          return ret;
        }

      locked = true;
    }

  if (guard_timeout_ms > 0)
    {
      ret = fc_guard_long_start(guard_stage, guard_timeout_ms, &guard);
      if (ret < 0)
        {
          if (locked)
            {
              pthread_mutex_unlock(&g_http_lock);
            }

          return ret;
        }

      guarded = true;
    }

  response[0] = '\0';

  memset(&ctx, 0, sizeof(ctx));
  memset(&acc, 0, sizeof(acc));
  webclient_set_defaults(&ctx);

  acc.buf = response;
  acc.cap = response_len;

  ctx.method = method;
  ctx.url = url;
  ctx.buffer = iobuf;
  ctx.buflen = sizeof(iobuf);
  ctx.sink_callback = fc_http_sink;
  ctx.sink_callback_arg = &acc;
  ctx.timeout_sec = request_timeout_sec > 0 ? request_timeout_sec : 30;
  ctx.protocol_version = WEBCLIENT_PROTOCOL_VERSION_HTTP_1_1;

#if defined(CONFIG_FRUITCLAW_ENABLE_TLS) && defined(CONFIG_CRYPTO_MBEDTLS)
  if (strncmp(url, "https://", 8) == 0)
    {
      ctx.tls_ops = &g_fc_tls_ops;
    }
#endif

  if (headers != NULL)
    {
      for (i = 0; i < headers->count && i < FC_HTTP_HEADER_MAX; i++)
        {
          snprintf(header_lines[i], sizeof(header_lines[i]), "%s: %s",
                   headers->items[i].name, headers->items[i].value);
          header_ptrs[i] = header_lines[i];
        }

      ctx.headers = header_ptrs;
      ctx.nheaders = i;
    }

  if (body != NULL)
    {
      webclient_set_static_body(&ctx, body, strlen(body));
    }

  fc_http_watchdog_start(&wd, guard_timeout_ms);
  ret = webclient_perform(&ctx);
  fc_http_watchdog_stop(&wd);
  if (guarded)
    {
      guard_ret = fc_guard_long_stop(&guard);
    }

  if (status_code != NULL)
    {
      *status_code = ctx.http_status;
    }

  if (locked)
    {
      pthread_mutex_unlock(&g_http_lock);
    }

  if (guard_ret < 0 && ret == 0)
    {
      ret = guard_ret;
    }

  if (ret < 0)
    {
      return ret;
    }

  return 0;
}

int fc_http_get(const char *url, const fc_http_headers_t *headers,
                char *response, size_t response_len, long *status_code)
{
  return fc_http_perform("GET", url, headers, NULL, response, response_len,
                         status_code, 30, 0, 0, true);
}

int fc_http_get_unlocked(const char *url, const fc_http_headers_t *headers,
                         char *response, size_t response_len,
                         long *status_code)
{
  return fc_http_perform("GET", url, headers, NULL, response, response_len,
                         status_code, 30, 0, 0, false);
}

int fc_http_get_unlocked_timeout(uint32_t request_timeout_sec,
                                 const char *url,
                                 const fc_http_headers_t *headers,
                                 char *response, size_t response_len,
                                 long *status_code)
{
  return fc_http_perform("GET", url, headers, NULL, response, response_len,
                         status_code, request_timeout_sec, 0, 0, false);
}

int fc_http_post_json(const char *url, const fc_http_headers_t *headers,
                      const char *json_body, char *response,
                      size_t response_len, long *status_code)
{
  fc_http_headers_t merged;

  memset(&merged, 0, sizeof(merged));
  merged.items[merged.count].name = "Content-Type";
  merged.items[merged.count++].value = "application/json";

  if (headers != NULL)
    {
      unsigned int i;
      for (i = 0; i < headers->count && merged.count < FC_HTTP_HEADER_MAX; i++)
        {
          merged.items[merged.count++] = headers->items[i];
        }
    }

  return fc_http_perform("POST", url, &merged, json_body ? json_body : "{}",
                         response, response_len, status_code, 30, 0, 0,
                         true);
}

int fc_http_post_json_unlocked_timeout(uint32_t request_timeout_sec,
                                       const char *url,
                                       const fc_http_headers_t *headers,
                                       const char *json_body, char *response,
                                       size_t response_len,
                                       long *status_code)
{
  fc_http_headers_t merged;

  memset(&merged, 0, sizeof(merged));
  merged.items[merged.count].name = "Content-Type";
  merged.items[merged.count++].value = "application/json";

  if (headers != NULL)
    {
      unsigned int i;
      for (i = 0; i < headers->count && merged.count < FC_HTTP_HEADER_MAX; i++)
        {
          merged.items[merged.count++] = headers->items[i];
        }
    }

  return fc_http_perform("POST", url, &merged, json_body ? json_body : "{}",
                         response, response_len, status_code,
                         request_timeout_sec, 0, 0, false);
}

int fc_http_get_guarded(uint32_t stage, uint32_t timeout_ms,
                        const char *url, const fc_http_headers_t *headers,
                        char *response, size_t response_len,
                        long *status_code)
{
  return fc_http_perform("GET", url, headers, NULL, response, response_len,
                         status_code, 30, stage, timeout_ms, true);
}

int fc_http_get_guarded_timeout(uint32_t stage, uint32_t guard_timeout_ms,
                                uint32_t request_timeout_sec,
                                const char *url,
                                const fc_http_headers_t *headers,
                                char *response, size_t response_len,
                                long *status_code)
{
  return fc_http_perform("GET", url, headers, NULL, response, response_len,
                         status_code, request_timeout_sec, stage,
                         guard_timeout_ms, true);
}

int fc_http_post_json_guarded(uint32_t stage, uint32_t timeout_ms,
                              const char *url,
                              const fc_http_headers_t *headers,
                              const char *json_body, char *response,
                              size_t response_len, long *status_code)
{
  fc_http_headers_t merged;

  memset(&merged, 0, sizeof(merged));
  merged.items[merged.count].name = "Content-Type";
  merged.items[merged.count++].value = "application/json";

  if (headers != NULL)
    {
      unsigned int i;
      for (i = 0; i < headers->count && merged.count < FC_HTTP_HEADER_MAX; i++)
        {
          merged.items[merged.count++] = headers->items[i];
        }
    }

  return fc_http_perform("POST", url, &merged, json_body ? json_body : "{}",
                         response, response_len, status_code, 30, stage,
                         timeout_ms, true);
}

int fc_http_post_json_guarded_timeout(uint32_t stage,
                                      uint32_t guard_timeout_ms,
                                      uint32_t request_timeout_sec,
                                      const char *url,
                                      const fc_http_headers_t *headers,
                                      const char *json_body,
                                      char *response, size_t response_len,
                                      long *status_code)
{
  fc_http_headers_t merged;

  memset(&merged, 0, sizeof(merged));
  merged.items[merged.count].name = "Content-Type";
  merged.items[merged.count++].value = "application/json";

  if (headers != NULL)
    {
      unsigned int i;
      for (i = 0; i < headers->count && merged.count < FC_HTTP_HEADER_MAX; i++)
        {
          merged.items[merged.count++] = headers->items[i];
        }
    }

  return fc_http_perform("POST", url, &merged, json_body ? json_body : "{}",
                         response, response_len, status_code,
                         request_timeout_sec, stage, guard_timeout_ms, true);
}
