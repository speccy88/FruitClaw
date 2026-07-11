/* SPDX-License-Identifier: Apache-2.0 */

#include "fruitclaw.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "netutils/cJSON.h"

#define FC_SCHED_LOCK_TIMEOUT_MS 2000
#define FC_SCHED_SCRIPT_RUN_PROMPT_PRE "tool:script.run "
#define FC_SCHED_BERRY_RUN_PROMPT_PRE "tool:berry.run_script "

static fc_schedule_t g_jobs[FC_MAX_SCHEDULES];
static pthread_mutex_t g_sched_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_sched_status_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_sched_thread;
static bool g_sched_started;
static unsigned long g_sched_tick_count;
static unsigned long g_sched_emit_count;
static unsigned long g_sched_emit_fail_count;
static int64_t g_sched_last_tick_ms;
static int64_t g_sched_last_fire_ms;
static int g_sched_last_ret;
static char g_sched_last_job[48];

static void fc_scheduler_add_ms(struct timespec *ts, uint32_t ms)
{
  ts->tv_sec += ms / 1000;
  ts->tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L)
    {
      ts->tv_sec++;
      ts->tv_nsec -= 1000000000L;
    }
}

static int fc_scheduler_lock_timed(void)
{
  struct timespec deadline;
  int ret;

  if (clock_gettime(CLOCK_REALTIME, &deadline) < 0)
    {
      ret = pthread_mutex_lock(&g_sched_lock);
      return ret == 0 ? 0 : -ret;
    }

  fc_scheduler_add_ms(&deadline, FC_SCHED_LOCK_TIMEOUT_MS);
  ret = pthread_mutex_timedlock(&g_sched_lock, &deadline);
  if (ret == ETIMEDOUT)
    {
      FC_LOGW("scheduler lock busy for %u ms",
              (unsigned int)FC_SCHED_LOCK_TIMEOUT_MS);
      return -ETIMEDOUT;
    }

  return ret == 0 ? 0 : -ret;
}

static void fc_scheduler_record_tick(void)
{
  pthread_mutex_lock(&g_sched_status_lock);
  g_sched_tick_count++;
  g_sched_last_tick_ms = fc_mono_ms();
  pthread_mutex_unlock(&g_sched_status_lock);
}

static void fc_scheduler_record_emit(const char *id, int ret)
{
  pthread_mutex_lock(&g_sched_status_lock);
  g_sched_emit_count++;
  g_sched_last_fire_ms = fc_mono_ms();
  g_sched_last_ret = ret;
  if (ret < 0)
    {
      g_sched_emit_fail_count++;
    }
  else
    {
      fc_operator_progress_mark("scheduler");
    }

  fc_strlcpy(g_sched_last_job, id ? id : "-", sizeof(g_sched_last_job));
  pthread_mutex_unlock(&g_sched_status_lock);
}

static int fc_schedules_path(char *out, size_t out_len)
{
  return fc_data_path("schedules.json", out, out_len);
}

static fc_schedule_t *fc_find_job(const char *id)
{
  unsigned int i;

  if (id == NULL)
    {
      return NULL;
    }

  for (i = 0; i < FC_MAX_SCHEDULES; i++)
    {
      if (g_jobs[i].used && strcmp(g_jobs[i].id, id) == 0)
        {
          return &g_jobs[i];
        }
    }

  return NULL;
}

static fc_schedule_t *fc_alloc_job(const char *id)
{
  unsigned int i;
  fc_schedule_t *job = fc_find_job(id);

  if (job != NULL)
    {
      return job;
    }

  for (i = 0; i < FC_MAX_SCHEDULES; i++)
    {
      if (!g_jobs[i].used)
        {
          memset(&g_jobs[i], 0, sizeof(g_jobs[i]));
          g_jobs[i].used = true;
          g_jobs[i].enabled = true;
          g_jobs[i].last_minute_key = -1;
          return &g_jobs[i];
        }
    }

  return NULL;
}

static void fc_schedule_next_interval(fc_schedule_t *job)
{
  job->next_ms = fc_time_ms() + (int64_t)job->every_sec * 1000;
}

static int fc_parse_uint(const char *s, int *out)
{
  char *end;
  long v;

  if (s == NULL || *s == '\0')
    {
      return -EINVAL;
    }

  v = strtol(s, &end, 10);
  if (*end != '\0')
    {
      return -EINVAL;
    }

  *out = (int)v;
  return 0;
}

static int fc_cron_parse_fields(const char *expr, char *copy,
                                size_t copy_len, char *fields[5])
{
  char *saveptr = NULL;
  char *tok;
  int n = 0;

  if (expr == NULL || copy == NULL || fields == NULL ||
      fc_strlcpy(copy, expr, copy_len) < 0)
    {
      return -EINVAL;
    }

  for (tok = strtok_r(copy, " \t", &saveptr);
       tok != NULL;
       tok = strtok_r(NULL, " \t", &saveptr))
    {
      if (n >= 5)
        {
          return -EINVAL;
        }

      fields[n++] = tok;
    }

  return n == 5 ? 0 : -EINVAL;
}

static bool fc_field_token_valid(char *tok, int minv, int maxv)
{
  char *slash;
  char *dash;
  int step = 1;
  int start = minv;
  int end = maxv;
  int single;

  if (tok == NULL || tok[0] == '\0')
    {
      return false;
    }

  slash = strchr(tok, '/');
  if (slash != NULL)
    {
      *slash++ = '\0';
      if (tok[0] == '\0' || slash[0] == '\0' ||
          fc_parse_uint(slash, &step) < 0 || step <= 0)
        {
          return false;
        }
    }

  if (strcmp(tok, "*") != 0)
    {
      dash = strchr(tok, '-');
      if (dash != NULL)
        {
          *dash++ = '\0';
          if (tok[0] == '\0' || dash[0] == '\0' ||
              fc_parse_uint(tok, &start) < 0 ||
              fc_parse_uint(dash, &end) < 0)
            {
              return false;
            }
        }
      else if (fc_parse_uint(tok, &single) == 0)
        {
          start = single;
          end = single;
        }
      else
        {
          return false;
        }
    }

  return start >= minv && end <= maxv && start <= end;
}

static bool fc_field_valid(const char *field, int minv, int maxv)
{
  char copy[48];
  char *tok;
  char *comma;

  if (field == NULL || field[0] == '\0' ||
      fc_strlcpy(copy, field, sizeof(copy)) < 0)
    {
      return false;
    }

  tok = copy;
  for (; ; )
    {
      comma = strchr(tok, ',');
      if (comma != NULL)
        {
          *comma = '\0';
        }

      if (!fc_field_token_valid(tok, minv, maxv))
        {
          return false;
        }

      if (comma == NULL)
        {
          break;
        }

      tok = comma + 1;
    }

  return true;
}

static bool fc_field_value_match(const char *field, int minv, int maxv,
                                 int value)
{
  char copy[48];
  char *saveptr = NULL;
  char *tok;

  if (field == NULL)
    {
      return false;
    }

  if (strcmp(field, "*") == 0)
    {
      return true;
    }

  if (fc_strlcpy(copy, field, sizeof(copy)) < 0)
    {
      return false;
    }

  for (tok = strtok_r(copy, ",", &saveptr);
       tok != NULL;
       tok = strtok_r(NULL, ",", &saveptr))
    {
      char *slash = strchr(tok, '/');
      int step = 1;
      int start = minv;
      int end = maxv;
      int single;
      char *dash;

      if (slash != NULL)
        {
          *slash++ = '\0';
          if (fc_parse_uint(slash, &step) < 0 || step <= 0)
            {
              continue;
            }
        }

      if (strcmp(tok, "*") != 0)
        {
          dash = strchr(tok, '-');
          if (dash != NULL)
            {
              *dash++ = '\0';
              if (fc_parse_uint(tok, &start) < 0 ||
                  fc_parse_uint(dash, &end) < 0)
                {
                  continue;
                }
            }
          else if (fc_parse_uint(tok, &single) == 0)
            {
              start = single;
              end = single;
            }
          else
            {
              continue;
            }
        }

      if (start < minv || end > maxv || start > end)
        {
          continue;
        }

      if (value >= start && value <= end && ((value - start) % step) == 0)
        {
          return true;
        }
    }

  return false;
}

int fc_cron_validate(const char *expr)
{
  char copy[64];
  char *fields[5];

  if (fc_cron_parse_fields(expr, copy, sizeof(copy), fields) < 0)
    {
      return -EINVAL;
    }

  if (!fc_field_valid(fields[0], 0, 59) ||
      !fc_field_valid(fields[1], 0, 23) ||
      !fc_field_valid(fields[2], 1, 31) ||
      !fc_field_valid(fields[3], 1, 12) ||
      !fc_field_valid(fields[4], 0, 6))
    {
      return -EINVAL;
    }

  return 0;
}

bool fc_cron_matches(const char *expr, const struct tm *tmv)
{
  char copy[64];
  char *fields[5];

  if (tmv == NULL ||
      fc_cron_parse_fields(expr, copy, sizeof(copy), fields) < 0 ||
      fc_cron_validate(expr) < 0)
    {
      return false;
    }

  return fc_field_value_match(fields[0], 0, 59, tmv->tm_min) &&
         fc_field_value_match(fields[1], 0, 23, tmv->tm_hour) &&
         fc_field_value_match(fields[2], 1, 31, tmv->tm_mday) &&
         fc_field_value_match(fields[3], 1, 12, tmv->tm_mon + 1) &&
         fc_field_value_match(fields[4], 0, 6, tmv->tm_wday);
}

int fc_scheduler_load(void)
{
  char path[FC_PATH_LEN];
  char *buf;
  cJSON *root;
  cJSON *item;
  int ret;

  ret = fc_schedules_path(path, sizeof(path));
  if (ret < 0)
    {
      return ret;
    }

  buf = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  if (buf == NULL)
    {
      return -ENOMEM;
    }

  ret = fc_read_text_file(path, buf, CONFIG_FRUITCLAW_MAX_JSON, false);
  if (ret < 0)
    {
      free(buf);
      return ret == -ENOENT ? 0 : ret;
    }

  root = cJSON_Parse(buf);
  free(buf);
  if (root == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_sched_lock);
  memset(g_jobs, 0, sizeof(g_jobs));
  cJSON_ArrayForEach(item, root)
    {
      const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(item,
                                                                  "type"));
      const char *prompt = cJSON_GetStringValue(cJSON_GetObjectItem(item,
                                                                    "prompt"));
      const char *channel = cJSON_GetStringValue(cJSON_GetObjectItem(item,
                                                                     "channel"));
      const char *chat_id = cJSON_GetStringValue(cJSON_GetObjectItem(item,
                                                                     "chat_id"));
      const char *session_id = cJSON_GetStringValue(cJSON_GetObjectItem(item,
                                                                     "session_id"));
      fc_schedule_t *job;
      cJSON *enabled;
      cJSON *owner;
      cJSON *created;

      if (id == NULL || type == NULL || prompt == NULL)
        {
          continue;
        }

      job = fc_alloc_job(id);
      if (job == NULL)
        {
          continue;
        }

      fc_strlcpy(job->id, id, sizeof(job->id));
      fc_strlcpy(job->prompt, prompt, sizeof(job->prompt));
      fc_strlcpy(job->channel, channel ? channel : "internal",
                 sizeof(job->channel));
      fc_strlcpy(job->chat_id, chat_id ? chat_id : "", sizeof(job->chat_id));
      if (session_id != NULL)
        {
          fc_strlcpy(job->session_id, session_id, sizeof(job->session_id));
        }
      else
        {
          snprintf(job->session_id, sizeof(job->session_id), "schedule:%s",
                   id);
        }

      enabled = cJSON_GetObjectItem(item, "enabled");
      job->enabled = enabled == NULL ? true : cJSON_IsTrue(enabled);
      owner = cJSON_GetObjectItem(item, "owner_mode");
      job->owner_mode = owner == NULL ? false : cJSON_IsTrue(owner);
      created = cJSON_GetObjectItem(item, "created_ms");
      job->created_ms = cJSON_IsNumber(created) ?
                        (int64_t)created->valuedouble : 0;

      if (strcmp(type, "interval") == 0)
        {
          cJSON *every = cJSON_GetObjectItem(item, "every_sec");
          job->type = FC_SCHED_INTERVAL;
          job->every_sec = cJSON_IsNumber(every) ?
                           (uint32_t)every->valuedouble : 0;
          fc_schedule_next_interval(job);
        }
      else if (strcmp(type, "once") == 0)
        {
          cJSON *at = cJSON_GetObjectItem(item, "at_epoch");
          job->type = FC_SCHED_ONCE;
          job->at_epoch = cJSON_IsNumber(at) ? (int64_t)at->valuedouble : 0;
        }
      else if (strcmp(type, "cron") == 0)
        {
          const char *expr = cJSON_GetStringValue(cJSON_GetObjectItem(item,
                                                                      "expr"));
          if (expr == NULL || strlen(expr) >= sizeof(job->expr) ||
              fc_cron_validate(expr) < 0)
            {
              memset(job, 0, sizeof(*job));
              continue;
            }

          job->type = FC_SCHED_CRON;
          fc_strlcpy(job->expr, expr, sizeof(job->expr));
          job->last_minute_key = -1;
        }
      else if (strcmp(type, "boot") == 0)
        {
          job->type = FC_SCHED_BOOT;
        }
      else
        {
          memset(job, 0, sizeof(*job));
        }
    }

  pthread_mutex_unlock(&g_sched_lock);
  cJSON_Delete(root);
  return 0;
}

int fc_scheduler_save(void)
{
  char path[FC_PATH_LEN];
  cJSON *arr;
  char *printed;
  unsigned int i;
  int ret;

  ret = fc_schedules_path(path, sizeof(path));
  if (ret < 0)
    {
      return ret;
    }

  arr = cJSON_CreateArray();
  if (arr == NULL)
    {
      return -ENOMEM;
    }

  ret = fc_scheduler_lock_timed();
  if (ret < 0)
    {
      cJSON_Delete(arr);
      return ret;
    }

  for (i = 0; i < FC_MAX_SCHEDULES; i++)
    {
      cJSON *obj;
      if (!g_jobs[i].used)
        {
          continue;
        }

      obj = cJSON_CreateObject();
      if (obj == NULL)
        {
          continue;
        }

      cJSON_AddStringToObject(obj, "id", g_jobs[i].id);
      cJSON_AddBoolToObject(obj, "enabled", g_jobs[i].enabled);
      cJSON_AddStringToObject(obj, "prompt", g_jobs[i].prompt);
      cJSON_AddStringToObject(obj, "channel", g_jobs[i].channel);
      cJSON_AddStringToObject(obj, "chat_id", g_jobs[i].chat_id);
      cJSON_AddStringToObject(obj, "session_id", g_jobs[i].session_id);
      cJSON_AddBoolToObject(obj, "owner_mode", g_jobs[i].owner_mode);
      cJSON_AddNumberToObject(obj, "created_ms",
                              (double)g_jobs[i].created_ms);
      if (g_jobs[i].type == FC_SCHED_INTERVAL)
        {
          cJSON_AddStringToObject(obj, "type", "interval");
          cJSON_AddNumberToObject(obj, "every_sec", g_jobs[i].every_sec);
        }
      else if (g_jobs[i].type == FC_SCHED_ONCE)
        {
          cJSON_AddStringToObject(obj, "type", "once");
          cJSON_AddNumberToObject(obj, "at_epoch",
                                  (double)g_jobs[i].at_epoch);
        }
      else if (g_jobs[i].type == FC_SCHED_CRON)
        {
          cJSON_AddStringToObject(obj, "type", "cron");
          cJSON_AddStringToObject(obj, "expr", g_jobs[i].expr);
        }
      else
        {
          cJSON_AddStringToObject(obj, "type", "boot");
        }

      cJSON_AddItemToArray(arr, obj);
    }
  pthread_mutex_unlock(&g_sched_lock);

  printed = cJSON_PrintUnformatted(arr);
  cJSON_Delete(arr);
  if (printed == NULL)
    {
      return -ENOMEM;
    }

  ret = fc_write_text_file_atomic(path, printed);
  cJSON_free(printed);
  return ret;
}

static void fc_schedule_set_context(fc_schedule_t *job,
                                    const fc_tool_context_t *ctx)
{
  if (job == NULL)
    {
      return;
    }

  if (ctx != NULL)
    {
      fc_strlcpy(job->channel, ctx->channel[0] ? ctx->channel : "internal",
                 sizeof(job->channel));
      fc_strlcpy(job->chat_id, ctx->chat_id, sizeof(job->chat_id));
      fc_strlcpy(job->session_id,
                 ctx->session_id[0] ? ctx->session_id : job->id,
                 sizeof(job->session_id));
      job->owner_mode = ctx->owner_mode;
    }
  else
    {
      fc_strlcpy(job->channel, "internal", sizeof(job->channel));
      job->chat_id[0] = '\0';
      snprintf(job->session_id, sizeof(job->session_id), "schedule:%s",
               job->id);
      job->owner_mode = false;
    }

  if (job->created_ms == 0)
    {
      job->created_ms = fc_time_ms();
    }
}

static const char *fc_schedule_type_name(fc_schedule_type_t type)
{
  switch (type)
    {
      case FC_SCHED_INTERVAL:
        return "interval";
      case FC_SCHED_ONCE:
        return "once";
      case FC_SCHED_CRON:
        return "cron";
      case FC_SCHED_BOOT:
      default:
        return "boot";
    }
}

static void fc_scheduler_add_script_ref(cJSON *obj, const char *prompt)
{
  const char *args_text;
  cJSON *args;
  cJSON *script;
  const char *path;
  const char *name;
  const char *kind;
  const char *args_json;

  if (obj == NULL || prompt == NULL)
    {
      return;
    }

  while (*prompt != '\0' && isspace((unsigned char)*prompt))
    {
      prompt++;
    }

  if (strncmp(prompt, FC_SCHED_SCRIPT_RUN_PROMPT_PRE,
              strlen(FC_SCHED_SCRIPT_RUN_PROMPT_PRE)) == 0)
    {
      args_text = prompt + strlen(FC_SCHED_SCRIPT_RUN_PROMPT_PRE);
    }
  else if (strncmp(prompt, FC_SCHED_BERRY_RUN_PROMPT_PRE,
                   strlen(FC_SCHED_BERRY_RUN_PROMPT_PRE)) == 0)
    {
      args_text = prompt + strlen(FC_SCHED_BERRY_RUN_PROMPT_PRE);
    }
  else
    {
      return;
    }

  args = cJSON_Parse(args_text);
  if (!cJSON_IsObject(args))
    {
      cJSON_Delete(args);
      return;
    }

  script = cJSON_CreateObject();
  if (script == NULL)
    {
      cJSON_Delete(args);
      return;
    }

  path = cJSON_GetStringValue(cJSON_GetObjectItem(args, "path"));
  name = cJSON_GetStringValue(cJSON_GetObjectItem(args, "name"));
  kind = cJSON_GetStringValue(cJSON_GetObjectItem(args, "kind"));
  args_json = cJSON_GetStringValue(cJSON_GetObjectItem(args, "args_json"));
  if (path != NULL)
    {
      cJSON_AddStringToObject(script, "path", path);
    }

  if (name != NULL)
    {
      cJSON_AddStringToObject(script, "name", name);
    }

  if (kind != NULL)
    {
      cJSON_AddStringToObject(script, "kind", kind);
    }
  else if (strncmp(prompt, FC_SCHED_BERRY_RUN_PROMPT_PRE,
                   strlen(FC_SCHED_BERRY_RUN_PROMPT_PRE)) == 0)
    {
      cJSON_AddStringToObject(script, "kind", "berry");
    }

  if (args_json != NULL)
    {
      cJSON_AddStringToObject(script, "args_json", args_json);
    }

  cJSON_AddItemToObject(obj, "script", script);
  cJSON_Delete(args);
}

static int fc_scheduler_add_job_json(cJSON *jobs, const fc_schedule_t *job,
                                     int64_t now_ms)
{
  cJSON *obj;

  if (jobs == NULL || job == NULL || !job->used)
    {
      return -EINVAL;
    }

  obj = cJSON_CreateObject();
  if (obj == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddStringToObject(obj, "id", job->id);
  cJSON_AddBoolToObject(obj, "enabled", job->enabled);
  cJSON_AddStringToObject(obj, "type", fc_schedule_type_name(job->type));
  cJSON_AddStringToObject(obj, "prompt", job->prompt);
  cJSON_AddStringToObject(obj, "channel",
                          job->channel[0] ? job->channel : "internal");
  cJSON_AddStringToObject(obj, "chat_id",
                          job->chat_id[0] ? job->chat_id : "");
  cJSON_AddStringToObject(obj, "session_id", job->session_id);
  cJSON_AddBoolToObject(obj, "owner_mode", job->owner_mode);
  cJSON_AddNumberToObject(obj, "created_ms", (double)job->created_ms);

  if (job->type == FC_SCHED_INTERVAL)
    {
      cJSON_AddNumberToObject(obj, "every_sec", job->every_sec);
      cJSON_AddNumberToObject(obj, "next_ms", (double)job->next_ms);
      cJSON_AddNumberToObject(obj, "next_in_ms",
                              job->next_ms > now_ms ?
                              (double)(job->next_ms - now_ms) : 0);
    }
  else if (job->type == FC_SCHED_ONCE)
    {
      cJSON_AddNumberToObject(obj, "at_epoch", (double)job->at_epoch);
      cJSON_AddNumberToObject(obj, "at_in_sec",
                              job->at_epoch > 0 ?
                              (double)(job->at_epoch - now_ms / 1000) : 0);
    }
  else if (job->type == FC_SCHED_CRON)
    {
      cJSON_AddStringToObject(obj, "expr", job->expr);
      cJSON_AddNumberToObject(obj, "last_minute_key",
                              job->last_minute_key);
    }

  fc_scheduler_add_script_ref(obj, job->prompt);
  cJSON_AddItemToArray(jobs, obj);
  return 0;
}

int fc_scheduler_add_interval_ctx(const char *id, uint32_t seconds,
                                  const char *prompt,
                                  const fc_tool_context_t *ctx)
{
  fc_schedule_t *job;
  int ret;

  if (id == NULL || prompt == NULL || seconds == 0)
    {
      return -EINVAL;
    }

  ret = fc_scheduler_lock_timed();
  if (ret < 0)
    {
      return ret;
    }

  job = fc_alloc_job(id);
  if (job == NULL)
    {
      pthread_mutex_unlock(&g_sched_lock);
      return -ENOSPC;
    }

  fc_strlcpy(job->id, id, sizeof(job->id));
  fc_strlcpy(job->prompt, prompt, sizeof(job->prompt));
  job->type = FC_SCHED_INTERVAL;
  job->every_sec = seconds;
  job->enabled = true;
  fc_schedule_set_context(job, ctx);
  fc_schedule_next_interval(job);
  pthread_mutex_unlock(&g_sched_lock);

  return fc_scheduler_save();
}

int fc_scheduler_add_interval(const char *id, uint32_t seconds,
                              const char *prompt)
{
  return fc_scheduler_add_interval_ctx(id, seconds, prompt, NULL);
}

int fc_scheduler_add_cron_ctx(const char *id, const char *expr,
                              const char *prompt,
                              const fc_tool_context_t *ctx)
{
  fc_schedule_t *job;
  int ret;

  if (id == NULL || expr == NULL || prompt == NULL)
    {
      return -EINVAL;
    }

  if (strlen(expr) >= sizeof(((fc_schedule_t *)0)->expr) ||
      fc_cron_validate(expr) < 0)
    {
      return -EINVAL;
    }

  ret = fc_scheduler_lock_timed();
  if (ret < 0)
    {
      return ret;
    }

  job = fc_alloc_job(id);
  if (job == NULL)
    {
      pthread_mutex_unlock(&g_sched_lock);
      return -ENOSPC;
    }

  fc_strlcpy(job->id, id, sizeof(job->id));
  fc_strlcpy(job->expr, expr, sizeof(job->expr));
  fc_strlcpy(job->prompt, prompt, sizeof(job->prompt));
  job->type = FC_SCHED_CRON;
  job->enabled = true;
  job->last_minute_key = -1;
  fc_schedule_set_context(job, ctx);
  pthread_mutex_unlock(&g_sched_lock);

  return fc_scheduler_save();
}

int fc_scheduler_add_cron(const char *id, const char *expr,
                          const char *prompt)
{
  return fc_scheduler_add_cron_ctx(id, expr, prompt, NULL);
}

int fc_scheduler_add_once_ctx(const char *id, int64_t epoch,
                              const char *prompt,
                              const fc_tool_context_t *ctx)
{
  fc_schedule_t *job;
  int ret;

  if (id == NULL || prompt == NULL || epoch <= 0)
    {
      return -EINVAL;
    }

  ret = fc_scheduler_lock_timed();
  if (ret < 0)
    {
      return ret;
    }

  job = fc_alloc_job(id);
  if (job == NULL)
    {
      pthread_mutex_unlock(&g_sched_lock);
      return -ENOSPC;
    }

  fc_strlcpy(job->id, id, sizeof(job->id));
  fc_strlcpy(job->prompt, prompt, sizeof(job->prompt));
  job->type = FC_SCHED_ONCE;
  job->at_epoch = epoch;
  job->enabled = true;
  fc_schedule_set_context(job, ctx);
  pthread_mutex_unlock(&g_sched_lock);

  return fc_scheduler_save();
}

int fc_scheduler_add_once(const char *id, int64_t epoch, const char *prompt)
{
  return fc_scheduler_add_once_ctx(id, epoch, prompt, NULL);
}

int fc_scheduler_add_boot_ctx(const char *id, const char *prompt,
                              const fc_tool_context_t *ctx)
{
  fc_schedule_t *job;
  int ret;

  if (id == NULL || prompt == NULL)
    {
      return -EINVAL;
    }

  ret = fc_scheduler_lock_timed();
  if (ret < 0)
    {
      return ret;
    }

  job = fc_alloc_job(id);
  if (job == NULL)
    {
      pthread_mutex_unlock(&g_sched_lock);
      return -ENOSPC;
    }

  fc_strlcpy(job->id, id, sizeof(job->id));
  fc_strlcpy(job->prompt, prompt, sizeof(job->prompt));
  job->type = FC_SCHED_BOOT;
  job->enabled = true;
  job->last_minute_key = -1;
  fc_schedule_set_context(job, ctx);
  pthread_mutex_unlock(&g_sched_lock);

  return fc_scheduler_save();
}

int fc_scheduler_add_boot(const char *id, const char *prompt)
{
  return fc_scheduler_add_boot_ctx(id, prompt, NULL);
}

int fc_scheduler_remove(const char *id)
{
  fc_schedule_t *job;
  int ret;

  ret = fc_scheduler_lock_timed();
  if (ret < 0)
    {
      return ret;
    }

  job = fc_find_job(id);
  if (job == NULL)
    {
      pthread_mutex_unlock(&g_sched_lock);
      return -ENOENT;
    }

  memset(job, 0, sizeof(*job));
  pthread_mutex_unlock(&g_sched_lock);
  return fc_scheduler_save();
}

int fc_scheduler_list(char *out, size_t out_len)
{
  unsigned int i;
  size_t off = 0;
  int ret;

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  out[0] = '\0';
  ret = fc_scheduler_lock_timed();
  if (ret < 0)
    {
      return ret;
    }

  for (i = 0; i < FC_MAX_SCHEDULES; i++)
    {
      int n;
      if (!g_jobs[i].used)
        {
          continue;
        }

      if (g_jobs[i].type == FC_SCHED_INTERVAL)
        {
          n = snprintf(out + off, out_len - off,
                       "%s %s interval every_sec=%lu next_ms=%lld "
                       "ch=%s chat=%s owner=%s\n",
                       g_jobs[i].enabled ? "on" : "off",
                       g_jobs[i].id,
                       (unsigned long)g_jobs[i].every_sec,
                       (long long)g_jobs[i].next_ms,
                       g_jobs[i].channel,
                       g_jobs[i].chat_id[0] ? g_jobs[i].chat_id : "-",
                       g_jobs[i].owner_mode ? "yes" : "no");
        }
      else if (g_jobs[i].type == FC_SCHED_ONCE)
        {
          n = snprintf(out + off, out_len - off,
                       "%s %s once at_epoch=%lld ch=%s chat=%s owner=%s\n",
                       g_jobs[i].enabled ? "on" : "off",
                       g_jobs[i].id,
                       (long long)g_jobs[i].at_epoch,
                       g_jobs[i].channel,
                       g_jobs[i].chat_id[0] ? g_jobs[i].chat_id : "-",
                       g_jobs[i].owner_mode ? "yes" : "no");
        }
      else if (g_jobs[i].type == FC_SCHED_CRON)
        {
          n = snprintf(out + off, out_len - off,
                       "%s %s cron expr=\"%s\" ch=%s chat=%s owner=%s\n",
                       g_jobs[i].enabled ? "on" : "off",
                       g_jobs[i].id,
                       g_jobs[i].expr,
                       g_jobs[i].channel,
                       g_jobs[i].chat_id[0] ? g_jobs[i].chat_id : "-",
                       g_jobs[i].owner_mode ? "yes" : "no");
        }
      else
        {
          n = snprintf(out + off, out_len - off,
                       "%s %s boot ch=%s chat=%s owner=%s\n",
                       g_jobs[i].enabled ? "on" : "off",
                       g_jobs[i].id,
                       g_jobs[i].channel,
                       g_jobs[i].chat_id[0] ? g_jobs[i].chat_id : "-",
                       g_jobs[i].owner_mode ? "yes" : "no");
        }

      if (n < 0 || off + n >= out_len)
        {
          pthread_mutex_unlock(&g_sched_lock);
          return -ENOSPC;
        }

      off += n;
    }
  pthread_mutex_unlock(&g_sched_lock);

  if (off == 0)
    {
      fc_strlcpy(out, "No schedules.\n", out_len);
    }

  return 0;
}

int fc_scheduler_list_json(char *out, size_t out_len)
{
  cJSON *root;
  cJSON *jobs;
  char *printed;
  unsigned int i;
  unsigned int count = 0;
  int64_t now_ms = fc_time_ms();
  int ret;

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  root = cJSON_CreateObject();
  jobs = cJSON_CreateArray();
  if (root == NULL || jobs == NULL)
    {
      cJSON_Delete(root);
      cJSON_Delete(jobs);
      return -ENOMEM;
    }

  ret = fc_scheduler_lock_timed();
  if (ret < 0)
    {
      cJSON_Delete(root);
      cJSON_Delete(jobs);
      return ret;
    }

  for (i = 0; i < FC_MAX_SCHEDULES; i++)
    {
      if (!g_jobs[i].used)
        {
          continue;
        }

      ret = fc_scheduler_add_job_json(jobs, &g_jobs[i], now_ms);
      if (ret < 0)
        {
          pthread_mutex_unlock(&g_sched_lock);
          cJSON_Delete(root);
          cJSON_Delete(jobs);
          return ret;
        }

      count++;
    }
  pthread_mutex_unlock(&g_sched_lock);

  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddNumberToObject(root, "count", count);
  cJSON_AddItemToObject(root, "jobs", jobs);
  printed = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (printed == NULL)
    {
      return -ENOMEM;
    }

  ret = fc_strlcpy(out, printed, out_len);
  cJSON_free(printed);
  return ret;
}

static int fc_emit_schedule(const fc_schedule_t *job)
{
  fc_event_t ev;
  int ret;

  memset(&ev, 0, sizeof(ev));
  fc_make_id(ev.id, sizeof(ev.id), "sched");
  fc_strlcpy(ev.source, "scheduler", sizeof(ev.source));
  fc_strlcpy(ev.type, "timer.fire", sizeof(ev.type));
  fc_strlcpy(ev.channel, job->channel[0] ? job->channel : "internal",
             sizeof(ev.channel));
  fc_strlcpy(ev.chat_id, job->chat_id, sizeof(ev.chat_id));
  fc_strlcpy(ev.session_id,
             job->session_id[0] ? job->session_id : "schedule",
             sizeof(ev.session_id));
  ev.owner_mode = job->owner_mode;
  fc_strlcpy(ev.text, job->prompt, sizeof(ev.text));
  snprintf(ev.payload_json, sizeof(ev.payload_json),
           "{\"job_id\":\"%s\"}", job->id);
  ev.ts_ms = fc_time_ms();

  ret = fc_queue_publish(fc_main_queue(), &ev);
  fc_scheduler_record_emit(job->id, ret);
  if (ret < 0)
    {
      FC_LOGW("scheduler event queue full");
    }

  return ret;
}

static void fc_scheduler_emit_boot_jobs(void)
{
  unsigned int i;
  int ret;

  ret = fc_scheduler_lock_timed();
  if (ret < 0)
    {
      fc_scheduler_record_emit("boot-scan", ret);
      return;
    }

  for (i = 0; i < FC_MAX_SCHEDULES; i++)
    {
      fc_schedule_t snapshot;

      if (!g_jobs[i].used || !g_jobs[i].enabled ||
          g_jobs[i].type != FC_SCHED_BOOT)
        {
          continue;
        }

      snapshot = g_jobs[i];
      pthread_mutex_unlock(&g_sched_lock);
      fc_emit_schedule(&snapshot);
      pthread_mutex_lock(&g_sched_lock);
    }

  pthread_mutex_unlock(&g_sched_lock);
}

static void *fc_scheduler_thread_main(void *arg)
{
  (void)arg;

  fc_scheduler_emit_boot_jobs();

  for (; ; )
    {
      int64_t now_ms = fc_time_ms();
      time_t now_s = now_ms / 1000;
      struct tm tmv;
      unsigned int i;

      gmtime_r(&now_s, &tmv);
      pthread_mutex_lock(&g_sched_lock);
      for (i = 0; i < FC_MAX_SCHEDULES; i++)
        {
          fc_schedule_t snapshot;
          int minute_key;

          if (!g_jobs[i].used || !g_jobs[i].enabled)
            {
              continue;
            }

          if (g_jobs[i].type == FC_SCHED_INTERVAL)
            {
              if (g_jobs[i].next_ms == 0)
                {
                  fc_schedule_next_interval(&g_jobs[i]);
                }

              if (now_ms >= g_jobs[i].next_ms)
                {
                  snapshot = g_jobs[i];
                  fc_schedule_next_interval(&g_jobs[i]);
                  pthread_mutex_unlock(&g_sched_lock);
                  fc_emit_schedule(&snapshot);
                  pthread_mutex_lock(&g_sched_lock);
                }
            }
          else if (g_jobs[i].type == FC_SCHED_ONCE)
            {
              if (g_jobs[i].at_epoch > 0 && now_s >= g_jobs[i].at_epoch)
                {
                  snapshot = g_jobs[i];
                  g_jobs[i].enabled = false;
                  pthread_mutex_unlock(&g_sched_lock);
                  fc_emit_schedule(&snapshot);
                  fc_scheduler_save();
                  pthread_mutex_lock(&g_sched_lock);
                }
            }
          else if (g_jobs[i].type == FC_SCHED_CRON)
            {
              minute_key = tmv.tm_yday * 24 * 60 + tmv.tm_hour * 60 +
                           tmv.tm_min;
              if (minute_key != g_jobs[i].last_minute_key &&
                  fc_cron_matches(g_jobs[i].expr, &tmv))
                {
                  snapshot = g_jobs[i];
                  g_jobs[i].last_minute_key = minute_key;
                  pthread_mutex_unlock(&g_sched_lock);
                  fc_emit_schedule(&snapshot);
                  pthread_mutex_lock(&g_sched_lock);
                }
            }
        }
      pthread_mutex_unlock(&g_sched_lock);
      fc_scheduler_record_tick();
      fc_guard_session_heartbeat("scheduler");
      sleep(1);
    }

  return NULL;
}

int fc_scheduler_worker_start(void)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_SCHEDULER
  pthread_attr_t attr;
  int ret;

  if (g_sched_started)
    {
      return 0;
    }

  ret = fc_scheduler_load();
  if (ret < 0)
    {
      return ret;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_FRUITCLAW_SMALL_STACKSIZE);
  ret = pthread_create(&g_sched_thread, &attr, fc_scheduler_thread_main,
                       NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      return -ret;
    }

  g_sched_started = true;
  return 0;
#else
  return -ENOSYS;
#endif
}

int fc_scheduler_status_format(char *out, size_t out_len)
{
  unsigned long ticks;
  unsigned long fired;
  unsigned long failures;
  int64_t tick_ms;
  int64_t fire_ms;
  int64_t now = fc_mono_ms();
  int last_ret;
  char last_job[48];
  unsigned int used = 0;
  unsigned int enabled = 0;
  unsigned int i;
  int ret;

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  ret = fc_scheduler_lock_timed();
  if (ret < 0)
    {
      return ret;
    }

  for (i = 0; i < FC_MAX_SCHEDULES; i++)
    {
      if (g_jobs[i].used)
        {
          used++;
          if (g_jobs[i].enabled)
            {
              enabled++;
            }
        }
    }
  pthread_mutex_unlock(&g_sched_lock);

  pthread_mutex_lock(&g_sched_status_lock);
  ticks = g_sched_tick_count;
  fired = g_sched_emit_count;
  failures = g_sched_emit_fail_count;
  tick_ms = g_sched_last_tick_ms;
  fire_ms = g_sched_last_fire_ms;
  last_ret = g_sched_last_ret;
  fc_strlcpy(last_job, g_sched_last_job[0] ? g_sched_last_job : "-",
             sizeof(last_job));
  pthread_mutex_unlock(&g_sched_status_lock);

  snprintf(out, out_len,
           "scheduler_status: worker=%s ticks=%lu fired=%lu failures=%lu "
           "last_ret=%d last_tick_age_ms=%lld last_fire_age_ms=%lld "
           "used=%u enabled=%u last_job=%s",
           g_sched_started ? "started" : "stopped",
           ticks, fired, failures, last_ret,
           tick_ms > 0 ? (long long)(now - tick_ms) : -1,
           fire_ms > 0 ? (long long)(now - fire_ms) : -1,
           used, enabled, last_job);
  return 0;
}

void fc_scheduler_status(FILE *out)
{
  char status[256];

  if (out == NULL)
    {
      return;
    }

  if (fc_scheduler_status_format(status, sizeof(status)) == 0)
    {
      fprintf(out, "  %s\n", status);
    }
}
