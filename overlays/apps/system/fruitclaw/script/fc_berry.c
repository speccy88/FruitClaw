/* SPDX-License-Identifier: Apache-2.0 */

#include "fruitclaw.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_FRUITCLAW_ENABLE_BERRY) && \
    defined(CONFIG_FRUITCLAW_BERRY_EXPERIMENTAL_RUNNER)
#  include "berry_runner.h"
#endif

static pthread_mutex_t g_berry_status_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_berry_call_count;
static unsigned long g_berry_fail_count;
static int64_t g_berry_last_start_ms;
static int64_t g_berry_last_done_ms;
static int g_berry_last_ret;
static char g_berry_last_path[FC_PATH_LEN];

static void fc_berry_record_start(const char *path)
{
  pthread_mutex_lock(&g_berry_status_lock);
  g_berry_call_count++;
  g_berry_last_start_ms = fc_mono_ms();
  fc_strlcpy(g_berry_last_path, path ? path : "-", sizeof(g_berry_last_path));
  pthread_mutex_unlock(&g_berry_status_lock);
}

static int fc_berry_record_result(int ret)
{
  pthread_mutex_lock(&g_berry_status_lock);
  g_berry_last_done_ms = fc_mono_ms();
  g_berry_last_ret = ret;
  if (ret < 0)
    {
      g_berry_fail_count++;
    }

  pthread_mutex_unlock(&g_berry_status_lock);
  return ret;
}

void fc_berry_clear_status(void)
{
  pthread_mutex_lock(&g_berry_status_lock);
  g_berry_call_count = 0;
  g_berry_fail_count = 0;
  g_berry_last_start_ms = 0;
  g_berry_last_done_ms = 0;
  g_berry_last_ret = 0;
  g_berry_last_path[0] = '\0';
  pthread_mutex_unlock(&g_berry_status_lock);
}

static int fc_berry_resolve_path(const char *path, char *full,
                                 size_t full_len)
{
  char scripts_root[FC_PATH_LEN];

  if (path == NULL || path[0] == '\0' || fc_path_has_parent_ref(path) ||
      full == NULL || full_len == 0)
    {
      return -EINVAL;
    }

  if (fc_data_path("scripts", scripts_root, sizeof(scripts_root)) < 0)
    {
      return -EINVAL;
    }

  if (path[0] == '/')
    {
      size_t root_len = strlen(scripts_root);

      if (strncmp(path, scripts_root, root_len) != 0 ||
          (path[root_len] != '\0' && path[root_len] != '/'))
        {
          return -EACCES;
        }

      return fc_strlcpy(full, path, full_len);
    }

  if (strncmp(path, "scripts/", 8) == 0)
    {
      return fc_data_path(path, full, full_len);
    }

  if (snprintf(full, full_len, "%s/%s", scripts_root, path) >=
      (int)full_len)
    {
      return -ENAMETOOLONG;
    }

  return 0;
}

#if defined(CONFIG_FRUITCLAW_ENABLE_BERRY) && \
    defined(CONFIG_FRUITCLAW_BERRY_EXPERIMENTAL_RUNNER)
static int fc_berry_host_call(void *opaque, const char *name,
                              const char *args_json, char *out,
                              size_t out_len)
{
  const fc_tool_context_t *ctx = opaque;

  return fc_cap_execute_ctx(ctx, name, args_json ? args_json : "{}",
                            out, out_len);
}

struct fc_berry_job_s
{
  const char *path;
  const char *args_json;
  const struct berry_claw_host_s *host;
  char *out;
  size_t out_len;
  int ret;
};

static void *fc_berry_thread_main(void *arg)
{
  struct fc_berry_job_s *job = arg;

  job->ret = berry_run_file_with_claw(job->path, job->args_json,
                                      job->host, job->out, job->out_len);
  return NULL;
}

static int fc_berry_run_worker(const char *path, const char *args_json,
                               const struct berry_claw_host_s *host,
                               char *out, size_t out_len)
{
  struct fc_berry_job_s job;
  pthread_attr_t attr;
  pthread_t thread;
  int ret;

  memset(&job, 0, sizeof(job));
  job.path = path;
  job.args_json = args_json;
  job.host = host;
  job.out = out;
  job.out_len = out_len;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_INTERPRETERS_BERRY_STACKSIZE);
  ret = pthread_create(&thread, &attr, fc_berry_thread_main, &job);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"Berry worker start failed\","
               "\"code\":%d}", -ret);
      return -ret;
    }

  pthread_join(thread, NULL);
  return job.ret;
}
#endif

int fc_berry_run_file(const fc_tool_context_t *ctx, const char *path,
                      const char *args_json, char *out, size_t out_len)
{
  char full[FC_PATH_LEN];
  int ret;

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  fc_berry_record_start(path);
#if defined(CONFIG_FRUITCLAW_ENABLE_BERRY) && \
    defined(CONFIG_FRUITCLAW_BERRY_EXPERIMENTAL_RUNNER)
  struct berry_claw_host_s host;
  fc_tool_context_t host_ctx;
  int guard_fd = -1;

  ret = fc_berry_resolve_path(path, full, sizeof(full));
  if (ret < 0)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"script path denied\"}");
      return fc_berry_record_result(ret);
    }

  memset(&host, 0, sizeof(host));
  memset(&host_ctx, 0, sizeof(host_ctx));
  if (ctx != NULL)
    {
      host_ctx = *ctx;
    }

  host_ctx.guarded = true;
  host.ctx = (void *)&host_ctx;
  host.call_tool = fc_berry_host_call;
  ret = fc_guard_arm(FC_GUARD_STAGE_BERRY, &guard_fd);
  if (ret < 0)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"Berry guard unavailable\","
               "\"code\":%d}", ret);
      return fc_berry_record_result(ret);
    }

  ret = fc_berry_run_worker(full, args_json ? args_json : "{}",
                            &host, out, out_len);
  fc_guard_disarm(guard_fd);
  return fc_berry_record_result(ret);
#elif defined(CONFIG_FRUITCLAW_ENABLE_BERRY)
  (void)ctx;
  (void)args_json;

  ret = fc_berry_resolve_path(path, full, sizeof(full));
  if (ret < 0)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"error\":\"script path denied\"}");
      return fc_berry_record_result(ret);
    }

  snprintf(out, out_len,
           "{\"ok\":false,\"error\":\"Berry runner integration disabled\","
           "\"detail\":\"direct VM wrapper is blocked after watchdog hang; "
           "enable CONFIG_FRUITCLAW_BERRY_EXPERIMENTAL_RUNNER only for "
           "supervised debugging\"}");
  return fc_berry_record_result(-ENOSYS);
#else
  (void)ctx;
  (void)path;
  (void)args_json;
  snprintf(out, out_len,
           "{\"ok\":false,\"error\":\"Berry support disabled\"}");
  return fc_berry_record_result(-ENOSYS);
#endif
}

int fc_berry_check_file(const char *path, char *out, size_t out_len)
{
  char full[FC_PATH_LEN];
  int ret;

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

#if defined(CONFIG_FRUITCLAW_ENABLE_BERRY) && \
    defined(CONFIG_FRUITCLAW_BERRY_EXPERIMENTAL_RUNNER)
  fc_berry_record_start(path);
  ret = fc_berry_resolve_path(path, full, sizeof(full));
  if (ret < 0)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"mode\":\"syntax\","
               "\"error\":\"script path denied\"}");
      return fc_berry_record_result(ret);
    }

  ret = berry_check_file(full, out, out_len);
  return fc_berry_record_result(ret);
#elif defined(CONFIG_FRUITCLAW_ENABLE_BERRY)
  fc_berry_record_start(path);
  ret = fc_berry_resolve_path(path, full, sizeof(full));
  if (ret < 0)
    {
      snprintf(out, out_len,
               "{\"ok\":false,\"mode\":\"syntax\","
               "\"error\":\"script path denied\"}");
      return fc_berry_record_result(ret);
    }

  snprintf(out, out_len,
           "{\"ok\":false,\"mode\":\"syntax\","
           "\"error\":\"Berry runner integration disabled\"}");
  return fc_berry_record_result(-ENOSYS);
#else
  (void)path;
  snprintf(out, out_len,
           "{\"ok\":false,\"mode\":\"syntax\","
           "\"error\":\"Berry support disabled\"}");
  return fc_berry_record_result(-ENOSYS);
#endif
}

int fc_berry_status_format(char *out, size_t out_len)
{
  unsigned long calls;
  unsigned long fails;
  int64_t start_ms;
  int64_t done_ms;
  int64_t now = fc_mono_ms();
  int last_ret;
  char path[FC_PATH_LEN];
  const char *runner;

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

#if defined(CONFIG_FRUITCLAW_ENABLE_BERRY) && \
    defined(CONFIG_FRUITCLAW_BERRY_EXPERIMENTAL_RUNNER)
  runner = "experimental";
#elif defined(CONFIG_FRUITCLAW_ENABLE_BERRY)
  runner = "stub";
#else
  runner = "disabled";
#endif

  pthread_mutex_lock(&g_berry_status_lock);
  calls = g_berry_call_count;
  fails = g_berry_fail_count;
  start_ms = g_berry_last_start_ms;
  done_ms = g_berry_last_done_ms;
  last_ret = g_berry_last_ret;
  fc_strlcpy(path, g_berry_last_path[0] ? g_berry_last_path : "-",
             sizeof(path));
  pthread_mutex_unlock(&g_berry_status_lock);

  snprintf(out, out_len,
           "berry_status: runner=%s calls=%lu failures=%lu last_ret=%d "
           "last_start_age_ms=%lld last_done_age_ms=%lld last_path=%s",
           runner, calls, fails, last_ret,
           start_ms > 0 ? (long long)(now - start_ms) : -1,
           done_ms > 0 ? (long long)(now - done_ms) : -1,
           path);
  return 0;
}

void fc_berry_status(FILE *out)
{
  char status[256];

  if (out == NULL)
    {
      return;
    }

  if (fc_berry_status_format(status, sizeof(status)) == 0)
    {
      fprintf(out, "  %s\n", status);
    }
}
