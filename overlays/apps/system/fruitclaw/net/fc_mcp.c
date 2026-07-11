/* SPDX-License-Identifier: Apache-2.0 */

#include "fruitclaw.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netutils/cJSON.h"
#include "netutils/httpd.h"

#ifdef CONFIG_FRUITCLAW_MCP_SERVER

static bool g_mcp_registered;
static pthread_mutex_t g_mcp_status_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_mcp_request_count;
static unsigned long g_mcp_failure_count;
static unsigned long g_mcp_notification_count;
static unsigned long g_mcp_notification_suppressed_count;
static unsigned long g_mcp_tool_call_count;
static unsigned long g_mcp_tool_failure_count;
static int64_t g_mcp_status_notify_ms;
static int64_t g_mcp_tool_notify_ms;
static int64_t g_mcp_activity_ms;
static int64_t g_mcp_last_ms;
static int g_mcp_last_ret;
static int g_mcp_last_http_status;
static int g_mcp_last_dispatch_status;
static char g_mcp_last_method[64];
static char g_mcp_last_tool[FC_TOOL_NAME_LEN];

void fc_mcp_mark_activity(void)
{
  pthread_mutex_lock(&g_mcp_status_lock);
  g_mcp_activity_ms = fc_mono_ms();
  pthread_mutex_unlock(&g_mcp_status_lock);
}

bool fc_mcp_recently_active(int64_t now_ms, int64_t quiet_ms)
{
  int64_t activity_ms;
  bool active;

  if (quiet_ms <= 0)
    {
      return false;
    }

  pthread_mutex_lock(&g_mcp_status_lock);
  activity_ms = g_mcp_activity_ms;
  pthread_mutex_unlock(&g_mcp_status_lock);

  if (activity_ms <= 0)
    {
      return false;
    }

  active = now_ms < activity_ms || now_ms - activity_ms < quiet_ms;
  return active;
}

static void fc_mcp_record_dispatch(const char *method, int ret,
                                   int dispatch_status, int http_status,
                                   bool rpc_error)
{
  pthread_mutex_lock(&g_mcp_status_lock);
  g_mcp_request_count++;
  if (ret < 0 || http_status >= 400 || rpc_error)
    {
      g_mcp_failure_count++;
    }

  if (dispatch_status == FC_MCP_DISPATCH_NOTIFICATION_ACCEPTED)
    {
      g_mcp_notification_count++;
    }

  g_mcp_last_ms = fc_mono_ms();
  g_mcp_last_ret = ret;
  g_mcp_last_http_status = http_status;
  g_mcp_last_dispatch_status = dispatch_status;
  fc_strlcpy(g_mcp_last_method, method ? method : "-",
             sizeof(g_mcp_last_method));
  pthread_mutex_unlock(&g_mcp_status_lock);

  if (ret >= 0 && http_status < 500)
    {
      fc_operator_progress_mark("mcp");
    }
}

static void fc_mcp_record_tool(const char *name, int ret)
{
  pthread_mutex_lock(&g_mcp_status_lock);
  g_mcp_tool_call_count++;
  if (ret < 0)
    {
      g_mcp_tool_failure_count++;
    }

  fc_strlcpy(g_mcp_last_tool, name ? name : "-", sizeof(g_mcp_last_tool));
  pthread_mutex_unlock(&g_mcp_status_lock);
}

void fc_mcp_clear_status(void)
{
  pthread_mutex_lock(&g_mcp_status_lock);
  g_mcp_request_count = 0;
  g_mcp_failure_count = 0;
  g_mcp_notification_count = 0;
  g_mcp_notification_suppressed_count = 0;
  g_mcp_tool_call_count = 0;
  g_mcp_tool_failure_count = 0;
  g_mcp_status_notify_ms = 0;
  g_mcp_tool_notify_ms = 0;
  g_mcp_activity_ms = 0;
  g_mcp_last_ms = 0;
  g_mcp_last_ret = 0;
  g_mcp_last_http_status = 0;
  g_mcp_last_dispatch_status = 0;
  g_mcp_last_method[0] = '\0';
  g_mcp_last_tool[0] = '\0';
  pthread_mutex_unlock(&g_mcp_status_lock);
}

static bool fc_mcp_should_notify_tool_call(const char *name, int ret)
{
  int64_t now;
  int64_t last_ms;
  int64_t min_interval_ms = CONFIG_FRUITCLAW_MCP_NOTIFY_TOOL_INTERVAL_MS;
  bool notify = true;

  if (name == NULL)
    {
      return false;
    }

  if (ret >= 0 && strcmp(name, "system.status") == 0)
    {
      min_interval_ms = CONFIG_FRUITCLAW_MCP_NOTIFY_STATUS_INTERVAL_MS;
    }

  now = fc_mono_ms();
  pthread_mutex_lock(&g_mcp_status_lock);
  if (min_interval_ms <= 0)
    {
      notify = false;
    }
  else
    {
      last_ms = strcmp(name, "system.status") == 0 ?
                g_mcp_status_notify_ms : g_mcp_tool_notify_ms;
      if (last_ms > 0 && now - last_ms < min_interval_ms)
        {
          notify = false;
        }
    }

  if (notify && strcmp(name, "system.status") == 0)
    {
      g_mcp_status_notify_ms = now;
    }
  else if (notify)
    {
      g_mcp_tool_notify_ms = now;
    }

  if (!notify)
    {
      g_mcp_notification_suppressed_count++;
    }

  pthread_mutex_unlock(&g_mcp_status_lock);
  return notify;
}

static void fc_mcp_notify_tool_call(const char *name, int ret)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_TELEGRAM
  char text[160];
  int notify_ret;

  if (!g_mcp_registered || name == NULL)
    {
      return;
    }

  if (!fc_mcp_should_notify_tool_call(name, ret))
    {
      return;
    }

  if (ret < 0)
    {
      snprintf(text, sizeof(text),
               "FruitClaw MCP tool call: %s -> error (%d)", name, ret);
    }
  else
    {
      snprintf(text, sizeof(text),
               "FruitClaw MCP tool call: %s -> ok", name);
    }

  notify_ret = fc_telegram_notify_owner_async(text);
  if (notify_ret < 0)
    {
      pthread_mutex_lock(&g_mcp_status_lock);
      g_mcp_notification_suppressed_count++;
      pthread_mutex_unlock(&g_mcp_status_lock);
    }
#else
  (void)name;
  (void)ret;
#endif
}

static int fc_mcp_copy_json(cJSON *root, char *out, size_t out_len)
{
  char *printed;
  int ret = 0;

  if (root == NULL || out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  printed = cJSON_PrintUnformatted(root);
  if (printed == NULL)
    {
      return -ENOMEM;
    }

  if (fc_strlcpy(out, printed, out_len) < 0)
    {
      ret = -ENOSPC;
    }

  cJSON_free(printed);
  return ret;
}

static void fc_mcp_add_id(cJSON *resp, cJSON *id)
{
  cJSON *copy;

  if (id == NULL)
    {
      cJSON_AddNullToObject(resp, "id");
      return;
    }

  copy = cJSON_Duplicate(id, true);
  if (copy == NULL)
    {
      cJSON_AddNullToObject(resp, "id");
      return;
    }

  cJSON_AddItemToObject(resp, "id", copy);
}

static int fc_mcp_error(cJSON *id, int code, const char *message,
                        char *out, size_t out_len)
{
  cJSON *resp = cJSON_CreateObject();
  cJSON *err = cJSON_CreateObject();
  int ret;

  if (resp == NULL || err == NULL)
    {
      cJSON_Delete(resp);
      cJSON_Delete(err);
      return -ENOMEM;
    }

  cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
  fc_mcp_add_id(resp, id);
  cJSON_AddNumberToObject(err, "code", code);
  cJSON_AddStringToObject(err, "message", message ? message : "error");
  cJSON_AddItemToObject(resp, "error", err);
  ret = fc_mcp_copy_json(resp, out, out_len);
  cJSON_Delete(resp);
  return ret;
}

static int fc_mcp_error_mark(cJSON *id, int code, const char *message,
                             char *out, size_t out_len, bool *rpc_error)
{
  if (rpc_error != NULL)
    {
      *rpc_error = true;
    }

  return fc_mcp_error(id, code, message, out, out_len);
}

static int fc_mcp_response(cJSON *id, cJSON *result, char *out,
                           size_t out_len)
{
  cJSON *resp = cJSON_CreateObject();
  int ret;

  if (resp == NULL || result == NULL)
    {
      cJSON_Delete(resp);
      cJSON_Delete(result);
      return -ENOMEM;
    }

  cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
  fc_mcp_add_id(resp, id);
  cJSON_AddItemToObject(resp, "result", result);
  ret = fc_mcp_copy_json(resp, out, out_len);
  cJSON_Delete(resp);
  return ret;
}

static cJSON *fc_mcp_initialize_result(void)
{
  cJSON *result = cJSON_CreateObject();
  cJSON *caps = cJSON_CreateObject();
  cJSON *tools = cJSON_CreateObject();
  cJSON *info = cJSON_CreateObject();

  if (result == NULL || caps == NULL || tools == NULL || info == NULL)
    {
      cJSON_Delete(result);
      cJSON_Delete(caps);
      cJSON_Delete(tools);
      cJSON_Delete(info);
      return NULL;
    }

  cJSON_AddStringToObject(result, "protocolVersion", "2025-11-25");
  cJSON_AddBoolToObject(tools, "listChanged", false);
  cJSON_AddItemToObject(caps, "tools", tools);
  cJSON_AddItemToObject(result, "capabilities", caps);
  cJSON_AddStringToObject(info, "name", "fruitclaw");
  cJSON_AddStringToObject(info, "version", "0.1.0");
  cJSON_AddItemToObject(result, "serverInfo", info);
  return result;
}

static int fc_mcp_tools_list(cJSON *id, char *out, size_t out_len,
                             bool *rpc_error)
{
  char *tools_json;
  cJSON *result;
  int ret;

  tools_json = calloc(1, CONFIG_FRUITCLAW_MCP_MAX_RESPONSE);
  if (tools_json == NULL)
    {
      return -ENOMEM;
    }

  ret = fc_cap_build_mcp_tools_json(tools_json,
                                    CONFIG_FRUITCLAW_MCP_MAX_RESPONSE,
                                    true);
  if (ret < 0)
    {
      free(tools_json);
      return fc_mcp_error_mark(id, -32603, "tools/list failed", out,
                               out_len, rpc_error);
    }

  result = cJSON_Parse(tools_json);
  free(tools_json);
  if (result == NULL)
    {
      return fc_mcp_error_mark(id, -32603, "tools/list invalid", out,
                               out_len, rpc_error);
    }

  return fc_mcp_response(id, result, out, out_len);
}

static bool fc_mcp_tool_has_own_guard(const char *name)
{
  return name != NULL &&
         (strcmp(name, "terminal.run") == 0 ||
          strcmp(name, "berry.run_script") == 0 ||
          strcmp(name, "neopixels.set") == 0 ||
          strcmp(name, "neopixels.off") == 0 ||
          strcmp(name, "neopixels.effect") == 0 ||
          strcmp(name, "file.write_limited") == 0 ||
          strcmp(name, "script.remove") == 0 ||
          strcmp(name, "script.schedule") == 0 ||
          strcmp(name, "device.write") == 0 ||
          strcmp(name, "scheduler.add") == 0 ||
          strcmp(name, "scheduler.remove") == 0 ||
          strcmp(name, "rtttl.play") == 0 ||
          strcmp(name, "http.request") == 0);
}

static bool fc_mcp_tool_needs_outer_guard(const char *name)
{
  return name != NULL &&
         (strcmp(name, "device.read") == 0 ||
          strcmp(name, "device.write") == 0 ||
          strcmp(name, "file.write_limited") == 0 ||
          strcmp(name, "script.remove") == 0 ||
          strcmp(name, "scheduler.add") == 0 ||
          strcmp(name, "scheduler.remove") == 0 ||
          strcmp(name, "terminal.run") == 0 ||
          strcmp(name, "berry.run_script") == 0 ||
          strcmp(name, "neopixels.set") == 0 ||
          strcmp(name, "neopixels.off") == 0 ||
          strcmp(name, "neopixels.effect") == 0 ||
          strcmp(name, "http.request") == 0);
}

static int fc_mcp_execute_tool_on_worker(const fc_tool_context_t *ctx,
                                         const char *name,
                                         const char *args_json,
                                         char *tool_out,
                                         size_t tool_out_len)
{
  if (ctx == NULL || name == NULL || args_json == NULL ||
      tool_out == NULL || tool_out_len == 0)
    {
      return -EINVAL;
    }

  return fc_cap_execute_ctx(ctx, name, args_json, tool_out, tool_out_len);
}

static int fc_mcp_tools_call(cJSON *id, cJSON *params, char *out,
                             size_t out_len, const fc_tool_context_t *ctx,
                             bool *rpc_error)
{
  const char *name;
  cJSON *arguments;
  char *args_json;
  char *tool_out;
  cJSON *result;
  cJSON *content;
  cJSON *text;
  cJSON *structured;
  const fc_cap_t *cap;
  fc_tool_context_t call_ctx;
  int guard_fd = -1;
  int ret;

  if (!cJSON_IsObject(params))
    {
      return fc_mcp_error_mark(id, -32602, "invalid params", out,
                               out_len, rpc_error);
    }

  name = cJSON_GetStringValue(cJSON_GetObjectItem(params, "name"));
  arguments = cJSON_GetObjectItem(params, "arguments");
  if (name == NULL || (arguments != NULL && !cJSON_IsObject(arguments)))
    {
      return fc_mcp_error_mark(id, -32602, "invalid tool call", out,
                               out_len, rpc_error);
    }

  if (arguments == NULL)
    {
      args_json = calloc(1, 3);
      if (args_json != NULL)
        {
          strcpy(args_json, "{}");
        }
    }
  else
    {
      args_json = cJSON_PrintUnformatted(arguments);
    }

  tool_out = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  if (args_json == NULL || tool_out == NULL)
    {
      free(args_json);
      free(tool_out);
      return -ENOMEM;
    }

  memset(&call_ctx, 0, sizeof(call_ctx));
  if (ctx != NULL)
    {
      call_ctx = *ctx;
    }

  fc_strlcpy(call_ctx.source, "mcp", sizeof(call_ctx.source));
  fc_strlcpy(call_ctx.channel, "mcp", sizeof(call_ctx.channel));
  if (call_ctx.session_id[0] == '\0')
    {
      fc_strlcpy(call_ctx.session_id, "mcp", sizeof(call_ctx.session_id));
    }

#ifdef CONFIG_FRUITCLAW_MCP_YOLO_MODE
  call_ctx.owner_mode = true;
#endif

  cap = fc_cap_find(name);
  if (cap != NULL && (cap->requires_confirmation ||
      fc_mcp_tool_needs_outer_guard(name)) &&
      !fc_mcp_tool_has_own_guard(name))
    {
      ret = fc_guard_arm(FC_GUARD_STAGE_MCP, &guard_fd);
      if (ret < 0)
        {
          snprintf(tool_out, CONFIG_FRUITCLAW_MAX_JSON,
                   "{\"ok\":false,\"error\":\"mcp guard unavailable\","
                   "\"code\":%d}", ret);
          fc_mcp_record_tool(name, ret);
          goto build_result;
        }

      call_ctx.guarded = true;
    }

  ret = fc_mcp_execute_tool_on_worker(&call_ctx, name, args_json, tool_out,
                                      CONFIG_FRUITCLAW_MAX_JSON);
  fc_guard_disarm(guard_fd);
  fc_mcp_record_tool(name, ret);

build_result:
  free(args_json);
  fc_mcp_notify_tool_call(name, ret);

  result = cJSON_CreateObject();
  content = cJSON_CreateArray();
  text = cJSON_CreateObject();
  if (result == NULL || content == NULL || text == NULL)
    {
      cJSON_Delete(result);
      cJSON_Delete(content);
      cJSON_Delete(text);
      free(tool_out);
      return -ENOMEM;
    }

  cJSON_AddStringToObject(text, "type", "text");
  cJSON_AddStringToObject(text, "text", tool_out);
  cJSON_AddItemToArray(content, text);
  cJSON_AddItemToObject(result, "content", content);
  cJSON_AddBoolToObject(result, "isError", ret < 0);

  structured = cJSON_Parse(tool_out);
  if (structured != NULL)
    {
      cJSON_AddItemToObject(result, "structuredContent", structured);
    }

  free(tool_out);
  return fc_mcp_response(id, result, out, out_len);
}

int fc_mcp_jsonrpc_dispatch(const char *body, size_t body_len, char *out,
                            size_t out_len, int *dispatch_status,
                            int *http_status,
                            const fc_tool_context_t *ctx)
{
  char *copy;
  cJSON *root;
  cJSON *id;
  const char *jsonrpc;
  const char *method = NULL;
  int ret = 0;
  int final_dispatch_status = FC_MCP_DISPATCH_RESPONSE;
  int final_http_status = 200;
  bool rpc_error = false;

  if (dispatch_status != NULL)
    {
      *dispatch_status = FC_MCP_DISPATCH_RESPONSE;
    }

  if (http_status != NULL)
    {
      *http_status = 200;
    }

  if (body == NULL || out == NULL || out_len == 0)
    {
      fc_mcp_record_dispatch("-", -EINVAL, final_dispatch_status,
                             final_http_status, false);
      return -EINVAL;
    }

  copy = calloc(1, body_len + 1);
  if (copy == NULL)
    {
      fc_mcp_record_dispatch("-", -ENOMEM, final_dispatch_status,
                             final_http_status, false);
      return -ENOMEM;
    }

  memcpy(copy, body, body_len);
  root = cJSON_Parse(copy);
  free(copy);
  if (root == NULL)
    {
      ret = fc_mcp_error_mark(NULL, -32700, "parse error", out, out_len,
                              &rpc_error);
      fc_mcp_record_dispatch("-", ret, final_dispatch_status,
                             final_http_status, rpc_error);
      return ret;
    }

  if (cJSON_IsArray(root))
    {
      ret = fc_mcp_error_mark(NULL, -32600, "batch unsupported", out,
                              out_len, &rpc_error);
      goto out;
    }

  jsonrpc = cJSON_GetStringValue(cJSON_GetObjectItem(root, "jsonrpc"));
  if (!cJSON_IsObject(root) || jsonrpc == NULL ||
      strcmp(jsonrpc, "2.0") != 0)
    {
      ret = fc_mcp_error_mark(NULL, -32600, "invalid request", out,
                              out_len, &rpc_error);
      goto out;
    }

  id = cJSON_GetObjectItem(root, "id");
  method = cJSON_GetStringValue(cJSON_GetObjectItem(root, "method"));
  if (method == NULL)
    {
      ret = fc_mcp_error_mark(id, -32600, "missing method", out, out_len,
                              &rpc_error);
      goto out;
    }

  if (id == NULL)
    {
      if (strcmp(method, "notifications/initialized") == 0)
        {
          if (dispatch_status != NULL)
            {
              *dispatch_status = FC_MCP_DISPATCH_NOTIFICATION_ACCEPTED;
            }
          final_dispatch_status = FC_MCP_DISPATCH_NOTIFICATION_ACCEPTED;

          if (http_status != NULL)
            {
              *http_status = 202;
            }
          final_http_status = 202;

          out[0] = '\0';
          goto out;
        }

      ret = fc_mcp_error_mark(NULL, -32600, "notifications unsupported",
                              out, out_len, &rpc_error);
      goto out;
    }

  if (strcmp(method, "initialize") == 0)
    {
      ret = fc_mcp_response(id, fc_mcp_initialize_result(), out, out_len);
    }
  else if (strcmp(method, "notifications/initialized") == 0)
    {
      ret = fc_mcp_response(id, cJSON_CreateObject(), out, out_len);
    }
  else if (strcmp(method, "ping") == 0)
    {
      ret = fc_mcp_response(id, cJSON_CreateObject(), out, out_len);
    }
  else if (strcmp(method, "tools/list") == 0)
    {
      ret = fc_mcp_tools_list(id, out, out_len, &rpc_error);
    }
  else if (strcmp(method, "tools/call") == 0)
    {
      ret = fc_mcp_tools_call(id, cJSON_GetObjectItem(root, "params"),
                              out, out_len, ctx, &rpc_error);
    }
  else
    {
      ret = fc_mcp_error_mark(id, -32601, "method not found", out,
                              out_len, &rpc_error);
    }

out:
  if (dispatch_status != NULL)
    {
      final_dispatch_status = *dispatch_status;
    }

  if (http_status != NULL)
    {
      final_http_status = *http_status;
    }

  fc_mcp_record_dispatch(method, ret, final_dispatch_status,
                         final_http_status, rpc_error);
  cJSON_Delete(root);
  return ret;
}

static bool fc_mcp_header_starts(const char *value, const char *prefix)
{
  size_t len;

  if (value == NULL || prefix == NULL)
    {
      return false;
    }

  len = strlen(prefix);
  return strncasecmp(value, prefix, len) == 0;
}

static void fc_mcp_http_handler(struct httpd_state *pstate, char *path)
{
  static const char allow[] = "Allow: POST, OPTIONS\r\n";
  char *resp;
  fc_tool_context_t ctx;
  int dispatch_status = FC_MCP_DISPATCH_RESPONSE;
  int http_status = 200;
  int ret;

  (void)path;
  fc_mcp_mark_activity();
  fc_guard_session_heartbeat("mcp");

  if (pstate->ht_method == HTTPD_METHOD_OPTIONS)
    {
      fc_operator_progress_mark("mcp-options");
      httpd_send_response(pstate, 204, "text/plain", allow, NULL, 0);
      return;
    }

  if (pstate->ht_method == HTTPD_METHOD_GET ||
      pstate->ht_method == HTTPD_METHOD_DELETE)
    {
      fc_operator_progress_mark("mcp-method");
      httpd_send_response(pstate, 405, "text/plain", allow, NULL, 0);
      return;
    }

  if (pstate->ht_method != HTTPD_METHOD_POST)
    {
      fc_operator_progress_mark("mcp-method");
      httpd_send_response(pstate, 405, "text/plain", allow, NULL, 0);
      return;
    }

  if (pstate->ht_content_type[0] != '\0' &&
      !fc_mcp_header_starts(pstate->ht_content_type, "application/json"))
    {
      httpd_send_response(pstate, 415, "text/plain", allow, NULL, 0);
      return;
    }

  if (pstate->ht_body == NULL)
    {
      httpd_send_response(pstate, 400, "text/plain", allow, NULL, 0);
      return;
    }

  resp = calloc(1, CONFIG_FRUITCLAW_MCP_MAX_RESPONSE);
  if (resp == NULL)
    {
      httpd_send_response(pstate, 500, "text/plain", allow, NULL, 0);
      return;
    }

  memset(&ctx, 0, sizeof(ctx));
  fc_strlcpy(ctx.source, "mcp", sizeof(ctx.source));
  fc_strlcpy(ctx.channel, "mcp", sizeof(ctx.channel));
  fc_strlcpy(ctx.session_id,
             pstate->ht_mcp_session_id[0] ? pstate->ht_mcp_session_id :
             "mcp", sizeof(ctx.session_id));
#ifdef CONFIG_FRUITCLAW_MCP_YOLO_MODE
  ctx.owner_mode = true;
#endif

  ret = fc_mcp_jsonrpc_dispatch(pstate->ht_body, pstate->ht_bodylen,
                                resp, CONFIG_FRUITCLAW_MCP_MAX_RESPONSE,
                                &dispatch_status, &http_status, &ctx);
  if (ret < 0)
    {
      httpd_send_response(pstate, 500, "text/plain", allow, NULL, 0);
      free(resp);
      return;
    }

  if (dispatch_status == FC_MCP_DISPATCH_NOTIFICATION_ACCEPTED)
    {
      httpd_send_response(pstate, 202, "text/plain", allow, NULL, 0);
    }
  else
    {
      httpd_send_response(pstate, http_status, "application/json", allow,
                          resp, strlen(resp));
    }

  free(resp);
}

HTTPD_CGI_CALL(g_fc_mcp_cgi, "/mcp", fc_mcp_http_handler);

int fc_mcp_register_http(void)
{
  if (g_mcp_registered)
    {
      return 0;
    }

  httpd_cgi_register(&g_fc_mcp_cgi);
  g_mcp_registered = true;
  FC_LOGI("MCP YOLO endpoint registered at /mcp");
  return 0;
}

int fc_mcp_status(FILE *out)
{
  if (out == NULL)
    {
      out = stdout;
    }

  fprintf(out, "  mcp: enabled\n");
    {
      char status[512];

      if (fc_mcp_status_format(status, sizeof(status)) == 0)
        {
          fprintf(out, "  %s\n", status);
        }
    }
  fprintf(out, "  mcp_route: %s\n", g_mcp_registered ? "registered" :
          "not registered");
  fprintf(out, "  mcp_yolo_mode: %s\n",
#ifdef CONFIG_FRUITCLAW_MCP_YOLO_MODE
          "enabled"
#else
          "disabled"
#endif
          );
  fprintf(out, "  mcp_visible_tools: %u\n", fc_cap_count(true));
  return 0;
}

int fc_mcp_status_format(char *out, size_t out_len)
{
  unsigned long requests;
  unsigned long failures;
  unsigned long notifications;
  unsigned long suppressed;
  unsigned long tools;
  unsigned long tool_failures;
  int64_t status_notify_ms;
  int64_t activity_ms;
  int64_t last_ms;
  int64_t now = fc_mono_ms();
  int last_ret;
  int last_http;
  int last_dispatch;
  char method[64];
  char tool[FC_TOOL_NAME_LEN];

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_mcp_status_lock);
  requests = g_mcp_request_count;
  failures = g_mcp_failure_count;
  notifications = g_mcp_notification_count;
  suppressed = g_mcp_notification_suppressed_count;
  tools = g_mcp_tool_call_count;
  tool_failures = g_mcp_tool_failure_count;
  status_notify_ms = g_mcp_status_notify_ms;
  activity_ms = g_mcp_activity_ms;
  last_ms = g_mcp_last_ms;
  last_ret = g_mcp_last_ret;
  last_http = g_mcp_last_http_status;
  last_dispatch = g_mcp_last_dispatch_status;
  fc_strlcpy(method, g_mcp_last_method[0] ? g_mcp_last_method : "-",
             sizeof(method));
  fc_strlcpy(tool, g_mcp_last_tool[0] ? g_mcp_last_tool : "-",
             sizeof(tool));
  pthread_mutex_unlock(&g_mcp_status_lock);

  snprintf(out, out_len,
           "mcp_status: registered=%s requests=%lu failures=%lu "
           "notifications=%lu notify_suppressed=%lu tools=%lu "
           "tool_failures=%lu last_ret=%d last_http=%d last_dispatch=%d "
           "last_age_ms=%lld activity_age_ms=%lld "
           "status_notify_age_ms=%lld last_method=%s last_tool=%s",
           g_mcp_registered ? "yes" : "no",
           requests, failures, notifications, suppressed, tools,
           tool_failures,
           last_ret, last_http, last_dispatch,
           last_ms > 0 ? (long long)(now - last_ms) : -1,
           activity_ms > 0 ? (long long)(now - activity_ms) : -1,
           status_notify_ms > 0 ? (long long)(now - status_notify_ms) : -1,
           method, tool);
  return 0;
}

#endif /* CONFIG_FRUITCLAW_MCP_SERVER */
