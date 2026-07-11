/* SPDX-License-Identifier: Apache-2.0 */

#include "fruitclaw.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <arch/board/board.h>
#include <nuttx/leds/ws2812.h>

#include "netutils/cJSON.h"

#ifndef CONFIG_SYSTEM_NEOPIXELS_DEVPATH
#  define CONFIG_SYSTEM_NEOPIXELS_DEVPATH "/dev/leds0"
#endif

#ifndef CONFIG_SYSTEM_DVICTRL_DEVPATH
#  define CONFIG_SYSTEM_DVICTRL_DEVPATH "/dev/fb0"
#endif

#ifdef CONFIG_WS2812_LED_COUNT
#  define FC_NEOPIXEL_COUNT CONFIG_WS2812_LED_COUNT
#else
#  define FC_NEOPIXEL_COUNT 5
#endif

#ifndef CONFIG_FRUITCLAW_WEB_HOME_MAX_BYTES
#  define CONFIG_FRUITCLAW_WEB_HOME_MAX_BYTES 4096
#endif

#define FC_TOOL_TEXT_MAX        2048
#define FC_DEVICE_MAX_IO        256
#define FC_SCRIPT_DESC_MAX      192
#define FC_SCRIPT_NAME_MAX      64
#define FC_SCRIPT_TEXT_MAX      (CONFIG_FRUITCLAW_MAX_JSON / 2)
#define FC_SCRIPT_ROOT_DIR      "scripts"
#define FC_SCRIPT_ROOT_PRE      "scripts/"
#define FC_SCRIPT_GENERATED_DIR "scripts/generated"
#define FC_SCRIPT_GENERATED_PRE "scripts/generated/"
#define FC_SCRIPT_RUN_PROMPT_PRE "tool:script.run "
#define FC_SCRIPT_BERRY_RUN_PROMPT_PRE "tool:berry.run_script "
#define FC_RTTTL_MAX_TUNE       512

typedef enum fc_script_kind_e
{
  FC_SCRIPT_KIND_AUTO = -1,
  FC_SCRIPT_KIND_BERRY = 0,
  FC_SCRIPT_KIND_SHELL
} fc_script_kind_t;

typedef enum fc_script_validation_mode_e
{
  FC_SCRIPT_VALIDATION_RUN = 0,
  FC_SCRIPT_VALIDATION_SYNTAX
} fc_script_validation_mode_t;

struct fc_color_s
{
  const char *name;
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

static const struct fc_color_s g_colors[] =
{
  { "red",     255,   0,   0 },
  { "green",     0, 255,   0 },
  { "blue",      0,   0, 255 },
  { "white",   255, 255, 255 },
  { "yellow",  255, 220,   0 },
  { "orange",  255,  80,   0 },
  { "purple",  128,   0, 255 },
  { "cyan",      0, 255, 255 },
  { "pink",    255,  32,  96 }
};

static bool fc_color_lookup(const char *name, uint8_t *r, uint8_t *g,
                            uint8_t *b)
{
  size_t i;

  if (name == NULL)
    {
      return false;
    }

  for (i = 0; i < sizeof(g_colors) / sizeof(g_colors[0]); i++)
    {
      if (strcmp(name, g_colors[i].name) == 0)
        {
          *r = g_colors[i].r;
          *g = g_colors[i].g;
          *b = g_colors[i].b;
          return true;
        }
    }

  return false;
}

static bool fc_parse_u8_component(const char **cursor, uint8_t *out)
{
  const char *p;
  char *endptr;
  long value;

  if (cursor == NULL || *cursor == NULL || out == NULL)
    {
      return false;
    }

  p = *cursor;
  while (*p == ' ' || *p == '\t')
    {
      p++;
    }

  if (*p == '\0')
    {
      return false;
    }

  value = strtol(p, &endptr, 10);
  if (endptr == p || value < 0 || value > 255)
    {
      return false;
    }

  while (*endptr == ' ' || *endptr == '\t')
    {
      endptr++;
    }

  *cursor = endptr;
  *out = (uint8_t)value;
  return true;
}

static bool fc_color_parse_rgb(const char *rgb, uint8_t *r, uint8_t *g,
                               uint8_t *b)
{
  const char *p = rgb;

  if (rgb == NULL || r == NULL || g == NULL || b == NULL)
    {
      return false;
    }

  if (!fc_parse_u8_component(&p, r) || *p++ != ',' ||
      !fc_parse_u8_component(&p, g) || *p++ != ',' ||
      !fc_parse_u8_component(&p, b))
    {
      return false;
    }

  while (*p == ' ' || *p == '\t')
    {
      p++;
    }

  return *p == '\0';
}

static int fc_json_out_error(char *out, size_t out_len, const char *error)
{
  snprintf(out, out_len, "{\"ok\":false,\"error\":\"%s\"}",
           error ? error : "failed");
  return -EINVAL;
}

static bool fc_ctx_owner(const fc_tool_context_t *ctx)
{
  return ctx != NULL && ctx->owner_mode;
}

static int fc_tool_guard_arm(const fc_tool_context_t *ctx, uint32_t stage,
                             int *guard_fd)
{
  if (guard_fd == NULL)
    {
      return -EINVAL;
    }

  if (ctx != NULL && ctx->guarded)
    {
      *guard_fd = -1;
      return 0;
    }

  return fc_guard_arm(stage, guard_fd);
}

static int fc_json_get_uint8(cJSON *root, const char *name, uint8_t *out)
{
  cJSON *item = cJSON_GetObjectItem(root, name);

  if (!cJSON_IsNumber(item) || item->valuedouble < 0 ||
      item->valuedouble > 255)
    {
      return -EINVAL;
    }

  *out = (uint8_t)item->valuedouble;
  return 0;
}

static int cap_time_now(const fc_tool_context_t *ctx, const char *args_json,
                        char *out, size_t out_len)
{
  struct timespec ts;
  struct tm tmv;
  char iso[40];

  (void)ctx;
  (void)args_json;
  clock_gettime(CLOCK_REALTIME, &ts);
  gmtime_r(&ts.tv_sec, &tmv);
  strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  snprintf(out, out_len,
           "{\"ok\":true,\"unix_ms\":%lld,\"iso\":\"%s\"}",
           (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000, iso);
  return 0;
}

static int cap_system_info(const fc_tool_context_t *ctx,
                           const char *args_json, char *out, size_t out_len)
{
  (void)ctx;
  (void)args_json;
  snprintf(out, out_len,
           "{\"ok\":true,\"app\":\"FruitClaw\",\"board\":\"%s\","
           "\"data_dir\":\"%s\",\"owner_mode\":%s,"
           "\"features\":{\"telegram\":%s,\"deepseek\":%s,"
           "\"scheduler\":%s,\"berry\":%s,\"http_tool\":%s,"
           "\"terminal\":%s,\"neopixels\":%s,\"device\":%s,"
           "\"telnetd\":%s,\"ftpd\":%s,\"lvgl\":%s,"
           "\"berry_lvgl\":%s,\"usb_keyboard\":%s,"
           "\"usb_mouse\":%s,\"usb_xbox\":%s}}",
#ifdef CONFIG_ARCH_BOARD
           CONFIG_ARCH_BOARD,
#else
           "unknown",
#endif
           fc_data_dir(),
#ifdef CONFIG_FRUITCLAW_OWNER_MODE
           "true",
#else
           "false",
#endif
#ifdef CONFIG_FRUITCLAW_ENABLE_TELEGRAM
           "true",
#else
           "false",
#endif
#ifdef CONFIG_FRUITCLAW_ENABLE_DEEPSEEK
           "true",
#else
           "false",
#endif
#ifdef CONFIG_FRUITCLAW_ENABLE_SCHEDULER
           "true",
#else
           "false",
#endif
#ifdef CONFIG_FRUITCLAW_ENABLE_BERRY
           "true",
#else
           "false",
#endif
#ifdef CONFIG_FRUITCLAW_ENABLE_HTTP_TOOL
           "true",
#else
           "false",
#endif
#ifdef CONFIG_FRUITCLAW_ENABLE_TERMINAL_TOOL
           "true",
#else
           "false",
#endif
#ifdef CONFIG_FRUITCLAW_ENABLE_NEOPIXELS_TOOL
           "true",
#else
           "false",
#endif
#ifdef CONFIG_FRUITCLAW_ENABLE_DEVICE_TOOL
           "true",
#else
           "false",
#endif
#ifdef CONFIG_SYSTEM_TELNETD
           "true",
#else
           "false",
#endif
#if defined(CONFIG_EXAMPLES_FTPD) && defined(CONFIG_NETUTILS_FTPD)
           "true",
#else
           "false",
#endif
#ifdef CONFIG_GRAPHICS_LVGL
           "true",
#else
           "false",
#endif
#ifdef CONFIG_INTERPRETERS_BERRY_LVGL
           "true",
#else
           "false",
#endif
#ifdef CONFIG_USBHOST_HIDKBD
           "true",
#else
           "false",
#endif
#ifdef CONFIG_USBHOST_HIDMOUSE
           "true",
#else
           "false",
#endif
#ifdef CONFIG_USBHOST_XBOXCONTROLLER
           "true"
#else
           "false"
#endif
           );
  return 0;
}

static void fc_status_add(cJSON *root, const char *key,
                          int (*format)(char *out, size_t out_len),
                          size_t scratch_len)
{
  char *scratch;

  if (root == NULL || key == NULL || format == NULL)
    {
      return;
    }

  scratch = calloc(1, scratch_len);
  if (scratch == NULL)
    {
      return;
    }

  if (format(scratch, scratch_len) == 0)
    {
      cJSON_AddStringToObject(root, key, scratch);
    }

  free(scratch);
}

static int cap_system_status(const fc_tool_context_t *ctx,
                             const char *args_json, char *out,
                             size_t out_len)
{
  cJSON *root;
  cJSON *queues;
  char *printed;
  int ret = 0;

  (void)ctx;
  (void)args_json;

  root = cJSON_CreateObject();
  queues = cJSON_CreateObject();
  if (root == NULL || queues == NULL)
    {
      cJSON_Delete(root);
      cJSON_Delete(queues);
      return -ENOMEM;
    }

  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddStringToObject(root, "data_dir", fc_data_dir());
  cJSON_AddNumberToObject(queues, "main", fc_queue_count(fc_main_queue()));
  cJSON_AddNumberToObject(queues, "agent", fc_queue_count(fc_agent_queue()));
  cJSON_AddItemToObject(root, "queues", queues);
  cJSON_AddNumberToObject(root, "visible_tools", fc_cap_count(true));

  fc_status_add(root, "guard", fc_guard_status, 768);
  fc_status_add(root, "router", fc_router_status_format, 256);
  fc_status_add(root, "agent", fc_agent_status_format, 512);
  fc_status_add(root, "network_recovery",
                fc_network_recovery_status_format, 256);
  fc_status_add(root, "webserver", fc_webserver_status_format, 256);
  fc_status_add(root, "services", fc_services_status_format, 384);
  fc_status_add(root, "telegram", fc_telegram_status_format, 512);
  fc_status_add(root, "deepseek", fc_deepseek_status_format, 256);
  fc_status_add(root, "scheduler", fc_scheduler_status_format, 384);
  fc_status_add(root, "berry", fc_berry_status_format, 256);
#ifdef CONFIG_FRUITCLAW_MCP_SERVER
  fc_status_add(root, "mcp", fc_mcp_status_format, 512);
#endif

  printed = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
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

static int cap_service_status(const fc_tool_context_t *ctx,
                              const char *args_json, char *out,
                              size_t out_len)
{
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  const char *service = NULL;
  int ret;

  (void)ctx;
  if (root != NULL)
    {
      service = cJSON_GetStringValue(cJSON_GetObjectItem(root, "service"));
    }

  ret = fc_service_control(service != NULL ? service : "all",
                           "status", out, out_len);
  cJSON_Delete(root);
  return ret;
}

static int cap_service_control(const fc_tool_context_t *ctx,
                               const char *args_json, char *out,
                               size_t out_len)
{
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  const char *service;
  const char *action;
  int ret;

  (void)ctx;
  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  service = cJSON_GetStringValue(cJSON_GetObjectItem(root, "service"));
  action = cJSON_GetStringValue(cJSON_GetObjectItem(root, "action"));
  if (service == NULL || action == NULL)
    {
      cJSON_Delete(root);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"missing service or action\"}");
      return -EINVAL;
    }

  ret = fc_service_control(service, action, out, out_len);
  cJSON_Delete(root);
  return ret;
}

static bool fc_memory_entry_is_hidden(cJSON *entry, bool include_selftest)
{
  const char *source;

  if (include_selftest || entry == NULL)
    {
      return false;
    }

  source = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "source"));
  return source != NULL && strcmp(source, "selftest") == 0;
}

static void fc_memory_append_text_line(char *out, size_t out_len,
                                       const char *line)
{
  size_t used;
  size_t line_len;

  if (out == NULL || out_len == 0 || line == NULL)
    {
      return;
    }

  used = strlen(out);
  if (used + 1 >= out_len)
    {
      return;
    }

  line_len = strlen(line);
  if (line_len > out_len - used - 2)
    {
      line_len = out_len - used - 2;
    }

  memcpy(out + used, line, line_len);
  used += line_len;
  out[used++] = '\n';
  out[used] = '\0';
}

static int fc_memory_add_jsonl_entries(cJSON *entries, char *jsonl,
                                       bool include_selftest,
                                       char *text, size_t text_len,
                                       int *count)
{
  char *line;

  if (entries == NULL || jsonl == NULL || text == NULL || text_len == 0 ||
      count == NULL)
    {
      return -EINVAL;
    }

  *count = 0;
  text[0] = '\0';
  line = jsonl;
  while (line[0] != '\0')
    {
      char *next = strchr(line, '\n');
      cJSON *entry;

      if (next != NULL)
        {
          *next = '\0';
        }

      if (line[0] != '\0')
        {
          entry = cJSON_Parse(line);
          if (entry != NULL && cJSON_IsObject(entry) &&
              !fc_memory_entry_is_hidden(entry, include_selftest))
            {
              fc_memory_append_text_line(text, text_len, line);
              cJSON_AddItemToArray(entries, entry);
              (*count)++;
            }
          else
            {
              cJSON_Delete(entry);
            }
        }

      if (next == NULL)
        {
          break;
        }

      *next = '\n';
      line = next + 1;
    }

  return 0;
}

static int cap_memory_append(const fc_tool_context_t *ctx,
                             const char *args_json, char *out,
                             size_t out_len)
{
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  const char *text;
  int ret = 0;

  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
  if (text == NULL)
    {
      cJSON_Delete(root);
      return fc_json_out_error(out, out_len, "missing text");
    }

  ret = fc_memory_append(ctx && ctx->source[0] ? ctx->source : "tool", text);
  cJSON_Delete(root);
  snprintf(out, out_len, ret == 0 ?
           "{\"ok\":true}" :
           "{\"ok\":false,\"error\":\"append failed\"}");
  return ret;
}

static int cap_memory_read(const fc_tool_context_t *ctx,
                           const char *args_json, char *out, size_t out_len)
{
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  cJSON *max_item;
  cJSON *include_item;
  size_t max_bytes = 2048;
  bool include_selftest = false;
  char *buf;
  char *text;
  cJSON *reply;
  cJSON *entries;
  char *printed;
  int count = 0;
  int ret;

  (void)ctx;
  if (root != NULL)
    {
      max_item = cJSON_GetObjectItem(root, "max_bytes");
      if (cJSON_IsNumber(max_item) && max_item->valuedouble > 0)
        {
          max_bytes = (size_t)max_item->valuedouble;
        }
      include_item = cJSON_GetObjectItem(root, "include_selftest");
      include_selftest = cJSON_IsTrue(include_item);
      cJSON_Delete(root);
    }

  if (max_bytes > CONFIG_FRUITCLAW_MAX_JSON / 2)
    {
      max_bytes = CONFIG_FRUITCLAW_MAX_JSON / 2;
    }

  buf = calloc(1, max_bytes + 1);
  text = calloc(1, max_bytes + 1);
  if (buf == NULL || text == NULL)
    {
      free(buf);
      free(text);
      return -ENOMEM;
    }

  ret = fc_memory_read_tail(max_bytes, buf, max_bytes + 1);
  if (ret == 0)
    {
      reply = cJSON_CreateObject();
      entries = cJSON_CreateArray();
      if (reply == NULL || entries == NULL)
        {
          cJSON_Delete(reply);
          cJSON_Delete(entries);
          free(buf);
          free(text);
          return -ENOMEM;
        }

      fc_memory_add_jsonl_entries(entries, buf, include_selftest, text,
                                  max_bytes + 1, &count);
      cJSON_AddBoolToObject(reply, "ok", true);
      cJSON_AddNumberToObject(reply, "count", count);
      cJSON_AddItemToObject(reply, "entries", entries);
      cJSON_AddStringToObject(reply, "text", text);
      printed = cJSON_PrintUnformatted(reply);
      if (printed != NULL && strlen(printed) >= out_len)
        {
          cJSON_free(printed);
          cJSON_DeleteItemFromObject(reply, "text");
          cJSON_AddBoolToObject(reply, "text_omitted", true);
          printed = cJSON_PrintUnformatted(reply);
        }

      if (printed == NULL)
        {
          ret = -ENOMEM;
        }
      else if (strlen(printed) >= out_len)
        {
          snprintf(out, out_len,
                   "{\"ok\":false,\"error\":\"memory result too large\"}");
          ret = -ENOSPC;
        }
      else
        {
          fc_strlcpy(out, printed, out_len);
        }

      cJSON_free(printed);
      cJSON_Delete(reply);
    }
  else
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"read failed\"}");
    }

  free(buf);
  free(text);
  return ret;
}

static int fc_resolve_safe_data_path(const char *input, char *path,
                                     size_t path_len, bool for_write)
{
  char tmp[FC_PATH_LEN];
  const char *leaf = input;
  const char *root = fc_data_dir();
  size_t root_len = strlen(root);

  if (input == NULL || input[0] == '\0' || fc_path_has_parent_ref(input))
    {
      return -EINVAL;
    }

  if (input[0] == '/')
    {
      if (strncmp(input, root, root_len) != 0 ||
          (input[root_len] != '\0' && input[root_len] != '/') ||
          fc_path_is_secret(input))
        {
          return -EACCES;
        }

      return fc_strlcpy(path, input, path_len);
    }

  if (for_write)
    {
      if (strncmp(input, "scripts/", 8) != 0 &&
          strncmp(input, "notes/", 6) != 0)
        {
          return -EACCES;
        }
    }

  if (fc_data_path(leaf, tmp, sizeof(tmp)) < 0 || fc_path_is_secret(tmp))
    {
      return -EACCES;
    }

  return fc_strlcpy(path, tmp, path_len);
}

static int cap_file_read(const fc_tool_context_t *ctx, const char *args_json,
                         char *out, size_t out_len)
{
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  const char *arg_path;
  cJSON *max_item;
  char path[FC_PATH_LEN];
  size_t max_bytes = 2048;
  char *buf;
  char *esc;
  int ret;

  (void)ctx;
  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  arg_path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
  max_item = cJSON_GetObjectItem(root, "max_bytes");
  if (cJSON_IsNumber(max_item) && max_item->valuedouble > 0)
    {
      max_bytes = (size_t)max_item->valuedouble;
    }

  ret = fc_resolve_safe_data_path(arg_path, path, sizeof(path), false);
  cJSON_Delete(root);
  if (ret < 0)
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"path denied\"}");
      return ret;
    }

  if (max_bytes > CONFIG_FRUITCLAW_MAX_JSON / 2)
    {
      max_bytes = CONFIG_FRUITCLAW_MAX_JSON / 2;
    }

  buf = calloc(1, max_bytes + 1);
  esc = calloc(1, max_bytes * 6 + 1);
  if (buf == NULL || esc == NULL)
    {
      free(buf);
      free(esc);
      return -ENOMEM;
    }

  ret = fc_read_text_file(path, buf, max_bytes + 1, false);
  if (ret == 0)
    {
      fc_json_escape(buf, esc, max_bytes * 6 + 1);
      snprintf(out, out_len, "{\"ok\":true,\"text\":\"%s\"}", esc);
    }
  else
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"read failed\"}");
    }

  free(buf);
  free(esc);
  return ret;
}

static int cap_file_write_limited(const fc_tool_context_t *ctx,
                                  const char *args_json, char *out,
                                  size_t out_len)
{
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  const char *arg_path;
  const char *text;
  char path[FC_PATH_LEN];
  int guard_fd = -1;
  int ret;

  (void)ctx;
  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  arg_path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
  text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
  if (text == NULL)
    {
      cJSON_Delete(root);
      return fc_json_out_error(out, out_len, "missing text");
    }

  ret = fc_resolve_safe_data_path(arg_path, path, sizeof(path), true);
  if (ret < 0)
    {
      cJSON_Delete(root);
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"path denied\"}");
      return ret;
    }

  ret = fc_tool_guard_arm(ctx, FC_GUARD_STAGE_FILE, &guard_fd);
  if (ret < 0)
    {
      cJSON_Delete(root);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"file guard unavailable\","
               "\"code\":%d}", ret);
      return ret;
    }

  ret = fc_write_text_file_atomic(path, text);
  fc_guard_disarm(guard_fd);
  cJSON_Delete(root);
  snprintf(out, out_len, ret == 0 ?
           "{\"ok\":true}" :
           "{\"ok\":false,\"error\":\"write failed\",\"code\":%d}", ret);
  return ret;
}

static int cap_web_home_read(const fc_tool_context_t *ctx,
                             const char *args_json, char *out,
                             size_t out_len)
{
  char *buf;
  char *esc;
  bool custom = false;
  int ret;

  (void)ctx;
  (void)args_json;

  buf = calloc(1, CONFIG_FRUITCLAW_WEB_HOME_MAX_BYTES + 1);
  esc = calloc(1, CONFIG_FRUITCLAW_WEB_HOME_MAX_BYTES * 6 + 1);
  if (buf == NULL || esc == NULL)
    {
      free(buf);
      free(esc);
      return -ENOMEM;
    }

  ret = fc_web_home_read(buf, CONFIG_FRUITCLAW_WEB_HOME_MAX_BYTES + 1,
                         &custom);
  if (ret == 0)
    {
      fc_json_escape(buf, esc, CONFIG_FRUITCLAW_WEB_HOME_MAX_BYTES * 6 + 1);
      snprintf(out, out_len,
               "{\"ok\":true,\"custom\":%s,\"path\":\"www/home.md\","
               "\"url\":\"/\",\"markdown\":\"%s\"}",
               custom ? "true" : "false", esc);
    }
  else
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"home read failed\"}");
    }

  free(buf);
  free(esc);
  return ret;
}

static int cap_web_home_write(const fc_tool_context_t *ctx,
                              const char *args_json, char *out,
                              size_t out_len)
{
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  const char *markdown;
  int guard_fd = -1;
  int ret;

  if (!fc_ctx_owner(ctx))
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"owner required\"}");
      return -EACCES;
    }

  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  markdown = cJSON_GetStringValue(cJSON_GetObjectItem(root, "markdown"));
  if (markdown == NULL)
    {
      markdown = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
    }

  if (markdown == NULL)
    {
      cJSON_Delete(root);
      return fc_json_out_error(out, out_len, "missing markdown");
    }

  ret = fc_tool_guard_arm(ctx, FC_GUARD_STAGE_FILE, &guard_fd);
  if (ret < 0)
    {
      cJSON_Delete(root);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"web home guard unavailable\","
               "\"code\":%d}", ret);
      return ret;
    }

  ret = fc_web_home_write(markdown);
  fc_guard_disarm(guard_fd);
  cJSON_Delete(root);

  if (ret == 0)
    {
      snprintf(out, out_len,
               "{\"ok\":true,\"path\":\"www/home.md\",\"url\":\"/\","
               "\"bytes\":%u}", (unsigned int)strlen(markdown));
    }
  else if (ret == -EFBIG)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"home markdown too large\","
               "\"max_bytes\":%u}",
               (unsigned int)CONFIG_FRUITCLAW_WEB_HOME_MAX_BYTES);
    }
  else
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"home write failed\","
               "\"code\":%d}", ret);
    }

  return ret;
}

static bool fc_script_leaf_ok(const char *leaf)
{
  const char *p;
  size_t len;

  if (leaf == NULL || leaf[0] == '\0' || leaf[0] == '.' ||
      strchr(leaf, '/') != NULL || strchr(leaf, '\\') != NULL ||
      strstr(leaf, "..") != NULL)
    {
      return false;
    }

  len = strlen(leaf);
  if (len == 0 || len > FC_SCRIPT_NAME_MAX)
    {
      return false;
    }

  for (p = leaf; *p != '\0'; p++)
    {
      if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' &&
          *p != '.')
        {
          return false;
        }
    }

  return true;
}

static const char *fc_script_kind_name(fc_script_kind_t kind)
{
  return kind == FC_SCRIPT_KIND_SHELL ? "shell" : "berry";
}

static const char *fc_script_kind_ext(fc_script_kind_t kind)
{
  return kind == FC_SCRIPT_KIND_SHELL ? ".nsh" : ".be";
}

static const char *fc_script_validation_mode_name(
                                    fc_script_validation_mode_t mode)
{
  return mode == FC_SCRIPT_VALIDATION_SYNTAX ? "syntax" : "run";
}

static int fc_script_kind_from_arg(const char *kind,
                                   fc_script_kind_t *out)
{
  if (out == NULL)
    {
      return -EINVAL;
    }

  if (kind == NULL || kind[0] == '\0' || strcmp(kind, "auto") == 0)
    {
      *out = FC_SCRIPT_KIND_AUTO;
      return 0;
    }

  if (strcmp(kind, "berry") == 0 || strcmp(kind, "be") == 0)
    {
      *out = FC_SCRIPT_KIND_BERRY;
      return 0;
    }

  if (strcmp(kind, "shell") == 0 || strcmp(kind, "nsh") == 0 ||
      strcmp(kind, "sh") == 0)
    {
      *out = FC_SCRIPT_KIND_SHELL;
      return 0;
    }

  return -EINVAL;
}

static int fc_script_validation_mode_from_arg(const char *mode,
                                      fc_script_validation_mode_t *out)
{
  if (out == NULL)
    {
      return -EINVAL;
    }

  if (mode == NULL || mode[0] == '\0' || strcmp(mode, "run") == 0)
    {
      *out = FC_SCRIPT_VALIDATION_RUN;
      return 0;
    }

  if (strcmp(mode, "syntax") == 0)
    {
      *out = FC_SCRIPT_VALIDATION_SYNTAX;
      return 0;
    }

  return -EINVAL;
}

static int fc_script_kind_from_leaf(const char *leaf,
                                    fc_script_kind_t *out)
{
  size_t len;

  if (leaf == NULL || out == NULL)
    {
      return -EINVAL;
    }

  len = strlen(leaf);
  if (len >= 3 && strcmp(leaf + len - 3, ".be") == 0)
    {
      *out = FC_SCRIPT_KIND_BERRY;
      return 0;
    }

  if (len >= 4 && strcmp(leaf + len - 4, ".nsh") == 0)
    {
      *out = FC_SCRIPT_KIND_SHELL;
      return 0;
    }

  return -EINVAL;
}

static int fc_script_make_paths_kind(const char *name_or_path,
                                     fc_script_kind_t requested_kind,
                                     char *leaf, size_t leaf_len,
                                     char *rel, size_t rel_len,
                                     char *full, size_t full_len,
                                     fc_script_kind_t *kind_out)
{
  char name[FC_SCRIPT_NAME_MAX + 4];
  const char *src = name_or_path;
  fc_script_kind_t found_kind = FC_SCRIPT_KIND_AUTO;
  int ret;

  if (name_or_path == NULL || leaf == NULL || rel == NULL || full == NULL)
    {
      return -EINVAL;
    }

  if (strncmp(src, FC_SCRIPT_GENERATED_PRE,
              strlen(FC_SCRIPT_GENERATED_PRE)) == 0)
    {
      src += strlen(FC_SCRIPT_GENERATED_PRE);
    }
  else if (strncmp(src, "generated/", 10) == 0)
    {
      src += 10;
    }
  else if (strncmp(src, "scripts/", 8) == 0 || src[0] == '/')
    {
      return -EACCES;
    }

  if (!fc_script_leaf_ok(src))
    {
      return -EINVAL;
    }

  if (fc_script_kind_from_leaf(src, &found_kind) == 0)
    {
      if (requested_kind != FC_SCRIPT_KIND_AUTO &&
          requested_kind != found_kind)
        {
          return -EINVAL;
        }

      ret = fc_strlcpy(name, src, sizeof(name));
    }
  else if (strchr(src, '.') == NULL)
    {
      if (requested_kind == FC_SCRIPT_KIND_AUTO)
        {
          requested_kind = FC_SCRIPT_KIND_BERRY;
        }

      found_kind = requested_kind;
      ret = snprintf(name, sizeof(name), "%s%s", src,
                     fc_script_kind_ext(found_kind)) >=
            (int)sizeof(name) ? -ENOSPC : 0;
    }
  else
    {
      ret = -EINVAL;
    }

  if (ret < 0 || !fc_script_leaf_ok(name))
    {
      return ret < 0 ? ret : -EINVAL;
    }

  ret = fc_data_path(FC_SCRIPT_GENERATED_DIR, full, full_len);
  if (ret < 0)
    {
      return ret;
    }

  ret = fc_mkdir_p(full);
  if (ret < 0)
    {
      return ret;
    }

  if (snprintf(rel, rel_len, "%s%s", FC_SCRIPT_GENERATED_PRE, name) >=
      (int)rel_len)
    {
      return -ENAMETOOLONG;
    }

  ret = fc_data_path(rel, full, full_len);
  if (ret < 0)
    {
      return ret;
    }

  if (kind_out != NULL)
    {
      *kind_out = found_kind;
    }

  return fc_strlcpy(leaf, name, leaf_len);
}

static int fc_script_make_uploaded_paths(const char *name_or_path,
                                     fc_script_kind_t requested_kind,
                                     char *leaf, size_t leaf_len,
                                     char *rel, size_t rel_len,
                                     char *full, size_t full_len,
                                     fc_script_kind_t *kind_out)
{
  char name[FC_SCRIPT_NAME_MAX + 4];
  const char *src = name_or_path;
  fc_script_kind_t found_kind = FC_SCRIPT_KIND_AUTO;
  int ret;

  if (name_or_path == NULL || leaf == NULL || rel == NULL || full == NULL)
    {
      return -EINVAL;
    }

  if (strncmp(src, FC_SCRIPT_ROOT_PRE, strlen(FC_SCRIPT_ROOT_PRE)) == 0)
    {
      src += strlen(FC_SCRIPT_ROOT_PRE);
    }

  if (strncmp(src, "generated/", 10) == 0 || strchr(src, '/') != NULL ||
      src[0] == '/')
    {
      return -EACCES;
    }

  if (!fc_script_leaf_ok(src))
    {
      return -EINVAL;
    }

  if (fc_script_kind_from_leaf(src, &found_kind) == 0)
    {
      if (requested_kind != FC_SCRIPT_KIND_AUTO &&
          requested_kind != found_kind)
        {
          return -EINVAL;
        }

      ret = fc_strlcpy(name, src, sizeof(name));
    }
  else if (strchr(src, '.') == NULL)
    {
      if (requested_kind == FC_SCRIPT_KIND_AUTO)
        {
          requested_kind = FC_SCRIPT_KIND_BERRY;
        }

      found_kind = requested_kind;
      ret = snprintf(name, sizeof(name), "%s%s", src,
                     fc_script_kind_ext(found_kind)) >=
            (int)sizeof(name) ? -ENOSPC : 0;
    }
  else
    {
      ret = -EINVAL;
    }

  if (ret < 0 || !fc_script_leaf_ok(name))
    {
      return ret < 0 ? ret : -EINVAL;
    }

  if (snprintf(rel, rel_len, "%s%s", FC_SCRIPT_ROOT_PRE, name) >=
      (int)rel_len)
    {
      return -ENAMETOOLONG;
    }

  ret = fc_data_path(rel, full, full_len);
  if (ret < 0)
    {
      return ret;
    }

  if (kind_out != NULL)
    {
      *kind_out = found_kind;
    }

  return fc_strlcpy(leaf, name, leaf_len);
}

static int fc_script_make_paths_access(const char *name_or_path,
                                     fc_script_kind_t requested_kind,
                                     bool allow_uploaded,
                                     char *leaf, size_t leaf_len,
                                     char *rel, size_t rel_len,
                                     char *full, size_t full_len,
                                     fc_script_kind_t *kind_out)
{
  if (name_or_path != NULL &&
      strncmp(name_or_path, FC_SCRIPT_ROOT_PRE,
              strlen(FC_SCRIPT_ROOT_PRE)) == 0 &&
      strncmp(name_or_path, FC_SCRIPT_GENERATED_PRE,
              strlen(FC_SCRIPT_GENERATED_PRE)) != 0)
    {
      if (!allow_uploaded)
        {
          return -EACCES;
        }

      return fc_script_make_uploaded_paths(name_or_path, requested_kind,
                                           leaf, leaf_len, rel, rel_len,
                                           full, full_len, kind_out);
    }

  return fc_script_make_paths_kind(name_or_path, requested_kind, leaf,
                                   leaf_len, rel, rel_len, full,
                                   full_len, kind_out);
}

static int fc_script_build_list_paths(const char *prefix, const char *leaf,
                                      char *rel, size_t rel_len,
                                      char *full, size_t full_len)
{
  int ret;

  if (prefix == NULL || leaf == NULL || rel == NULL || full == NULL ||
      !fc_script_leaf_ok(leaf))
    {
      return -EINVAL;
    }

  if (snprintf(rel, rel_len, "%s%s", prefix, leaf) >= (int)rel_len)
    {
      return -ENAMETOOLONG;
    }

  ret = fc_data_path(rel, full, full_len);
  if (ret < 0)
    {
      return ret;
    }

  return 0;
}

static uint32_t fc_script_meta_hash(const char *name)
{
  uint32_t hash = 2166136261u;

  if (name == NULL)
    {
      return hash;
    }

  while (*name != '\0')
    {
      hash ^= (unsigned char)*name++;
      hash *= 16777619u;
    }

  return hash;
}

static int fc_script_meta_path(const char *full, char *meta, size_t meta_len)
{
  const char *slash;
  const char *name;
  size_t dir_len;
  uint32_t hash;

  if (full == NULL || meta == NULL || meta_len == 0)
    {
      return -EINVAL;
    }

  slash = strrchr(full, '/');
  name = slash != NULL ? slash + 1 : full;
  dir_len = slash != NULL ? (size_t)(slash - full) : 0;
  hash = fc_script_meta_hash(name);

  if (slash != NULL)
    {
      if (snprintf(meta, meta_len, "%.*s/.m%08lx.json", (int)dir_len,
                   full, (unsigned long)hash) >= (int)meta_len)
        {
          return -ENAMETOOLONG;
        }
    }
  else if (snprintf(meta, meta_len, ".m%08lx.json", (unsigned long)hash) >=
           (int)meta_len)
    {
      return -ENAMETOOLONG;
    }

  return 0;
}

static cJSON *fc_script_load_meta(const char *full)
{
  char meta_path[FC_PATH_LEN];
  char text[1024];
  cJSON *root;

  if (fc_script_meta_path(full, meta_path, sizeof(meta_path)) < 0 ||
      fc_read_text_file(meta_path, text, sizeof(text), false) < 0)
    {
      return NULL;
    }

  root = cJSON_Parse(text);
  if (!cJSON_IsObject(root))
    {
      cJSON_Delete(root);
      return NULL;
    }

  return root;
}

static void fc_script_add_meta_fields(cJSON *item, const char *full)
{
  cJSON *meta;
  cJSON *value;
  const char *text;

  if (item == NULL || full == NULL)
    {
      return;
    }

  meta = fc_script_load_meta(full);
  if (meta == NULL)
    {
      return;
    }

  value = cJSON_GetObjectItem(meta, "validated");
  if (cJSON_IsBool(value))
    {
      cJSON_AddBoolToObject(item, "validated", cJSON_IsTrue(value));
    }

  text = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "validation_mode"));
  if (text != NULL)
    {
      cJSON_AddStringToObject(item, "validation_mode", text);
    }

  value = cJSON_GetObjectItem(meta, "last_validation_ret");
  if (cJSON_IsNumber(value))
    {
      cJSON_AddNumberToObject(item, "last_validation_ret",
                              value->valuedouble);
    }

  value = cJSON_GetObjectItem(meta, "last_validation_ms");
  if (cJSON_IsNumber(value))
    {
      cJSON_AddNumberToObject(item, "last_validation_ms",
                              value->valuedouble);
    }

  value = cJSON_GetObjectItem(meta, "updated_ms");
  if (cJSON_IsNumber(value))
    {
      cJSON_AddNumberToObject(item, "metadata_updated_ms",
                              value->valuedouble);
    }

  text = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "last_action"));
  if (text != NULL)
    {
      cJSON_AddStringToObject(item, "last_action", text);
    }

  cJSON_Delete(meta);
}

static int fc_script_write_meta(const char *full, const char *leaf,
                                const char *rel, fc_script_kind_t kind,
                                const char *desc, bool validated,
                                const char *validation_mode,
                                int validation_ret,
                                const char *action)
{
  char meta_path[FC_PATH_LEN];
  cJSON *root;
  char *printed;
  const char *source;
  int ret;

  ret = fc_script_meta_path(full, meta_path, sizeof(meta_path));
  if (ret < 0)
    {
      return ret;
    }

  root = cJSON_CreateObject();
  if (root == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddStringToObject(root, "name", leaf ? leaf : "");
  cJSON_AddStringToObject(root, "path", rel ? rel : "");
  cJSON_AddStringToObject(root, "kind", fc_script_kind_name(kind));
  source = rel != NULL &&
           strncmp(rel, FC_SCRIPT_GENERATED_PRE,
                   strlen(FC_SCRIPT_GENERATED_PRE)) == 0 ?
           "generated" : "uploaded";
  cJSON_AddStringToObject(root, "source", source);
  if (desc != NULL && desc[0] != '\0')
    {
      cJSON_AddStringToObject(root, "description", desc);
    }

  cJSON_AddBoolToObject(root, "validated", validated);
  if (validation_mode != NULL)
    {
      cJSON_AddStringToObject(root, "validation_mode", validation_mode);
      cJSON_AddNumberToObject(root, "last_validation_ret", validation_ret);
      cJSON_AddNumberToObject(root, "last_validation_ms",
                              (double)fc_time_ms());
    }

  cJSON_AddStringToObject(root, "last_action", action ? action : "write");
  cJSON_AddNumberToObject(root, "updated_ms", (double)fc_time_ms());

  printed = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (printed == NULL)
    {
      return -ENOMEM;
    }

  ret = fc_write_text_file_atomic(meta_path, printed);
  cJSON_free(printed);
  return ret;
}

static void fc_script_description_from_text(char *text, char *desc,
                                            size_t desc_len)
{
  char *line;

  if (desc == NULL || desc_len == 0)
    {
      return;
    }

  desc[0] = '\0';
  if (text == NULL)
    {
      return;
    }

  for (line = text; line != NULL && line[0] != '\0'; )
    {
      char *next = strchr(line, '\n');
      char *p = line;
      char *value = NULL;

      if (next != NULL)
        {
          *next = '\0';
        }

      while (isspace((unsigned char)*p))
        {
          p++;
        }

      if (*p == '#')
        {
          p++;
          while (isspace((unsigned char)*p))
            {
              p++;
            }

          if (strncasecmp(p, "fruitclaw-description:", 22) == 0)
            {
              value = p + 22;
            }
          else if (strncasecmp(p, "description:", 12) == 0)
            {
              value = p + 12;
            }
          else if (strncasecmp(p, "desc:", 5) == 0)
            {
              value = p + 5;
            }
          else if (desc[0] == '\0' && p[0] != '\0')
            {
              value = p;
            }
        }

      if (value != NULL)
        {
          while (isspace((unsigned char)*value))
            {
              value++;
            }

          fc_strlcpy(desc, value, desc_len);
          fc_trim(desc);
          if (desc[0] != '\0')
            {
              if (next != NULL)
                {
                  *next = '\n';
                }

              return;
            }
        }

      if (next == NULL)
        {
          return;
        }

      *next = '\n';
      line = next + 1;
    }
}

static void fc_script_read_description(const char *path, char *desc,
                                       size_t desc_len)
{
  char text[512];

  if (desc == NULL || desc_len == 0)
    {
      return;
    }

  desc[0] = '\0';
  if (fc_read_text_file(path, text, sizeof(text), false) == 0)
    {
      fc_script_description_from_text(text, desc, desc_len);
    }
}

static cJSON *fc_script_load_schedules(void)
{
  char path[FC_PATH_LEN];
  char *text;
  cJSON *root;
  int ret;

  ret = fc_data_path("schedules.json", path, sizeof(path));
  if (ret < 0)
    {
      return NULL;
    }

  text = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  if (text == NULL)
    {
      return NULL;
    }

  ret = fc_read_text_file(path, text, CONFIG_FRUITCLAW_MAX_JSON, false);
  if (ret < 0)
    {
      free(text);
      return NULL;
    }

  root = cJSON_Parse(text);
  free(text);
  if (!cJSON_IsArray(root))
    {
      cJSON_Delete(root);
      return NULL;
    }

  return root;
}

static bool fc_script_schedule_matches(const cJSON *job, const char *rel,
                                       const char *leaf)
{
  const char *prompt;
  const char *args_text;
  const char *path;
  const char *name;
  cJSON *args;
  bool match = false;

  prompt = cJSON_GetStringValue(cJSON_GetObjectItem(job, "prompt"));
  if (prompt == NULL)
    {
      return false;
    }

  while (isspace((unsigned char)*prompt))
    {
      prompt++;
    }

  if (strncmp(prompt, FC_SCRIPT_RUN_PROMPT_PRE,
              strlen(FC_SCRIPT_RUN_PROMPT_PRE)) == 0)
    {
      args_text = prompt + strlen(FC_SCRIPT_RUN_PROMPT_PRE);
    }
  else if (strncmp(prompt, FC_SCRIPT_BERRY_RUN_PROMPT_PRE,
                   strlen(FC_SCRIPT_BERRY_RUN_PROMPT_PRE)) == 0)
    {
      args_text = prompt + strlen(FC_SCRIPT_BERRY_RUN_PROMPT_PRE);
    }
  else
    {
      return false;
    }

  args = cJSON_Parse(args_text);
  if (args == NULL)
    {
      return false;
    }

  path = cJSON_GetStringValue(cJSON_GetObjectItem(args, "path"));
  name = cJSON_GetStringValue(cJSON_GetObjectItem(args, "name"));
  if (path != NULL &&
      (strcmp(path, rel) == 0 || strcmp(path, leaf) == 0))
    {
      match = true;
    }
  else if (name != NULL &&
           (strcmp(name, rel) == 0 || strcmp(name, leaf) == 0))
    {
      match = true;
    }

  cJSON_Delete(args);
  return match;
}

static void fc_script_add_schedule_refs(cJSON *item, const cJSON *schedules,
                                        const char *rel, const char *leaf)
{
  const cJSON *job;
  cJSON *refs;
  int count = 0;

  if (item == NULL || !cJSON_IsArray(schedules) || rel == NULL ||
      leaf == NULL)
    {
      cJSON_AddBoolToObject(item, "scheduled", false);
      return;
    }

  refs = cJSON_CreateArray();
  if (refs == NULL)
    {
      cJSON_AddBoolToObject(item, "scheduled", false);
      return;
    }

  cJSON_ArrayForEach(job, schedules)
    {
      const char *id;
      const char *type;
      const char *expr;
      cJSON *ref;
      cJSON *enabled;
      cJSON *every;
      cJSON *at;
      cJSON *owner;
      cJSON *created;

      if (!fc_script_schedule_matches(job, rel, leaf))
        {
          continue;
        }

      id = cJSON_GetStringValue(cJSON_GetObjectItem(job, "id"));
      type = cJSON_GetStringValue(cJSON_GetObjectItem(job, "type"));
      ref = cJSON_CreateObject();
      if (ref == NULL)
        {
          continue;
        }

      if (id != NULL)
        {
          cJSON_AddStringToObject(ref, "id", id);
        }

      if (type != NULL)
        {
          cJSON_AddStringToObject(ref, "type", type);
        }

      enabled = cJSON_GetObjectItem(job, "enabled");
      cJSON_AddBoolToObject(ref, "enabled",
                            enabled == NULL ? true : cJSON_IsTrue(enabled));
      every = cJSON_GetObjectItem(job, "every_sec");
      if (cJSON_IsNumber(every))
        {
          cJSON_AddNumberToObject(ref, "every_sec", every->valuedouble);
        }

      at = cJSON_GetObjectItem(job, "at_epoch");
      if (cJSON_IsNumber(at))
        {
          cJSON_AddNumberToObject(ref, "at_epoch", at->valuedouble);
        }

      expr = cJSON_GetStringValue(cJSON_GetObjectItem(job, "expr"));
      if (expr != NULL)
        {
          cJSON_AddStringToObject(ref, "expr", expr);
        }

      owner = cJSON_GetObjectItem(job, "owner_mode");
      if (cJSON_IsBool(owner))
        {
          cJSON_AddBoolToObject(ref, "owner_mode", cJSON_IsTrue(owner));
        }

      created = cJSON_GetObjectItem(job, "created_ms");
      if (cJSON_IsNumber(created))
        {
          cJSON_AddNumberToObject(ref, "created_ms", created->valuedouble);
        }

      cJSON_AddItemToArray(refs, ref);
      count++;
    }

  cJSON_AddBoolToObject(item, "scheduled", count > 0);
  if (count > 0)
    {
      cJSON_AddItemToObject(item, "schedules", refs);
    }
  else
    {
      cJSON_Delete(refs);
    }
}

static int fc_script_validate_args_json(const char *args_json,
                                        char *out, size_t out_len)
{
  cJSON *parsed;

  if (args_json == NULL || args_json[0] == '\0')
    {
      return 0;
    }

  if (strlen(args_json) > 512)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"script args too large\"}");
      return -ENOSPC;
    }

  parsed = cJSON_Parse(args_json);
  if (parsed == NULL)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"script args invalid JSON\"}");
      return -EINVAL;
    }

  cJSON_Delete(parsed);
  return 0;
}

static int fc_script_run_generated(const fc_tool_context_t *ctx,
                                   fc_script_kind_t kind,
                                   const char *rel, const char *full,
                                   const char *args_json,
                                   char *out, size_t out_len)
{
  if (kind == FC_SCRIPT_KIND_BERRY)
    {
      return fc_berry_run_file(ctx, rel, args_json ? args_json : "{}",
                               out, out_len);
    }

  if (kind == FC_SCRIPT_KIND_SHELL)
    {
      char command[FC_TERMINAL_MAX_COMMAND + 1];
      char command_esc[(FC_TERMINAL_MAX_COMMAND * 6) + 1];
      char terminal_args[(FC_TERMINAL_MAX_COMMAND * 6) + 32];

      (void)rel;
      (void)args_json;

      if (full == NULL ||
          snprintf(command, sizeof(command), "sh %s", full) >=
          (int)sizeof(command))
        {
          snprintf(out, out_len,
                   "{\"ok\":false,\"error\":\"shell script path too long\"}");
          return -ENAMETOOLONG;
        }

      fc_json_escape(command, command_esc, sizeof(command_esc));
      snprintf(terminal_args, sizeof(terminal_args),
               "{\"command\":\"%s\"}", command_esc);
      return fc_builtin_terminal_run(ctx, terminal_args, out, out_len);
    }

  snprintf(out, out_len, "{\"ok\":false,\"error\":\"unknown script kind\"}");
  return -EINVAL;
}

static int fc_script_add_validation(cJSON *root,
                                    const fc_tool_context_t *ctx,
                                    fc_script_kind_t kind,
                                    fc_script_validation_mode_t mode,
                                    const char *rel, const char *full,
                                    int *validation_ret)
{
  char *run_out;
  cJSON *parsed;
  int ret;

  if (root == NULL || rel == NULL || full == NULL ||
      validation_ret == NULL)
    {
      return -EINVAL;
    }

  run_out = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  if (run_out == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddStringToObject(root, "validation_mode",
                          fc_script_validation_mode_name(mode));
  if (mode == FC_SCRIPT_VALIDATION_SYNTAX)
    {
      if (kind == FC_SCRIPT_KIND_BERRY)
        {
          (void)ctx;
          (void)full;
          ret = fc_berry_check_file(rel, run_out,
                                    CONFIG_FRUITCLAW_MAX_JSON);
        }
      else
        {
          snprintf(run_out, CONFIG_FRUITCLAW_MAX_JSON,
                   "{\"ok\":false,\"mode\":\"syntax\","
                   "\"error\":\"syntax validation supports Berry only\"}");
          ret = -ENOSYS;
        }
    }
  else
    {
      ret = fc_script_run_generated(ctx, kind, rel, full, "{}",
                                    run_out, CONFIG_FRUITCLAW_MAX_JSON);
    }

  *validation_ret = ret;
  parsed = cJSON_Parse(run_out);
  if (parsed != NULL)
    {
      cJSON_AddItemToObject(root, "validation", parsed);
    }
  else
    {
      cJSON_AddStringToObject(root, "validation_text", run_out);
    }

  free(run_out);
  return 0;
}

static int cap_script_list(const fc_tool_context_t *ctx,
                           const char *args_json, char *out,
                           size_t out_len)
{
  cJSON *root;
  cJSON *scripts;
  cJSON *schedules;
  char *printed;
  int count = 0;
  int pass;
  int ret = 0;

  (void)ctx;
  (void)args_json;

  root = cJSON_CreateObject();
  scripts = cJSON_CreateArray();
  schedules = fc_script_load_schedules();
  if (root == NULL || scripts == NULL)
    {
      cJSON_Delete(root);
      cJSON_Delete(scripts);
      cJSON_Delete(schedules);
      return -ENOMEM;
    }

  cJSON_AddStringToObject(root, "directory", FC_SCRIPT_GENERATED_DIR);
  cJSON_AddStringToObject(root, "generated_directory",
                          FC_SCRIPT_GENERATED_DIR);
  cJSON_AddStringToObject(root, "uploaded_directory", FC_SCRIPT_ROOT_DIR);

  for (pass = 0; pass < 2 && ret == 0; pass++)
    {
      const char *dir_rel = pass == 0 ? FC_SCRIPT_GENERATED_DIR :
                                        FC_SCRIPT_ROOT_DIR;
      const char *prefix = pass == 0 ? FC_SCRIPT_GENERATED_PRE :
                                       FC_SCRIPT_ROOT_PRE;
      const char *source = pass == 0 ? "generated" : "uploaded";
      char dir_path[FC_PATH_LEN];
      DIR *dir;
      struct dirent *ent;

      ret = fc_data_path(dir_rel, dir_path, sizeof(dir_path));
      if (ret < 0)
        {
          break;
        }

      if (pass == 0)
        {
          ret = fc_mkdir_p(dir_path);
          if (ret < 0)
            {
              break;
            }
        }

      dir = opendir(dir_path);
      if (dir == NULL)
        {
          if (pass == 1 && errno == ENOENT)
            {
              ret = 0;
              continue;
            }

          ret = -errno;
          break;
        }

      while ((ent = readdir(dir)) != NULL)
        {
          char rel[FC_PATH_LEN];
          char full[FC_PATH_LEN];
          char desc[FC_SCRIPT_DESC_MAX];
          fc_script_kind_t kind;
          struct stat st;
          cJSON *item;

          if (strcmp(ent->d_name, ".") == 0 ||
              strcmp(ent->d_name, "..") == 0 ||
              (pass == 1 && strcmp(ent->d_name, "generated") == 0) ||
              !fc_script_leaf_ok(ent->d_name))
            {
              continue;
            }

          if (fc_script_kind_from_leaf(ent->d_name, &kind) < 0)
            {
              continue;
            }

          if (fc_script_build_list_paths(prefix, ent->d_name, rel,
                                         sizeof(rel), full,
                                         sizeof(full)) < 0)
            {
              continue;
            }

          if (stat(full, &st) < 0 || !S_ISREG(st.st_mode))
            {
              continue;
            }

          item = cJSON_CreateObject();
          if (item == NULL)
            {
              ret = -ENOMEM;
              break;
            }

          fc_script_read_description(full, desc, sizeof(desc));
          cJSON_AddStringToObject(item, "name", ent->d_name);
          cJSON_AddStringToObject(item, "path", rel);
          cJSON_AddStringToObject(item, "kind", fc_script_kind_name(kind));
          cJSON_AddStringToObject(item, "source", source);
          if (desc[0] != '\0')
            {
              cJSON_AddStringToObject(item, "description", desc);
            }

          cJSON_AddNumberToObject(item, "bytes", (double)st.st_size);
          fc_script_add_meta_fields(item, full);
          fc_script_add_schedule_refs(item, schedules, rel, ent->d_name);
          cJSON_AddItemToArray(scripts, item);
          count++;
        }

      closedir(dir);
    }

  if (ret < 0)
    {
      cJSON_Delete(root);
      cJSON_Delete(scripts);
      cJSON_Delete(schedules);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"script list failed\","
               "\"code\":%d}", ret);
      return ret;
    }

  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddNumberToObject(root, "count", count);
  cJSON_AddItemToObject(root, "scripts", scripts);
  cJSON_Delete(schedules);
  printed = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
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

static int cap_script_read(const fc_tool_context_t *ctx,
                           const char *args_json, char *out,
                           size_t out_len)
{
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  cJSON *reply;
  const char *name;
  const char *kind_arg;
  char leaf[FC_SCRIPT_NAME_MAX + 4];
  char rel[FC_PATH_LEN];
  char full[FC_PATH_LEN];
  char desc[FC_SCRIPT_DESC_MAX];
  fc_script_kind_t requested_kind;
  fc_script_kind_t kind;
  char *text;
  char *printed;
  int ret;

  (void)ctx;
  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
  if (name == NULL)
    {
      name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    }

  kind_arg = cJSON_GetStringValue(cJSON_GetObjectItem(root, "kind"));
  ret = fc_script_kind_from_arg(kind_arg, &requested_kind);
  if (ret == 0)
    {
      ret = fc_script_make_paths_access(name, requested_kind, true,
                                        leaf, sizeof(leaf), rel,
                                        sizeof(rel), full, sizeof(full),
                                        &kind);
    }

  cJSON_Delete(root);
  if (ret < 0)
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"script denied\"}");
      return ret;
    }

  text = calloc(1, FC_SCRIPT_TEXT_MAX + 1);
  if (text == NULL)
    {
      return -ENOMEM;
    }

  ret = fc_read_text_file(full, text, FC_SCRIPT_TEXT_MAX + 1, false);
  if (ret < 0)
    {
      free(text);
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"script read failed\"}");
      return ret;
    }

  reply = cJSON_CreateObject();
  if (reply == NULL)
    {
      free(text);
      return -ENOMEM;
    }

  fc_script_description_from_text(text, desc, sizeof(desc));
  cJSON_AddBoolToObject(reply, "ok", true);
  cJSON_AddStringToObject(reply, "name", leaf);
  cJSON_AddStringToObject(reply, "path", rel);
  cJSON_AddStringToObject(reply, "kind", fc_script_kind_name(kind));
  cJSON_AddStringToObject(reply, "source",
                          strncmp(rel, FC_SCRIPT_GENERATED_PRE,
                                  strlen(FC_SCRIPT_GENERATED_PRE)) == 0 ?
                          "generated" : "uploaded");
  if (desc[0] != '\0')
    {
      cJSON_AddStringToObject(reply, "description", desc);
    }

  fc_script_add_meta_fields(reply, full);
  cJSON_AddStringToObject(reply, "text", text);
  free(text);
  printed = cJSON_PrintUnformatted(reply);
  cJSON_Delete(reply);
  if (printed == NULL)
    {
      return -ENOMEM;
    }

  ret = fc_strlcpy(out, printed, out_len);
  cJSON_free(printed);
  return ret;
}

static int cap_script_write(const fc_tool_context_t *ctx,
                            const char *args_json, char *out,
                            size_t out_len)
{
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  cJSON *validate_item;
  cJSON *reply;
  const char *name;
  const char *text;
  const char *description;
  const char *kind_arg;
  const char *validation_mode_arg;
  char leaf[FC_SCRIPT_NAME_MAX + 4];
  char rel[FC_PATH_LEN];
  char full[FC_PATH_LEN];
  char desc_copy[FC_SCRIPT_DESC_MAX];
  fc_script_kind_t requested_kind;
  fc_script_kind_t kind;
  fc_script_validation_mode_t validation_mode = FC_SCRIPT_VALIDATION_RUN;
  char *content;
  char *printed;
  bool validate = true;
  int validation_ret = 0;
  int ret;

  if (!fc_ctx_owner(ctx))
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"owner required\"}");
      return -EACCES;
    }

  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
  if (name == NULL)
    {
      name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    }

  text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
  description = cJSON_GetStringValue(cJSON_GetObjectItem(root,
                                                         "description"));
  kind_arg = cJSON_GetStringValue(cJSON_GetObjectItem(root, "kind"));
  validation_mode_arg = cJSON_GetStringValue(cJSON_GetObjectItem(root,
                                                                 "validate_mode"));
  validate_item = cJSON_GetObjectItem(root, "validate");
  if (cJSON_IsBool(validate_item))
    {
      validate = cJSON_IsTrue(validate_item);
    }

  desc_copy[0] = '\0';
  if (description != NULL && description[0] != '\0')
    {
      fc_strlcpy(desc_copy, description, sizeof(desc_copy));
    }

  if (text == NULL || strlen(text) > FC_SCRIPT_TEXT_MAX)
    {
      cJSON_Delete(root);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"missing or too large text\"}");
      return -EINVAL;
    }

  ret = fc_script_validation_mode_from_arg(validation_mode_arg,
                                           &validation_mode);
  if (ret < 0)
    {
      cJSON_Delete(root);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"bad validation mode\"}");
      return ret;
    }

  ret = fc_script_kind_from_arg(kind_arg, &requested_kind);
  if (ret == 0)
    {
      ret = fc_script_make_paths_kind(name, requested_kind, leaf,
                                      sizeof(leaf), rel, sizeof(rel),
                                      full, sizeof(full), &kind);
    }

  if (ret < 0)
    {
      cJSON_Delete(root);
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"script denied\"}");
      return ret;
    }

  content = calloc(1, strlen(text) + FC_SCRIPT_DESC_MAX + 48);
  if (content == NULL)
    {
      cJSON_Delete(root);
      return -ENOMEM;
    }

  if (desc_copy[0] != '\0')
    {
      snprintf(content, strlen(text) + FC_SCRIPT_DESC_MAX + 48,
               "# fruitclaw-description: %.*s\n%s",
               FC_SCRIPT_DESC_MAX - 1, desc_copy, text);
    }
  else
    {
      fc_strlcpy(content, text, strlen(text) + FC_SCRIPT_DESC_MAX + 48);
    }

  ret = fc_write_text_file_atomic(full, content);
  free(content);
  cJSON_Delete(root);
  if (ret < 0)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"script write failed\","
               "\"code\":%d}", ret);
      return ret;
    }

  reply = cJSON_CreateObject();
  if (reply == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddStringToObject(reply, "name", leaf);
  cJSON_AddStringToObject(reply, "path", rel);
  cJSON_AddStringToObject(reply, "kind", fc_script_kind_name(kind));
  cJSON_AddBoolToObject(reply, "saved", true);
  if (desc_copy[0] != '\0')
    {
      cJSON_AddStringToObject(reply, "description", desc_copy);
    }

  cJSON_AddBoolToObject(reply, "validated", false);
  if (validate)
    {
      ret = fc_script_add_validation(reply, ctx, kind, validation_mode,
                                     rel, full,
                                     &validation_ret);
      cJSON_ReplaceItemInObject(reply, "validated",
                                cJSON_CreateBool(validation_ret == 0));
      if (ret == 0)
        {
          ret = validation_ret;
        }
    }

  (void)fc_script_write_meta(full, leaf, rel, kind, desc_copy,
                             validate && validation_ret == 0,
                             validate ?
                             fc_script_validation_mode_name(validation_mode) :
                             NULL,
                             validation_ret, "write");
  cJSON_AddBoolToObject(reply, "ok", ret == 0);
  if (ret < 0)
    {
      cJSON_AddStringToObject(reply, "error", "script validation failed");
      cJSON_AddNumberToObject(reply, "code", ret);
    }

  printed = cJSON_PrintUnformatted(reply);
  cJSON_Delete(reply);
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

static int cap_script_validate(const fc_tool_context_t *ctx,
                               const char *args_json, char *out,
                               size_t out_len)
{
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  cJSON *reply;
  const char *name;
  const char *kind_arg;
  const char *validation_mode_arg;
  char leaf[FC_SCRIPT_NAME_MAX + 4];
  char rel[FC_PATH_LEN];
  char full[FC_PATH_LEN];
  fc_script_kind_t requested_kind;
  fc_script_kind_t kind;
  fc_script_validation_mode_t validation_mode = FC_SCRIPT_VALIDATION_RUN;
  char *printed;
  int validation_ret = 0;
  int ret;

  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
  if (name == NULL)
    {
      name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    }

  kind_arg = cJSON_GetStringValue(cJSON_GetObjectItem(root, "kind"));
  validation_mode_arg = cJSON_GetStringValue(cJSON_GetObjectItem(root,
                                                                 "mode"));
  if (validation_mode_arg == NULL)
    {
      validation_mode_arg = cJSON_GetStringValue(cJSON_GetObjectItem(root,
                                                   "validate_mode"));
    }

  ret = fc_script_validation_mode_from_arg(validation_mode_arg,
                                           &validation_mode);
  if (ret < 0)
    {
      cJSON_Delete(root);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"bad validation mode\"}");
      return ret;
    }

  ret = fc_script_kind_from_arg(kind_arg, &requested_kind);
  if (ret == 0)
    {
      ret = fc_script_make_paths_access(name, requested_kind, true,
                                        leaf, sizeof(leaf), rel,
                                        sizeof(rel), full, sizeof(full),
                                        &kind);
    }

  cJSON_Delete(root);
  if (ret < 0)
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"script denied\"}");
      return ret;
    }

  if (access(full, F_OK) < 0)
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"script missing\"}");
      return -ENOENT;
    }

  reply = cJSON_CreateObject();
  if (reply == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddStringToObject(reply, "name", leaf);
  cJSON_AddStringToObject(reply, "path", rel);
  cJSON_AddStringToObject(reply, "kind", fc_script_kind_name(kind));
  ret = fc_script_add_validation(reply, ctx, kind, validation_mode,
                                 rel, full,
                                 &validation_ret);
  if (ret == 0)
    {
      char desc[FC_SCRIPT_DESC_MAX];

      fc_script_read_description(full, desc, sizeof(desc));
      (void)fc_script_write_meta(full, leaf, rel, kind, desc,
                                 validation_ret == 0,
                                 fc_script_validation_mode_name(validation_mode),
                                 validation_ret, "validate");
    }

  cJSON_AddBoolToObject(reply, "ok", ret == 0 && validation_ret == 0);
  cJSON_AddBoolToObject(reply, "validated", validation_ret == 0);
  if (ret == 0 && validation_ret < 0)
    {
      ret = validation_ret;
      cJSON_AddStringToObject(reply, "error", "script validation failed");
      cJSON_AddNumberToObject(reply, "code", ret);
    }

  printed = cJSON_PrintUnformatted(reply);
  cJSON_Delete(reply);
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

static int cap_script_remove(const fc_tool_context_t *ctx,
                             const char *args_json, char *out,
                             size_t out_len)
{
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  cJSON *reply;
  const char *name;
  const char *kind_arg;
  char leaf[FC_SCRIPT_NAME_MAX + 4];
  char rel[FC_PATH_LEN];
  char full[FC_PATH_LEN];
  fc_script_kind_t requested_kind;
  fc_script_kind_t kind;
  char *printed;
  int guard_fd = -1;
  int ret;

  if (!fc_ctx_owner(ctx))
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"owner required\"}");
      return -EACCES;
    }

  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
  if (name == NULL)
    {
      name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    }

  kind_arg = cJSON_GetStringValue(cJSON_GetObjectItem(root, "kind"));
  ret = fc_script_kind_from_arg(kind_arg, &requested_kind);
  if (ret == 0)
    {
      ret = fc_script_make_paths_kind(name, requested_kind, leaf,
                                      sizeof(leaf), rel, sizeof(rel),
                                      full, sizeof(full), &kind);
    }

  cJSON_Delete(root);
  if (ret < 0)
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"script denied\"}");
      return ret;
    }

  ret = fc_tool_guard_arm(ctx, FC_GUARD_STAGE_FILE, &guard_fd);
  if (ret < 0)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"script remove guard "
               "unavailable\",\"code\":%d}", ret);
      return ret;
    }

  ret = unlink(full);
  if (ret < 0)
    {
      ret = -errno;
    }
  else
    {
      char meta_path[FC_PATH_LEN];

      if (fc_script_meta_path(full, meta_path, sizeof(meta_path)) == 0)
        {
          unlink(meta_path);
        }
    }

  fc_guard_disarm(guard_fd);

  reply = cJSON_CreateObject();
  if (reply == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddBoolToObject(reply, "ok", ret == 0);
  cJSON_AddStringToObject(reply, "name", leaf);
  cJSON_AddStringToObject(reply, "path", rel);
  cJSON_AddStringToObject(reply, "kind", fc_script_kind_name(kind));
  if (ret < 0)
    {
      cJSON_AddStringToObject(reply, "error", "script remove failed");
      cJSON_AddNumberToObject(reply, "code", ret);
    }

  printed = cJSON_PrintUnformatted(reply);
  cJSON_Delete(reply);
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

static int cap_script_run(const fc_tool_context_t *ctx,
                          const char *args_json, char *out,
                          size_t out_len)
{
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  cJSON *reply;
  cJSON *parsed;
  const char *name;
  const char *kind_arg;
  const char *script_args = "{}";
  char leaf[FC_SCRIPT_NAME_MAX + 4];
  char rel[FC_PATH_LEN];
  char full[FC_PATH_LEN];
  fc_script_kind_t requested_kind;
  fc_script_kind_t kind;
  char *run_out;
  char *printed;
  int ret;

  if (!fc_ctx_owner(ctx))
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"owner required\"}");
      return -EACCES;
    }

  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
  if (name == NULL)
    {
      name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    }

  kind_arg = cJSON_GetStringValue(cJSON_GetObjectItem(root, "kind"));
  script_args = cJSON_GetStringValue(cJSON_GetObjectItem(root, "args_json"));
  if (script_args == NULL)
    {
      script_args = "{}";
    }

  ret = fc_script_validate_args_json(script_args, out, out_len);
  if (ret < 0)
    {
      cJSON_Delete(root);
      return ret;
    }

  ret = fc_script_kind_from_arg(kind_arg, &requested_kind);
  if (ret == 0)
    {
      ret = fc_script_make_paths_access(name, requested_kind, true,
                                        leaf, sizeof(leaf), rel,
                                        sizeof(rel), full, sizeof(full),
                                        &kind);
    }

  cJSON_Delete(root);
  if (ret < 0)
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"script denied\"}");
      return ret;
    }

  if (access(full, F_OK) < 0)
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"script missing\"}");
      return -ENOENT;
    }

  run_out = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  if (run_out == NULL)
    {
      return -ENOMEM;
    }

  ret = fc_script_run_generated(ctx, kind, rel, full, script_args,
                                run_out, CONFIG_FRUITCLAW_MAX_JSON);
  reply = cJSON_CreateObject();
  if (reply == NULL)
    {
      free(run_out);
      return -ENOMEM;
    }

  cJSON_AddBoolToObject(reply, "ok", ret == 0);
  cJSON_AddStringToObject(reply, "name", leaf);
  cJSON_AddStringToObject(reply, "path", rel);
  cJSON_AddStringToObject(reply, "kind", fc_script_kind_name(kind));
  parsed = cJSON_Parse(run_out);
  if (parsed != NULL)
    {
      cJSON_AddItemToObject(reply, "result", parsed);
    }
  else
    {
      cJSON_AddStringToObject(reply, "output", run_out);
    }

  free(run_out);
  if (ret < 0)
    {
      cJSON_AddStringToObject(reply, "error", "script run failed");
      cJSON_AddNumberToObject(reply, "code", ret);
    }

  printed = cJSON_PrintUnformatted(reply);
  cJSON_Delete(reply);
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

static int cap_script_schedule(const fc_tool_context_t *ctx,
                               const char *args_json, char *out,
                               size_t out_len)
{
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  cJSON *tool_args;
  cJSON *printed_args;
  const char *name;
  const char *kind_arg;
  const char *type;
  const char *id;
  const char *script_args = "{}";
  char generated_id[48];
  char saved_id[48];
  char saved_type[16];
  char leaf[FC_SCRIPT_NAME_MAX + 4];
  char rel[FC_PATH_LEN];
  char full[FC_PATH_LEN];
  char prompt[CONFIG_FRUITCLAW_MAX_EVENT_TEXT];
  fc_script_kind_t requested_kind;
  fc_script_kind_t kind;
  char *tool_args_json;
  int guard_fd = -1;
  int ret = -EINVAL;

  if (!fc_ctx_owner(ctx))
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"owner required\"}");
      return -EACCES;
    }

  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
  if (name == NULL)
    {
      name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    }

  type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
  id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "id"));
  kind_arg = cJSON_GetStringValue(cJSON_GetObjectItem(root, "kind"));
  script_args = cJSON_GetStringValue(cJSON_GetObjectItem(root, "args_json"));
  if (script_args == NULL)
    {
      script_args = "{}";
    }

  if (type == NULL)
    {
      cJSON_Delete(root);
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"missing type\"}");
      return -EINVAL;
    }

  ret = fc_script_validate_args_json(script_args, out, out_len);
  if (ret < 0)
    {
      cJSON_Delete(root);
      return ret;
    }

  ret = fc_script_kind_from_arg(kind_arg, &requested_kind);
  if (ret == 0)
    {
      ret = fc_script_make_paths_access(name, requested_kind, true,
                                        leaf, sizeof(leaf), rel,
                                        sizeof(rel), full, sizeof(full),
                                        &kind);
    }

  if (ret < 0 || access(full, F_OK) < 0)
    {
      cJSON_Delete(root);
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"script missing\"}");
      return ret < 0 ? ret : -ENOENT;
    }

  if (id == NULL || id[0] == '\0')
    {
      fc_make_id(generated_id, sizeof(generated_id), "script");
      id = generated_id;
    }

  fc_strlcpy(saved_id, id, sizeof(saved_id));
  fc_strlcpy(saved_type, type, sizeof(saved_type));
  tool_args = cJSON_CreateObject();
  if (tool_args == NULL)
    {
      cJSON_Delete(root);
      return -ENOMEM;
    }

  cJSON_AddStringToObject(tool_args, "path", rel);
  cJSON_AddStringToObject(tool_args, "kind", fc_script_kind_name(kind));
  cJSON_AddStringToObject(tool_args, "args_json", script_args);
  tool_args_json = cJSON_PrintUnformatted(tool_args);
  cJSON_Delete(tool_args);
  if (tool_args_json == NULL)
    {
      cJSON_Delete(root);
      return -ENOMEM;
    }

  if (snprintf(prompt, sizeof(prompt), "tool:script.run %s",
               tool_args_json) >= (int)sizeof(prompt))
    {
      cJSON_free(tool_args_json);
      cJSON_Delete(root);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"schedule prompt too large\"}");
      return -ENOSPC;
    }

  cJSON_free(tool_args_json);
  ret = fc_tool_guard_arm(ctx, FC_GUARD_STAGE_SCHED, &guard_fd);
  if (ret < 0)
    {
      cJSON_Delete(root);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"scheduler guard unavailable\","
               "\"code\":%d}", ret);
      return ret;
    }

  if (strcmp(type, "interval") == 0)
    {
      cJSON *every = cJSON_GetObjectItem(root, "every_sec");

      if (cJSON_IsNumber(every) && every->valuedouble > 0)
        {
          ret = fc_scheduler_add_interval_ctx(id,
              (uint32_t)every->valuedouble, prompt, ctx);
        }
      else
        {
          ret = -EINVAL;
        }
    }
  else if (strcmp(type, "cron") == 0)
    {
      const char *expr = cJSON_GetStringValue(cJSON_GetObjectItem(root,
                                                                  "expr"));
      if (expr == NULL || fc_cron_validate(expr) < 0)
        {
          ret = -EINVAL;
        }
      else
        {
          ret = fc_scheduler_add_cron_ctx(id, expr, prompt, ctx);
        }
    }
  else if (strcmp(type, "once") == 0)
    {
      cJSON *at = cJSON_GetObjectItem(root, "at_epoch");
      cJSON *after = cJSON_GetObjectItem(root, "after_sec");
      int64_t epoch = 0;

      if (cJSON_IsNumber(at))
        {
          epoch = (int64_t)at->valuedouble;
        }
      else if (cJSON_IsNumber(after) && after->valuedouble > 0)
        {
          epoch = (fc_time_ms() / 1000) + (int64_t)after->valuedouble;
        }

      ret = epoch > 0 ? fc_scheduler_add_once_ctx(id, epoch, prompt, ctx) :
                        -EINVAL;
    }
  else if (strcmp(type, "boot") == 0)
    {
      ret = fc_scheduler_add_boot_ctx(id, prompt, ctx);
    }
  else
    {
      ret = -EINVAL;
    }

  cJSON_Delete(root);
  fc_guard_disarm(guard_fd);

  printed_args = cJSON_CreateObject();
  if (printed_args == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddBoolToObject(printed_args, "ok", ret == 0);
  cJSON_AddStringToObject(printed_args, "id", saved_id);
  cJSON_AddStringToObject(printed_args, "script", rel);
  cJSON_AddStringToObject(printed_args, "kind", fc_script_kind_name(kind));
  cJSON_AddStringToObject(printed_args, "type", saved_type);
  if (ret < 0)
    {
      cJSON_AddStringToObject(printed_args, "error",
                              strcmp(saved_type, "cron") == 0 &&
                              ret == -EINVAL ?
                              "invalid cron expression" :
                              "script schedule failed");
      cJSON_AddNumberToObject(printed_args, "code", ret);
    }

  tool_args_json = cJSON_PrintUnformatted(printed_args);
  cJSON_Delete(printed_args);
  if (tool_args_json == NULL)
    {
      return -ENOMEM;
    }

  if (fc_strlcpy(out, tool_args_json, out_len) < 0)
    {
      ret = -ENOSPC;
    }

  cJSON_free(tool_args_json);
  return ret;
}

static int cap_telegram_send(const fc_tool_context_t *ctx,
                             const char *args_json, char *out,
                             size_t out_len)
{
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  const char *chat_id;
  const char *text;
  bool implicit_chat = false;
  int ret;

  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  chat_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "chat_id"));
  text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
  if ((chat_id == NULL || chat_id[0] == '\0') && ctx != NULL)
    {
      chat_id = ctx->chat_id;
      implicit_chat = chat_id != NULL && chat_id[0] != '\0';
    }

  if (text == NULL || text[0] == '\0')
    {
      cJSON_Delete(root);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"missing text\"}");
      return -EINVAL;
    }

  if (ctx != NULL && strcmp(ctx->source, "mcp") == 0)
    {
      ret = fc_telegram_notify_owner_async(text);
      cJSON_Delete(root);
      snprintf(out, out_len, ret == 0 ?
               "{\"ok\":true,\"queued\":true}" :
               "{\"ok\":false,\"error\":\"telegram queue failed\"}");
      return ret;
    }

  if ((ctx == NULL || strcmp(ctx->source, "telegram") != 0 ||
       !implicit_chat) && (chat_id == NULL || chat_id[0] == '\0'))
    {
      ret = fc_telegram_notify_owner_async(text);
      cJSON_Delete(root);
      snprintf(out, out_len, ret == 0 ?
               "{\"ok\":true,\"queued\":true}" :
               "{\"ok\":false,\"error\":\"telegram queue failed\"}");
      return ret;
    }

  if (chat_id == NULL || chat_id[0] == '\0')
    {
      cJSON_Delete(root);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"missing chat_id\"}");
      return -EINVAL;
    }

  ret = fc_telegram_send_message(chat_id, text);
  cJSON_Delete(root);
  snprintf(out, out_len, ret == 0 ?
           "{\"ok\":true}" :
           "{\"ok\":false,\"error\":\"telegram send failed\"}");
  return ret;
}

static int cap_scheduler_list(const fc_tool_context_t *ctx,
                              const char *args_json, char *out,
                              size_t out_len)
{
  char *list;
  char *json;
  cJSON *root;
  char *printed;
  int ret;

  (void)ctx;
  (void)args_json;
  list = calloc(1, CONFIG_FRUITCLAW_MAX_JSON / 2);
  json = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  if (list == NULL || json == NULL)
    {
      free(list);
      free(json);
      return -ENOMEM;
    }

  ret = fc_scheduler_list(list, CONFIG_FRUITCLAW_MAX_JSON / 2);
  if (ret == 0)
    {
      ret = fc_scheduler_list_json(json, CONFIG_FRUITCLAW_MAX_JSON);
    }

  if (ret == 0)
    {
      root = cJSON_Parse(json);
      if (root == NULL)
        {
          ret = -EINVAL;
        }
      else
        {
          cJSON_AddStringToObject(root, "schedules", list);
          printed = cJSON_PrintUnformatted(root);
          cJSON_Delete(root);
          if (printed == NULL)
            {
              ret = -ENOMEM;
            }
          else
            {
              ret = fc_strlcpy(out, printed, out_len);
              cJSON_free(printed);
            }
        }
    }

  if (ret < 0)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"schedule list failed\","
               "\"code\":%d}", ret);
    }

  free(list);
  free(json);
  return ret;
}

static int cap_scheduler_add(const fc_tool_context_t *ctx,
                             const char *args_json, char *out,
                             size_t out_len)
{
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  const char *id;
  const char *type;
  const char *prompt;
  char generated_id[48];
  char saved_id[48];
  int guard_fd = -1;
  int ret = -EINVAL;

  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "id"));
  type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
  prompt = cJSON_GetStringValue(cJSON_GetObjectItem(root, "prompt"));
  if (id == NULL || id[0] == '\0')
    {
      fc_make_id(generated_id, sizeof(generated_id), "job");
      id = generated_id;
    }

  if (type == NULL || prompt == NULL)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"missing type or prompt\"}");
      cJSON_Delete(root);
      return -EINVAL;
    }

  fc_strlcpy(saved_id, id, sizeof(saved_id));

  if (strcmp(type, "interval") == 0)
    {
      cJSON *every = cJSON_GetObjectItem(root, "every_sec");
      if (cJSON_IsNumber(every) && every->valuedouble > 0)
        {
          ret = fc_tool_guard_arm(ctx, FC_GUARD_STAGE_SCHED, &guard_fd);
          if (ret < 0)
            {
              cJSON_Delete(root);
              snprintf(out, out_len,
                       "{\"ok\":false,\"error\":\"scheduler guard "
                       "unavailable\",\"code\":%d}", ret);
              return ret;
            }

          ret = fc_scheduler_add_interval_ctx(id,
              (uint32_t)every->valuedouble, prompt, ctx);
        }
    }
  else if (strcmp(type, "cron") == 0)
    {
      const char *expr = cJSON_GetStringValue(cJSON_GetObjectItem(root,
                                                                  "expr"));
      if (expr == NULL || fc_cron_validate(expr) < 0)
        {
          cJSON_Delete(root);
          snprintf(out, out_len,
                   "{\"ok\":false,\"error\":\"invalid cron expression\","
                   "\"detail\":\"use 5 fields: minute hour day month "
                   "weekday\"}");
          return -EINVAL;
        }

      ret = fc_tool_guard_arm(ctx, FC_GUARD_STAGE_SCHED, &guard_fd);
      if (ret < 0)
        {
          cJSON_Delete(root);
          snprintf(out, out_len,
                   "{\"ok\":false,\"error\":\"scheduler guard "
                   "unavailable\",\"code\":%d}", ret);
          return ret;
        }

      ret = fc_scheduler_add_cron_ctx(id, expr, prompt, ctx);
    }
  else if (strcmp(type, "once") == 0)
    {
      cJSON *at = cJSON_GetObjectItem(root, "at_epoch");
      cJSON *after = cJSON_GetObjectItem(root, "after_sec");
      int64_t epoch = 0;

      if (cJSON_IsNumber(at))
        {
          epoch = (int64_t)at->valuedouble;
        }
      else if (cJSON_IsNumber(after) && after->valuedouble > 0)
        {
          epoch = (fc_time_ms() / 1000) + (int64_t)after->valuedouble;
        }

      if (epoch > 0)
        {
          ret = fc_tool_guard_arm(ctx, FC_GUARD_STAGE_SCHED, &guard_fd);
          if (ret < 0)
            {
              cJSON_Delete(root);
              snprintf(out, out_len,
                       "{\"ok\":false,\"error\":\"scheduler guard "
                       "unavailable\",\"code\":%d}", ret);
              return ret;
            }

          ret = fc_scheduler_add_once_ctx(id, epoch, prompt, ctx);
        }
    }
  else if (strcmp(type, "boot") == 0)
    {
      ret = fc_tool_guard_arm(ctx, FC_GUARD_STAGE_SCHED, &guard_fd);
      if (ret < 0)
        {
          cJSON_Delete(root);
          snprintf(out, out_len,
                   "{\"ok\":false,\"error\":\"scheduler guard "
                   "unavailable\",\"code\":%d}", ret);
          return ret;
        }

      ret = fc_scheduler_add_boot_ctx(id, prompt, ctx);
    }

  cJSON_Delete(root);
  fc_guard_disarm(guard_fd);
  if (ret == 0)
    {
      snprintf(out, out_len, "{\"ok\":true,\"id\":\"%s\"}", saved_id);
    }
  else
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"schedule add failed\","
               "\"code\":%d}", ret);
    }

  return ret;
}

static int cap_scheduler_remove(const fc_tool_context_t *ctx,
                                const char *args_json, char *out,
                                size_t out_len)
{
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  const char *id;
  int guard_fd = -1;
  int ret;

  (void)ctx;
  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "id"));
  if (id == NULL)
    {
      ret = -EINVAL;
    }
  else
    {
      ret = fc_tool_guard_arm(ctx, FC_GUARD_STAGE_SCHED, &guard_fd);
      if (ret == 0)
        {
          ret = fc_scheduler_remove(id);
        }
    }

  cJSON_Delete(root);
  fc_guard_disarm(guard_fd);
  snprintf(out, out_len, ret == 0 ?
           "{\"ok\":true}" :
           "{\"ok\":false,\"error\":\"schedule remove failed\"}");
  return ret;
}

static int cap_berry_run_script(const fc_tool_context_t *ctx,
                                const char *args_json, char *out,
                                size_t out_len)
{
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  const char *path;
  const char *args = "{}";
  int ret;

  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
  args = cJSON_GetStringValue(cJSON_GetObjectItem(root, "args_json"));
  ret = fc_berry_run_file(ctx, path, args ? args : "{}", out, out_len);
  cJSON_Delete(root);
  return ret;
}

static int cap_http_request(const fc_tool_context_t *ctx,
                            const char *args_json, char *out, size_t out_len)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_HTTP_TOOL
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  const char *method;
  const char *url;
  const char *body;
  char *resp;
  long status = 0;
  int ret;

  (void)ctx;
  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  method = cJSON_GetStringValue(cJSON_GetObjectItem(root, "method"));
  url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
  body = cJSON_GetStringValue(cJSON_GetObjectItem(root, "body"));
  if (method == NULL)
    {
      method = "GET";
    }

  if (url == NULL || !fc_url_host_allowed(url))
    {
      cJSON_Delete(root);
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"URL not allowed\"}");
      return -EACCES;
    }

  resp = calloc(1, CONFIG_FRUITCLAW_MAX_HTTP_BODY);
  if (resp == NULL)
    {
      cJSON_Delete(root);
      return -ENOMEM;
    }

  if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0)
    {
      free(resp);
      cJSON_Delete(root);
      snprintf(out, out_len,
               "{\"ok\":false,\"status\":%ld,\"error\":\"bad method\"}",
               status);
      return -EINVAL;
    }

  if (strcmp(method, "GET") == 0)
    {
      if (ctx != NULL && ctx->guarded)
        {
          ret = fc_http_get(url, NULL, resp, CONFIG_FRUITCLAW_MAX_HTTP_BODY,
                            &status);
        }
      else
        {
          ret = fc_http_get_guarded(FC_GUARD_STAGE_HTTP_TOOL,
                                    CONFIG_FRUITCLAW_HTTP_GUARD_TIMEOUT_MS,
                                    url, NULL, resp,
                                    CONFIG_FRUITCLAW_MAX_HTTP_BODY, &status);
        }
    }
  else
    {
      if (ctx != NULL && ctx->guarded)
        {
          ret = fc_http_post_json(url, NULL, body ? body : "{}", resp,
                                  CONFIG_FRUITCLAW_MAX_HTTP_BODY, &status);
        }
      else
        {
          ret = fc_http_post_json_guarded(
                  FC_GUARD_STAGE_HTTP_TOOL,
                  CONFIG_FRUITCLAW_HTTP_GUARD_TIMEOUT_MS,
                  url, NULL, body ? body : "{}", resp,
                  CONFIG_FRUITCLAW_MAX_HTTP_BODY, &status);
        }
    }

  if (ret == 0)
    {
      char *esc = calloc(1, strlen(resp) * 6 + 1);
      if (esc == NULL)
        {
          free(resp);
          cJSON_Delete(root);
          return -ENOMEM;
        }

      fc_json_escape(resp, esc, strlen(resp) * 6 + 1);
      snprintf(out, out_len,
               "{\"ok\":true,\"status\":%ld,\"body\":\"%s\"}",
               status, esc);
      free(esc);
    }
  else
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"status\":%ld,\"error\":\"request failed\"}",
               status);
    }

  free(resp);
  cJSON_Delete(root);
  return ret;
#else
  (void)ctx;
  (void)args_json;
  snprintf(out, out_len,
           "{\"ok\":false,\"error\":\"http.request disabled\"}");
  return -ENOSYS;
#endif
}

static int fc_terminal_append(char *buf, size_t buf_len, size_t *off,
                              const char *text)
{
  size_t len;

  if (buf == NULL || off == NULL || text == NULL)
    {
      return -EINVAL;
    }

  len = strlen(text);
  if (*off >= buf_len)
    {
      return -ENOSPC;
    }

  if (len >= buf_len - *off)
    {
      len = buf_len - *off - 1;
    }

  memcpy(buf + *off, text, len);
  *off += len;
  buf[*off] = '\0';

  return len == strlen(text) ? 0 : -ENOSPC;
}

static int fc_terminal_run_fast(const char *command, char *buf,
                                size_t buf_len, int *status)
{
  size_t off = 0;
  int ret;

  if (command == NULL || buf == NULL || status == NULL)
    {
      return -EINVAL;
    }

  if (strcmp(command, "uname") == 0 || strcmp(command, "uname -a") == 0)
    {
      struct utsname uts;

      if (uname(&uts) < 0)
        {
          *status = errno;
          snprintf(buf, buf_len, "uname failed: %d\n", errno);
          return 0;
        }

      snprintf(buf, buf_len, "%s %s %s %s %s\n",
               uts.sysname, uts.nodename, uts.release, uts.version,
               uts.machine);
      *status = 0;
      return 0;
    }

  if (strcmp(command, "help") == 0 || strcmp(command, "?") == 0)
    {
      ret = fc_terminal_append(buf, buf_len, &off,
        "FruitClaw terminal.run fast help\n"
        "Fast in-process commands:\n"
        "  help\n"
        "  uname -a\n"
        "Notes:\n"
        "  Use MCP device.list instead of terminal-run ls /dev.\n"
        "  Use serial/local dvictrl info for framebuffer diagnostics.\n"
        "  Use MCP system.status instead of terminal-run fruitclaw status.\n"
        "  Other shell commands are guarded and may watchdog-reset if they wedge.\n");
      *status = ret == 0 ? 0 : ENOSPC;
      return 0;
    }

  if (strcmp(command, "ls /dev") == 0 || strcmp(command, "ls /dev/") == 0)
    {
      ret = fc_terminal_append(buf, buf_len, &off,
        "Use the MCP device.list tool for /dev enumeration.\n"
        "terminal.run keeps this alias non-enumerating so an unsafe device "
        "filesystem walk cannot monopolize the HTTP service.\n");
      *status = ret == 0 ? 0 : ENOSPC;
      return 0;
    }

  if (strcmp(command, "dvictrl info") == 0)
    {
      ret = fc_terminal_append(buf, buf_len, &off,
        "DVI diagnostics are local-only from terminal.run for now.\n"
        "Use serial/local dvictrl info while framebuffer debug ioctls are "
        "kept out of the MCP HTTP worker path.\n");
      *status = ret == 0 ? 0 : ENOSPC;
      return 0;
    }

  return -ENOENT;
}

static const char *fc_terminal_reboot_command(const char *command)
{
  if (command == NULL)
    {
      return NULL;
    }

  if (strcmp(command, "bootsel") == 0)
    {
      return "bootsel";
    }

  if (strcmp(command, "reboot") == 0)
    {
      return "reboot";
    }

  if (strcmp(command, "bootguard off; bootsel") == 0 ||
      strcmp(command, "bootguard off && bootsel") == 0)
    {
      return "bootsel";
    }

  return NULL;
}

static bool fc_terminal_command_uses_fruitclaw(const char *command)
{
  const char *prog = CONFIG_FRUITCLAW_PROGNAME;
  size_t prog_len = strlen(prog);
  const char *p;

  if (command == NULL || prog_len == 0)
    {
      return false;
    }

  for (p = command; *p != '\0'; p++)
    {
      char before;
      char after;

      if (strncmp(p, prog, prog_len) != 0)
        {
          continue;
        }

      before = p == command ? '\0' : p[-1];
      after = p[prog_len];
      if ((before == '\0' || before == ' ' || before == '\t' ||
           before == ';' || before == '&' || before == '|' ||
           before == '(') &&
          (after == '\0' || after == ' ' || after == '\t' ||
           after == ';' || after == '&' || after == '|' ||
           after == ')' || after == '>'))
        {
          return true;
        }
    }

  return false;
}

int fc_builtin_terminal_run(const fc_tool_context_t *ctx,
                            const char *args_json, char *out,
                            size_t out_len)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_TERMINAL_TOOL
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  const char *arg_command;
  char command_buf[FC_TERMINAL_MAX_COMMAND + 1];
  const char *command;
  const char *reboot_command;
  char tmp_path[FC_PATH_LEN];
  char line[FC_TERMINAL_MAX_COMMAND + FC_PATH_LEN + 24];
  char *buf;
  char *esc;
  int sysret;
  int fastret;
  int ret;
  int guard_fd = -1;

  if (!fc_ctx_owner(ctx))
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"owner required\"}");
      return -EACCES;
    }

  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  arg_command = cJSON_GetStringValue(cJSON_GetObjectItem(root, "command"));
  if (arg_command == NULL || strlen(arg_command) > FC_TERMINAL_MAX_COMMAND)
    {
      cJSON_Delete(root);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"missing or too long command\"}");
      return -EINVAL;
    }

  fc_strlcpy(command_buf, arg_command, sizeof(command_buf));
  fc_trim(command_buf);
  command = command_buf;
  if (command[0] == '\0')
    {
      cJSON_Delete(root);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"missing or too long command\"}");
      return -EINVAL;
    }

  reboot_command = fc_terminal_reboot_command(command);
  if (reboot_command == NULL && fc_terminal_command_uses_fruitclaw(command))
    {
      cJSON_Delete(root);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"recursive fruitclaw command "
               "denied; use MCP tools directly\"}");
      return -ELOOP;
    }

  buf = calloc(1, FC_TOOL_TEXT_MAX + 1);
  esc = calloc(1, FC_TOOL_TEXT_MAX * 6 + 1);
  if (buf == NULL || esc == NULL)
    {
      cJSON_Delete(root);
      free(buf);
      free(esc);
      return -ENOMEM;
    }

  fastret = reboot_command != NULL ? -ENOENT :
            fc_terminal_run_fast(command, buf, FC_TOOL_TEXT_MAX + 1,
                                 &sysret);
  if (fastret < 0)
    {
      if (reboot_command != NULL && strcmp(reboot_command, "bootsel") == 0)
        {
          cJSON_Delete(root);
          free(buf);
          free(esc);
          fc_guard_force_recovery(FC_GUARD_STAGE_TERMINAL);
          return -ETIMEDOUT;
        }

      ret = fc_data_path("tmp/terminal.out", tmp_path, sizeof(tmp_path));
      if (ret < 0)
        {
          cJSON_Delete(root);
          free(buf);
          free(esc);
          return ret;
        }

      snprintf(line, sizeof(line), "%s > %s 2>&1",
               reboot_command != NULL ? reboot_command : command,
               tmp_path);
      if (reboot_command != NULL)
        {
          fc_guard_prepare_controlled_reboot();
        }
      else
        {
          ret = fc_tool_guard_arm(ctx, FC_GUARD_STAGE_TERMINAL, &guard_fd);
          if (ret < 0)
            {
              cJSON_Delete(root);
              free(buf);
              free(esc);
              snprintf(out, out_len,
                       "{\"ok\":false,\"error\":\"terminal guard "
                       "unavailable\",\"code\":%d}", ret);
              return ret;
            }
        }

      sysret = system(line);
      if (guard_fd >= 0)
        {
          fc_guard_disarm(guard_fd);
        }

      ret = fc_read_text_file(tmp_path, buf, FC_TOOL_TEXT_MAX + 1, false);
      if (ret < 0)
        {
          buf[0] = '\0';
        }
    }

  cJSON_Delete(root);

  fc_json_escape(buf, esc, FC_TOOL_TEXT_MAX * 6 + 1);
  snprintf(out, out_len,
           "{\"ok\":%s,\"status\":%d,\"output\":\"%s\"}",
           sysret == 0 ? "true" : "false", sysret, esc);
  free(buf);
  free(esc);
  return sysret == 0 ? 0 : -EIO;
#else
  (void)ctx;
  (void)args_json;
  snprintf(out, out_len,
           "{\"ok\":false,\"error\":\"terminal.run disabled\"}");
  return -ENOSYS;
#endif
}

static bool fc_rtttl_arg_safe(const char *tune)
{
  const char *p;

  if (tune == NULL || tune[0] == '\0' ||
      strlen(tune) > FC_RTTTL_MAX_TUNE)
    {
      return false;
    }

  for (p = tune; *p != '\0'; p++)
    {
      if (!isalnum((unsigned char)*p) && *p != ':' && *p != ',' &&
          *p != '=' && *p != '#' && *p != '.' && *p != '_' &&
          *p != '-')
        {
          return false;
        }
    }

  return true;
}

static int cap_rtttl_play(const fc_tool_context_t *ctx,
                          const char *args_json, char *out,
                          size_t out_len)
{
#ifdef CONFIG_SYSTEM_RTTTL
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  const char *tune;
  cJSON *volume_item;
  char tmp_leaf[64];
  char tmp_path[FC_PATH_LEN];
  char command[FC_RTTTL_MAX_TUNE + FC_PATH_LEN + 64];
  char *buf;
  char *esc;
  int volume = CONFIG_SYSTEM_RTTTL_VOLUME;
  int guard_fd = -1;
  int sysret;
  int ret;

  if (!fc_ctx_owner(ctx))
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"owner required\"}");
      return -EACCES;
    }

  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  tune = cJSON_GetStringValue(cJSON_GetObjectItem(root, "tune"));
  if (tune == NULL)
    {
      tune = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    }

  volume_item = cJSON_GetObjectItem(root, "volume");
  if (cJSON_IsNumber(volume_item) && volume_item->valuedouble >= 0 &&
      volume_item->valuedouble <= 100)
    {
      volume = (int)volume_item->valuedouble;
    }

  if (!fc_rtttl_arg_safe(tune))
    {
      cJSON_Delete(root);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"missing or unsafe tune\"}");
      return -EINVAL;
    }

  snprintf(tmp_leaf, sizeof(tmp_leaf), "tmp/rtttl-%ld.out", (long)getpid());
  ret = fc_data_path(tmp_leaf, tmp_path, sizeof(tmp_path));
  if (ret < 0)
    {
      cJSON_Delete(root);
      return ret;
    }

  if (snprintf(command, sizeof(command), "rtttl -v %d %s > %s 2>&1",
               volume, tune, tmp_path) >= (int)sizeof(command))
    {
      cJSON_Delete(root);
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"command too long\"}");
      return -ENAMETOOLONG;
    }

  cJSON_Delete(root);
  buf = calloc(1, FC_TOOL_TEXT_MAX + 1);
  esc = calloc(1, FC_TOOL_TEXT_MAX * 6 + 1);
  if (buf == NULL || esc == NULL)
    {
      free(buf);
      free(esc);
      return -ENOMEM;
    }

  ret = fc_tool_guard_arm(ctx, FC_GUARD_STAGE_TERMINAL, &guard_fd);
  if (ret < 0)
    {
      free(buf);
      free(esc);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"rtttl guard unavailable\","
               "\"code\":%d}", ret);
      return ret;
    }

#if defined(CONFIG_ARCH_BOARD_ADAFRUIT_FRUIT_JAM_RP2350) && \
    defined(CONFIG_RP23XX_I2S)
  (void)board_fruitjam_audio_codec_recover();
#endif

  sysret = system(command);
  fc_guard_disarm(guard_fd);
  ret = fc_read_text_file(tmp_path, buf, FC_TOOL_TEXT_MAX + 1, false);
  unlink(tmp_path);
  if (ret < 0)
    {
      buf[0] = '\0';
    }

  fc_json_escape(buf, esc, FC_TOOL_TEXT_MAX * 6 + 1);
  snprintf(out, out_len,
           "{\"ok\":%s,\"status\":%d,\"volume\":%d,\"output\":\"%s\"}",
           sysret == 0 ? "true" : "false", sysret, volume, esc);
  free(buf);
  free(esc);
  return sysret == 0 ? 0 : -EIO;
#else
  (void)ctx;
  (void)args_json;
  snprintf(out, out_len, "{\"ok\":false,\"error\":\"rtttl disabled\"}");
  return -ENOSYS;
#endif
}

static int fc_neopixels_open(void)
{
  int fd = open(CONFIG_SYSTEM_NEOPIXELS_DEVPATH, O_WRONLY);

  return fd < 0 ? -errno : fd;
}

static uint32_t fc_neopixels_rgb(uint8_t r, uint8_t g, uint8_t b,
                                 uint8_t brightness)
{
  uint32_t sr = ((uint32_t)r * brightness + 127) / 255;
  uint32_t sg = ((uint32_t)g * brightness + 127) / 255;
  uint32_t sb = ((uint32_t)b * brightness + 127) / 255;

  return ws2812_gamma_correct((sr << 16) | (sg << 8) | sb);
}

static int fc_neopixels_write(int fd, const uint32_t *pixels)
{
  ssize_t expected = (ssize_t)(FC_NEOPIXEL_COUNT * sizeof(uint32_t));
  ssize_t nwritten;

  lseek(fd, 0, SEEK_SET);
  nwritten = write(fd, pixels, expected);
  if (nwritten < 0)
    {
      return -errno;
    }

  return nwritten == expected ? 0 : -EIO;
}

static int fc_neopixels_fill_fd(int fd, uint8_t r, uint8_t g, uint8_t b,
                                uint8_t brightness)
{
  uint32_t *pixels;
  uint32_t pixel;
  int i;
  int ret;

  pixels = calloc(FC_NEOPIXEL_COUNT, sizeof(uint32_t));
  if (pixels == NULL)
    {
      return -ENOMEM;
    }

  pixel = fc_neopixels_rgb(r, g, b, brightness);
  for (i = 0; i < FC_NEOPIXEL_COUNT; i++)
    {
      pixels[i] = pixel;
    }

  ret = fc_neopixels_write(fd, pixels);
  free(pixels);
  return ret;
}

static int fc_neopixels_effect_fd(int fd, const char *effect, uint8_t r,
                                  uint8_t g, uint8_t b, int cycles,
                                  int delay_ms, uint8_t brightness)
{
  uint32_t *pixels;
  int frame;
  int i;
  int ret = 0;

  if (cycles < 1)
    {
      cycles = 1;
    }

  if (cycles > 64)
    {
      cycles = 64;
    }

  if (delay_ms < 1)
    {
      delay_ms = 20;
    }

  if (delay_ms > 1000)
    {
      delay_ms = 1000;
    }

  pixels = calloc(FC_NEOPIXEL_COUNT, sizeof(uint32_t));
  if (pixels == NULL)
    {
      return -ENOMEM;
    }

  if (strcmp(effect, "rainbow") == 0)
    {
      for (frame = 0; frame < cycles * 64 && ret == 0; frame++)
        {
          for (i = 0; i < FC_NEOPIXEL_COUNT; i++)
            {
              pixels[i] = ws2812_gamma_correct(
                          ws2812_hsv_to_rgb((frame * 4 + i * 36) & 0xff,
                                            0xff, brightness));
            }

          ret = fc_neopixels_write(fd, pixels);
          usleep((useconds_t)delay_ms * 1000);
        }
    }
  else if (strcmp(effect, "chase") == 0)
    {
      uint32_t lit = fc_neopixels_rgb(r, g, b, brightness);
      uint32_t dim = fc_neopixels_rgb(r, g, b, brightness / 8);
      for (frame = 0; frame < cycles * FC_NEOPIXEL_COUNT && ret == 0;
           frame++)
        {
          for (i = 0; i < FC_NEOPIXEL_COUNT; i++)
            {
              pixels[i] = (i == frame % FC_NEOPIXEL_COUNT) ? lit : dim;
            }

          ret = fc_neopixels_write(fd, pixels);
          usleep((useconds_t)delay_ms * 1000);
        }
    }
  else if (strcmp(effect, "pulse") == 0)
    {
      int step;
      for (frame = 0; frame < cycles && ret == 0; frame++)
        {
          for (step = 0; step <= 16 && ret == 0; step++)
            {
              ret = fc_neopixels_fill_fd(fd, r, g, b,
                    (uint8_t)(((uint32_t)brightness * step) / 16));
              usleep((useconds_t)delay_ms * 1000);
            }

          for (step = 15; step >= 0 && ret == 0; step--)
            {
              ret = fc_neopixels_fill_fd(fd, r, g, b,
                    (uint8_t)(((uint32_t)brightness * step) / 16));
              usleep((useconds_t)delay_ms * 1000);
            }
        }
    }
  else
    {
      ret = -EINVAL;
    }

  free(pixels);
  return ret;
}

int fc_builtin_neopixels_set(const fc_tool_context_t *ctx,
                             const char *args_json, char *out,
                             size_t out_len)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_NEOPIXELS_TOOL
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  const char *effect;
  const char *color;
  const char *rgb;
  char effect_buf[16];
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t brightness = 96;
  int cycles = 2;
  int delay_ms = 20;
  int duration_ms = 0;
  int fd;
  int ret;
  int guard_fd = -1;

  if (!fc_ctx_owner(ctx))
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"owner required\"}");
      return -EACCES;
    }

  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  effect = cJSON_GetStringValue(cJSON_GetObjectItem(root, "effect"));
  color = cJSON_GetStringValue(cJSON_GetObjectItem(root, "color"));
  rgb = cJSON_GetStringValue(cJSON_GetObjectItem(root, "rgb"));
  if (effect == NULL)
    {
      effect = "fill";
    }

  fc_strlcpy(effect_buf, effect, sizeof(effect_buf));
  effect = effect_buf;

  if (strcmp(effect, "off") == 0)
    {
      brightness = 0;
    }
  else if (fc_json_get_uint8(root, "r", &r) == 0 &&
           fc_json_get_uint8(root, "g", &g) == 0 &&
           fc_json_get_uint8(root, "b", &b) == 0)
    {
      ;
    }
  else if (color != NULL && fc_color_lookup(color, &r, &g, &b))
    {
      ;
    }
  else if (rgb != NULL && fc_color_parse_rgb(rgb, &r, &g, &b))
    {
      ;
    }
  else if (strcmp(effect, "rainbow") != 0)
    {
      cJSON_Delete(root);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"missing color or rgb\"}");
      return -EINVAL;
    }

  if (cJSON_IsNumber(cJSON_GetObjectItem(root, "brightness")))
    {
      cJSON *item = cJSON_GetObjectItem(root, "brightness");
      if (item->valuedouble >= 0 && item->valuedouble <= 255)
        {
          brightness = (uint8_t)item->valuedouble;
        }
    }

  if (cJSON_IsNumber(cJSON_GetObjectItem(root, "cycles")))
    {
      cycles = (int)cJSON_GetObjectItem(root, "cycles")->valuedouble;
    }

  if (cJSON_IsNumber(cJSON_GetObjectItem(root, "delay_ms")))
    {
      delay_ms = (int)cJSON_GetObjectItem(root, "delay_ms")->valuedouble;
    }

  if (cJSON_IsNumber(cJSON_GetObjectItem(root, "duration_ms")))
    {
      duration_ms = (int)cJSON_GetObjectItem(root, "duration_ms")->valuedouble;
    }

  if (duration_ms > 0 && strcmp(effect, "fill") != 0 &&
      strcmp(effect, "off") != 0)
    {
      int frames_per_cycle = 64;
      int cycle_ms;

      if (delay_ms < 1)
        {
          delay_ms = 20;
        }

      if (strcmp(effect, "chase") == 0)
        {
          frames_per_cycle = FC_NEOPIXEL_COUNT;
        }
      else if (strcmp(effect, "pulse") == 0)
        {
          frames_per_cycle = 33;
        }

      cycle_ms = frames_per_cycle * delay_ms;
      if (cycle_ms > 0)
        {
          cycles = (duration_ms + cycle_ms - 1) / cycle_ms;
        }
    }

  cJSON_Delete(root);
  ret = fc_tool_guard_arm(ctx, FC_GUARD_STAGE_NEOPIXELS, &guard_fd);
  if (ret < 0)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"neopixel guard unavailable\","
               "\"code\":%d}", ret);
      return ret;
    }

  fd = fc_neopixels_open();
  if (fd < 0)
    {
      fc_guard_disarm(guard_fd);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"open %s failed\",\"errno\":%d}",
               CONFIG_SYSTEM_NEOPIXELS_DEVPATH, -fd);
      return fd;
    }

  if (strcmp(effect, "fill") == 0 || strcmp(effect, "off") == 0)
    {
      ret = fc_neopixels_fill_fd(fd, r, g, b, brightness);
    }
  else
    {
      ret = fc_neopixels_effect_fd(fd, effect, r, g, b, cycles, delay_ms,
                                   brightness);
    }

  close(fd);
  fc_guard_disarm(guard_fd);
  snprintf(out, out_len, ret == 0 ?
           "{\"ok\":true,\"device\":\"%s\",\"effect\":\"%s\"}" :
           "{\"ok\":false,\"error\":\"neopixels failed\"}",
           CONFIG_SYSTEM_NEOPIXELS_DEVPATH, effect);
  return ret;
#else
  (void)ctx;
  (void)args_json;
  snprintf(out, out_len,
           "{\"ok\":false,\"error\":\"neopixels disabled\"}");
  return -ENOSYS;
#endif
}

static int cap_neopixels_off(const fc_tool_context_t *ctx,
                             const char *args_json, char *out,
                             size_t out_len)
{
  (void)args_json;
  return fc_builtin_neopixels_set(ctx, "{\"effect\":\"off\"}", out, out_len);
}

static bool fc_device_path_ok(const char *path)
{
  return path != NULL && strncmp(path, "/dev/", 5) == 0 &&
         !fc_path_has_parent_ref(path) && strlen(path) < FC_PATH_LEN;
}

static bool fc_device_path_known_block(const char *path)
{
  return path != NULL &&
         (strncmp(path, "/dev/mmcsd", strlen("/dev/mmcsd")) == 0 ||
          strncmp(path, "/dev/ram", strlen("/dev/ram")) == 0 ||
          strncmp(path, "/dev/mtd", strlen("/dev/mtd")) == 0 ||
          strncmp(path, "/dev/smart", strlen("/dev/smart")) == 0);
}

static int fc_device_path_check(const char *path, char *out, size_t out_len)
{
  struct stat st;
  int saved_errno;

  if (!fc_device_path_ok(path))
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"path denied\"}");
      return -EACCES;
    }

  if (fc_device_path_known_block(path))
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"block device denied\"}");
      return -EACCES;
    }

  if (stat(path, &st) < 0)
    {
      saved_errno = errno;
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"stat failed\",\"errno\":%d}",
               saved_errno);
      return -saved_errno;
    }

  if (S_ISBLK(st.st_mode))
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"block device denied\"}");
      return -EACCES;
    }

  return 0;
}

static int cap_device_list(const fc_tool_context_t *ctx,
                           const char *args_json, char *out, size_t out_len)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_DEVICE_TOOL
  DIR *dir;
  struct dirent *ent;
  size_t off = 0;

  (void)ctx;
  (void)args_json;
  dir = opendir("/dev");
  if (dir == NULL)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"opendir /dev failed\"}");
      return -errno;
    }

  off += snprintf(out + off, out_len - off, "{\"ok\":true,\"devices\":[");
  while ((ent = readdir(dir)) != NULL)
    {
      char esc[96];
      int n;

      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        {
          continue;
        }

      fc_json_escape(ent->d_name, esc, sizeof(esc));
      n = snprintf(out + off, out_len - off, "%s\"%s\"",
                   off > strlen("{\"ok\":true,\"devices\":[") ? "," : "",
                   esc);
      if (n < 0 || off + n >= out_len)
        {
          closedir(dir);
          return -ENOSPC;
        }

      off += n;
    }

  closedir(dir);
  if (off + 3 >= out_len)
    {
      return -ENOSPC;
    }

  snprintf(out + off, out_len - off, "]}");
  return 0;
#else
  (void)ctx;
  (void)args_json;
  snprintf(out, out_len, "{\"ok\":false,\"error\":\"device tools disabled\"}");
  return -ENOSYS;
#endif
}

static int cap_device_read(const fc_tool_context_t *ctx,
                           const char *args_json, char *out, size_t out_len)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_DEVICE_TOOL
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  const char *path;
  cJSON *max_item;
  uint8_t *buf;
  char *hex;
  char *text;
  char *esc;
  size_t max_bytes = FC_DEVICE_MAX_IO;
  ssize_t nread;
  int fd;
  int ret = 0;
  size_t i;

  (void)ctx;
  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
  max_item = cJSON_GetObjectItem(root, "max_bytes");
  if (cJSON_IsNumber(max_item) && max_item->valuedouble > 0 &&
      max_item->valuedouble < FC_DEVICE_MAX_IO)
    {
      max_bytes = (size_t)max_item->valuedouble;
    }

  ret = fc_device_path_check(path, out, out_len);
  if (ret < 0)
    {
      cJSON_Delete(root);
      return ret;
    }

  fd = open(path, O_RDONLY | O_NONBLOCK);
  cJSON_Delete(root);
  if (fd < 0)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"open failed\",\"errno\":%d}",
               errno);
      return -errno;
    }

  buf = calloc(1, max_bytes);
  hex = calloc(1, max_bytes * 2 + 1);
  text = calloc(1, max_bytes + 1);
  esc = calloc(1, max_bytes * 6 + 1);
  if (buf == NULL || hex == NULL || text == NULL || esc == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  nread = read(fd, buf, max_bytes);
  if (nread < 0)
    {
      ret = -errno;
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"read failed\",\"errno\":%d}",
               errno);
      goto out;
    }

  for (i = 0; i < (size_t)nread; i++)
    {
      snprintf(hex + i * 2, 3, "%02x", buf[i]);
      text[i] = isprint(buf[i]) || isspace(buf[i]) ? (char)buf[i] : '.';
    }

  fc_json_escape(text, esc, max_bytes * 6 + 1);
  snprintf(out, out_len,
           "{\"ok\":true,\"bytes\":%ld,\"text\":\"%s\",\"hex\":\"%s\"}",
           (long)nread, esc, hex);

out:
  close(fd);
  free(buf);
  free(hex);
  free(text);
  free(esc);
  return ret;
#else
  (void)ctx;
  (void)args_json;
  snprintf(out, out_len, "{\"ok\":false,\"error\":\"device tools disabled\"}");
  return -ENOSYS;
#endif
}

static int fc_parse_hex(const char *hex, uint8_t *out, size_t out_len)
{
  size_t count = 0;
  int high = -1;

  if (hex == NULL || out == NULL)
    {
      return -EINVAL;
    }

  for (; *hex != '\0'; hex++)
    {
      int value;

      if (isspace((unsigned char)*hex) || *hex == ':' || *hex == ',')
        {
          continue;
        }

      if (*hex >= '0' && *hex <= '9')
        {
          value = *hex - '0';
        }
      else if (*hex >= 'a' && *hex <= 'f')
        {
          value = *hex - 'a' + 10;
        }
      else if (*hex >= 'A' && *hex <= 'F')
        {
          value = *hex - 'A' + 10;
        }
      else
        {
          return -EINVAL;
        }

      if (high < 0)
        {
          high = value;
        }
      else
        {
          if (count >= out_len)
            {
              return -ENOSPC;
            }

          out[count++] = (uint8_t)((high << 4) | value);
          high = -1;
        }
    }

  return high < 0 ? (int)count : -EINVAL;
}

static int cap_device_write(const fc_tool_context_t *ctx,
                            const char *args_json, char *out,
                            size_t out_len)
{
#ifdef CONFIG_FRUITCLAW_ENABLE_DEVICE_TOOL
  cJSON *root = cJSON_Parse(args_json ? args_json : "{}");
  const char *path;
  const char *mode;
  const char *data;
  uint8_t bytes[FC_DEVICE_MAX_IO];
  size_t len = 0;
  ssize_t nwritten;
  int fd;
  int guard_fd = -1;
  int ret;

  if (!fc_ctx_owner(ctx))
    {
      snprintf(out, out_len, "{\"ok\":false,\"error\":\"owner required\"}");
      return -EACCES;
    }

  if (root == NULL)
    {
      return fc_json_out_error(out, out_len, "invalid JSON");
    }

  path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
  mode = cJSON_GetStringValue(cJSON_GetObjectItem(root, "mode"));
  data = cJSON_GetStringValue(cJSON_GetObjectItem(root, "data"));
  if (mode == NULL)
    {
      mode = "text";
    }

  ret = fc_device_path_check(path, out, out_len);
  if (ret < 0 || data == NULL)
    {
      cJSON_Delete(root);
      if (data == NULL)
        {
          snprintf(out, out_len,
                   "{\"ok\":false,\"error\":\"path or data denied\"}");
          return -EACCES;
        }

      return ret;
    }

  if (strcmp(mode, "hex") == 0)
    {
      int parsed = fc_parse_hex(data, bytes, sizeof(bytes));
      if (parsed < 0)
        {
          cJSON_Delete(root);
          snprintf(out, out_len, "{\"ok\":false,\"error\":\"invalid hex\"}");
          return parsed;
        }

      len = (size_t)parsed;
    }
  else
    {
      len = strlen(data);
      if (len > sizeof(bytes))
        {
          cJSON_Delete(root);
          snprintf(out, out_len, "{\"ok\":false,\"error\":\"data too long\"}");
          return -ENOSPC;
        }

      memcpy(bytes, data, len);
    }

  ret = fc_tool_guard_arm(ctx, FC_GUARD_STAGE_DEVICE, &guard_fd);
  if (ret < 0)
    {
      cJSON_Delete(root);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"device guard unavailable\","
               "\"code\":%d}", ret);
      return ret;
    }

  fd = open(path, O_WRONLY | O_NONBLOCK);
  cJSON_Delete(root);
  if (fd < 0)
    {
      fc_guard_disarm(guard_fd);
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"open failed\",\"errno\":%d}",
               errno);
      return -errno;
    }

  nwritten = write(fd, bytes, len);
  close(fd);
  fc_guard_disarm(guard_fd);
  if (nwritten < 0)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"write failed\",\"errno\":%d}",
               errno);
      return -errno;
    }

  snprintf(out, out_len, "{\"ok\":true,\"bytes\":%ld}", (long)nwritten);
  return (size_t)nwritten == len ? 0 : -EIO;
#else
  (void)ctx;
  (void)args_json;
  snprintf(out, out_len, "{\"ok\":false,\"error\":\"device tools disabled\"}");
  return -ENOSYS;
#endif
}

static int cap_gpio_stub(const fc_tool_context_t *ctx, const char *args_json,
                         char *out, size_t out_len)
{
  (void)ctx;
  (void)args_json;
  snprintf(out, out_len,
           "{\"ok\":false,\"error\":\"GPIO tool not configured\"}");
  return -ENOSYS;
}

static const fc_cap_t g_caps[] =
{
  {
    "time.now", "Current time",
    "Return current device time.",
    "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
    true, false, cap_time_now
  },
  {
    "system.info", "System info",
    "Return non-secret FruitClaw and board information.",
    "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
    true, false, cap_system_info
  },
  {
    "system.status", "System status",
    "Return non-secret FruitClaw runtime health counters without running a "
    "terminal command.",
    "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
    true, false, cap_system_status
  },
  {
    "service.status", "Service status",
    "Return Telnet, FTP, and webserver/MCP service state, autostart flags, "
    "counters, and known lifecycle limitations.",
    "{\"type\":\"object\",\"properties\":{\"service\":{\"type\":\"string\","
    "\"enum\":[\"all\",\"telnetd\",\"ftpd\",\"webserver\",\"http\","
    "\"httpd\",\"mcp\"]}},"
    "\"additionalProperties\":false}",
    true, false, cap_service_status
  },
  {
    "service.control", "Control service",
    "Start, stop, restart, enable, disable, or query Telnet/FTP services and "
    "restart/query the always-on webserver/MCP supervisor. Telnet stop/restart "
    "uses the local telnetd PID file; FTP uses ftpd_stop.",
    "{\"type\":\"object\",\"properties\":{\"service\":{\"type\":\"string\","
    "\"enum\":[\"telnetd\",\"ftpd\",\"webserver\",\"http\",\"httpd\","
    "\"mcp\"]},\"action\":{\"type\":\"string\","
    "\"enum\":[\"status\",\"start\",\"stop\",\"restart\",\"enable\","
    "\"disable\"]}},"
    "\"required\":[\"service\",\"action\"],\"additionalProperties\":false}",
    true, true, cap_service_control
  },
  {
    "memory.append", "Append memory",
    "Append a short memory note to FruitClaw memory.",
    "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},"
    "\"required\":[\"text\"],\"additionalProperties\":false}",
    true, false, cap_memory_append
  },
  {
    "memory.read", "Read memory",
    "Read a bounded tail from FruitClaw memory.",
    "{\"type\":\"object\",\"properties\":{\"max_bytes\":{\"type\":\"integer\"},"
    "\"include_selftest\":{\"type\":\"boolean\"}},"
    "\"additionalProperties\":false}",
    true, false, cap_memory_read
  },
  {
    "file.read", "Read data file",
    "Read a bounded file under the FruitClaw data directory.",
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"max_bytes\":{\"type\":\"integer\"}},\"required\":[\"path\"],"
    "\"additionalProperties\":false}",
    true, false, cap_file_read
  },
  {
    "file.write_limited", "Write limited data file",
    "Write a file under scripts/ or notes/ in the FruitClaw data directory.",
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"text\":{\"type\":\"string\"}},\"required\":[\"path\",\"text\"],"
    "\"additionalProperties\":false}",
    true, true, cap_file_write_limited
  },
  {
    "web.home.read", "Read web home page",
    "Read the Markdown body rendered by the root web page. Custom content "
    "lives at www/home.md under the active FruitClaw data root.",
    "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
    true, false, cap_web_home_read
  },
  {
    "web.home.write", "Write web home page",
    "Replace the Markdown body rendered by the root web page and served "
    "through /site/home.md. The static root shell links to /docs/index.html "
    "for the "
    "manual.",
    "{\"type\":\"object\",\"properties\":{\"markdown\":{\"type\":\"string\"},"
    "\"text\":{\"type\":\"string\"}},\"additionalProperties\":false}",
    true, true, cap_web_home_write
  },
  {
    "script.list", "List scripts",
    "List generated scripts under scripts/generated/ and uploaded Berry/NSH "
    "scripts under scripts/. Items include source, descriptions, validation "
    "status, and schedule references when present.",
    "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
    true, false, cap_script_list
  },
  {
    "script.read", "Read script",
    "Read a generated Berry or NSH script from scripts/generated/, or a "
    "root-level FTP-uploaded script from scripts/, so the owner or agent can "
    "inspect and rework it.",
    "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},"
    "\"path\":{\"type\":\"string\"},\"kind\":{\"type\":\"string\","
    "\"enum\":[\"auto\",\"berry\",\"be\",\"shell\",\"nsh\",\"sh\"]}},"
    "\"additionalProperties\":false}",
    true, false, cap_script_read
  },
  {
    "script.write", "Write generated script",
    "Create or replace a generated Berry .be or NSH .nsh script under "
    "scripts/generated/. By default the script is validated by running it "
    "once through the guarded runner; use validate_mode=syntax for "
    "long-running Berry UI scripts. Validation metadata is saved for "
    "script.list and script.read.",
    "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},"
    "\"path\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"},"
    "\"kind\":{\"type\":\"string\",\"enum\":[\"auto\",\"berry\",\"be\","
    "\"shell\",\"nsh\",\"sh\"]},\"text\":{\"type\":\"string\"},"
    "\"validate\":{\"type\":\"boolean\"},\"validate_mode\":{\"type\":"
    "\"string\",\"enum\":[\"run\",\"syntax\"]}},"
    "\"required\":[\"text\"],\"additionalProperties\":false}",
    true, true, cap_script_write
  },
  {
    "script.validate", "Validate script",
    "Validate a generated Berry/NSH script or a root-level FTP-uploaded "
    "script. Default mode runs it once through the guarded runner; "
    "mode=syntax checks Berry syntax without executing the script.",
    "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},"
    "\"path\":{\"type\":\"string\"},\"kind\":{\"type\":\"string\","
    "\"enum\":[\"auto\",\"berry\",\"be\",\"shell\",\"nsh\",\"sh\"]},"
    "\"mode\":{\"type\":\"string\",\"enum\":[\"run\",\"syntax\"]},"
    "\"validate_mode\":{\"type\":\"string\",\"enum\":[\"run\",\"syntax\"]}},"
    "\"additionalProperties\":false}",
    true, true, cap_script_validate
  },
  {
    "script.remove", "Remove generated script",
    "Remove a generated Berry or NSH script from scripts/generated/ after "
    "the owner confirms it is no longer wanted.",
    "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},"
    "\"path\":{\"type\":\"string\"},\"kind\":{\"type\":\"string\","
    "\"enum\":[\"auto\",\"berry\",\"be\",\"shell\",\"nsh\",\"sh\"]}},"
    "\"additionalProperties\":false}",
    true, true, cap_script_remove
  },
  {
    "script.run", "Run script",
    "Run a generated Berry/NSH script under scripts/generated/ or a "
    "root-level FTP-uploaded script under scripts/ through the guarded "
    "runner.",
    "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},"
    "\"path\":{\"type\":\"string\"},\"kind\":{\"type\":\"string\","
    "\"enum\":[\"auto\",\"berry\",\"be\",\"shell\",\"nsh\",\"sh\"]},"
    "\"args_json\":{\"type\":\"string\"}},\"additionalProperties\":false}",
    true, true, cap_script_run
  },
  {
    "script.schedule", "Schedule script",
    "Schedule a generated Berry/NSH script or root-level FTP-uploaded script "
    "to run at boot, once, on an interval, or from a 5-field cron expression. "
    "The scheduler fires script.run directly without needing the LLM.",
    "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},"
    "\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},"
    "\"kind\":{\"type\":\"string\",\"enum\":[\"auto\",\"berry\",\"be\","
    "\"shell\",\"nsh\",\"sh\"]},"
    "\"type\":{\"type\":\"string\",\"enum\":[\"interval\",\"once\",\"cron\","
    "\"boot\"]},"
    "\"every_sec\":{\"type\":\"integer\"},\"at_epoch\":{\"type\":\"integer\"},"
    "\"after_sec\":{\"type\":\"integer\"},\"expr\":{\"type\":\"string\"},"
    "\"args_json\":{\"type\":\"string\"}},"
    "\"required\":[\"type\"],\"additionalProperties\":false}",
    true, true, cap_script_schedule
  },
  {
    "telegram.send_message", "Send Telegram message",
    "Send a plain text Telegram message to an allowed chat. If chat_id is "
    "omitted, use the current Telegram chat.",
    "{\"type\":\"object\",\"properties\":{\"chat_id\":{\"type\":\"string\"},"
    "\"text\":{\"type\":\"string\"}},\"required\":[\"text\"],"
    "\"additionalProperties\":false}",
    true, false, cap_telegram_send
  },
  {
    "scheduler.add", "Add schedule",
    "Add a boot, interval, once, or cron schedule. Schedules inherit the "
    "current owner chat and session. Plain prompts are delivered directly at "
    "fire time; prefix prompt with agent: to run the full LLM/tool loop.",
    "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},"
    "\"type\":{\"type\":\"string\",\"enum\":[\"interval\",\"once\",\"cron\","
    "\"boot\"]},"
    "\"every_sec\":{\"type\":\"integer\"},\"at_epoch\":{\"type\":\"integer\"},"
    "\"after_sec\":{\"type\":\"integer\"},"
    "\"expr\":{\"type\":\"string\"},\"prompt\":{\"type\":\"string\"}},"
    "\"required\":[\"type\",\"prompt\"],\"additionalProperties\":false}",
    true, true, cap_scheduler_add
  },
  {
    "scheduler.list", "List schedules",
    "List configured FruitClaw schedules.",
    "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
    true, false, cap_scheduler_list
  },
  {
    "scheduler.remove", "Remove schedule",
    "Remove a configured schedule by id.",
    "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},"
    "\"required\":[\"id\"],\"additionalProperties\":false}",
    true, true, cap_scheduler_remove
  },
  {
    "berry.run_script", "Run Berry script",
    "Run a Berry script under scripts/ with JSON args. The script can use "
    "the constrained claw module.",
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"args_json\":{\"type\":\"string\"}},\"required\":[\"path\"],"
    "\"additionalProperties\":false}",
    true, true, cap_berry_run_script
  },
  {
    "terminal.run", "Run terminal command",
    "Run a bounded NuttX/NSH command as the device owner and return captured "
    "output.",
    "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
    "\"required\":[\"command\"],\"additionalProperties\":false}",
    true, true, fc_builtin_terminal_run
  },
  {
    "rtttl.play", "Play RTTTL tune",
    "Play a built-in tune name or bounded RTTTL string through the installed "
    "NuttX rtttl app. Built-ins include gta3, scratchy, simpsons, and "
    "cantina.",
    "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},"
    "\"tune\":{\"type\":\"string\"},\"volume\":{\"type\":\"integer\"}},"
    "\"additionalProperties\":false}",
    true, true, cap_rtttl_play
  },
  {
    "neopixels.set", "Set NeoPixels",
    "Set /dev/leds0 to a color or run an effect. Effects: off, fill, "
    "rainbow, chase, pulse.",
    "{\"type\":\"object\",\"properties\":{\"effect\":{\"type\":\"string\"},"
    "\"color\":{\"type\":\"string\"},\"r\":{\"type\":\"integer\"},"
    "\"g\":{\"type\":\"integer\"},\"b\":{\"type\":\"integer\"},"
    "\"brightness\":{\"type\":\"integer\"},\"cycles\":{\"type\":\"integer\"},"
    "\"delay_ms\":{\"type\":\"integer\"},\"duration_ms\":{\"type\":\"integer\"}},"
    "\"additionalProperties\":false}",
    true, true, fc_builtin_neopixels_set
  },
  {
    "neopixels.off", "NeoPixels off",
    "Turn all /dev/leds0 NeoPixels off.",
    "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
    true, true, cap_neopixels_off
  },
  {
    "neopixels.effect", "NeoPixel effect",
    "Run a NeoPixel rainbow, chase, or pulse effect on /dev/leds0.",
    "{\"type\":\"object\",\"properties\":{\"effect\":{\"type\":\"string\"},"
    "\"color\":{\"type\":\"string\"},\"r\":{\"type\":\"integer\"},"
    "\"g\":{\"type\":\"integer\"},\"b\":{\"type\":\"integer\"},"
    "\"brightness\":{\"type\":\"integer\"},\"cycles\":{\"type\":\"integer\"},"
    "\"delay_ms\":{\"type\":\"integer\"},\"duration_ms\":{\"type\":\"integer\"}},"
    "\"required\":[\"effect\"],\"additionalProperties\":false}",
    true, true, fc_builtin_neopixels_set
  },
  {
    "device.list", "List /dev",
    "List registered NuttX device nodes under /dev.",
    "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
    true, false, cap_device_list
  },
  {
    "device.read", "Read character /dev node",
    "Read bounded bytes from a non-block /dev path and return text plus hex.",
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"max_bytes\":{\"type\":\"integer\"}},\"required\":[\"path\"],"
    "\"additionalProperties\":false}",
    true, true, cap_device_read
  },
  {
    "device.write", "Write character /dev node",
    "Write bounded text or hex bytes to a non-block /dev path.",
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"mode\":{\"type\":\"string\",\"enum\":[\"text\",\"hex\"]},"
    "\"data\":{\"type\":\"string\"}},\"required\":[\"path\",\"data\"],"
    "\"additionalProperties\":false}",
    true, true, cap_device_write
  },
  {
    "http.request", "HTTP request",
    "Run an allowlisted HTTP GET or POST request.",
    "{\"type\":\"object\",\"properties\":{\"method\":{\"type\":\"string\"},"
    "\"url\":{\"type\":\"string\"},\"body\":{\"type\":\"string\"}},"
    "\"required\":[\"url\"],\"additionalProperties\":false}",
    true, true, cap_http_request
  },
  {
    "gpio.read", "Read GPIO", "GPIO read stub.",
    "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"}},"
    "\"required\":[\"pin\"],\"additionalProperties\":false}",
    false, false, cap_gpio_stub
  },
  {
    "gpio.write", "Write GPIO", "GPIO write stub.",
    "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"},"
    "\"value\":{\"type\":\"integer\"}},\"required\":[\"pin\",\"value\"],"
    "\"additionalProperties\":false}",
    false, true, cap_gpio_stub
  },
#ifdef CONFIG_FRUITCLAW_ENABLE_SHELL_TOOL
  {
    "shell.safe_command", "Safe shell command",
    "Legacy alias for terminal.run.",
    "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
    "\"required\":[\"command\"],\"additionalProperties\":false}",
    false, true, fc_builtin_terminal_run
  },
#endif
};

int fc_register_builtin_caps(void)
{
  unsigned int i;
  int ret;

  for (i = 0; i < sizeof(g_caps) / sizeof(g_caps[0]); i++)
    {
      ret = fc_cap_register(&g_caps[i]);
      if (ret < 0)
        {
          return ret;
        }
    }

  return 0;
}
