/* SPDX-License-Identifier: Apache-2.0 */

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "berry.h"
#include "berry_runner.h"
#include "be_module.h"
#include "be_string.h"

#define BERRY_CLAW_REPLY_MAX 2048
#define BERRY_CLAW_TOOL_MAX  2048
#define BERRY_CLAW_SOURCE_MAX 8192

#ifdef CONFIG_FRUITCLAW_BERRY_TRACE
#  define BERRY_CLAW_TRACE(fmt, ...) \
  printf("berry-claw: " fmt "\n", ##__VA_ARGS__)
#else
#  define BERRY_CLAW_TRACE(fmt, ...) \
  do { } while (0)
#endif

struct berry_claw_state_s
{
  const char *args_json;
  const struct berry_claw_host_s *host;
  char reply[BERRY_CLAW_REPLY_MAX];
  size_t reply_off;
};

static pthread_mutex_t g_runner_lock = PTHREAD_MUTEX_INITIALIZER;
static struct berry_claw_state_s *g_state;

static int berry_claw_json_escape(const char *in, char *out, size_t out_len)
{
  size_t off = 0;

  if (in == NULL || out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  for (; *in != '\0'; in++)
    {
      unsigned char ch = (unsigned char)*in;
      const char *esc = NULL;
      char ubuf[7];

      switch (ch)
        {
          case '"':
            esc = "\\\"";
            break;
          case '\\':
            esc = "\\\\";
            break;
          case '\n':
            esc = "\\n";
            break;
          case '\r':
            esc = "\\r";
            break;
          case '\t':
            esc = "\\t";
            break;
          default:
            if (ch < 0x20)
              {
                snprintf(ubuf, sizeof(ubuf), "\\u%04x", ch);
                esc = ubuf;
              }
            break;
        }

      if (esc != NULL)
        {
          size_t elen = strlen(esc);
          if (off + elen >= out_len)
            {
              return -ENOSPC;
            }

          memcpy(out + off, esc, elen);
          off += elen;
        }
      else
        {
          if (off + 1 >= out_len)
            {
              return -ENOSPC;
            }

          out[off++] = ch;
        }
    }

  out[off] = '\0';
  return 0;
}

static int berry_claw_append_reply(const char *text)
{
  size_t len;

  if (g_state == NULL || text == NULL)
    {
      return -EINVAL;
    }

  len = strlen(text);
  if (g_state->reply_off + len + 2 >= sizeof(g_state->reply))
    {
      return -ENOSPC;
    }

  if (g_state->reply_off > 0)
    {
      g_state->reply[g_state->reply_off++] = '\n';
    }

  memcpy(g_state->reply + g_state->reply_off, text, len);
  g_state->reply_off += len;
  g_state->reply[g_state->reply_off] = '\0';
  return 0;
}

static int berry_claw_call_tool(const char *name, const char *args_json,
                                char *out, size_t out_len)
{
  if (g_state == NULL || g_state->host == NULL ||
      g_state->host->call_tool == NULL)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"FruitClaw host unavailable\"}");
      return -ENOSYS;
    }

  return g_state->host->call_tool(g_state->host->ctx, name,
                                  args_json ? args_json : "{}",
                                  out, out_len);
}

static int berry_claw_args(bvm *vm)
{
  be_pushstring(vm, g_state && g_state->args_json ?
                g_state->args_json : "{}");
  be_return(vm);
}

static int berry_claw_reply(bvm *vm)
{
  const char *text = be_top(vm) >= 1 ? be_tostring(vm, 1) : "";
  int ret = berry_claw_append_reply(text);

  be_pushbool(vm, ret == 0);
  be_return(vm);
}

static int berry_claw_tool(bvm *vm)
{
  const char *name = be_top(vm) >= 1 ? be_tostring(vm, 1) : NULL;
  const char *args_json = be_top(vm) >= 2 ? be_tostring(vm, 2) : "{}";
  char result[BERRY_CLAW_TOOL_MAX];

  if (name == NULL || name[0] == '\0')
    {
      be_pushstring(vm, "{\"ok\":false,\"error\":\"missing tool name\"}");
      be_return(vm);
    }

  berry_claw_call_tool(name, args_json, result, sizeof(result));
  be_pushstring(vm, result);
  be_return(vm);
}

static int berry_claw_call_text_tool(bvm *vm, const char *tool,
                                     const char *field)
{
  const char *value = be_top(vm) >= 1 ? be_tostring(vm, 1) : "";
  char esc[1024];
  char args[1200];
  char result[BERRY_CLAW_TOOL_MAX];

  berry_claw_json_escape(value, esc, sizeof(esc));
  snprintf(args, sizeof(args), "{\"%s\":\"%s\"}", field, esc);
  berry_claw_call_tool(tool, args, result, sizeof(result));
  be_pushstring(vm, result);
  be_return(vm);
}

static int berry_claw_memory_append(bvm *vm)
{
  return berry_claw_call_text_tool(vm, "memory.append", "text");
}

static int berry_claw_terminal_run(bvm *vm)
{
  return berry_claw_call_text_tool(vm, "terminal.run", "command");
}

static int berry_claw_neopixels_set(bvm *vm)
{
  const char *arg = be_top(vm) >= 1 ? be_tostring(vm, 1) : "{}";
  char esc[128];
  char args[180];
  char result[BERRY_CLAW_TOOL_MAX];

  if (arg[0] == '{')
    {
      berry_claw_call_tool("neopixels.set", arg, result, sizeof(result));
    }
  else
    {
      berry_claw_json_escape(arg, esc, sizeof(esc));
      snprintf(args, sizeof(args), "{\"effect\":\"fill\",\"color\":\"%s\"}",
               esc);
      berry_claw_call_tool("neopixels.set", args, result, sizeof(result));
    }

  be_pushstring(vm, result);
  be_return(vm);
}

static int berry_claw_neopixels_off(bvm *vm)
{
  char result[BERRY_CLAW_TOOL_MAX];

  berry_claw_call_tool("neopixels.off", "{}", result, sizeof(result));
  be_pushstring(vm, result);
  be_return(vm);
}

static int berry_claw_neopixels_effect(bvm *vm)
{
  const char *effect = be_top(vm) >= 1 ? be_tostring(vm, 1) : "rainbow";
  const char *color = be_top(vm) >= 2 ? be_tostring(vm, 2) : "";
  char effect_esc[128];
  char color_esc[128];
  char args[320];
  char result[BERRY_CLAW_TOOL_MAX];

  if (effect[0] == '{')
    {
      berry_claw_call_tool("neopixels.effect", effect, result,
                           sizeof(result));
    }
  else
    {
      berry_claw_json_escape(effect, effect_esc, sizeof(effect_esc));
      berry_claw_json_escape(color, color_esc, sizeof(color_esc));
      if (color_esc[0] != '\0')
        {
          snprintf(args, sizeof(args),
                   "{\"effect\":\"%s\",\"color\":\"%s\"}",
                   effect_esc, color_esc);
        }
      else
        {
          snprintf(args, sizeof(args), "{\"effect\":\"%s\"}", effect_esc);
        }

      berry_claw_call_tool("neopixels.effect", args, result,
                           sizeof(result));
    }

  be_pushstring(vm, result);
  be_return(vm);
}

static int berry_claw_schedule_add(bvm *vm)
{
  const char *args_json = be_top(vm) >= 1 ? be_tostring(vm, 1) : "{}";
  char result[BERRY_CLAW_TOOL_MAX];

  berry_claw_call_tool("scheduler.add", args_json, result, sizeof(result));
  be_pushstring(vm, result);
  be_return(vm);
}

static int berry_claw_script_run(bvm *vm)
{
  const char *args_json = be_top(vm) >= 1 ? be_tostring(vm, 1) : "{}";
  char result[BERRY_CLAW_TOOL_MAX];

  berry_claw_call_tool("script.run", args_json, result, sizeof(result));
  be_pushstring(vm, result);
  be_return(vm);
}

static int berry_claw_rtttl_play(bvm *vm)
{
  const char *arg = be_top(vm) >= 1 ? be_tostring(vm, 1) : "";
  char esc[768];
  char args[900];
  char result[BERRY_CLAW_TOOL_MAX];

  if (arg[0] == '{')
    {
      berry_claw_call_tool("rtttl.play", arg, result, sizeof(result));
    }
  else
    {
      berry_claw_json_escape(arg, esc, sizeof(esc));
      snprintf(args, sizeof(args), "{\"name\":\"%s\"}", esc);
      berry_claw_call_tool("rtttl.play", args, result, sizeof(result));
    }

  be_pushstring(vm, result);
  be_return(vm);
}

static int berry_claw_service_control(bvm *vm)
{
  const char *service = be_top(vm) >= 1 ? be_tostring(vm, 1) : "";
  const char *action = be_top(vm) >= 2 ? be_tostring(vm, 2) : "status";
  char service_esc[64];
  char action_esc[64];
  char args[180];
  char result[BERRY_CLAW_TOOL_MAX];

  berry_claw_json_escape(service, service_esc, sizeof(service_esc));
  berry_claw_json_escape(action, action_esc, sizeof(action_esc));
  snprintf(args, sizeof(args),
           "{\"service\":\"%s\",\"action\":\"%s\"}",
           service_esc, action_esc);
  berry_claw_call_tool("service.control", args, result, sizeof(result));
  be_pushstring(vm, result);
  be_return(vm);
}

static int berry_claw_telegram_send(bvm *vm)
{
  return berry_claw_call_text_tool(vm, "telegram.send_message", "text");
}

static void berry_claw_set_func(bvm *vm, const char *name, bntvfunc fn)
{
  BERRY_CLAW_TRACE("bind %s", name);
  be_pushntvfunction(vm, fn);
  be_setmember(vm, -2, name);
  be_pop(vm, 1);
}

static void berry_claw_register(bvm *vm)
{
  BERRY_CLAW_TRACE("register begin");
  be_newmodule(vm);
  be_setname(vm, -1, "claw");
  berry_claw_set_func(vm, "args", berry_claw_args);
  berry_claw_set_func(vm, "reply", berry_claw_reply);
  berry_claw_set_func(vm, "tool", berry_claw_tool);
  berry_claw_set_func(vm, "memory_append", berry_claw_memory_append);
  berry_claw_set_func(vm, "terminal_run", berry_claw_terminal_run);
  berry_claw_set_func(vm, "neopixels_set", berry_claw_neopixels_set);
  berry_claw_set_func(vm, "neopixels_off", berry_claw_neopixels_off);
  berry_claw_set_func(vm, "neopixels_effect", berry_claw_neopixels_effect);
  berry_claw_set_func(vm, "schedule_add", berry_claw_schedule_add);
  berry_claw_set_func(vm, "script_run", berry_claw_script_run);
  berry_claw_set_func(vm, "rtttl_play", berry_claw_rtttl_play);
  berry_claw_set_func(vm, "service_control", berry_claw_service_control);
  berry_claw_set_func(vm, "telegram_send", berry_claw_telegram_send);
  be_cache_module(vm, be_newstr(vm, "claw"));
  BERRY_CLAW_TRACE("set global");
  be_setglobal(vm, "claw");
  be_pop(vm, 1);
  BERRY_CLAW_TRACE("register done");
}

static int berry_claw_read_source(const char *path, char *buf,
                                  size_t buf_len)
{
  int fd;
  size_t off = 0;

  if (path == NULL || buf == NULL || buf_len < 2)
    {
      return -EINVAL;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  for (; ; )
    {
      ssize_t nread;

      if (off >= buf_len - 1)
        {
          close(fd);
          return -EFBIG;
        }

      nread = read(fd, buf + off, buf_len - 1 - off);
      if (nread < 0)
        {
          int err = -errno;
          close(fd);
          return err;
        }

      if (nread == 0)
        {
          break;
        }

      off += nread;
    }

  close(fd);
  buf[off] = '\0';
  return 0;
}

int berry_run_file_with_claw(const char *path, const char *args_json,
                             const struct berry_claw_host_s *host,
                             char *out, size_t out_len)
{
  struct berry_claw_state_s state;
  bvm *vm;
  int res;
  int ret = 0;
  char esc[BERRY_CLAW_REPLY_MAX * 6 + 1];
  char *source;

  if (path == NULL || out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_runner_lock);
  BERRY_CLAW_TRACE("start path=%s", path);
  memset(&state, 0, sizeof(state));
  state.args_json = args_json ? args_json : "{}";
  state.host = host;
  g_state = &state;

  source = malloc(BERRY_CLAW_SOURCE_MAX);
  if (source == NULL)
    {
      g_state = NULL;
      pthread_mutex_unlock(&g_runner_lock);
      return -ENOMEM;
    }

  BERRY_CLAW_TRACE("read source");
  res = berry_claw_read_source(path, source, BERRY_CLAW_SOURCE_MAX);
  BERRY_CLAW_TRACE("read res=%d", res);
  if (res < 0)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"script read failed\",\"code\":%d}",
               res);
      free(source);
      g_state = NULL;
      pthread_mutex_unlock(&g_runner_lock);
      return res;
    }

  BERRY_CLAW_TRACE("vm new");
  vm = be_vm_new();
  if (vm == NULL)
    {
      free(source);
      g_state = NULL;
      pthread_mutex_unlock(&g_runner_lock);
      return -ENOMEM;
    }

  BERRY_CLAW_TRACE("vm ok");
  berry_claw_register(vm);
  BERRY_CLAW_TRACE("loadstring");
  res = be_loadstring(vm, source);
  BERRY_CLAW_TRACE("loadstring res=%d", res);
  free(source);
  if (res == BE_OK)
    {
      BERRY_CLAW_TRACE("pcall");
      res = be_pcall(vm, 0);
      BERRY_CLAW_TRACE("pcall res=%d", res);
    }

  if (res == BE_OK)
    {
      berry_claw_json_escape(state.reply, esc, sizeof(esc));
      snprintf(out, out_len, "{\"ok\":true,\"reply\":\"%s\"}", esc);
    }
  else
    {
      const char *msg = be_top(vm) > 0 ? be_tostring(vm, -1) : "Berry error";
      berry_claw_json_escape(msg ? msg : "Berry error", esc, sizeof(esc));
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"%s\",\"code\":%d}", esc, res);
      ret = -EIO;
    }

  BERRY_CLAW_TRACE("vm delete");
  be_vm_delete(vm);
  g_state = NULL;
  pthread_mutex_unlock(&g_runner_lock);
  BERRY_CLAW_TRACE("done ret=%d", ret);
  return ret;
}

int berry_check_file(const char *path, char *out, size_t out_len)
{
  bvm *vm;
  char *source;
  char esc[BERRY_CLAW_REPLY_MAX * 6 + 1];
  int res;
  int ret = 0;

  if (path == NULL || out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_runner_lock);
  source = malloc(BERRY_CLAW_SOURCE_MAX);
  if (source == NULL)
    {
      pthread_mutex_unlock(&g_runner_lock);
      return -ENOMEM;
    }

  res = berry_claw_read_source(path, source, BERRY_CLAW_SOURCE_MAX);
  if (res < 0)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"mode\":\"syntax\","
               "\"error\":\"script read failed\",\"code\":%d}", res);
      free(source);
      pthread_mutex_unlock(&g_runner_lock);
      return res;
    }

  vm = be_vm_new();
  if (vm == NULL)
    {
      free(source);
      pthread_mutex_unlock(&g_runner_lock);
      return -ENOMEM;
    }

  res = be_loadstring(vm, source);
  free(source);
  if (res == BE_OK)
    {
      snprintf(out, out_len, "{\"ok\":true,\"mode\":\"syntax\"}");
    }
  else
    {
      const char *msg = be_top(vm) > 0 ? be_tostring(vm, -1) :
                        "Berry syntax error";

      berry_claw_json_escape(msg ? msg : "Berry syntax error",
                             esc, sizeof(esc));
      snprintf(out, out_len,
               "{\"ok\":false,\"mode\":\"syntax\","
               "\"error\":\"%s\",\"code\":%d}", esc, res);
      ret = -EIO;
    }

  be_vm_delete(vm);
  pthread_mutex_unlock(&g_runner_lock);
  return ret;
}
