/* SPDX-License-Identifier: Apache-2.0 */

#include "fruitclaw.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>

static pthread_mutex_t g_progress_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_progress_last_ms;
static char g_progress_last_source[FC_SOURCE_LEN];

void fc_operator_progress_mark(const char *source)
{
  pthread_mutex_lock(&g_progress_lock);
  g_progress_last_ms = fc_mono_ms();
  fc_strlcpy(g_progress_last_source, source ? source : "unknown",
             sizeof(g_progress_last_source));
  pthread_mutex_unlock(&g_progress_lock);
}

int64_t fc_operator_progress_age_ms(char *source, size_t source_len)
{
  int64_t last_ms;
  int64_t now_ms;

  pthread_mutex_lock(&g_progress_lock);
  last_ms = g_progress_last_ms;
  if (source != NULL && source_len > 0)
    {
      fc_strlcpy(source, g_progress_last_source[0] ?
                 g_progress_last_source : "-", source_len);
    }

  pthread_mutex_unlock(&g_progress_lock);

  if (last_ms <= 0)
    {
      return -1;
    }

  now_ms = fc_mono_ms();
  return now_ms >= last_ms ? now_ms - last_ms : 0;
}

int fc_operator_progress_status_format(char *out, size_t out_len)
{
  char source[FC_SOURCE_LEN];
  int64_t age_ms;

  if (out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  source[0] = '\0';
  age_ms = fc_operator_progress_age_ms(source, sizeof(source));
  snprintf(out, out_len, "operator_progress: age_ms=%lld source=%s",
           (long long)age_ms, source[0] ? source : "-");
  return 0;
}

bool fc_operator_progress_stale(int64_t runtime_start_ms, int64_t now_ms,
                                int64_t progress_age_ms,
                                uint32_t timeout_ms)
{
  if (timeout_ms == 0)
    {
      return false;
    }

  if (progress_age_ms >= 0)
    {
      return progress_age_ms > timeout_ms;
    }

  if (runtime_start_ms <= 0 || now_ms < runtime_start_ms)
    {
      return false;
    }

  return now_ms - runtime_start_ms > timeout_ms;
}
