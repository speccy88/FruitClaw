/* SPDX-License-Identifier: Apache-2.0 */

#include "fruitclaw.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static fc_queue_t g_main_queue;
static fc_queue_t g_agent_queue;

int fc_queue_init(fc_queue_t *q)
{
  fc_event_t *items;
  int ret;

  if (q == NULL)
    {
      return -EINVAL;
    }

  if (q->initialized)
    {
      return 0;
    }

  memset(q, 0, sizeof(*q));
  items = calloc(CONFIG_FRUITCLAW_MAX_EVENT_QUEUE, sizeof(fc_event_t));
  if (items == NULL)
    {
      return -ENOMEM;
    }

  ret = pthread_mutex_init(&q->lock, NULL);
  if (ret != 0)
    {
      free(items);
      return -ret;
    }

  ret = pthread_cond_init(&q->cond, NULL);
  if (ret != 0)
    {
      pthread_mutex_destroy(&q->lock);
      free(items);
      return -ret;
    }

  q->items = items;
  q->capacity = CONFIG_FRUITCLAW_MAX_EVENT_QUEUE;
  q->initialized = true;
  return 0;
}

int fc_queue_publish(fc_queue_t *q, const fc_event_t *ev)
{
  if (q == NULL || ev == NULL || !q->initialized || q->items == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&q->lock);
  if (q->count >= q->capacity)
    {
      pthread_mutex_unlock(&q->lock);
      return -ENOSPC;
    }

  q->items[q->tail] = *ev;
  q->tail = (q->tail + 1) % q->capacity;
  q->count++;
  pthread_cond_signal(&q->cond);
  pthread_mutex_unlock(&q->lock);
  return 0;
}

static void fc_abs_timeout(struct timespec *ts, int timeout_ms)
{
  clock_gettime(CLOCK_REALTIME, ts);
  ts->tv_sec += timeout_ms / 1000;
  ts->tv_nsec += (timeout_ms % 1000) * 1000000;
  if (ts->tv_nsec >= 1000000000)
    {
      ts->tv_sec++;
      ts->tv_nsec -= 1000000000;
    }
}

int fc_queue_receive(fc_queue_t *q, fc_event_t *ev, int timeout_ms)
{
  int ret = 0;

  if (q == NULL || ev == NULL || !q->initialized || q->items == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&q->lock);
  while (q->count == 0)
    {
      if (timeout_ms < 0)
        {
          ret = pthread_cond_wait(&q->cond, &q->lock);
        }
      else
        {
          struct timespec ts;
          fc_abs_timeout(&ts, timeout_ms);
          ret = pthread_cond_timedwait(&q->cond, &q->lock, &ts);
        }

      if (ret == ETIMEDOUT)
        {
          pthread_mutex_unlock(&q->lock);
          return -ETIMEDOUT;
        }

      if (ret != 0)
        {
          pthread_mutex_unlock(&q->lock);
          return -ret;
        }
    }

  *ev = q->items[q->head];
  q->head = (q->head + 1) % q->capacity;
  q->count--;
  pthread_mutex_unlock(&q->lock);
  return 0;
}

unsigned int fc_queue_count(fc_queue_t *q)
{
  unsigned int count;

  if (q == NULL || !q->initialized)
    {
      return 0;
    }

  pthread_mutex_lock(&q->lock);
  count = q->count;
  pthread_mutex_unlock(&q->lock);
  return count;
}

fc_queue_t *fc_main_queue(void)
{
  return &g_main_queue;
}

fc_queue_t *fc_agent_queue(void)
{
  return &g_agent_queue;
}
