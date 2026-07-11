/* SPDX-License-Identifier: Apache-2.0 */

#include "fruitclaw.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netutils/cJSON.h"

static pthread_t g_agent_thread;
static bool g_agent_started;
static pthread_mutex_t g_agent_status_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_agent_events_started;
static unsigned long g_agent_events_done;
static unsigned long g_agent_event_failures;
static unsigned long g_agent_tool_calls;
static unsigned long g_agent_tool_failures;
static unsigned long g_agent_replies_sent;
static unsigned long g_agent_reply_failures;
static int64_t g_agent_last_event_ms;
static int64_t g_agent_last_done_ms;
static int64_t g_agent_last_reply_ms;
static int g_agent_last_ret;
static int g_agent_last_reply_ret;
static char g_agent_last_source[FC_SOURCE_LEN];
static char g_agent_last_channel[FC_CHANNEL_LEN];
static char g_agent_last_session[FC_SESSION_ID_LEN];
static char g_agent_last_tool[FC_TOOL_NAME_LEN];

static void fc_agent_record_tool_result(const char *id, int ret);

static const char *fc_agent_skip_space(const char *s)
{
  if (s == NULL)
    {
      return "";
    }

  while (*s != '\0' && isspace((unsigned char)*s))
    {
      s++;
    }

  return s;
}

static bool fc_agent_match_prefix_ci(const char *s, const char *prefix,
                                     const char **after)
{
  size_t i;

  if (s == NULL || prefix == NULL)
    {
      return false;
    }

  for (i = 0; prefix[i] != '\0'; i++)
    {
      if (s[i] == '\0' ||
          tolower((unsigned char)s[i]) !=
          tolower((unsigned char)prefix[i]))
        {
          return false;
        }
    }

  if (after != NULL)
    {
      *after = s + i;
    }

  return true;
}

static const char *fc_agent_find_marker_ci(const char *s,
                                           const char *marker)
{
  size_t marker_len;
  const char *p;

  if (s == NULL || marker == NULL)
    {
      return NULL;
    }

  marker_len = strlen(marker);
  if (marker_len == 0)
    {
      return NULL;
    }

  for (p = s; *p != '\0'; p++)
    {
      size_t i;

      for (i = 0; i < marker_len; i++)
        {
          if (p[i] == '\0' ||
              tolower((unsigned char)p[i]) !=
              tolower((unsigned char)marker[i]))
            {
              break;
            }
        }

      if (i == marker_len)
        {
          return p + marker_len;
        }
    }

  return NULL;
}

static const char *fc_agent_schedule_agent_text(const char *text)
{
  const char *s;
  const char *after;

  s = fc_agent_skip_space(text);
  if (fc_agent_match_prefix_ci(s, "agent:", &after) ||
      fc_agent_match_prefix_ci(s, "llm:", &after) ||
      fc_agent_match_prefix_ci(s, "tool:", &after))
    {
      return fc_agent_skip_space(after);
    }

  return text ? text : "";
}

static const char *fc_agent_schedule_direct_text(const char *text)
{
  const char *s;
  const char *marker;

  s = fc_agent_skip_space(text);
  if (fc_agent_match_prefix_ci(s, "agent:", NULL) ||
      fc_agent_match_prefix_ci(s, "llm:", NULL) ||
      fc_agent_match_prefix_ci(s, "tool:", NULL))
    {
      return NULL;
    }

  marker = fc_agent_find_marker_ci(s, "exactly:");
  if (marker != NULL)
    {
      return fc_agent_skip_space(marker);
    }

  if (fc_agent_match_prefix_ci(s, "reply:", &marker))
    {
      return fc_agent_skip_space(marker);
    }

  return s;
}

static bool fc_agent_try_direct_scheduler_reply(const fc_event_t *ev,
                                                char *reply,
                                                size_t reply_len)
{
  const char *text;

  if (ev == NULL || reply == NULL || reply_len == 0 ||
      strcmp(ev->source, "scheduler") != 0 ||
      strcmp(ev->type, "timer.fire") != 0)
    {
      return false;
    }

  text = fc_agent_schedule_direct_text(ev->text);
  if (text == NULL)
    {
      return false;
    }

  if (text[0] == '\0')
    {
      text = "Scheduled task fired.";
    }

  fc_strlcpy(reply, text, reply_len);
  fc_session_append(ev->session_id, "user", NULL, ev->text);
  fc_session_append(ev->session_id, "assistant", NULL, reply);
  fc_operator_progress_mark("agent-schedule-direct");
  return true;
}

static bool fc_agent_try_direct_scheduler_tool(const fc_event_t *ev,
                                               char *reply,
                                               size_t reply_len)
{
  const char *text;
  const char *after;
  const char *args;
  char tool[FC_TOOL_NAME_LEN];
  char result[CONFIG_FRUITCLAW_MAX_JSON];
  fc_tool_context_t ctx;
  size_t len = 0;
  int ret;

  if (ev == NULL || reply == NULL || reply_len == 0 ||
      strcmp(ev->source, "scheduler") != 0 ||
      strcmp(ev->type, "timer.fire") != 0)
    {
      return false;
    }

  text = fc_agent_skip_space(ev->text);
  if (!fc_agent_match_prefix_ci(text, "tool:", &after))
    {
      return false;
    }

  after = fc_agent_skip_space(after);
  while (after[len] != '\0' && !isspace((unsigned char)after[len]) &&
         len + 1 < sizeof(tool))
    {
      tool[len] = after[len];
      len++;
    }

  tool[len] = '\0';
  if (tool[0] == '\0')
    {
      fc_strlcpy(reply, "Scheduled tool failed: missing tool name.",
                 reply_len);
      fc_session_append(ev->session_id, "user", NULL, ev->text);
      fc_session_append(ev->session_id, "assistant", NULL, reply);
      fc_operator_progress_mark("agent-schedule-tool-bad");
      return true;
    }

  args = fc_agent_skip_space(after + len);
  if (args[0] == '\0')
    {
      args = "{}";
    }

  fc_tool_context_from_event(ev, &ctx);
  ctx.guarded = true;
  ret = fc_cap_execute_ctx(&ctx, tool, args, result, sizeof(result));
  fc_agent_record_tool_result(tool, ret);
  snprintf(reply, reply_len, "Scheduled tool %s -> %s: %.512s",
           tool, ret == 0 ? "ok" : "failed", result);
  fc_session_append(ev->session_id, "user", NULL, ev->text);
  fc_session_append(ev->session_id, "tool", tool, result);
  fc_session_append(ev->session_id, "assistant", NULL, reply);
  fc_operator_progress_mark(ret == 0 ? "agent-schedule-tool-ok" :
                                     "agent-schedule-tool-fail");
  return true;
}

static void fc_agent_record_event_start(const fc_event_t *ev)
{
  fc_operator_progress_mark("agent-start");
  pthread_mutex_lock(&g_agent_status_lock);
  g_agent_events_started++;
  g_agent_last_event_ms = fc_mono_ms();
  if (ev != NULL)
    {
      fc_strlcpy(g_agent_last_source, ev->source,
                 sizeof(g_agent_last_source));
      fc_strlcpy(g_agent_last_channel, ev->channel,
                 sizeof(g_agent_last_channel));
      fc_strlcpy(g_agent_last_session, ev->session_id,
                 sizeof(g_agent_last_session));
    }

  pthread_mutex_unlock(&g_agent_status_lock);
}

static void fc_agent_record_event_done(int ret)
{
  fc_operator_progress_mark(ret < 0 ? "agent-fail" : "agent-done");
  pthread_mutex_lock(&g_agent_status_lock);
  g_agent_events_done++;
  g_agent_last_done_ms = fc_mono_ms();
  g_agent_last_ret = ret;
  if (ret < 0)
    {
      g_agent_event_failures++;
    }

  pthread_mutex_unlock(&g_agent_status_lock);
}

static void fc_agent_record_tool_result(const char *id, int ret)
{
  fc_operator_progress_mark(ret < 0 ? "agent-tool-fail" : "agent-tool-ok");
  pthread_mutex_lock(&g_agent_status_lock);
  g_agent_tool_calls++;
  if (ret < 0)
    {
      g_agent_tool_failures++;
    }

  fc_strlcpy(g_agent_last_tool, id ? id : "unknown",
             sizeof(g_agent_last_tool));
  pthread_mutex_unlock(&g_agent_status_lock);
}

static void fc_agent_record_reply_result(int ret)
{
  fc_operator_progress_mark(ret < 0 ? "agent-reply-fail" :
                                      "agent-reply-ok");
  pthread_mutex_lock(&g_agent_status_lock);
  g_agent_replies_sent++;
  g_agent_last_reply_ms = fc_mono_ms();
  g_agent_last_reply_ret = ret;
  if (ret < 0)
    {
      g_agent_reply_failures++;
    }

  pthread_mutex_unlock(&g_agent_status_lock);
}

static void fc_add_message(cJSON *messages, const char *role,
                           const char *content)
{
  cJSON *msg = cJSON_CreateObject();
  if (msg == NULL)
    {
      return;
    }

  cJSON_AddStringToObject(msg, "role", role);
  cJSON_AddStringToObject(msg, "content", content ? content : "");
  cJSON_AddItemToArray(messages, msg);
}

static char *fc_build_system_context(const fc_event_t *ev)
{
  char *system_md = calloc(1, 2048);
  char *user_md = calloc(1, 1024);
  char *memory = calloc(1, 2048);
  char *session = calloc(1, 2048);
  char *ctx = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  char system_path[FC_PATH_LEN];
  char user_path[FC_PATH_LEN];

  if (system_md == NULL || user_md == NULL || memory == NULL ||
      session == NULL || ctx == NULL)
    {
      free(system_md);
      free(user_md);
      free(memory);
      free(session);
      free(ctx);
      return NULL;
    }

  fc_data_path("system.md", system_path, sizeof(system_path));
  fc_data_path("user.md", user_path, sizeof(user_path));
  fc_read_text_file(system_path, system_md, 2048, false);
  fc_read_text_file(user_path, user_md, 1024, false);
  fc_memory_read_tail(1800, memory, 2048);
  fc_session_read_tail(ev->session_id, 1800, session, 2048);

  snprintf(ctx, CONFIG_FRUITCLAW_MAX_JSON,
           "%s\n%s\n\nDevice: FruitClaw on NuttX RP2350.\n"
           "Current unix_ms: %lld.\n"
           "Current turn: source=%s channel=%s chat_id=%s session=%s "
           "owner_mode=%s.\n"
           "Safety: never reveal secrets, credentials, authorization headers, "
           "or secret file contents. Use tools directly when useful. "
           "When owner_mode is true, the user is the device owner and you may "
           "run terminal, scheduler, Berry, device, and NeoPixel tools without "
           "asking for extra confirmation. For scheduled Telegram reminders, "
           "use scheduler.add; the tool inherits this chat/session. Simple "
           "schedule prompts are delivered directly at fire time. Prefix a "
           "scheduled prompt with agent: only when the scheduled job should "
           "wake the LLM and use tools. For reusable Berry or NSH automation, "
           "use script.write with kind=berry or kind=shell and a short "
           "description; generated scripts live in scripts/generated/. FTP "
           "uploads live at scripts/name.be or scripts/name.nsh and can be "
           "read, validated, run, and scheduled by path. Then use "
           "script.validate, script.run, script.schedule, script.read, "
           "script.list, and script.remove to test, rework, schedule, inspect, "
           "or clean them up. script.list and script.read expose last "
           "validation status for scripts. For LVGL or other long-running "
           "Berry UI "
           "scripts, use script.write validate_mode=syntax or "
           "script.validate mode=syntax so validation parses the script "
           "without executing lv.run() forever; schedule boot UIs with "
           "script.schedule type=boot after syntax validation. Use Berry for "
           "small board workflows, NeoPixel effects, RTTTL, LVGL UI "
           "experiments, and maintenance scripts. In "
           "Berry, prefer the constrained claw helpers: reply(), tool(), "
           "memory_append(), terminal_run(), neopixels_set(), "
           "neopixels_off(), neopixels_effect(), schedule_add(), "
           "script_run(), rtttl_play(), service_control(), and "
           "telegram_send(). Use the global lv module for LVGL widgets when "
           "Berry LVGL is compiled. Keep terminal commands bounded and "
           "return concise summaries.\n\n"
           "Memory JSONL tail:\n%s\n\nSession JSONL tail:\n%s\n",
           system_md, user_md, (long long)fc_time_ms(),
           ev->source, ev->channel, ev->chat_id, ev->session_id,
           ev->owner_mode ? "true" : "false", memory, session);

  free(system_md);
  free(user_md);
  free(memory);
  free(session);
  return ctx;
}

static int fc_execute_tool_call(cJSON *call, cJSON *messages,
                                const fc_event_t *ev)
{
  cJSON *fn = cJSON_GetObjectItem(call, "function");
  const char *call_id = cJSON_GetStringValue(cJSON_GetObjectItem(call, "id"));
  const char *api_name = cJSON_GetStringValue(cJSON_GetObjectItem(fn, "name"));
  const char *args = cJSON_GetStringValue(cJSON_GetObjectItem(fn,
                                                             "arguments"));
  char cap_id[FC_TOOL_NAME_LEN];
  char result[CONFIG_FRUITCLAW_MAX_JSON];
  fc_tool_context_t ctx;
  cJSON *tool_msg;
  int ret = -ENOENT;

  if (call_id == NULL)
    {
      call_id = "tool-call";
    }

  if (api_name == NULL ||
      fc_tool_name_from_openai(api_name, cap_id, sizeof(cap_id)) < 0)
    {
      snprintf(result, sizeof(result),
               "{\"ok\":false,\"error\":\"unknown tool\"}");
      fc_strlcpy(cap_id, api_name ? api_name : "unknown", sizeof(cap_id));
    }
  else
    {
      fc_tool_context_from_event(ev, &ctx);
      ret = fc_cap_execute_ctx(&ctx, cap_id, args ? args : "{}", result,
                               sizeof(result));
      if (ret < 0 && result[0] == '\0')
        {
          snprintf(result, sizeof(result),
                   "{\"ok\":false,\"error\":\"tool failed\"}");
        }
    }

  fc_agent_record_tool_result(cap_id, ret);
  tool_msg = cJSON_CreateObject();
  if (tool_msg == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddStringToObject(tool_msg, "role", "tool");
  cJSON_AddStringToObject(tool_msg, "tool_call_id", call_id);
  cJSON_AddStringToObject(tool_msg, "content", result);
  cJSON_AddItemToArray(messages, tool_msg);
  fc_session_append(ev->session_id, "tool", cap_id, result);
  return 0;
}

int fc_agent_handle_event(const fc_event_t *ev, char *reply,
                          size_t reply_len)
{
  cJSON *messages;
  cJSON *assistant_msg = NULL;
  fc_guard_long_t guard;
  char *system_ctx;
  const char *content = NULL;
  const char *user_text;
  int iter;
  int ret = 0;
  bool guarded = false;

  if (ev == NULL || reply == NULL || reply_len == 0)
    {
      return -EINVAL;
    }

  fc_agent_record_event_start(ev);
  reply[0] = '\0';

  if (fc_agent_try_direct_scheduler_tool(ev, reply, reply_len))
    {
      fc_agent_record_event_done(0);
      return 0;
    }

  if (fc_agent_try_direct_scheduler_reply(ev, reply, reply_len))
    {
      fc_agent_record_event_done(0);
      return 0;
    }

  user_text = ev->text;
  if (strcmp(ev->source, "scheduler") == 0 &&
      strcmp(ev->type, "timer.fire") == 0)
    {
      user_text = fc_agent_schedule_agent_text(ev->text);
    }

  system_ctx = fc_build_system_context(ev);
  if (system_ctx == NULL)
    {
      fc_agent_record_event_done(-ENOMEM);
      return -ENOMEM;
    }

  messages = cJSON_CreateArray();
  if (messages == NULL)
    {
      free(system_ctx);
      fc_agent_record_event_done(-ENOMEM);
      return -ENOMEM;
    }

#if CONFIG_FRUITCLAW_AGENT_GUARD_TIMEOUT_MS > 0
  ret = fc_guard_long_start(FC_GUARD_STAGE_AGENT,
                            CONFIG_FRUITCLAW_AGENT_GUARD_TIMEOUT_MS,
                            &guard);
  if (ret < 0)
    {
      snprintf(reply, reply_len, "Agent guard unavailable: %d", ret);
      cJSON_Delete(messages);
      free(system_ctx);
      fc_agent_record_event_done(ret);
      return ret;
    }

  guarded = true;
#endif
  fc_add_message(messages, "system", system_ctx);
  fc_add_message(messages, "user", user_text);
  fc_session_append(ev->session_id, "user", NULL, user_text);
  free(system_ctx);

  for (iter = 0; iter < CONFIG_FRUITCLAW_MAX_TOOL_ITERATIONS; iter++)
    {
      cJSON *tool_calls;

      fc_operator_progress_mark("agent-llm");
      ret = fc_deepseek_chat(messages, (void **)&assistant_msg);
      if (ret < 0)
        {
          snprintf(reply, reply_len, "LLM request failed: %d", ret);
          break;
        }

      cJSON_AddItemToArray(messages, assistant_msg);
      tool_calls = cJSON_GetObjectItem(assistant_msg, "tool_calls");
      if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0)
        {
          cJSON *call;
          cJSON_ArrayForEach(call, tool_calls)
            {
              fc_execute_tool_call(call, messages, ev);
            }

          assistant_msg = NULL;
          continue;
        }

      content = cJSON_GetStringValue(cJSON_GetObjectItem(assistant_msg,
                                                         "content"));
      fc_strlcpy(reply, content && content[0] != '\0' ?
                 content : "(no response)", reply_len);
      fc_session_append(ev->session_id, "assistant", NULL, reply);
      assistant_msg = NULL;
      break;
    }

  if (reply[0] == '\0' && ret == 0)
    {
      fc_strlcpy(reply, "Tool iteration limit reached.", reply_len);
      fc_session_append(ev->session_id, "assistant", NULL, reply);
    }

  fc_operator_progress_mark("agent-cleanup");
  cJSON_Delete(messages);
  if (guarded)
    {
      int guard_ret = fc_guard_long_stop(&guard);
      if (guard_ret < 0 && ret == 0)
        {
          ret = guard_ret;
        }
    }

  fc_agent_record_event_done(ret);
  return ret;
}

static void fc_agent_reply(const fc_event_t *ev, const char *reply)
{
  int ret = 0;

  fc_operator_progress_mark("agent-reply");
  if (strcmp(ev->channel, "telegram") == 0 && ev->chat_id[0] != '\0')
    {
      ret = fc_telegram_send_message(ev->chat_id, reply);
      if (ret < 0)
        {
          FC_LOGW("telegram reply failed: %d", ret);
        }
    }
  else
    {
      printf("%s\n", reply);
    }

  fc_agent_record_reply_result(ret);
}

static void *fc_agent_thread_main(void *arg)
{
  fc_event_t ev;
  char *reply;

  (void)arg;
  reply = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  if (reply == NULL)
    {
      FC_LOGE("agent reply allocation failed");
      return NULL;
    }

  for (; ; )
    {
      if (fc_queue_receive(fc_agent_queue(), &ev, -1) == 0)
        {
          fc_guard_session_heartbeat("agent");
          if (fc_agent_handle_event(&ev, reply,
                                    CONFIG_FRUITCLAW_MAX_JSON) == 0 ||
              reply[0] != '\0')
            {
              fc_agent_reply(&ev, reply);
            }

          fc_guard_session_heartbeat("agent");
          reply[0] = '\0';
        }
    }

  free(reply);
  return NULL;
}

int fc_agent_worker_start(void)
{
  pthread_attr_t attr;
  int ret;

  if (g_agent_started)
    {
      return 0;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_FRUITCLAW_WORKER_STACKSIZE);
  ret = pthread_create(&g_agent_thread, &attr, fc_agent_thread_main, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      return -ret;
    }

  g_agent_started = true;
  return 0;
}

int fc_agent_status_format(char *out, size_t out_len)
{
  unsigned long started;
  unsigned long done;
  unsigned long failures;
  unsigned long tool_calls;
  unsigned long tool_failures;
  unsigned long replies;
  unsigned long reply_failures;
  int64_t event_ms;
  int64_t done_ms;
  int64_t reply_ms;
  int64_t now = fc_mono_ms();
  int last_ret;
  int last_reply_ret;
  char source[FC_SOURCE_LEN];
  char channel[FC_CHANNEL_LEN];
  char session[FC_SESSION_ID_LEN];
  char tool[FC_TOOL_NAME_LEN];

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_agent_status_lock);
  started = g_agent_events_started;
  done = g_agent_events_done;
  failures = g_agent_event_failures;
  tool_calls = g_agent_tool_calls;
  tool_failures = g_agent_tool_failures;
  replies = g_agent_replies_sent;
  reply_failures = g_agent_reply_failures;
  event_ms = g_agent_last_event_ms;
  done_ms = g_agent_last_done_ms;
  reply_ms = g_agent_last_reply_ms;
  last_ret = g_agent_last_ret;
  last_reply_ret = g_agent_last_reply_ret;
  fc_strlcpy(source, g_agent_last_source[0] ? g_agent_last_source : "-",
             sizeof(source));
  fc_strlcpy(channel, g_agent_last_channel[0] ? g_agent_last_channel : "-",
             sizeof(channel));
  fc_strlcpy(session, g_agent_last_session[0] ? g_agent_last_session : "-",
             sizeof(session));
  fc_strlcpy(tool, g_agent_last_tool[0] ? g_agent_last_tool : "-",
             sizeof(tool));
  pthread_mutex_unlock(&g_agent_status_lock);

  snprintf(out, out_len,
           "agent_status: worker=%s started=%lu done=%lu failures=%lu "
           "tools=%lu tool_failures=%lu replies=%lu reply_failures=%lu "
           "last_ret=%d last_reply_ret=%d last_event_age_ms=%lld "
           "last_done_age_ms=%lld last_reply_age_ms=%lld "
           "last=%s/%s session=%s tool=%s",
           g_agent_started ? "started" : "stopped",
           started, done, failures, tool_calls, tool_failures,
           replies, reply_failures, last_ret, last_reply_ret,
           event_ms > 0 ? (long long)(now - event_ms) : -1,
           done_ms > 0 ? (long long)(now - done_ms) : -1,
           reply_ms > 0 ? (long long)(now - reply_ms) : -1,
           source, channel, session, tool);
  return 0;
}

void fc_agent_status(FILE *out)
{
  char status[512];

  if (out == NULL)
    {
      return;
    }

  if (fc_agent_status_format(status, sizeof(status)) == 0)
    {
      fprintf(out, "  %s\n", status);
    }
}
