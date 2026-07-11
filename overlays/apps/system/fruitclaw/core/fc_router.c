/* SPDX-License-Identifier: Apache-2.0 */

#include "fruitclaw.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>

static pthread_t g_router_thread;
static bool g_router_started;
static pthread_mutex_t g_router_status_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_router_routed_count;
static unsigned long g_router_dropped_count;
static unsigned long g_router_failed_count;
static int64_t g_router_last_ms;
static int g_router_last_ret;
static char g_router_last_source[FC_SOURCE_LEN];
static char g_router_last_type[FC_TYPE_LEN];

static void fc_router_record(const fc_event_t *ev, int ret, bool routed)
{
  pthread_mutex_lock(&g_router_status_lock);
  if (routed && ret == 0)
    {
      g_router_routed_count++;
    }
  else if (routed)
    {
      g_router_failed_count++;
    }
  else
    {
      g_router_dropped_count++;
    }

  g_router_last_ms = fc_mono_ms();
  g_router_last_ret = ret;
  fc_strlcpy(g_router_last_source, ev ? ev->source : "",
             sizeof(g_router_last_source));
  fc_strlcpy(g_router_last_type, ev ? ev->type : "",
             sizeof(g_router_last_type));
  pthread_mutex_unlock(&g_router_status_lock);
}

int fc_router_route_event(const fc_event_t *ev)
{
  int ret;

  if (ev == NULL)
    {
      return -EINVAL;
    }

  if ((strcmp(ev->source, "telegram") == 0 &&
       strcmp(ev->type, "message.in") == 0) ||
      (strcmp(ev->source, "scheduler") == 0 &&
      strcmp(ev->type, "timer.fire") == 0) ||
      (strcmp(ev->source, "cli") == 0 &&
       strcmp(ev->type, "message.in") == 0))
    {
      ret = fc_queue_publish(fc_agent_queue(), ev);
      fc_router_record(ev, ret, true);
      return ret;
    }

  FC_LOGI("drop event source=%s type=%s", ev->source, ev->type);
  fc_router_record(ev, 0, false);
  return 0;
}

static void *fc_router_thread_main(void *arg)
{
  fc_event_t ev;

  (void)arg;
  for (; ; )
    {
      if (fc_queue_receive(fc_main_queue(), &ev, -1) == 0)
        {
          int ret = fc_router_route_event(&ev);
          if (ret < 0)
            {
              FC_LOGW("route failed: %d", ret);
            }
        }
    }

  return NULL;
}

int fc_router_worker_start(void)
{
  pthread_attr_t attr;
  int ret;

  if (g_router_started)
    {
      return 0;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_FRUITCLAW_SMALL_STACKSIZE);
  ret = pthread_create(&g_router_thread, &attr, fc_router_thread_main, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      return -ret;
    }

  g_router_started = true;
  return 0;
}

int fc_router_status_format(char *out, size_t out_len)
{
  unsigned long routed;
  unsigned long dropped;
  unsigned long failed;
  int64_t last_ms;
  int last_ret;
  char source[FC_SOURCE_LEN];
  char type[FC_TYPE_LEN];
  int64_t now = fc_mono_ms();

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_router_status_lock);
  routed = g_router_routed_count;
  dropped = g_router_dropped_count;
  failed = g_router_failed_count;
  last_ms = g_router_last_ms;
  last_ret = g_router_last_ret;
  fc_strlcpy(source, g_router_last_source, sizeof(source));
  fc_strlcpy(type, g_router_last_type, sizeof(type));
  pthread_mutex_unlock(&g_router_status_lock);

  snprintf(out, out_len,
           "router_status: worker=%s routed=%lu dropped=%lu failed=%lu "
           "last_ret=%d last_age_ms=%lld last=%s/%s",
           g_router_started ? "started" : "stopped",
           routed, dropped, failed, last_ret,
           last_ms > 0 ? (long long)(now - last_ms) : -1,
           source[0] ? source : "-", type[0] ? type : "-");
  return 0;
}

void fc_router_status(FILE *out)
{
  char status[256];

  if (out == NULL)
    {
      return;
    }

  if (fc_router_status_format(status, sizeof(status)) == 0)
    {
      fprintf(out, "  %s\n", status);
    }
}
