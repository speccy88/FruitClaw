/* SPDX-License-Identifier: Apache-2.0 */

#include "fruitclaw.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "netutils/cJSON.h"

#define FC_MAX_CAPS 48

static const fc_cap_t *g_caps[FC_MAX_CAPS];
static pthread_mutex_t g_cap_init_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_cap_status_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int g_cap_count;
static bool g_caps_initialized;
static int g_last_tool_ret;
static int64_t g_last_tool_ms;
static char g_last_tool_id[FC_TOOL_NAME_LEN];

static void fc_cap_record_error(const char *id, int ret)
{
  if (ret >= 0)
    {
      return;
    }

  pthread_mutex_lock(&g_cap_status_lock);
  g_last_tool_ret = ret;
  g_last_tool_ms = fc_mono_ms();
  fc_strlcpy(g_last_tool_id, id ? id : "unknown",
             sizeof(g_last_tool_id));
  pthread_mutex_unlock(&g_cap_status_lock);
}

int fc_cap_init(void)
{
  int ret;

  pthread_mutex_lock(&g_cap_init_lock);
  if (g_caps_initialized)
    {
      pthread_mutex_unlock(&g_cap_init_lock);
      return 0;
    }

  g_cap_count = 0;
  memset(g_caps, 0, sizeof(g_caps));
  ret = fc_register_builtin_caps();
  if (ret == 0)
    {
      g_caps_initialized = true;
    }

  pthread_mutex_unlock(&g_cap_init_lock);
  return ret;
}

int fc_cap_register(const fc_cap_t *cap)
{
  if (cap == NULL || cap->id == NULL || cap->execute == NULL)
    {
      return -EINVAL;
    }

  if (fc_cap_find(cap->id) != NULL)
    {
      return -EEXIST;
    }

  if (g_cap_count >= FC_MAX_CAPS)
    {
      return -ENOSPC;
    }

  g_caps[g_cap_count++] = cap;
  return 0;
}

const fc_cap_t *fc_cap_find(const char *id)
{
  unsigned int i;

  if (id == NULL)
    {
      return NULL;
    }

  for (i = 0; i < g_cap_count; i++)
    {
      if (strcmp(g_caps[i]->id, id) == 0)
        {
          return g_caps[i];
        }
    }

  return NULL;
}

unsigned int fc_cap_count(bool visible_only)
{
  unsigned int i;
  unsigned int count = 0;

  for (i = 0; i < g_cap_count; i++)
    {
      if (!visible_only || g_caps[i]->visible_to_llm)
        {
          count++;
        }
    }

  return count;
}

void fc_tool_context_from_event(const fc_event_t *ev, fc_tool_context_t *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  memset(ctx, 0, sizeof(*ctx));
  if (ev == NULL)
    {
      return;
    }

  fc_strlcpy(ctx->source, ev->source, sizeof(ctx->source));
  fc_strlcpy(ctx->channel, ev->channel, sizeof(ctx->channel));
  fc_strlcpy(ctx->chat_id, ev->chat_id, sizeof(ctx->chat_id));
  fc_strlcpy(ctx->session_id, ev->session_id, sizeof(ctx->session_id));
  ctx->owner_mode = ev->owner_mode;
}

void fc_tool_context_local(fc_tool_context_t *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  memset(ctx, 0, sizeof(*ctx));
  fc_strlcpy(ctx->source, "cli", sizeof(ctx->source));
  fc_strlcpy(ctx->channel, "cli", sizeof(ctx->channel));
  fc_strlcpy(ctx->session_id, "cli", sizeof(ctx->session_id));
#ifdef CONFIG_FRUITCLAW_OWNER_MODE
  ctx->owner_mode = true;
#endif
}

int fc_cap_execute_ctx(const fc_tool_context_t *ctx, const char *id,
                       const char *args_json, char *out_json,
                       size_t out_len)
{
  const fc_cap_t *cap = fc_cap_find(id);
  int ret;

  if (out_json == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  if (cap == NULL)
    {
      snprintf(out_json, out_len,
               "{\"ok\":false,\"error\":\"unknown tool\"}");
      fc_cap_record_error(id, -ENOENT);
      return -ENOENT;
    }

  if (cap->requires_confirmation && (ctx == NULL || !ctx->owner_mode))
    {
      snprintf(out_json, out_len,
               "{\"ok\":false,\"error\":\"owner mode required\"}");
      fc_cap_record_error(id, -EACCES);
      return -EACCES;
    }

  fc_operator_progress_mark("tool-start");
  ret = cap->execute(ctx, args_json ? args_json : "{}", out_json, out_len);
  fc_operator_progress_mark(ret < 0 ? "tool-fail" : "tool-ok");
  fc_cap_record_error(id, ret);
  return ret;
}

int fc_cap_execute(const char *id, const char *args_json, char *out_json,
                   size_t out_len)
{
  fc_tool_context_t ctx;

  fc_tool_context_local(&ctx);
  return fc_cap_execute_ctx(&ctx, id, args_json, out_json, out_len);
}

int fc_tool_name_to_openai(const char *id, char *out, size_t out_len)
{
  size_t i;

  if (id == NULL || out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  for (i = 0; id[i] != '\0'; i++)
    {
      if (i + 1 >= out_len)
        {
          return -ENOSPC;
        }

      if (isalnum((unsigned char)id[i]) || id[i] == '_')
        {
          out[i] = id[i];
        }
      else
        {
          out[i] = '_';
        }
    }

  out[i] = '\0';
  return 0;
}

int fc_tool_name_from_openai(const char *name, char *out, size_t out_len)
{
  unsigned int i;
  char mapped[FC_TOOL_NAME_LEN];

  if (name == NULL || out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  for (i = 0; i < g_cap_count; i++)
    {
      if (fc_tool_name_to_openai(g_caps[i]->id, mapped, sizeof(mapped)) == 0 &&
          strcmp(mapped, name) == 0)
        {
          return fc_strlcpy(out, g_caps[i]->id, out_len);
        }
    }

  return -ENOENT;
}

int fc_cap_build_openai_tools_json(char *out_json, size_t out_len,
                                   bool llm_visible_only)
{
  cJSON *arr;
  char *printed;
  unsigned int i;
  int ret = 0;

  if (out_json == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  arr = cJSON_CreateArray();
  if (arr == NULL)
    {
      return -ENOMEM;
    }

  for (i = 0; i < g_cap_count; i++)
    {
      const fc_cap_t *cap = g_caps[i];
      cJSON *tool;
      cJSON *fn;
      cJSON *schema;
      char mapped[FC_TOOL_NAME_LEN];

      if (llm_visible_only && !cap->visible_to_llm)
        {
          continue;
        }

      if (fc_tool_name_to_openai(cap->id, mapped, sizeof(mapped)) < 0)
        {
          continue;
        }

      tool = cJSON_CreateObject();
      fn = cJSON_CreateObject();
      schema = cJSON_Parse(cap->input_schema_json ?
                           cap->input_schema_json : "{}");
      if (tool == NULL || fn == NULL || schema == NULL)
        {
          cJSON_Delete(tool);
          cJSON_Delete(fn);
          cJSON_Delete(schema);
          ret = -ENOMEM;
          goto out;
        }

      cJSON_AddStringToObject(tool, "type", "function");
      cJSON_AddStringToObject(fn, "name", mapped);
      cJSON_AddStringToObject(fn, "description",
                              cap->description ? cap->description : cap->id);
      cJSON_AddItemToObject(fn, "parameters", schema);
      cJSON_AddItemToObject(tool, "function", fn);
      cJSON_AddItemToArray(arr, tool);
    }

  printed = cJSON_PrintUnformatted(arr);
  if (printed == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  if (fc_strlcpy(out_json, printed, out_len) < 0)
    {
      ret = -ENOSPC;
    }

  cJSON_free(printed);

out:
  cJSON_Delete(arr);
  return ret;
}

int fc_cap_build_mcp_tools_json(char *out_json, size_t out_len,
                                bool visible_only)
{
  cJSON *root;
  cJSON *arr;
  char *printed;
  unsigned int i;
  int ret = 0;

  if (out_json == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  root = cJSON_CreateObject();
  arr = cJSON_CreateArray();
  if (root == NULL || arr == NULL)
    {
      cJSON_Delete(root);
      cJSON_Delete(arr);
      return -ENOMEM;
    }

  cJSON_AddItemToObject(root, "tools", arr);

  for (i = 0; i < g_cap_count; i++)
    {
      const fc_cap_t *cap = g_caps[i];
      cJSON *tool;
      cJSON *schema;

      if (visible_only && !cap->visible_to_llm)
        {
          continue;
        }

      tool = cJSON_CreateObject();
      schema = cJSON_Parse(cap->input_schema_json ?
                           cap->input_schema_json : "{}");
      if (schema == NULL)
        {
          schema = cJSON_CreateObject();
        }

      if (tool == NULL || schema == NULL)
        {
          cJSON_Delete(tool);
          cJSON_Delete(schema);
          ret = -ENOMEM;
          goto out;
        }

      cJSON_AddStringToObject(tool, "name", cap->id);
      cJSON_AddStringToObject(tool, "title",
                              cap->name ? cap->name : cap->id);
      cJSON_AddStringToObject(tool, "description",
                              cap->description ? cap->description : cap->id);
      cJSON_AddItemToObject(tool, "inputSchema", schema);
      cJSON_AddItemToArray(arr, tool);
    }

  printed = cJSON_PrintUnformatted(root);
  if (printed == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  if (fc_strlcpy(out_json, printed, out_len) < 0)
    {
      ret = -ENOSPC;
    }

  cJSON_free(printed);

out:
  cJSON_Delete(root);
  return ret;
}

void fc_cap_list(FILE *out)
{
  unsigned int i;

  if (out == NULL)
    {
      out = stdout;
    }

  for (i = 0; i < g_cap_count; i++)
    {
      fprintf(out, "%s\t%s%s\n",
              g_caps[i]->id,
              g_caps[i]->visible_to_llm ? "llm" : "local",
              g_caps[i]->requires_confirmation ? ",owner" : "");
    }
}

void fc_cap_status(FILE *out)
{
  int ret;
  int64_t ms;
  char id[FC_TOOL_NAME_LEN];

  if (out == NULL)
    {
      return;
    }

  pthread_mutex_lock(&g_cap_status_lock);
  ret = g_last_tool_ret;
  ms = g_last_tool_ms;
  fc_strlcpy(id, g_last_tool_id, sizeof(id));
  pthread_mutex_unlock(&g_cap_status_lock);

  if (ms == 0)
    {
      fprintf(out, "  last_tool_error: none\n");
    }
  else
    {
      fprintf(out, "  last_tool_error: tool=%s ret=%d age_ms=%lld\n",
              id[0] ? id : "unknown", ret,
              (long long)(fc_mono_ms() - ms));
    }
}

void fc_cap_clear_status(void)
{
  pthread_mutex_lock(&g_cap_status_lock);
  g_last_tool_ret = 0;
  g_last_tool_ms = 0;
  g_last_tool_id[0] = '\0';
  pthread_mutex_unlock(&g_cap_status_lock);
}
