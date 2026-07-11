/* SPDX-License-Identifier: Apache-2.0 */

#include "fruitclaw.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netutils/cJSON.h"

static pthread_mutex_t g_deepseek_status_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_deepseek_call_count;
static unsigned long g_deepseek_fail_count;
static int64_t g_deepseek_last_start_ms;
static int64_t g_deepseek_last_success_ms;
static int g_deepseek_last_ret;
static long g_deepseek_last_http_status;

static void fc_deepseek_record_start(void)
{
  fc_operator_progress_mark("deepseek-start");
  pthread_mutex_lock(&g_deepseek_status_lock);
  g_deepseek_call_count++;
  g_deepseek_last_start_ms = fc_mono_ms();
  pthread_mutex_unlock(&g_deepseek_status_lock);
}

static void fc_deepseek_record_result(int ret, long status)
{
  fc_operator_progress_mark(ret < 0 ? "deepseek-fail" : "deepseek-ok");
  pthread_mutex_lock(&g_deepseek_status_lock);
  g_deepseek_last_ret = ret;
  g_deepseek_last_http_status = status;
  if (ret < 0)
    {
      g_deepseek_fail_count++;
    }
  else
    {
      g_deepseek_last_success_ms = fc_mono_ms();
    }

  pthread_mutex_unlock(&g_deepseek_status_lock);
}

static int fc_deepseek_read_key(char *key, size_t key_len)
{
  char path[FC_PATH_LEN];
  int ret;

  ret = fc_secret_path("deepseek_api_key", path, sizeof(path));
  if (ret < 0)
    {
      return ret;
    }

  return fc_read_text_file(path, key, key_len, true);
}

int fc_deepseek_chat(void *messages_array, void **out_message)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_DEEPSEEK
  cJSON *messages = (cJSON *)messages_array;
  cJSON *root;
  cJSON *tools;
  cJSON *resp_json;
  cJSON *choices;
  cJSON *choice0;
  cJSON *msg;
  char *body;
  char *resp;
  char *tools_json;
  char key[256];
  char auth[320];
  fc_http_headers_t headers;
  long status = 0;
  int ret;

  if (messages == NULL || out_message == NULL)
    {
      return -EINVAL;
    }

  fc_deepseek_record_start();
  *out_message = NULL;
  ret = fc_deepseek_read_key(key, sizeof(key));
  if (ret < 0 || key[0] == '\0')
    {
      fc_deepseek_record_result(-ENOENT, 0);
      return -ENOENT;
    }

  root = cJSON_CreateObject();
  if (root == NULL)
    {
      fc_deepseek_record_result(-ENOMEM, 0);
      return -ENOMEM;
    }

  cJSON_AddStringToObject(root, "model", CONFIG_FRUITCLAW_DEEPSEEK_MODEL);
  cJSON_AddItemToObject(root, "messages", cJSON_Duplicate(messages, true));
  cJSON_AddBoolToObject(root, "stream", false);

  tools_json = calloc(1, CONFIG_FRUITCLAW_MCP_MAX_RESPONSE);
  if (tools_json == NULL)
    {
      cJSON_Delete(root);
      fc_deepseek_record_result(-ENOMEM, 0);
      return -ENOMEM;
    }

  if (fc_cap_build_openai_tools_json(tools_json,
                                     CONFIG_FRUITCLAW_MCP_MAX_RESPONSE,
                                     true) == 0)
    {
      tools = cJSON_Parse(tools_json);
      if (tools != NULL && cJSON_GetArraySize(tools) > 0)
        {
          cJSON_AddItemToObject(root, "tools", tools);
          cJSON_AddStringToObject(root, "tool_choice", "auto");
        }
      else
        {
          cJSON_Delete(tools);
        }
    }

  free(tools_json);

  body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (body == NULL)
    {
      fc_deepseek_record_result(-ENOMEM, 0);
      return -ENOMEM;
    }

  resp = calloc(1, CONFIG_FRUITCLAW_MAX_HTTP_BODY);
  if (resp == NULL)
    {
      cJSON_free(body);
      fc_deepseek_record_result(-ENOMEM, 0);
      return -ENOMEM;
    }

  memset(&headers, 0, sizeof(headers));
  snprintf(auth, sizeof(auth), "Bearer %s", key);
  headers.items[headers.count].name = "Authorization";
  headers.items[headers.count++].value = auth;

  ret = fc_http_post_json_guarded_timeout(
          FC_GUARD_STAGE_LLM, CONFIG_FRUITCLAW_LLM_GUARD_TIMEOUT_MS,
          CONFIG_FRUITCLAW_DEEPSEEK_HTTP_TIMEOUT_SEC,
          CONFIG_FRUITCLAW_DEEPSEEK_ENDPOINT, &headers, body, resp,
          CONFIG_FRUITCLAW_MAX_HTTP_BODY, &status);
  cJSON_free(body);

  if (ret < 0)
    {
      free(resp);
      fc_deepseek_record_result(ret, status);
      return ret;
    }

  if (status < 200 || status >= 300)
    {
      FC_LOGW("deepseek HTTP status=%ld body=%s", status, resp);
      free(resp);
      fc_deepseek_record_result(-EIO, status);
      return -EIO;
    }

  resp_json = cJSON_Parse(resp);
  free(resp);
  if (resp_json == NULL)
    {
      fc_deepseek_record_result(-EINVAL, status);
      return -EINVAL;
    }

  choices = cJSON_GetObjectItem(resp_json, "choices");
  choice0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
  msg = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
  if (msg == NULL)
    {
      cJSON_Delete(resp_json);
      fc_deepseek_record_result(-EINVAL, status);
      return -EINVAL;
    }

  *out_message = cJSON_Duplicate(msg, true);
  cJSON_Delete(resp_json);

  ret = *out_message == NULL ? -ENOMEM : 0;
  fc_deepseek_record_result(ret, status);
  return ret;
#else
  return -ENOSYS;
#endif
}

int fc_deepseek_test(char *out, size_t out_len)
{
  cJSON *messages;
  cJSON *msg;
  cJSON *reply = NULL;
  const char *content;
  int ret;

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  messages = cJSON_CreateArray();
  msg = cJSON_CreateObject();
  if (messages == NULL || msg == NULL)
    {
      cJSON_Delete(messages);
      cJSON_Delete(msg);
      return -ENOMEM;
    }

  cJSON_AddStringToObject(msg, "role", "user");
  cJSON_AddStringToObject(msg, "content",
                          "Reply with one short sentence that starts with OK.");
  cJSON_AddItemToArray(messages, msg);

  ret = fc_deepseek_chat(messages, (void **)&reply);
  cJSON_Delete(messages);
  if (ret < 0)
    {
      if (ret == -ENOENT)
        {
          char path[FC_PATH_LEN];
          fc_secret_path("deepseek_api_key", path, sizeof(path));
          snprintf(out, out_len, "DeepSeek API key not found at %s",
                   path);
        }
      else
        {
          snprintf(out, out_len, "DeepSeek request failed: %d", ret);
        }

      return ret;
    }

  content = cJSON_GetStringValue(cJSON_GetObjectItem(reply, "content"));
  fc_strlcpy(out, content ? content : "", out_len);
  cJSON_Delete(reply);
  return 0;
}

int fc_deepseek_status_format(char *out, size_t out_len)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_DEEPSEEK
  unsigned long calls;
  unsigned long fails;
  int64_t start_ms;
  int64_t success_ms;
  int ret;
  long http_status;
  int64_t now = fc_mono_ms();

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_deepseek_status_lock);
  calls = g_deepseek_call_count;
  fails = g_deepseek_fail_count;
  start_ms = g_deepseek_last_start_ms;
  success_ms = g_deepseek_last_success_ms;
  ret = g_deepseek_last_ret;
  http_status = g_deepseek_last_http_status;
  pthread_mutex_unlock(&g_deepseek_status_lock);

  snprintf(out, out_len,
           "deepseek_status: calls=%lu fails=%lu last_ret=%d http=%ld "
           "last_start_age_ms=%lld last_success_age_ms=%lld",
           calls, fails, ret, http_status,
           start_ms > 0 ? (long long)(now - start_ms) : -1,
           success_ms > 0 ? (long long)(now - success_ms) : -1);
  return 0;
#else
  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  snprintf(out, out_len, "deepseek_status: disabled");
  return 0;
#endif
}

void fc_deepseek_status(FILE *out)
{
  char status[256];

  if (out == NULL)
    {
      return;
    }

  if (fc_deepseek_status_format(status, sizeof(status)) == 0)
    {
      fprintf(out, "  %s\n", status);
    }
}
