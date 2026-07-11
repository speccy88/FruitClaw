/* SPDX-License-Identifier: Apache-2.0 */

#ifndef __APPS_INTERPRETERS_BERRY_INCLUDE_BERRY_RUNNER_H
#define __APPS_INTERPRETERS_BERRY_INCLUDE_BERRY_RUNNER_H

#include <stddef.h>

struct berry_claw_host_s
{
  void *ctx;
  int (*call_tool)(void *ctx, const char *name, const char *args_json,
                   char *out, size_t out_len);
};

int berry_run_file_with_claw(const char *path, const char *args_json,
                             const struct berry_claw_host_s *host,
                             char *out, size_t out_len);
int berry_check_file(const char *path, char *out, size_t out_len);

#endif /* __APPS_INTERPRETERS_BERRY_INCLUDE_BERRY_RUNNER_H */
