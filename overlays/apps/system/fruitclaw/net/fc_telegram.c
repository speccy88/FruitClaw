/* SPDX-License-Identifier: Apache-2.0 */

#include "fruitclaw.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "netutils/cJSON.h"

static pthread_t g_tg_thread;
static bool g_tg_started;
static int64_t g_update_offset = -1;
static pthread_mutex_t g_tg_status_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_tg_poll_count;
static unsigned long g_tg_poll_fail_count;
static unsigned long g_tg_update_count;
static unsigned long g_tg_queued_count;
static unsigned long g_tg_ignored_count;
static int64_t g_tg_last_poll_ms;
static int64_t g_tg_last_success_ms;
static int64_t g_tg_last_update_id = -1;
static int64_t g_tg_last_saved_offset = -1;
static int g_tg_last_ret;
static long g_tg_last_http_status;
static char g_tg_last_chat_id[FC_CHAT_ID_LEN];
static bool g_tg_poll_active;

#define FC_TG_NOTIFY_QUEUE_LEN CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_QUEUE_LEN
#define FC_TG_NOTIFY_TEXT_LEN 192
#define FC_TG_NOTIFY_SEND_LEN 2048

static pthread_t g_tg_notify_thread;
static bool g_tg_notify_started;
static pthread_mutex_t g_tg_notify_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tg_notify_cond = PTHREAD_COND_INITIALIZER;
static char g_tg_notify_queue[FC_TG_NOTIFY_QUEUE_LEN][FC_TG_NOTIFY_TEXT_LEN];
static unsigned int g_tg_notify_head;
static unsigned int g_tg_notify_count;
static unsigned int g_tg_notify_inflight;
static unsigned long g_tg_notify_enqueued;
static unsigned long g_tg_notify_dropped;
static unsigned long g_tg_notify_sent;
static unsigned long g_tg_notify_failed;

static void fc_telegram_record_transport_success(const char *source,
                                                 long status)
{
  pthread_mutex_lock(&g_tg_status_lock);
  g_tg_last_success_ms = fc_mono_ms();
  if (status > 0)
    {
      g_tg_last_http_status = status;
    }

  pthread_mutex_unlock(&g_tg_status_lock);
  fc_operator_progress_mark(source ? source : "telegram");
}

static void fc_telegram_record_poll(int ret, long status)
{
  pthread_mutex_lock(&g_tg_status_lock);
  g_tg_poll_count++;
  g_tg_last_poll_ms = fc_mono_ms();
  g_tg_last_ret = ret;
  g_tg_last_http_status = status;
  if (ret < 0)
    {
      g_tg_poll_fail_count++;
    }
  else
    {
      g_tg_last_success_ms = g_tg_last_poll_ms;
    }

  pthread_mutex_unlock(&g_tg_status_lock);
  if (ret >= 0)
    {
      fc_operator_progress_mark("telegram");
    }
}

static void fc_telegram_set_poll_active(bool active)
{
  pthread_mutex_lock(&g_tg_status_lock);
  g_tg_poll_active = active;
  if (active)
    {
      g_tg_last_poll_ms = fc_mono_ms();
    }

  pthread_mutex_unlock(&g_tg_status_lock);
}

static void fc_telegram_record_update(int64_t update_id,
                                      const char *chat_id, bool queued)
{
  pthread_mutex_lock(&g_tg_status_lock);
  g_tg_update_count++;
  g_tg_last_update_id = update_id;
  fc_strlcpy(g_tg_last_chat_id, chat_id ? chat_id : "",
             sizeof(g_tg_last_chat_id));
  if (queued)
    {
      g_tg_queued_count++;
    }
  else
    {
      g_tg_ignored_count++;
    }

  pthread_mutex_unlock(&g_tg_status_lock);
}

static void fc_telegram_record_offset(int64_t offset)
{
  pthread_mutex_lock(&g_tg_status_lock);
  g_tg_last_saved_offset = offset;
  pthread_mutex_unlock(&g_tg_status_lock);
}

static void fc_telegram_record_notify_result(int ret, unsigned int count)
{
  pthread_mutex_lock(&g_tg_status_lock);
  if (ret < 0)
    {
      g_tg_notify_failed += count;
    }
  else
    {
      g_tg_notify_sent += count;
    }

  pthread_mutex_unlock(&g_tg_status_lock);

  pthread_mutex_lock(&g_tg_notify_lock);
  if (g_tg_notify_inflight >= count)
    {
      g_tg_notify_inflight -= count;
    }
  else
    {
      g_tg_notify_inflight = 0;
    }

  pthread_mutex_unlock(&g_tg_notify_lock);
}

static bool fc_telegram_notify_pending(void)
{
  bool pending;

  pthread_mutex_lock(&g_tg_notify_lock);
  pending = g_tg_notify_count > 0 || g_tg_notify_inflight > 0;
  pthread_mutex_unlock(&g_tg_notify_lock);
  return pending;
}

static bool fc_telegram_poll_active(void)
{
  bool active;

  pthread_mutex_lock(&g_tg_status_lock);
  active = g_tg_poll_active;
  pthread_mutex_unlock(&g_tg_status_lock);
  return active;
}

static bool fc_telegram_transport_ready(void)
{
  int64_t last_success;
  int64_t now = fc_mono_ms();
  int64_t max_age_ms = CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS;

  pthread_mutex_lock(&g_tg_status_lock);
  last_success = g_tg_last_success_ms;
  pthread_mutex_unlock(&g_tg_status_lock);

  if (last_success <= 0 || now < last_success)
    {
      return false;
    }

  if (max_age_ms <= 0)
    {
      max_age_ms = 120000;
    }

  return now - last_success <= max_age_ms;
}

static void fc_telegram_sleep_ms(const char *label, unsigned int total_ms)
{
  while (total_ms > 0)
    {
      unsigned int step_ms = total_ms;

      if (step_ms > 250)
        {
          step_ms = 250;
        }

      fc_guard_session_heartbeat(label ? label : "telegram-sleep");
      usleep((useconds_t)step_ms * USEC_PER_MSEC);
      total_ms -= step_ms;
    }
}

static unsigned int fc_telegram_poll_idle_ms(void)
{
  unsigned int idle_ms = CONFIG_FRUITCLAW_TELEGRAM_POLL_IDLE_MS;

  if (idle_ms < CONFIG_FRUITCLAW_TELEGRAM_POLL_MIN_IDLE_MS)
    {
      idle_ms = CONFIG_FRUITCLAW_TELEGRAM_POLL_MIN_IDLE_MS;
    }

  return idle_ms;
}

bool fc_telegram_poll_stale(int64_t now_ms, int64_t timeout_ms)
{
  bool started;
  bool active;
  int64_t poll_ms;

  if (timeout_ms <= 0)
    {
      return false;
    }

  pthread_mutex_lock(&g_tg_status_lock);
  started = g_tg_started;
  active = g_tg_poll_active;
  poll_ms = g_tg_last_poll_ms;
  pthread_mutex_unlock(&g_tg_status_lock);

  return started && active && poll_ms > 0 && now_ms - poll_ms > timeout_ms;
}

int64_t fc_telegram_poll_age_ms(bool *active_out)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_TELEGRAM
  bool active;
  int64_t poll_ms;

  pthread_mutex_lock(&g_tg_status_lock);
  active = g_tg_poll_active;
  poll_ms = g_tg_last_poll_ms;
  pthread_mutex_unlock(&g_tg_status_lock);

  if (active_out != NULL)
    {
      *active_out = active;
    }

  if (!active || poll_ms <= 0)
    {
      return -1;
    }

  return fc_mono_ms() - poll_ms;
#else
  if (active_out != NULL)
    {
      *active_out = false;
    }

  return -1;
#endif
}

static void fc_telegram_notify_yield_to_poller(void)
{
  unsigned int waited_ms = 0;
#if CONFIG_FRUITCLAW_TELEGRAM_MCP_MAX_YIELD_MS > 0
  unsigned int mcp_waited_ms = 0;
#endif

  while (fc_telegram_poll_active() && waited_ms < 3000)
    {
      fc_guard_session_heartbeat("telegram-notify-wait");
      usleep(100 * USEC_PER_MSEC);
      waited_ms += 100;
    }

  while (fc_mcp_recently_active(fc_mono_ms(),
                                CONFIG_FRUITCLAW_TELEGRAM_MCP_YIELD_MS))
    {
      unsigned int step_ms = 250;

#if CONFIG_FRUITCLAW_TELEGRAM_MCP_MAX_YIELD_MS > 0
      if (mcp_waited_ms + step_ms >
          CONFIG_FRUITCLAW_TELEGRAM_MCP_MAX_YIELD_MS)
        {
          step_ms = CONFIG_FRUITCLAW_TELEGRAM_MCP_MAX_YIELD_MS -
                    mcp_waited_ms;
        }

      if (step_ms == 0)
        {
          break;
        }
#endif

      fc_guard_session_heartbeat("telegram-notify-mcp-yield");
      usleep((useconds_t)step_ms * USEC_PER_MSEC);

#if CONFIG_FRUITCLAW_TELEGRAM_MCP_MAX_YIELD_MS > 0
      mcp_waited_ms += step_ms;
#endif
    }
}

static int fc_telegram_read_token(char *token, size_t token_len)
{
  char path[FC_PATH_LEN];
  int ret;

  ret = fc_secret_path("telegram_token", path, sizeof(path));
  if (ret < 0)
    {
      return ret;
    }

  return fc_read_text_file(path, token, token_len, true);
}

static int fc_telegram_http_get(const char *url, char *resp, size_t resp_len,
                                long *status)
{
#if CONFIG_FRUITCLAW_TELEGRAM_HTTP_GUARD_TIMEOUT_MS > 0
  return fc_http_get_guarded_timeout(FC_GUARD_STAGE_TELEGRAM,
                                     CONFIG_FRUITCLAW_TELEGRAM_HTTP_GUARD_TIMEOUT_MS,
                                     CONFIG_FRUITCLAW_TELEGRAM_HTTP_TIMEOUT_SEC,
                                     url, NULL, resp, resp_len, status);
#else
  return fc_http_get_unlocked_timeout(CONFIG_FRUITCLAW_TELEGRAM_HTTP_TIMEOUT_SEC,
                                      url, NULL, resp, resp_len, status);
#endif
}

static int fc_telegram_http_post_json(bool guarded, const char *url,
                                      const char *body, char *resp,
                                      size_t resp_len, long *status)
{
  if (guarded)
    {
#if CONFIG_FRUITCLAW_TELEGRAM_HTTP_GUARD_TIMEOUT_MS > 0
      return fc_http_post_json_guarded_timeout(
              FC_GUARD_STAGE_TELEGRAM,
              CONFIG_FRUITCLAW_TELEGRAM_HTTP_GUARD_TIMEOUT_MS,
              CONFIG_FRUITCLAW_TELEGRAM_HTTP_TIMEOUT_SEC,
              url, NULL, body, resp, resp_len, status);
#else
      return fc_http_post_json_unlocked_timeout(
              CONFIG_FRUITCLAW_TELEGRAM_HTTP_TIMEOUT_SEC,
              url, NULL, body, resp, resp_len, status);
#endif
    }

  return fc_http_post_json(url, NULL, body, resp, resp_len, status);
}

static int fc_offset_path(char *out, size_t out_len)
{
  return fc_data_path("telegram_offset", out, out_len);
}

static int64_t fc_telegram_load_offset(void)
{
  char path[FC_PATH_LEN];
  char buf[48];
  int64_t offset;

  if (fc_offset_path(path, sizeof(path)) < 0)
    {
      return 0;
    }

  if (fc_read_text_file(path, buf, sizeof(buf), true) < 0)
    {
      return 0;
    }

  offset = strtoll(buf, NULL, 10);
  fc_telegram_record_offset(offset);
  return offset;
}

static int fc_telegram_save_offset(int64_t offset)
{
  char path[FC_PATH_LEN];
  char buf[48];

  if (fc_offset_path(path, sizeof(path)) < 0)
    {
      return -EINVAL;
    }

  snprintf(buf, sizeof(buf), "%lld\n", (long long)offset);
  return fc_write_text_file_atomic(path, buf);
}

static void fc_telegram_ack_offset(int64_t next_offset)
{
  if (next_offset > g_update_offset)
    {
      g_update_offset = next_offset;
      fc_telegram_save_offset(g_update_offset);
      fc_telegram_record_offset(g_update_offset);
    }
}

bool fc_telegram_should_ack_offset(bool ignorable_update, int publish_ret)
{
  return ignorable_update || publish_ret == 0;
}

static void fc_telegram_finish_update(int64_t update_id, int64_t next_offset,
                                      const char *chat_id, bool queued,
                                      bool ignorable_update, int publish_ret)
{
  fc_telegram_record_update(update_id, chat_id, queued);
  if (fc_telegram_should_ack_offset(ignorable_update, publish_ret))
    {
      fc_telegram_ack_offset(next_offset);
    }
}

static bool fc_telegram_chat_allowed(const char *chat_id)
{
  FILE *fp;
  char line[80];
  char path[FC_PATH_LEN];

  if (chat_id == NULL || chat_id[0] == '\0')
    {
      return false;
    }

  if (fc_data_path("telegram_allowed_chats.txt", path, sizeof(path)) < 0)
    {
      return false;
    }

  fp = fopen(path, "r");
  if (fp == NULL)
    {
      return false;
    }

  while (fgets(line, sizeof(line), fp) != NULL)
    {
      fc_trim(line);
      if (line[0] == '\0' || line[0] == '#')
        {
          continue;
        }

      if (strcmp(line, chat_id) == 0)
        {
          fclose(fp);
          return true;
        }
    }

  fclose(fp);
  return false;
}

int fc_telegram_first_allowed_chat(char *out, size_t out_len)
{
  FILE *fp;
  char line[80];
  char path[FC_PATH_LEN];

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  out[0] = '\0';
  if (fc_data_path("telegram_allowed_chats.txt", path, sizeof(path)) < 0)
    {
      return -EINVAL;
    }

  fp = fopen(path, "r");
  if (fp == NULL)
    {
      return -errno;
    }

  while (fgets(line, sizeof(line), fp) != NULL)
    {
      fc_trim(line);
      if (line[0] != '\0' && line[0] != '#')
        {
          fclose(fp);
          return fc_strlcpy(out, line, out_len);
        }
    }

  fclose(fp);
  return -ENOENT;
}

static int fc_telegram_out_append(char *out, size_t out_len, size_t *off,
                                  const char *fmt, ...)
{
  va_list ap;
  int n;

  if (out == NULL || off == NULL || *off >= out_len)
    {
      return -EINVAL;
    }

  va_start(ap, fmt);
  n = vsnprintf(out + *off, out_len - *off, fmt, ap);
  va_end(ap);

  if (n < 0 || *off + n >= out_len)
    {
      return -ENOSPC;
    }

  *off += n;
  return 0;
}

static void fc_telegram_preview_text(const char *in, char *out,
                                     size_t out_len)
{
  size_t i;

  if (out == NULL || out_len == 0)
    {
      return;
    }

  out[0] = '\0';
  if (in == NULL)
    {
      return;
    }

  for (i = 0; i + 1 < out_len && in[i] != '\0'; i++)
    {
      unsigned char ch = (unsigned char)in[i];
      out[i] = ch < 0x20 ? ' ' : (char)ch;
    }

  out[i] = '\0';
}

int fc_telegram_discover_chats(char *out, size_t out_len)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_TELEGRAM
  char token[160];
  char url[320];
  char *resp;
  long status = 0;
  cJSON *root;
  cJSON *result;
  cJSON *item;
  size_t off = 0;
  int found = 0;
  int ret;

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  out[0] = '\0';
  ret = fc_telegram_read_token(token, sizeof(token));
  if (ret < 0 || token[0] == '\0')
    {
      char path[FC_PATH_LEN];
      fc_secret_path("telegram_token", path, sizeof(path));
      snprintf(out, out_len, "Telegram token not found at %s\n",
               path);
      return -ENOENT;
    }

  snprintf(url, sizeof(url),
           "https://api.telegram.org/bot%s/getUpdates?timeout=0&limit=10",
           token);

  resp = calloc(1, CONFIG_FRUITCLAW_MAX_HTTP_BODY);
  if (resp == NULL)
    {
      return -ENOMEM;
    }

  fc_telegram_set_poll_active(true);
  ret = fc_telegram_http_get(url, resp, CONFIG_FRUITCLAW_MAX_HTTP_BODY,
                             &status);
  fc_telegram_set_poll_active(false);
  if (ret < 0)
    {
      free(resp);
      snprintf(out, out_len, "Telegram discover failed: %d\n", ret);
      return ret;
    }

  if (status < 200 || status >= 300)
    {
      snprintf(out, out_len, "Telegram discover HTTP status: %ld body: %s\n",
               status, resp);
      free(resp);
      return -EIO;
    }

  root = cJSON_Parse(resp);
  free(resp);
  if (root == NULL)
    {
      snprintf(out, out_len, "Telegram discover returned invalid JSON\n");
      return -EINVAL;
    }

  result = cJSON_GetObjectItem(root, "result");
  if (cJSON_IsArray(result))
    {
      cJSON_ArrayForEach(item, result)
        {
          cJSON *update_id = cJSON_GetObjectItem(item, "update_id");
          cJSON *msg = cJSON_GetObjectItem(item, "message");
          cJSON *chat = msg ? cJSON_GetObjectItem(msg, "chat") : NULL;
          cJSON *chat_id = chat ? cJSON_GetObjectItem(chat, "id") : NULL;
          const char *text = msg ?
            cJSON_GetStringValue(cJSON_GetObjectItem(msg, "text")) : NULL;
          char preview[80];

          if (!cJSON_IsNumber(update_id) || !cJSON_IsNumber(chat_id))
            {
              continue;
            }

          fc_telegram_preview_text(text, preview, sizeof(preview));
          ret = fc_telegram_out_append(out, out_len, &off,
              "chat_id=%lld update_id=%lld text=\"%s\"\n",
              (long long)chat_id->valuedouble,
              (long long)update_id->valuedouble, preview);
          if (ret < 0)
            {
              cJSON_Delete(root);
              return ret;
            }

          found++;
        }
    }

  cJSON_Delete(root);
  if (found == 0)
    {
      fc_strlcpy(out,
                 "No Telegram updates. Send the bot a text message, then run "
                 "telegram-discover again.\n",
                 out_len);
    }

  return 0;
#else
  if (out != NULL && out_len > 0)
    {
      fc_strlcpy(out, "Telegram support is disabled.\n", out_len);
    }

  return -ENOSYS;
#endif
}

static int fc_telegram_publish_message(const char *chat_id, const char *text,
                                       const char *payload)
{
  fc_event_t ev;
  int ret;

  memset(&ev, 0, sizeof(ev));
  fc_make_id(ev.id, sizeof(ev.id), "tg");
  fc_strlcpy(ev.source, "telegram", sizeof(ev.source));
  fc_strlcpy(ev.type, "message.in", sizeof(ev.type));
  fc_strlcpy(ev.channel, "telegram", sizeof(ev.channel));
  fc_strlcpy(ev.chat_id, chat_id, sizeof(ev.chat_id));
  snprintf(ev.session_id, sizeof(ev.session_id), "telegram:%s", chat_id);
  fc_strlcpy(ev.text, text, sizeof(ev.text));
  fc_strlcpy(ev.payload_json, payload ? payload : "{}", sizeof(ev.payload_json));
  ev.ts_ms = fc_time_ms();
#ifdef CONFIG_FRUITCLAW_OWNER_MODE
  ev.owner_mode = true;
#endif

  ret = fc_queue_publish(fc_main_queue(), &ev);
  if (ret < 0)
    {
      FC_LOGW("telegram event queue full");
    }

  return ret;
}

int fc_telegram_inject_text(const char *chat_id, const char *text)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_TELEGRAM
  char default_chat[FC_CHAT_ID_LEN];
  char payload[192];
  int ret;

  if (text == NULL || text[0] == '\0')
    {
      return -EINVAL;
    }

  if (chat_id == NULL || chat_id[0] == '\0')
    {
      ret = fc_telegram_first_allowed_chat(default_chat,
                                           sizeof(default_chat));
      if (ret < 0)
        {
          return ret;
        }

      chat_id = default_chat;
    }

  if (!fc_telegram_chat_allowed(chat_id))
    {
      return -EACCES;
    }

  snprintf(payload, sizeof(payload), "{\"chat_id\":\"%s\","
           "\"injected\":true}", chat_id);
  return fc_telegram_publish_message(chat_id, text, payload);
#else
  (void)chat_id;
  (void)text;
  return -ENOSYS;
#endif
}

static void fc_telegram_handle_update(cJSON *update)
{
  cJSON *update_id;
  cJSON *msg;
  cJSON *chat;
  cJSON *chat_id_item;
  const char *text;
  char chat_id[FC_CHAT_ID_LEN];
  char payload[192];
  int64_t next_offset;
  int ret;

  update_id = cJSON_GetObjectItem(update, "update_id");
  if (cJSON_IsNumber(update_id))
    {
      next_offset = (int64_t)update_id->valuedouble + 1;
    }
  else
    {
      next_offset = g_update_offset;
    }

  msg = cJSON_GetObjectItem(update, "message");
  if (msg == NULL)
    {
      fc_telegram_finish_update(next_offset - 1, next_offset, NULL, false,
                                true, 0);
      return;
    }

  text = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "text"));
  if (text == NULL)
    {
      fc_telegram_finish_update(next_offset - 1, next_offset, NULL, false,
                                true, 0);
      return;
    }

  chat = cJSON_GetObjectItem(msg, "chat");
  chat_id_item = chat ? cJSON_GetObjectItem(chat, "id") : NULL;
  if (!cJSON_IsNumber(chat_id_item))
    {
      fc_telegram_finish_update(next_offset - 1, next_offset, NULL, false,
                                true, 0);
      return;
    }

  snprintf(chat_id, sizeof(chat_id), "%lld",
           (long long)chat_id_item->valuedouble);

  if (!fc_telegram_chat_allowed(chat_id))
    {
      FC_LOGW("telegram chat not allowed: %s", chat_id);
      fc_telegram_finish_update(next_offset - 1, next_offset, chat_id,
                                false, true, 0);
      return;
    }

  snprintf(payload, sizeof(payload), "{\"chat_id\":\"%s\"}", chat_id);
  ret = fc_telegram_publish_message(chat_id, text, payload);
  if (ret == 0)
    {
      fc_telegram_finish_update(next_offset - 1, next_offset, chat_id,
                                true, false, ret);
    }
  else
    {
      fc_telegram_finish_update(next_offset - 1, next_offset, chat_id,
                                false, false, ret);
    }
}

int fc_telegram_poll_once(void)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_TELEGRAM
  char token[160];
  char url[320];
  char *resp;
  long status = 0;
  cJSON *root;
  cJSON *result;
  cJSON *item;
  int ret;

  ret = fc_telegram_read_token(token, sizeof(token));
  if (ret < 0 || token[0] == '\0')
    {
      fc_telegram_record_poll(-ENOENT, 0);
      return -ENOENT;
    }

  if (g_update_offset < 0)
    {
      g_update_offset = fc_telegram_load_offset();
    }

  snprintf(url, sizeof(url),
           "https://api.telegram.org/bot%s/getUpdates?timeout=%u&offset=%lld",
           token,
           (unsigned int)CONFIG_FRUITCLAW_TELEGRAM_POLL_TIMEOUT_SEC,
           (long long)g_update_offset);

  resp = calloc(1, CONFIG_FRUITCLAW_MAX_HTTP_BODY);
  if (resp == NULL)
    {
      fc_telegram_record_poll(-ENOMEM, 0);
      return -ENOMEM;
    }

  fc_telegram_set_poll_active(true);
  ret = fc_telegram_http_get(url, resp, CONFIG_FRUITCLAW_MAX_HTTP_BODY,
                             &status);
  fc_telegram_set_poll_active(false);
  if (ret < 0)
    {
      free(resp);
      fc_telegram_record_poll(ret, status);
      return ret;
    }

  if (status < 200 || status >= 300)
    {
      free(resp);
      fc_telegram_record_poll(-EIO, status);
      return -EIO;
    }

  root = cJSON_Parse(resp);
  free(resp);
  if (root == NULL)
    {
      fc_telegram_record_poll(-EINVAL, status);
      return -EINVAL;
    }

  result = cJSON_GetObjectItem(root, "result");
  if (cJSON_IsArray(result))
    {
      cJSON_ArrayForEach(item, result)
        {
          fc_telegram_handle_update(item);
        }
    }

  cJSON_Delete(root);
  fc_telegram_record_poll(0, status);
  return 0;
#else
  return -ENOSYS;
#endif
}

static int fc_telegram_send_message_common(const char *chat_id,
                                           const char *text, bool guarded)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_TELEGRAM
  char token[160];
  char url[256];
  char *body;
  char *resp;
  long status = 0;
  cJSON *root;
  int ret;

  if (chat_id == NULL || text == NULL)
    {
      return -EINVAL;
    }

  if (!fc_telegram_chat_allowed(chat_id))
    {
      return -EACCES;
    }

  ret = fc_telegram_read_token(token, sizeof(token));
  if (ret < 0 || token[0] == '\0')
    {
      return -ENOENT;
    }

  root = cJSON_CreateObject();
  if (root == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddStringToObject(root, "chat_id", chat_id);
  cJSON_AddStringToObject(root, "text", text);
  cJSON_AddBoolToObject(root, "disable_web_page_preview", true);
  body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (body == NULL)
    {
      return -ENOMEM;
    }

  resp = calloc(1, CONFIG_FRUITCLAW_MAX_HTTP_BODY);
  if (resp == NULL)
    {
      cJSON_free(body);
      return -ENOMEM;
    }

  snprintf(url, sizeof(url),
           "https://api.telegram.org/bot%s/sendMessage", token);
  ret = fc_telegram_http_post_json(guarded, url, body, resp,
                                   CONFIG_FRUITCLAW_MAX_HTTP_BODY, &status);
  cJSON_free(body);
  if (ret < 0)
    {
      free(resp);
      return ret;
    }

  if (status < 200 || status >= 300)
    {
      FC_LOGW("telegram send HTTP status=%ld body=%s", status, resp);
      free(resp);
      return -EIO;
    }

  fc_operator_progress_mark("telegram-send");
  fc_telegram_record_transport_success("telegram-send", status);
  free(resp);
  return 0;
#else
  return -ENOSYS;
#endif
}

int fc_telegram_send_message(const char *chat_id, const char *text)
{
  /* Telegram sends are operator notifications and agent replies, not board
   * mutation primitives.  Keep them serialized through the shared HTTP lane and
   * arm the long HTTP guard so a stuck TLS/webclient send cannot wedge MCP,
   * telnet, or CDC while the operator is away.
   */

  return fc_telegram_send_message_common(chat_id, text, true);
}

static void *fc_telegram_notify_thread_main(void *arg)
{
  (void)arg;

  for (; ; )
    {
      char text[FC_TG_NOTIFY_SEND_LEN];
      char chat_id[FC_CHAT_ID_LEN];
      unsigned int batched = 0;
      size_t off = 0;
      int ret;

      pthread_mutex_lock(&g_tg_notify_lock);
      while (g_tg_notify_count == 0)
        {
          pthread_cond_wait(&g_tg_notify_cond, &g_tg_notify_lock);
        }

#if CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_COALESCE_MS > 0
      pthread_mutex_unlock(&g_tg_notify_lock);
      fc_telegram_sleep_ms("telegram-notify-coalesce",
                           CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_COALESCE_MS);
      pthread_mutex_lock(&g_tg_notify_lock);
#endif

      pthread_mutex_unlock(&g_tg_notify_lock);
      ret = fc_telegram_first_allowed_chat(chat_id, sizeof(chat_id));
      if (ret >= 0 && !fc_telegram_transport_ready())
        {
          fc_guard_session_heartbeat("telegram-notify-defer");
          fc_telegram_sleep_ms("telegram-notify-defer", 1000);
          continue;
        }

      pthread_mutex_lock(&g_tg_notify_lock);
      text[0] = '\0';
      while (g_tg_notify_count > 0 &&
             batched < CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_BATCH_MAX)
        {
          const char *line = g_tg_notify_queue[g_tg_notify_head];
          size_t line_len = strnlen(line, FC_TG_NOTIFY_TEXT_LEN);

          if (off + line_len + (off > 0 ? 1 : 0) + 1 > sizeof(text))
            {
              break;
            }

          if (off > 0)
            {
              text[off++] = '\n';
            }

          memcpy(text + off, line, line_len);
          off += line_len;
          text[off] = '\0';
          g_tg_notify_head = (g_tg_notify_head + 1) %
                             FC_TG_NOTIFY_QUEUE_LEN;
          g_tg_notify_count--;
          g_tg_notify_inflight++;
          batched++;
        }
      pthread_mutex_unlock(&g_tg_notify_lock);

      if (ret >= 0)
        {
          fc_telegram_notify_yield_to_poller();
          ret = fc_telegram_send_message(chat_id, text);
        }

      fc_telegram_record_notify_result(ret, batched);
      fc_guard_session_heartbeat("telegram-notify");
      if (CONFIG_FRUITCLAW_TELEGRAM_POLL_IDLE_MS > 0)
        {
          usleep((useconds_t)CONFIG_FRUITCLAW_TELEGRAM_POLL_IDLE_MS *
                 USEC_PER_MSEC);
        }
    }

  return NULL;
}

static int fc_telegram_notify_start_locked(void)
{
  pthread_attr_t attr;
  int ret;

  if (g_tg_notify_started)
    {
      return 0;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_FRUITCLAW_WORKER_STACKSIZE);
  ret = pthread_create(&g_tg_notify_thread, &attr,
                       fc_telegram_notify_thread_main, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      return -ret;
    }

  g_tg_notify_started = true;
  return 0;
}

int fc_telegram_notify_owner_async(const char *text)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_TELEGRAM
  unsigned int tail;
  int ret;

  if (text == NULL || text[0] == '\0')
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_tg_notify_lock);
  ret = fc_telegram_notify_start_locked();
  if (ret < 0)
    {
      pthread_mutex_unlock(&g_tg_notify_lock);
      return ret;
    }

  if (g_tg_notify_count >= FC_TG_NOTIFY_QUEUE_LEN)
    {
      pthread_mutex_lock(&g_tg_status_lock);
      g_tg_notify_dropped++;
      pthread_mutex_unlock(&g_tg_status_lock);
      pthread_mutex_unlock(&g_tg_notify_lock);
      return -ENOSPC;
    }

  tail = (g_tg_notify_head + g_tg_notify_count) %
         FC_TG_NOTIFY_QUEUE_LEN;
  fc_strlcpy(g_tg_notify_queue[tail], text,
             sizeof(g_tg_notify_queue[tail]));
  g_tg_notify_count++;
  pthread_mutex_lock(&g_tg_status_lock);
  g_tg_notify_enqueued++;
  pthread_mutex_unlock(&g_tg_status_lock);
  pthread_cond_signal(&g_tg_notify_cond);
  pthread_mutex_unlock(&g_tg_notify_lock);
  return 0;
#else
  (void)text;
  return -ENOSYS;
#endif
}

static void *fc_telegram_thread_main(void *arg)
{
#if CONFIG_FRUITCLAW_TELEGRAM_START_DELAY_MS > 0
  unsigned int waited_ms = 0;
#endif
  unsigned int failure_backoff_ms = 0;
#if CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_MAX_YIELD_MS > 0
  unsigned int notify_yielded_ms = 0;
#endif
#if CONFIG_FRUITCLAW_TELEGRAM_MCP_MAX_YIELD_MS > 0
  unsigned int mcp_yielded_ms = 0;
#endif
  (void)arg;

#if CONFIG_FRUITCLAW_TELEGRAM_START_DELAY_MS > 0
  while (waited_ms < CONFIG_FRUITCLAW_TELEGRAM_START_DELAY_MS)
    {
      unsigned int step_ms = CONFIG_FRUITCLAW_TELEGRAM_START_DELAY_MS -
                             waited_ms;

      if (step_ms > 250)
        {
          step_ms = 250;
        }

      fc_guard_session_heartbeat("telegram-start-delay");
      usleep((useconds_t)step_ms * USEC_PER_MSEC);
      waited_ms += step_ms;
    }
#endif

  fc_telegram_sleep_ms("telegram-boot-quiet",
                       CONFIG_FRUITCLAW_TELEGRAM_BOOT_QUIET_MS);

  for (; ; )
    {
      /* Let the first poll establish Telegram transport before queued MCP
       * notices get priority; otherwise boot-time notices can starve polling.
       */

      if (fc_telegram_notify_pending() && fc_telegram_transport_ready())
        {
          unsigned int step_ms = 250;

#if CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_MAX_YIELD_MS > 0
          if (notify_yielded_ms >=
              CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_MAX_YIELD_MS)
            {
              notify_yielded_ms = 0;
              goto poll_now;
            }

          if (notify_yielded_ms + step_ms >
              CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_MAX_YIELD_MS)
            {
              step_ms = CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_MAX_YIELD_MS -
                        notify_yielded_ms;
            }
#endif

          fc_telegram_sleep_ms("telegram-notify-priority", step_ms);
#if CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_MAX_YIELD_MS > 0
          notify_yielded_ms += step_ms;
#endif
          continue;
        }

#if CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_MAX_YIELD_MS > 0
      notify_yielded_ms = 0;
#endif

      if (fc_mcp_recently_active(fc_mono_ms(),
                                 CONFIG_FRUITCLAW_TELEGRAM_MCP_YIELD_MS))
        {
          unsigned int step_ms = 250;

#if CONFIG_FRUITCLAW_TELEGRAM_MCP_MAX_YIELD_MS > 0
          if (mcp_yielded_ms >= CONFIG_FRUITCLAW_TELEGRAM_MCP_MAX_YIELD_MS)
            {
              mcp_yielded_ms = 0;
              goto poll_now;
            }

          if (mcp_yielded_ms + step_ms >
              CONFIG_FRUITCLAW_TELEGRAM_MCP_MAX_YIELD_MS)
            {
              step_ms = CONFIG_FRUITCLAW_TELEGRAM_MCP_MAX_YIELD_MS -
                        mcp_yielded_ms;
            }
#endif

          fc_telegram_sleep_ms("telegram-mcp-yield", step_ms);
#if CONFIG_FRUITCLAW_TELEGRAM_MCP_MAX_YIELD_MS > 0
          mcp_yielded_ms += step_ms;
#endif
          continue;
        }

#if CONFIG_FRUITCLAW_TELEGRAM_MCP_MAX_YIELD_MS > 0 || \
    CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_MAX_YIELD_MS > 0
poll_now:
#endif
      int ret = fc_telegram_poll_once();
#if CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_MAX_YIELD_MS > 0
      notify_yielded_ms = 0;
#endif
#if CONFIG_FRUITCLAW_TELEGRAM_MCP_MAX_YIELD_MS > 0
      mcp_yielded_ms = 0;
#endif
      fc_guard_session_heartbeat("telegram");
      if (ret < 0)
        {
          FC_LOGW("telegram poll failed: %d", ret);
          if (failure_backoff_ms == 0)
            {
              failure_backoff_ms =
                CONFIG_FRUITCLAW_TELEGRAM_FAIL_BACKOFF_MIN_MS;
            }
          else if (failure_backoff_ms <
                   CONFIG_FRUITCLAW_TELEGRAM_FAIL_BACKOFF_MAX_MS / 2)
            {
              failure_backoff_ms *= 2;
            }
          else
            {
              failure_backoff_ms =
                CONFIG_FRUITCLAW_TELEGRAM_FAIL_BACKOFF_MAX_MS;
            }

          fc_telegram_sleep_ms("telegram-backoff", failure_backoff_ms);
        }
      else
        {
          failure_backoff_ms = 0;
          if (fc_telegram_poll_idle_ms() > 0)
            {
              fc_telegram_sleep_ms("telegram-idle",
                                   fc_telegram_poll_idle_ms());
            }
        }
    }

  return NULL;
}

int fc_telegram_worker_start(void)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_TELEGRAM
  pthread_attr_t attr;
  int ret;

  if (g_tg_started)
    {
      return 0;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_FRUITCLAW_WORKER_STACKSIZE);
  ret = pthread_create(&g_tg_thread, &attr, fc_telegram_thread_main, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      return -ret;
    }

  g_tg_started = true;
  return 0;
#else
  return -ENOSYS;
#endif
}

int fc_telegram_status_format(char *out, size_t out_len)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_TELEGRAM
  unsigned long poll_count;
  unsigned long fail_count;
  unsigned long update_count;
  unsigned long queued_count;
  unsigned long ignored_count;
  unsigned long notify_enqueued;
  unsigned long notify_dropped;
  unsigned long notify_sent;
  unsigned long notify_failed;
  unsigned int notify_count;
  unsigned int notify_inflight;
  bool poll_active;
  int64_t poll_ms;
  int64_t success_ms;
  int64_t update_id;
  int64_t saved_offset;
  int ret;
  long http_status;
  char chat_id[FC_CHAT_ID_LEN];
  int64_t now = fc_mono_ms();

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_tg_status_lock);
  poll_count = g_tg_poll_count;
  fail_count = g_tg_poll_fail_count;
  update_count = g_tg_update_count;
  queued_count = g_tg_queued_count;
  ignored_count = g_tg_ignored_count;
  notify_enqueued = g_tg_notify_enqueued;
  notify_dropped = g_tg_notify_dropped;
  notify_sent = g_tg_notify_sent;
  notify_failed = g_tg_notify_failed;
  poll_active = g_tg_poll_active;
  poll_ms = g_tg_last_poll_ms;
  success_ms = g_tg_last_success_ms;
  update_id = g_tg_last_update_id;
  saved_offset = g_tg_last_saved_offset;
  ret = g_tg_last_ret;
  http_status = g_tg_last_http_status;
  fc_strlcpy(chat_id, g_tg_last_chat_id, sizeof(chat_id));
  pthread_mutex_unlock(&g_tg_status_lock);

  if (pthread_mutex_trylock(&g_tg_notify_lock) == 0)
    {
      notify_count = g_tg_notify_count;
      notify_inflight = g_tg_notify_inflight;
      pthread_mutex_unlock(&g_tg_notify_lock);
    }
  else
    {
      notify_count = 0;
      notify_inflight = 0;
    }

  snprintf(out, out_len,
           "telegram_status: worker=%s polls=%lu fails=%lu "
           "last_ret=%d http=%ld last_poll_age_ms=%lld "
           "last_success_age_ms=%lld offset=%lld saved_offset=%lld "
           "poll_active=%s updates=%lu queued=%lu ignored=%lu "
           "last_update=%lld last_chat=%s notify_queue=%u "
           "notify_inflight=%u "
           "notify_enqueued=%lu notify_sent=%lu notify_failed=%lu "
           "notify_dropped=%lu",
           g_tg_started ? "started" : "stopped",
           poll_count, fail_count, ret, http_status,
           poll_ms > 0 ? (long long)(now - poll_ms) : -1,
           success_ms > 0 ? (long long)(now - success_ms) : -1,
           (long long)g_update_offset, (long long)saved_offset,
           poll_active ? "yes" : "no",
           update_count, queued_count, ignored_count,
           (long long)update_id, chat_id[0] ? chat_id : "-",
           notify_count, notify_inflight, notify_enqueued, notify_sent,
           notify_failed, notify_dropped);
  return 0;
#else
  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  snprintf(out, out_len, "telegram_status: disabled");
  return 0;
#endif
}

int64_t fc_telegram_last_success_age_ms(void)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_TELEGRAM
  int64_t success_ms;

  pthread_mutex_lock(&g_tg_status_lock);
  success_ms = g_tg_last_success_ms;
  pthread_mutex_unlock(&g_tg_status_lock);

  if (success_ms <= 0)
    {
      return -1;
    }

  return fc_mono_ms() - success_ms;
#else
  return -1;
#endif
}

void fc_telegram_status(FILE *out)
{
  char status[512];

  if (out == NULL)
    {
      return;
    }

  if (fc_telegram_status_format(status, sizeof(status)) == 0)
    {
      fprintf(out, "  %s\n", status);
    }
}
