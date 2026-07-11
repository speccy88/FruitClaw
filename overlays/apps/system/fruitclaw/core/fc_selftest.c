/* SPDX-License-Identifier: Apache-2.0 */

#include "fruitclaw.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_FRUITCLAW_MCP_SERVER
#include "netutils/cJSON.h"
#define FC_SELFTEST_JSON_BUF_SIZE CONFIG_FRUITCLAW_MCP_MAX_RESPONSE
#else
#define FC_SELFTEST_JSON_BUF_SIZE CONFIG_FRUITCLAW_MAX_JSON
#endif

static int g_fake_called;
static fc_tool_context_t g_fake_last_ctx;

static int fake_cap(const fc_tool_context_t *ctx, const char *args_json,
                    char *out_json, size_t out_len)
{
  (void)args_json;

  g_fake_called++;
  memset(&g_fake_last_ctx, 0, sizeof(g_fake_last_ctx));
  if (ctx != NULL)
    {
      g_fake_last_ctx = *ctx;
    }

  snprintf(out_json, out_len, "{\"ok\":true}");
  return 0;
}

static int check(bool expr, const char *name)
{
  fc_guard_session_heartbeat("selftest");
  if (!expr)
    {
      printf("FAIL: %s\n", name);
      fflush(stdout);
      return -EINVAL;
    }

  printf("PASS: %s\n", name);
  fflush(stdout);
  return 0;
}

static int check_detail(bool expr, const char *name, const char *detail)
{
  if (!expr && detail != NULL && detail[0] != '\0')
    {
      printf("DETAIL: %s: %.512s\n", name, detail);
      fflush(stdout);
    }

  return check(expr, name);
}

static int restore_disabled_marker(const char *path, bool existed)
{
  if (path == NULL || path[0] == '\0')
    {
      return 0;
    }

  if (existed)
    {
      return fc_write_text_file_atomic(path, "disabled\n");
    }

  if (unlink(path) < 0 && errno != ENOENT)
    {
      return -errno;
    }

  return 0;
}

#ifdef CONFIG_FRUITCLAW_MCP_SERVER
static int json_key_count(const char *json, const char *key)
{
  cJSON *root;
  cJSON *item;
  int count = 0;

  if (json == NULL || key == NULL)
    {
      return -1;
    }

  root = cJSON_Parse(json);
  if (root == NULL)
    {
      return -1;
    }

  for (item = root->child; item != NULL; item = item->next)
    {
      if (item->string != NULL && strcmp(item->string, key) == 0)
        {
          count++;
        }
    }

  cJSON_Delete(root);
  return count;
}

static long status_counter(const char *status, const char *key)
{
  const char *p;

  if (status == NULL || key == NULL)
    {
      return -1;
    }

  p = strstr(status, key);
  if (p == NULL)
    {
      return -1;
    }

  p += strlen(key);
  if (*p != '=')
    {
      return -1;
    }

  return strtol(p + 1, NULL, 10);
}
#endif

int fc_selftest_main(void)
{
  static const fc_cap_t fake =
  {
    "self.fake", "Fake", "Fake test cap",
    "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
    false, false, fake_cap
  };
  static const fc_cap_t owner_fake =
  {
    "self.owner_fake", "Owner fake", "Owner fake test cap",
    "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
    false, true, fake_cap
  };

  static char buf[FC_SELFTEST_JSON_BUF_SIZE];
  static char mapped[FC_TOOL_NAME_LEN];
  static char safe[FC_SESSION_ID_LEN];
  static char path[FC_PATH_LEN];
#ifdef CONFIG_FRUITCLAW_MCP_SERVER
  static char small[64];
#endif
  static char status[512];
  static fc_event_t ev;
  static struct tm tmv;
  static fc_tool_context_t tool_ctx;
  fc_queue_t *q = fc_main_queue();
  int ret = 0;
#ifdef CONFIG_FRUITCLAW_MCP_SERVER
  int dispatch_status;
  int http_status;
#endif

  fc_guard_session_heartbeat("selftest-start");
  ret |= check(fc_json_escape("a\"b\\c\n", buf, sizeof(buf)) == 0 &&
               strcmp(buf, "a\\\"b\\\\c\\n") == 0, "json escape");

  fc_strlcpy(buf, "  token\\r\\n  ", sizeof(buf));
  fc_trim(buf);
  ret |= check(strcmp(buf, "token") == 0, "trim escaped newline");

  fc_queue_init(q);
  memset(&ev, 0, sizeof(ev));
  fc_strlcpy(ev.source, "cli", sizeof(ev.source));
  fc_strlcpy(ev.type, "message.in", sizeof(ev.type));
  fc_strlcpy(ev.text, "hello", sizeof(ev.text));
  ret |= check(fc_queue_publish(q, &ev) == 0, "queue publish");
  ret |= check(fc_queue_count(q) == 1, "queue count");
  memset(&ev, 0, sizeof(ev));
  ret |= check(fc_queue_receive(q, &ev, 10) == 0 &&
               strcmp(ev.text, "hello") == 0, "queue receive");
  ret |= check(fc_queue_count(q) == 0, "queue empty");

  fc_cap_init();
  if (fc_cap_find("self.fake") == NULL)
    {
      ret |= check(fc_cap_register(&fake) == 0, "cap register");
    }
  else
    {
      ret |= check(true, "cap register");
    }

  ret |= check(fc_cap_find("self.fake") != NULL, "cap find");
  ret |= check(fc_cap_find("script.run") != NULL, "script run cap");
#ifdef CONFIG_FRUITCLAW_ENABLE_SHELL_TOOL
  ret |= check(fc_cap_find("shell.safe_command") != NULL,
               "shell alias enabled");
#else
  ret |= check(fc_cap_find("shell.safe_command") == NULL,
               "shell alias disabled");
#endif
  g_fake_called = 0;
  buf[0] = '\0';
  ret |= check(fc_cap_execute("self.fake", "{}", buf, sizeof(buf)) == 0 &&
               g_fake_called == 1, "cap execute");
  if (fc_cap_find("self.owner_fake") == NULL)
    {
      ret |= check(fc_cap_register(&owner_fake) == 0, "owner cap register");
    }
  else
    {
      ret |= check(true, "owner cap register");
    }

  memset(&tool_ctx, 0, sizeof(tool_ctx));
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "self.owner_fake", "{}",
                                  buf, sizeof(buf)) < 0 &&
               strstr(buf, "owner mode") != NULL, "owner policy deny");
  tool_ctx.owner_mode = true;
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "self.owner_fake", "{}",
                                  buf, sizeof(buf)) == 0,
               "owner policy allow");

  buf[0] = '\0';
  ret |= check(fc_service_control("all", "status", buf, sizeof(buf)) == 0 &&
               strstr(buf, "\"telnetd\"") != NULL &&
               strstr(buf, "\"ftpd\"") != NULL &&
               strstr(buf, "\"webserver\"") != NULL,
               "service status all");
  buf[0] = '\0';
  ret |= check(fc_service_control("not-a-service", "status",
                                  buf, sizeof(buf)) < 0 &&
               strstr(buf, "unknown service") != NULL,
               "service status unknown");
  buf[0] = '\0';
  ret |= check(fc_service_control("all", "restart", buf, sizeof(buf)) < 0 &&
               strstr(buf, "service required") != NULL,
               "service all action rejected");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "service.control",
                                  "{\"service\":\"telnetd\"}",
                                  buf, sizeof(buf)) < 0 &&
               strstr(buf, "missing service or action") != NULL,
               "service control missing action");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "service.status",
                                  "{\"service\":\"webserver\"}",
                                  buf, sizeof(buf)) == 0 &&
               strstr(buf, "\"service\":\"webserver\"") != NULL &&
               strstr(buf, "\"httpd_activity\"") != NULL,
               "service status webserver");

#ifdef CONFIG_SYSTEM_TELNETD
  {
    bool marker_existed = false;
    bool marker_ok;

    marker_ok = fc_data_path("services/telnetd.disabled",
                             path, sizeof(path)) == 0;
    if (marker_ok)
      {
        marker_existed = access(path, F_OK) == 0;
      }

    buf[0] = '\0';
    ret |= check(fc_service_control("telnetd", "status",
                                    buf, sizeof(buf)) == 0 &&
                 strstr(buf, "\"service\":\"telnetd\"") != NULL,
                 "service status telnetd");
    buf[0] = '\0';
    ret |= check(fc_service_control("telnetd", "disable",
                                    buf, sizeof(buf)) == 0 &&
                 strstr(buf, "\"enabled\":false") != NULL &&
                 (!marker_ok || access(path, F_OK) == 0),
                 "service disable telnetd marker");
    buf[0] = '\0';
    ret |= check(fc_service_control("telnetd", "enable",
                                    buf, sizeof(buf)) == 0 &&
                 strstr(buf, "\"enabled\":true") != NULL &&
                 (!marker_ok || access(path, F_OK) < 0),
                 "service enable telnetd marker");
    ret |= check(restore_disabled_marker(marker_ok ? path : "",
                                         marker_existed) == 0,
                 "service restore telnetd marker");
  }
#endif

#if defined(CONFIG_EXAMPLES_FTPD) && defined(CONFIG_NETUTILS_FTPD)
  {
    bool marker_existed = false;
    bool marker_ok;

    marker_ok = fc_data_path("services/ftpd.disabled",
                             path, sizeof(path)) == 0;
    if (marker_ok)
      {
        marker_existed = access(path, F_OK) == 0;
      }

    buf[0] = '\0';
    ret |= check(fc_service_control("ftpd", "status",
                                    buf, sizeof(buf)) == 0 &&
                 strstr(buf, "\"service\":\"ftpd\"") != NULL,
                 "service status ftpd");
    buf[0] = '\0';
    ret |= check(fc_service_control("ftpd", "disable",
                                    buf, sizeof(buf)) == 0 &&
                 strstr(buf, "\"enabled\":false") != NULL &&
                 (!marker_ok || access(path, F_OK) == 0),
                 "service disable ftpd marker");
    buf[0] = '\0';
    ret |= check(fc_service_control("ftpd", "enable",
                                    buf, sizeof(buf)) == 0 &&
                 strstr(buf, "\"enabled\":true") != NULL &&
                 (!marker_ok || access(path, F_OK) < 0),
                 "service enable ftpd marker");
    ret |= check(restore_disabled_marker(marker_ok ? path : "",
                                         marker_existed) == 0,
                 "service restore ftpd marker");
  }
#endif

#ifdef CONFIG_FRUITCLAW_MCP_SERVER
  fc_mcp_clear_status();
  memset(&tool_ctx, 0, sizeof(tool_ctx));
  fc_strlcpy(tool_ctx.source, "mcp", sizeof(tool_ctx.source));
  fc_strlcpy(tool_ctx.channel, "mcp", sizeof(tool_ctx.channel));
  fc_strlcpy(tool_ctx.session_id, "selftest", sizeof(tool_ctx.session_id));
#ifdef CONFIG_FRUITCLAW_MCP_YOLO_MODE
  tool_ctx.owner_mode = false;
#else
  tool_ctx.owner_mode = true;
#endif
  dispatch_status = -1;
  http_status = -1;
  buf[0] = '\0';
  ret |= check(fc_mcp_jsonrpc_dispatch(
               "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
               "\"params\":{}}",
               strlen("{\"jsonrpc\":\"2.0\",\"id\":1,"
                      "\"method\":\"initialize\",\"params\":{}}"),
               buf, sizeof(buf), &dispatch_status, &http_status,
               &tool_ctx) == 0 &&
               strstr(buf, "\"protocolVersion\":\"2025-11-25\"") != NULL,
               "mcp initialize");
  ret |= check(json_key_count(buf, "result") == 1,
               "mcp single result member");
  ret |= check(dispatch_status == FC_MCP_DISPATCH_RESPONSE &&
               http_status == 200, "mcp initialize status");
  dispatch_status = -1;
  http_status = -1;
  buf[0] = '\0';
  ret |= check(fc_mcp_jsonrpc_dispatch(
               "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\","
               "\"params\":{}}",
               strlen("{\"jsonrpc\":\"2.0\",\"method\":"
                      "\"notifications/initialized\",\"params\":{}}"),
               buf, sizeof(buf), &dispatch_status, &http_status,
               &tool_ctx) == 0 &&
               buf[0] == '\0',
               "mcp initialized notification");
  ret |= check(dispatch_status ==
               FC_MCP_DISPATCH_NOTIFICATION_ACCEPTED &&
               http_status == 202, "mcp notification status");
  buf[0] = '\0';
  ret |= check(fc_mcp_jsonrpc_dispatch(
               "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"ping\"}",
               strlen("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"ping\"}"),
               buf, sizeof(buf), NULL, NULL, &tool_ctx) == 0 &&
               strstr(buf, "\"result\":{}") != NULL,
               "mcp ping");
  buf[0] = '\0';
	  ret |= check(fc_mcp_jsonrpc_dispatch(
	               "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\","
	               "\"params\":{}}",
	               strlen("{\"jsonrpc\":\"2.0\",\"id\":2,"
	                      "\"method\":\"tools/list\",\"params\":{}}"),
	               buf, sizeof(buf), NULL, NULL, &tool_ctx) == 0 &&
	               strstr(buf, "\"time.now\"") != NULL,
	               "mcp tools list");
	  buf[0] = '\0';
	  ret |= check(fc_cap_build_openai_tools_json(
	               buf, sizeof(buf), true) == 0 &&
	               strstr(buf, "\"name\":\"time_now\"") != NULL &&
	               strstr(buf, "\"name\":\"system_status\"") != NULL,
	               "openai tools schema fits");
	  buf[0] = '\0';
	  g_fake_called = 0;
  memset(&g_fake_last_ctx, 0, sizeof(g_fake_last_ctx));
  ret |= check(fc_mcp_jsonrpc_dispatch(
               "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
               "\"params\":{\"name\":\"self.owner_fake\","
               "\"arguments\":{}}}",
               strlen("{\"jsonrpc\":\"2.0\",\"id\":3,"
                      "\"method\":\"tools/call\",\"params\":{\"name\":"
                      "\"self.owner_fake\",\"arguments\":{}}}"),
               buf, sizeof(buf), NULL, NULL, &tool_ctx) == 0 &&
#ifdef CONFIG_FRUITCLAW_MCP_YOLO_MODE
               strstr(buf, "\"isError\":false") != NULL,
               "mcp yolo owner tool call");
  ret |= check(g_fake_called == 1 &&
               g_fake_last_ctx.owner_mode &&
               g_fake_last_ctx.guarded &&
               strcmp(g_fake_last_ctx.source, "mcp") == 0 &&
               strcmp(g_fake_last_ctx.channel, "mcp") == 0 &&
               strcmp(g_fake_last_ctx.session_id, "selftest") == 0,
               "mcp owner guarded context");
#else
               strstr(buf, "\"isError\":false") != NULL,
               "mcp owner tool call");
#endif
  buf[0] = '\0';
  ret |= check(fc_mcp_jsonrpc_dispatch(
               "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\","
               "\"params\":{\"name\":\"system.status\","
               "\"arguments\":{}}}",
               strlen("{\"jsonrpc\":\"2.0\",\"id\":9,"
                      "\"method\":\"tools/call\",\"params\":{\"name\":"
                      "\"system.status\",\"arguments\":{}}}"),
               buf, sizeof(buf), NULL, NULL, &tool_ctx) == 0 &&
               strstr(buf, "\"isError\":false") != NULL,
               "mcp status tool call");
  buf[0] = '\0';
  ret |= check(fc_mcp_jsonrpc_dispatch(
               "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\","
               "\"params\":{\"name\":\"system.status\","
               "\"arguments\":{}}}",
               strlen("{\"jsonrpc\":\"2.0\",\"id\":10,"
                      "\"method\":\"tools/call\",\"params\":{\"name\":"
                      "\"system.status\",\"arguments\":{}}}"),
               buf, sizeof(buf), NULL, NULL, &tool_ctx) == 0 &&
               strstr(buf, "\"isError\":false") != NULL,
               "mcp repeated status tool call");
  status[0] = '\0';
  ret |= check(fc_mcp_status_format(status, sizeof(status)) == 0 &&
               status_counter(status, "notify_suppressed") > 0,
               "mcp status notification suppression");
  buf[0] = '\0';
  ret |= check(fc_mcp_jsonrpc_dispatch("{bad json", 9, buf, sizeof(buf),
                                       NULL, NULL, &tool_ctx) == 0 &&
               strstr(buf, "\"code\":-32700") != NULL,
               "mcp malformed json");
  status[0] = '\0';
  ret |= check(fc_mcp_status_format(status, sizeof(status)) == 0 &&
               status_counter(status, "failures") > 0,
               "mcp records jsonrpc failure");
  buf[0] = '\0';
  ret |= check(fc_mcp_jsonrpc_dispatch(
               "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
               "\"params\":{\"name\":\"time.now\",\"arguments\":[]}}",
               strlen("{\"jsonrpc\":\"2.0\",\"id\":4,"
                      "\"method\":\"tools/call\",\"params\":{\"name\":"
                      "\"time.now\",\"arguments\":[]}}"),
               buf, sizeof(buf), NULL, NULL, &tool_ctx) == 0 &&
               strstr(buf, "\"code\":-32602") != NULL,
               "mcp invalid params");
  buf[0] = '\0';
  ret |= check(fc_mcp_jsonrpc_dispatch(
               "[{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"ping\"}]",
               strlen("[{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"ping\"}]"),
               buf, sizeof(buf), NULL, NULL, &tool_ctx) == 0 &&
               strstr(buf, "\"code\":-32600") != NULL,
               "mcp reject batch");
  buf[0] = '\0';
  ret |= check(fc_mcp_jsonrpc_dispatch(
               "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"nope\"}",
               strlen("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"nope\"}"),
               buf, sizeof(buf), NULL, NULL, &tool_ctx) == 0 &&
               strstr(buf, "\"code\":-32601") != NULL,
               "mcp unknown method");
  small[0] = '\0';
  ret |= check(fc_mcp_jsonrpc_dispatch(
               "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\","
               "\"params\":{}}",
               strlen("{\"jsonrpc\":\"2.0\",\"id\":7,"
                      "\"method\":\"tools/list\",\"params\":{}}"),
               small, sizeof(small), NULL, NULL, &tool_ctx) < 0,
               "mcp bounded response");
  status[0] = '\0';
  ret |= check(fc_mcp_status_format(status, sizeof(status)) == 0 &&
               strstr(status, "mcp_status:") != NULL &&
               strstr(status, "requests=") != NULL &&
               strstr(status, "tools=") != NULL,
               "mcp status");
#endif

  ret |= check(fc_tool_name_to_openai("time.now", mapped, sizeof(mapped)) == 0 &&
               strcmp(mapped, "time_now") == 0, "tool map forward");
  ret |= check(fc_tool_name_from_openai("time_now", mapped,
                                        sizeof(mapped)) == 0 &&
               strcmp(mapped, "time.now") == 0, "tool map reverse");

  memset(&tool_ctx, 0, sizeof(tool_ctx));
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "system.status", "{}",
                                  buf, sizeof(buf)) == 0 &&
               strstr(buf, "\"ok\":true") != NULL &&
               strstr(buf, "\"queues\"") != NULL &&
               strstr(buf, "\"visible_tools\"") != NULL,
               "system status tool");

  memset(&tmv, 0, sizeof(tmv));
  tmv.tm_min = 10;
  tmv.tm_hour = 8;
  tmv.tm_mday = 15;
  tmv.tm_mon = 5;
  tmv.tm_wday = 1;
  ret |= check(fc_cron_validate("*/5 8 * * 1") == 0,
               "cron validate");
  ret |= check(fc_cron_validate("*/0 8 * * 1") < 0,
               "cron reject zero step");
  ret |= check(fc_cron_validate("*/5 24 * * 1") < 0,
               "cron reject hour range");
  ret |= check(fc_cron_validate("*/5 8 * * 1 extra") < 0,
               "cron reject extra field");
  ret |= check(fc_cron_validate("*/5 8 * * 1,") < 0,
               "cron reject trailing comma");
  ret |= check(fc_cron_matches("*/5 8 * * 1", &tmv), "cron match");
  ret |= check(!fc_cron_matches("*/7 8 * * 1", &tmv), "cron reject");

  ret |= check(fc_session_safe_filename("telegram:1234", safe,
                                        sizeof(safe)) == 0 &&
               strcmp(safe, "telegram_1234") == 0, "session sanitize");
  ret |= check(fc_session_safe_filename("../bad", safe,
                                        sizeof(safe)) < 0,
               "session reject traversal");
  ret |= check(fc_telegram_should_ack_offset(true, -ENOSPC),
               "telegram offset ack ignored");
  ret |= check(fc_telegram_should_ack_offset(false, 0),
               "telegram offset ack queued");
  ret |= check(!fc_telegram_should_ack_offset(false, -ENOSPC),
               "telegram offset holds queue failure");
  ret |= check(!fc_operator_progress_stale(1000, 5000, 2000, 0),
               "operator guard disabled");
  ret |= check(!fc_operator_progress_stale(1000, 5000, 2000, 3000),
               "operator guard fresh progress");
  ret |= check(fc_operator_progress_stale(1000, 5000, 4000, 3000),
               "operator guard stale progress");
  ret |= check(!fc_operator_progress_stale(1000, 3000, -1, 3000),
               "operator guard waits for first progress");
  ret |= check(fc_operator_progress_stale(1000, 5000, -1, 3000),
               "operator guard no progress");
  fc_operator_progress_mark("selftest");
  status[0] = '\0';
  ret |= check(fc_operator_progress_status_format(status,
                                                  sizeof(status)) == 0 &&
               strstr(status, "operator_progress:") != NULL &&
               strstr(status, "source=selftest") != NULL,
               "operator progress status");

  fc_init_data_dir();
  memset(&tool_ctx, 0, sizeof(tool_ctx));
  tool_ctx.owner_mode = true;
  tool_ctx.guarded = true;
  buf[0] = '\0';
  ret |= check_detail(fc_cap_execute_ctx(&tool_ctx, "file.write_limited",
                      "{\"path\":\"scripts/selftest_write.txt\","
                      "\"text\":\"claw.reply(\\\"selftest\\\")\\n\"}",
                      buf, sizeof(buf)) == 0, "file write limited", buf);
  buf[0] = '\0';
  ret |= check(fc_data_path("scripts/selftest_write.txt", path,
                            sizeof(path)) == 0 &&
               fc_read_text_file(path, buf, sizeof(buf), false) == 0 &&
               strcmp(buf, "claw.reply(\"selftest\")\n") == 0,
               "file write preserves text");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "file.write_limited",
               "{\"path\":\"../bad\",\"text\":\"nope\"}",
               buf, sizeof(buf)) < 0 &&
               strstr(buf, "path denied") != NULL,
               "file write rejects traversal");

  buf[0] = '\0';
  ret |= check(fc_builtin_terminal_run(&tool_ctx,
               "{\"command\":\"uname -a\"}", buf, sizeof(buf)) == 0 &&
               strstr(buf, "\"status\":0") != NULL,
               "terminal fast path");
  buf[0] = '\0';
  ret |= check(fc_builtin_terminal_run(&tool_ctx,
               "{\"command\":\"  uname -a  \"}", buf, sizeof(buf)) == 0 &&
               strstr(buf, "\"status\":0") != NULL,
               "terminal trims command");
  buf[0] = '\0';
  ret |= check(fc_builtin_terminal_run(&tool_ctx,
               "{\"command\":\"help\"}", buf, sizeof(buf)) == 0 &&
               strstr(buf, "Fast in-process commands") != NULL,
               "terminal help fast path");
  buf[0] = '\0';
  ret |= check(fc_builtin_terminal_run(&tool_ctx,
               "{\"command\":\"ls /dev\"}", buf, sizeof(buf)) == 0 &&
               strstr(buf, "device.list") != NULL,
               "terminal ls dev fast path");
  buf[0] = '\0';
  ret |= check(fc_builtin_terminal_run(&tool_ctx,
               "{\"command\":\"fruitclaw telegram-discover\"}",
               buf, sizeof(buf)) < 0 &&
               strstr(buf, "recursive fruitclaw command denied") != NULL,
               "terminal rejects recursive fruitclaw");

  fc_scheduler_remove("selftest-job");
  fc_scheduler_remove("selftest-once");
  fc_scheduler_remove("selftest-after");
  fc_scheduler_remove("selftest-boot");
  fc_scheduler_remove("selftest-cron");
  buf[0] = '\0';
  ret |= check_detail(fc_cap_execute_ctx(&tool_ctx, "scheduler.add",
                      "{\"id\":\"selftest-job\",\"type\":\"interval\","
                      "\"every_sec\":60,\"prompt\":\"selftest\"}",
                      buf, sizeof(buf)) == 0 &&
                      strstr(buf, "\"id\":\"selftest-job\"") != NULL,
                      "scheduler add", buf);
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "scheduler.list", "{}",
                                  buf, sizeof(buf)) == 0 &&
               strstr(buf, "selftest-job") != NULL &&
               strstr(buf, "\"jobs\"") != NULL &&
               strstr(buf, "\"type\":\"interval\"") != NULL &&
               strstr(buf, "\"prompt\":\"selftest\"") != NULL,
               "scheduler list structured interval");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "scheduler.remove",
               "{\"id\":\"selftest-job\"}", buf, sizeof(buf)) == 0,
               "scheduler remove");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "scheduler.add",
               "{\"id\":\"selftest-once\",\"type\":\"once\","
               "\"at_epoch\":4102444800,\"prompt\":\"selftest once\"}",
               buf, sizeof(buf)) == 0 &&
               strstr(buf, "\"id\":\"selftest-once\"") != NULL,
               "scheduler add once");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "scheduler.list", "{}",
                                  buf, sizeof(buf)) == 0 &&
               strstr(buf, "selftest-once") != NULL &&
               strstr(buf, "once") != NULL &&
               strstr(buf, "at_epoch=4102444800") != NULL &&
               strstr(buf, "\"at_epoch\":4102444800") != NULL,
               "scheduler list structured once");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "scheduler.remove",
               "{\"id\":\"selftest-once\"}", buf, sizeof(buf)) == 0,
               "scheduler remove once");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "scheduler.add",
               "{\"id\":\"selftest-after\",\"type\":\"once\","
               "\"after_sec\":60,\"prompt\":\"selftest after\"}",
               buf, sizeof(buf)) == 0 &&
               strstr(buf, "\"id\":\"selftest-after\"") != NULL,
               "scheduler add after");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "scheduler.remove",
               "{\"id\":\"selftest-after\"}", buf, sizeof(buf)) == 0,
               "scheduler remove after");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "scheduler.add",
               "{\"id\":\"selftest-cron\",\"type\":\"cron\","
               "\"expr\":\"*/5 8 * * 1\",\"prompt\":\"selftest cron\"}",
               buf, sizeof(buf)) == 0 &&
               strstr(buf, "\"id\":\"selftest-cron\"") != NULL,
               "scheduler add cron");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "scheduler.list", "{}",
                                  buf, sizeof(buf)) == 0 &&
               strstr(buf, "selftest-cron") != NULL &&
               strstr(buf, "\"expr\":\"*/5 8 * * 1\"") != NULL,
               "scheduler list structured cron");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "scheduler.remove",
               "{\"id\":\"selftest-cron\"}", buf, sizeof(buf)) == 0,
               "scheduler remove cron");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "scheduler.add",
               "{\"id\":\"selftest-bad-cron\",\"type\":\"cron\","
               "\"expr\":\"*/0 * * * *\",\"prompt\":\"bad cron\"}",
               buf, sizeof(buf)) < 0 &&
               strstr(buf, "invalid cron expression") != NULL,
               "scheduler rejects bad cron");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "scheduler.add",
               "{\"id\":\"selftest-boot\",\"type\":\"boot\","
               "\"prompt\":\"selftest boot\"}",
               buf, sizeof(buf)) == 0 &&
               strstr(buf, "\"id\":\"selftest-boot\"") != NULL,
               "scheduler add boot");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "scheduler.list", "{}",
                                  buf, sizeof(buf)) == 0 &&
               strstr(buf, "selftest-boot") != NULL &&
               strstr(buf, "boot") != NULL &&
               strstr(buf, "\"type\":\"boot\"") != NULL,
               "scheduler list structured boot");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "scheduler.remove",
               "{\"id\":\"selftest-boot\"}", buf, sizeof(buf)) == 0,
               "scheduler remove boot");
  status[0] = '\0';
  ret |= check(fc_scheduler_status_format(status, sizeof(status)) == 0 &&
               strstr(status, "scheduler_status:") != NULL &&
               strstr(status, "used=") != NULL,
               "scheduler status");

  {
    char session_path[FC_PATH_LEN] = "";

    if (fc_data_path("sessions/selftest-scheduled-tool.jsonl",
                     session_path, sizeof(session_path)) == 0)
      {
        unlink(session_path);
      }

    memset(&ev, 0, sizeof(ev));
    fc_strlcpy(ev.source, "scheduler", sizeof(ev.source));
    fc_strlcpy(ev.type, "timer.fire", sizeof(ev.type));
    fc_strlcpy(ev.channel, "telegram", sizeof(ev.channel));
    fc_strlcpy(ev.chat_id, "6216681418", sizeof(ev.chat_id));
    fc_strlcpy(ev.session_id, "selftest-scheduled-tool",
               sizeof(ev.session_id));
    fc_strlcpy(ev.text, "tool:self.owner_fake {\"from\":\"schedule\"}",
               sizeof(ev.text));
    ev.owner_mode = true;
    g_fake_called = 0;
    memset(&g_fake_last_ctx, 0, sizeof(g_fake_last_ctx));
    buf[0] = '\0';
    ret |= check(fc_agent_handle_event(&ev, buf, sizeof(buf)) == 0 &&
                 strstr(buf, "Scheduled tool self.owner_fake -> ok") !=
                 NULL &&
                 g_fake_called == 1 &&
                 g_fake_last_ctx.owner_mode &&
                 strcmp(g_fake_last_ctx.source, "scheduler") == 0 &&
                 strcmp(g_fake_last_ctx.channel, "telegram") == 0 &&
                 strcmp(g_fake_last_ctx.chat_id, "6216681418") == 0 &&
                 strcmp(g_fake_last_ctx.session_id,
                        "selftest-scheduled-tool") == 0,
                 "scheduler direct owner tool");

    if (session_path[0] != '\0')
      {
        unlink(session_path);
      }
  }

  status[0] = '\0';
  ret |= check(fc_berry_status_format(status, sizeof(status)) == 0 &&
               strstr(status, "berry_status:") != NULL &&
               strstr(status, "runner=") != NULL,
               "berry status");

  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "device.read",
               "{\"path\":\"/tmp/notdev\"}", buf, sizeof(buf)) < 0 &&
               strstr(buf, "path denied") != NULL,
               "device read path bounds");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "device.read",
               "{\"path\":\"/dev/mmcsd0\",\"max_bytes\":16}",
               buf, sizeof(buf)) < 0 &&
               strstr(buf, "block device denied") != NULL,
               "device read rejects block devices");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "device.write",
               "{\"path\":\"/dev/null\",\"mode\":\"hex\",\"data\":\"0\"}",
               buf, sizeof(buf)) < 0 &&
               strstr(buf, "invalid hex") != NULL,
               "device write invalid hex");

  buf[0] = '\0';
  ret |= check(fc_berry_run_file(&tool_ctx, "../bad.be", "{}",
                                 buf, sizeof(buf)) < 0 &&
               strstr(buf, "path denied") != NULL,
               "berry path jail");

  {
    char script_path[FC_PATH_LEN] = "";

    fc_scheduler_remove("selftest-script-boot");
    if (fc_data_path("scripts/generated/selftest_shell.nsh",
                     script_path, sizeof(script_path)) == 0)
      {
        unlink(script_path);
      }

    buf[0] = '\0';
    ret |= check_detail(fc_cap_execute_ctx(&tool_ctx, "script.write",
                        "{\"name\":\"selftest_shell\",\"kind\":\"shell\","
                        "\"description\":\"selftest generated shell\","
                        "\"text\":\"echo selftest-shell\\n\","
                        "\"validate\":false}",
	                        buf, sizeof(buf)) == 0 &&
	                        strstr(buf, "\"kind\":\"shell\"") != NULL &&
	                        strstr(buf, "\"saved\":true") != NULL &&
	                        strstr(buf, "\"description\":"
	                               "\"selftest generated shell\"") != NULL,
	                        "script write shell", buf);
    buf[0] = '\0';
    ret |= check(fc_cap_execute_ctx(&tool_ctx, "script.read",
                 "{\"name\":\"selftest_shell\",\"kind\":\"shell\"}",
                 buf, sizeof(buf)) == 0 &&
                 strstr(buf, "\"selftest_shell.nsh\"") != NULL &&
                 strstr(buf, "\"kind\":\"shell\"") != NULL,
                 "script read shell");
    buf[0] = '\0';
	    ret |= check(fc_cap_execute_ctx(&tool_ctx, "script.list", "{}",
	                 buf, sizeof(buf)) == 0 &&
	                 strstr(buf, "selftest_shell.nsh") != NULL &&
	                 strstr(buf, "\"validated\":false") != NULL,
	                 "script list shell");
	    ret |= check(strstr(buf, "selftest generated shell") != NULL,
	                 "script list description");
    buf[0] = '\0';
    ret |= check(fc_cap_execute_ctx(&tool_ctx, "script.run",
                 "{\"name\":\"selftest_shell\",\"kind\":\"shell\","
                 "\"args_json\":\"{bad\"}",
                 buf, sizeof(buf)) < 0 &&
                 strstr(buf, "script args invalid JSON") != NULL,
                 "script run bad args rejected");
    buf[0] = '\0';
    ret |= check(fc_cap_execute_ctx(&tool_ctx, "script.schedule",
                 "{\"name\":\"selftest_shell\",\"kind\":\"shell\","
                 "\"type\":\"boot\",\"id\":\"selftest-script-bad\","
                 "\"args_json\":\"{bad\"}",
                 buf, sizeof(buf)) < 0 &&
                 strstr(buf, "script args invalid JSON") != NULL,
                 "script schedule bad args rejected");
    buf[0] = '\0';
    ret |= check(fc_cap_execute_ctx(&tool_ctx, "script.schedule",
                 "{\"name\":\"selftest_shell\",\"kind\":\"shell\","
                 "\"type\":\"cron\",\"id\":\"selftest-script-bad-cron\","
                 "\"expr\":\"61 * * * *\"}",
                 buf, sizeof(buf)) < 0 &&
                 strstr(buf, "invalid cron expression") != NULL,
                 "script schedule bad cron rejected");
    buf[0] = '\0';
    ret |= check(fc_cap_execute_ctx(&tool_ctx, "script.schedule",
                 "{\"name\":\"selftest_shell\",\"kind\":\"shell\","
                 "\"type\":\"boot\",\"id\":\"selftest-script-boot\"}",
                 buf, sizeof(buf)) == 0 &&
                 strstr(buf, "\"id\":\"selftest-script-boot\"") != NULL &&
                 strstr(buf, "\"type\":\"boot\"") != NULL,
                 "script schedule boot");
    buf[0] = '\0';
    ret |= check(fc_cap_execute_ctx(&tool_ctx, "scheduler.list", "{}",
                 buf, sizeof(buf)) == 0 &&
                 strstr(buf, "\"id\":\"selftest-script-boot\"") != NULL &&
                 strstr(buf, "\"script\"") != NULL &&
                 strstr(buf, "\"path\":\"scripts/generated/"
                             "selftest_shell.nsh\"") != NULL &&
                 strstr(buf, "\"kind\":\"shell\"") != NULL,
                 "scheduler list generated script ref");
    buf[0] = '\0';
    ret |= check(fc_cap_execute_ctx(&tool_ctx, "script.list", "{}",
                 buf, sizeof(buf)) == 0 &&
                 strstr(buf, "\"scheduled\":true") != NULL &&
                 strstr(buf, "selftest-script-boot") != NULL,
                 "script list schedule refs");
    buf[0] = '\0';
    ret |= check(fc_cap_execute_ctx(&tool_ctx, "scheduler.remove",
                 "{\"id\":\"selftest-script-boot\"}", buf, sizeof(buf)) == 0,
                 "script schedule boot remove");
    buf[0] = '\0';
    ret |= check(fc_cap_execute_ctx(&tool_ctx, "script.remove",
                 "{\"name\":\"selftest_shell\",\"kind\":\"shell\"}",
                 buf, sizeof(buf)) == 0 &&
                 strstr(buf, "\"selftest_shell.nsh\"") != NULL,
                 "script remove shell");
    if (script_path[0] != '\0')
      {
        ret |= check(access(script_path, F_OK) < 0,
                     "script remove deleted file");
      }

    if (script_path[0] != '\0')
      {
        unlink(script_path);
      }
  }

#if defined(CONFIG_FRUITCLAW_ENABLE_BERRY) && \
    defined(CONFIG_FRUITCLAW_BERRY_EXPERIMENTAL_RUNNER)
  {
    char lvgl_script_path[FC_PATH_LEN] = "";

    if (fc_data_path("scripts/generated/selftest_lvgl_ui.be",
                     lvgl_script_path, sizeof(lvgl_script_path)) == 0)
      {
        unlink(lvgl_script_path);
      }

    buf[0] = '\0';
    ret |= check(fc_cap_execute_ctx(&tool_ctx, "script.write",
                 "{\"name\":\"selftest_lvgl_ui\",\"kind\":\"berry\","
                 "\"text\":\"import lv\\n"
                 "lv.start()\\n"
                 "scr = lv.scr_act()\\n"
                 "label = lv.label(scr)\\n"
                 "label.set_text(\\\"selftest\\\")\\n\","
                 "\"validate_mode\":\"syntax\"}",
                 buf, sizeof(buf)) == 0 &&
                 strstr(buf, "\"validation_mode\":\"syntax\"") != NULL &&
                 strstr(buf, "\"validated\":true") != NULL,
                 "berry lvgl syntax script write");
    buf[0] = '\0';
	    ret |= check(fc_cap_execute_ctx(&tool_ctx, "script.validate",
	                 "{\"name\":\"selftest_lvgl_ui\",\"kind\":\"berry\","
	                 "\"mode\":\"syntax\"}",
	                 buf, sizeof(buf)) == 0 &&
	                 strstr(buf, "\"mode\":\"syntax\"") != NULL,
	                 "berry lvgl syntax validate");
	    buf[0] = '\0';
	    ret |= check(fc_cap_execute_ctx(&tool_ctx, "script.list", "{}",
	                 buf, sizeof(buf)) == 0 &&
	                 strstr(buf, "selftest_lvgl_ui.be") != NULL &&
	                 strstr(buf, "\"validated\":true") != NULL &&
	                 strstr(buf, "\"validation_mode\":\"syntax\"") != NULL,
	                 "berry lvgl validation metadata");
	    buf[0] = '\0';
	    ret |= check(fc_cap_execute_ctx(&tool_ctx, "script.remove",
	                 "{\"name\":\"selftest_lvgl_ui\",\"kind\":\"berry\"}",
                 buf, sizeof(buf)) == 0,
                 "berry lvgl syntax remove");

    if (lvgl_script_path[0] != '\0')
      {
        unlink(lvgl_script_path);
      }
  }

  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "file.write_limited",
               "{\"path\":\"scripts/selftest_import_claw.be\","
               "\"text\":\"import claw\\n"
               "claw.reply(\\\"selftest import\\\")\\n\"}",
               buf, sizeof(buf)) == 0,
               "berry import script write");
  fc_scheduler_remove("selftest-uploaded-berry-boot");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "script.list", "{}",
               buf, sizeof(buf)) == 0 &&
               strstr(buf, "\"path\":\"scripts/selftest_import_claw.be\"") !=
                 NULL &&
               strstr(buf, "\"source\":\"uploaded\"") != NULL,
               "script list uploaded berry");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "script.read",
               "{\"path\":\"scripts/selftest_import_claw.be\","
               "\"kind\":\"berry\"}",
               buf, sizeof(buf)) == 0 &&
               strstr(buf, "\"source\":\"uploaded\"") != NULL &&
               strstr(buf, "selftest import") != NULL,
               "script read uploaded berry");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "script.validate",
               "{\"path\":\"scripts/selftest_import_claw.be\","
               "\"kind\":\"berry\",\"mode\":\"syntax\"}",
               buf, sizeof(buf)) == 0 &&
               strstr(buf, "\"validated\":true") != NULL,
               "script validate uploaded berry");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "script.run",
               "{\"path\":\"scripts/selftest_import_claw.be\","
               "\"kind\":\"berry\"}",
               buf, sizeof(buf)) == 0 &&
               strstr(buf, "\"reply\":\"selftest import\"") != NULL,
               "script run uploaded berry");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "script.schedule",
               "{\"id\":\"selftest-uploaded-berry-boot\","
               "\"type\":\"boot\","
               "\"path\":\"scripts/selftest_import_claw.be\","
               "\"kind\":\"berry\"}",
               buf, sizeof(buf)) == 0 &&
               strstr(buf, "selftest-uploaded-berry-boot") != NULL,
               "script schedule uploaded berry");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "script.list", "{}",
               buf, sizeof(buf)) == 0 &&
               strstr(buf, "selftest-uploaded-berry-boot") != NULL &&
               strstr(buf, "\"scheduled\":true") != NULL,
               "script list uploaded schedule refs");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "scheduler.list", "{}",
               buf, sizeof(buf)) == 0 &&
               strstr(buf, "\"id\":\"selftest-uploaded-berry-boot\"") !=
                 NULL &&
               strstr(buf, "\"script\"") != NULL &&
               strstr(buf, "\"kind\":\"berry\"") != NULL,
               "scheduler list uploaded berry ref");
  fc_scheduler_remove("selftest-uploaded-berry-boot");
  fc_scheduler_remove("berry-selftest-after");
  buf[0] = '\0';
  ret |= check(fc_cap_execute_ctx(&tool_ctx, "file.write_limited",
               "{\"path\":\"scripts/selftest_schedule_claw.be\","
               "\"text\":\"import claw\\n"
               "claw.reply(claw.schedule_add(\\\"{\\\\\\\"id\\\\\\\":"
               "\\\\\\\"berry-selftest-after\\\\\\\",\\\\\\\"type\\\\\\\":"
               "\\\\\\\"once\\\\\\\",\\\\\\\"after_sec\\\\\\\":60,"
               "\\\\\\\"prompt\\\\\\\":\\\\\\\"berry selftest\\\\\\\"}\\\"))"
               "\\n\"}",
               buf, sizeof(buf)) == 0,
               "berry schedule script write");
  buf[0] = '\0';
  ret |= check(fc_berry_run_file(&tool_ctx, "selftest_schedule_claw.be",
                                 "{}", buf, sizeof(buf)) == 0 &&
               strstr(buf, "berry-selftest-after") != NULL,
               "berry nested scheduler tool");
  fc_scheduler_remove("berry-selftest-after");
#endif

  status[0] = '\0';
  ret |= check(fc_guard_status(status, sizeof(status)) == 0 &&
               strstr(status, "exec=") != NULL
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
               &&
               strstr(status, "max_uptime_ms=") != NULL,
#else
               ,
#endif
               "guard status");
#ifdef CONFIG_FRUITCLAW_ENABLE_EXEC_GUARD
    {
      fc_guard_long_t guard;
      int guard_ret;

      memset(&guard, 0, sizeof(guard));
      guard_ret = fc_guard_long_start(FC_GUARD_STAGE_TEST, 60000,
                                      &guard);
      status[0] = '\0';
      ret |= check(guard_ret == 0 &&
                   fc_guard_status(status, sizeof(status)) == 0 &&
                   strstr(status, "short_active=yes") != NULL &&
                   strstr(status, "recovery_target=") != NULL &&
                   strstr(status, "active_stage=0x46435453") != NULL,
                   "long guard arm status");
      if (guard_ret == 0)
        {
          int nested_fd = -1;

          ret |= check(fc_guard_arm(FC_GUARD_STAGE_MCP,
                                    &nested_fd) == 0 &&
                       nested_fd == -1,
                       "guard allows reentrant overlap");
          ret |= check(fc_guard_long_stop(&guard) == 0,
                       "long guard stop");
        }
      else
        {
          ret |= check(false, "long guard stop");
        }
    }
#endif

  {
    const char *selftest_session = "selftest-memory-tail";
    char selftest_path[FC_PATH_LEN] = "";

    if (fc_data_path("sessions/selftest-memory-tail.jsonl",
                     selftest_path, sizeof(selftest_path)) == 0)
      {
        unlink(selftest_path);
      }

    ret |= check(fc_session_append(selftest_session, "user", NULL,
                  "first selftest memory tail line with extra padding") == 0,
                 "jsonl append setup");
    ret |= check(fc_session_append(selftest_session, "user", NULL,
                  "hello memory") == 0,
                 "jsonl append");
    buf[0] = '\0';
    ret |= check(fc_session_read_tail(selftest_session, 96, buf,
                                      sizeof(buf)) == 0 &&
                 buf[0] == '{' &&
                 strstr(buf, "hello memory") != NULL,
                 "jsonl line-aligned tail");

    if (selftest_path[0] != '\0')
      {
        unlink(selftest_path);
      }
  }

  fc_queue_init(fc_agent_queue());
  memset(&ev, 0, sizeof(ev));
  fc_strlcpy(ev.source, "cli", sizeof(ev.source));
  fc_strlcpy(ev.type, "message.in", sizeof(ev.type));
  ret |= check(fc_router_route_event(&ev) == 0, "router route");
  ret |= check(fc_queue_receive(fc_agent_queue(), &ev, 10) == 0,
               "router queued agent event");
  status[0] = '\0';
  ret |= check(fc_router_status_format(status, sizeof(status)) == 0 &&
               strstr(status, "routed=") != NULL &&
               strstr(status, "last=cli/message.in") != NULL,
               "router status");
  status[0] = '\0';
  ret |= check(fc_agent_status_format(status, sizeof(status)) == 0 &&
               strstr(status, "agent_status:") != NULL &&
               strstr(status, "started=") != NULL,
               "agent status");

  fc_cap_clear_status();
#ifdef CONFIG_FRUITCLAW_MCP_SERVER
  fc_mcp_clear_status();
#endif
  fc_berry_clear_status();

  if (ret == 0)
    {
      printf("FruitClaw selftest passed.\n");
      return 0;
    }

  printf("FruitClaw selftest failed.\n");
  return 1;
}
