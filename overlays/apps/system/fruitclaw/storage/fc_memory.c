/* SPDX-License-Identifier: Apache-2.0 */

#include "fruitclaw.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fc_read_tail_file(const char *path, size_t max_bytes, char *out,
                             size_t out_len)
{
  int fd;
  off_t end;
  off_t start;
  ssize_t nread;
  size_t window;
  size_t want;

  if (path == NULL || out == NULL || out_len == 0)
    {
      return -EINVAL;
    }

  out[0] = '\0';
  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  end = lseek(fd, 0, SEEK_END);
  if (end < 0)
    {
      int err = -errno;
      close(fd);
      return err;
    }

  if (max_bytes > out_len - 1)
    {
      max_bytes = out_len - 1;
    }

  window = max_bytes;
  if (window == 0)
    {
      close(fd);
      return 0;
    }

  for (; ; )
    {
      start = end > (off_t)window ? end - (off_t)window : 0;
      if (lseek(fd, start, SEEK_SET) < 0)
        {
          int err = -errno;
          close(fd);
          return err;
        }

      want = end - start;
      nread = read(fd, out, want);
      if (nread < 0)
        {
          int err = -errno;
          close(fd);
          return err;
        }

      out[nread] = '\0';
      if (start > 0 && nread > 0)
        {
          char *next_line = strchr(out, '\n');

          if (next_line != NULL)
            {
              size_t skip = (size_t)(next_line - out) + 1;

              if (skip < (size_t)nread)
                {
                  memmove(out, out + skip, (size_t)nread - skip + 1);
                  break;
                }
            }

          if (window < out_len - 1)
            {
              size_t next_window = window * 2;

              if (next_window <= window || next_window > out_len - 1)
                {
                  next_window = out_len - 1;
                }

              window = next_window;
              continue;
            }

          out[0] = '\0';
        }

      break;
    }

  close(fd);
  return 0;
}

static bool fc_jsonl_line_looks_like_memory(char *line)
{
  size_t len;

  if (line == NULL)
    {
      return false;
    }

  fc_trim(line);
  len = strlen(line);
  return len >= 2 && line[0] == '{' && line[len - 1] == '}' &&
         strstr(line, "\"ts_ms\"") != NULL &&
         strstr(line, "\"source\"") != NULL &&
         strstr(line, "\"text\"") != NULL;
}

static void fc_filter_memory_jsonl_tail(char *buf)
{
  char *readp;
  char *writep;

  if (buf == NULL)
    {
      return;
    }

  readp = buf;
  writep = buf;
  while (readp[0] != '\0')
    {
      char *next = strchr(readp, '\n');
      char saved = '\0';

      if (next != NULL)
        {
          saved = *next;
          *next = '\0';
        }

      if (fc_jsonl_line_looks_like_memory(readp))
        {
          size_t len = strlen(readp);

          memmove(writep, readp, len);
          writep += len;
          *writep++ = '\n';
        }

      if (next == NULL)
        {
          break;
        }

      *next = saved;
      readp = next + 1;
    }

  *writep = '\0';
}

int fc_memory_append(const char *source, const char *text)
{
  char path[FC_PATH_LEN];
  char esc_source[64];
  char esc_text[CONFIG_FRUITCLAW_MAX_EVENT_TEXT * 6];
  char line[CONFIG_FRUITCLAW_MAX_EVENT_TEXT * 6 + 160];

  if (text == NULL)
    {
      return -EINVAL;
    }

  if (fc_data_path("memory.jsonl", path, sizeof(path)) < 0)
    {
      return -EINVAL;
    }

  fc_json_escape(source ? source : "agent", esc_source, sizeof(esc_source));
  fc_json_escape(text, esc_text, sizeof(esc_text));
  snprintf(line, sizeof(line),
           "{\"ts_ms\":%lld,\"source\":\"%s\",\"text\":\"%s\"}\n",
           (long long)fc_time_ms(), esc_source, esc_text);

  return fc_append_text_file(path, line);
}

int fc_memory_read_tail(size_t max_bytes, char *out, size_t out_len)
{
  char path[FC_PATH_LEN];
  int ret;

  if (fc_data_path("memory.jsonl", path, sizeof(path)) < 0)
    {
      return -EINVAL;
    }

  ret = fc_read_tail_file(path, max_bytes, out, out_len);
  if (ret == -ENOENT)
    {
      out[0] = '\0';
      return 0;
    }

  if (ret == 0)
    {
      fc_filter_memory_jsonl_tail(out);
    }

  return ret;
}

int fc_session_safe_filename(const char *session_id, char *out,
                             size_t out_len)
{
  size_t i;
  size_t off = 0;

  if (session_id == NULL || out == NULL || out_len == 0 ||
      fc_path_has_parent_ref(session_id))
    {
      return -EINVAL;
    }

  for (i = 0; session_id[i] != '\0'; i++)
    {
      unsigned char ch = (unsigned char)session_id[i];

      if (off + 1 >= out_len)
        {
          return -ENOSPC;
        }

      if (isalnum(ch) || ch == '-' || ch == '_')
        {
          out[off++] = ch;
        }
      else if (ch == ':' || ch == '/' || ch == '.')
        {
          out[off++] = '_';
        }
      else
        {
          return -EINVAL;
        }
    }

  if (off == 0)
    {
      return -EINVAL;
    }

  out[off] = '\0';
  return 0;
}

static int fc_session_path(const char *session_id, char *out, size_t out_len)
{
  char safe[FC_SESSION_ID_LEN + 8];
  char leaf[FC_PATH_LEN];

  if (fc_session_safe_filename(session_id, safe, sizeof(safe)) < 0)
    {
      return -EINVAL;
    }

  if (snprintf(leaf, sizeof(leaf), "sessions/%s.jsonl", safe) >=
      (int)sizeof(leaf))
    {
      return -ENAMETOOLONG;
    }

  return fc_data_path(leaf, out, out_len);
}

int fc_session_append(const char *session_id, const char *role,
                      const char *name, const char *content)
{
  char path[FC_PATH_LEN];
  char esc_role[32];
  char esc_name[FC_TOOL_NAME_LEN];
  char *esc_content;
  char *line;
  size_t line_len;
  int ret;

  if (session_id == NULL || role == NULL || content == NULL)
    {
      return -EINVAL;
    }

  ret = fc_session_path(session_id, path, sizeof(path));
  if (ret < 0)
    {
      return ret;
    }

  esc_content = malloc(strlen(content) * 6 + 1);
  if (esc_content == NULL)
    {
      return -ENOMEM;
    }

  fc_json_escape(role, esc_role, sizeof(esc_role));
  fc_json_escape(name ? name : "", esc_name, sizeof(esc_name));
  ret = fc_json_escape(content, esc_content, strlen(content) * 6 + 1);
  if (ret < 0)
    {
      free(esc_content);
      return ret;
    }

  line_len = strlen(esc_content) + 192;
  line = malloc(line_len);
  if (line == NULL)
    {
      free(esc_content);
      return -ENOMEM;
    }

  if (name != NULL && name[0] != '\0')
    {
      snprintf(line, line_len,
               "{\"ts_ms\":%lld,\"role\":\"%s\",\"name\":\"%s\","
               "\"content\":\"%s\"}\n",
               (long long)fc_time_ms(), esc_role, esc_name, esc_content);
    }
  else
    {
      snprintf(line, line_len,
               "{\"ts_ms\":%lld,\"role\":\"%s\",\"content\":\"%s\"}\n",
               (long long)fc_time_ms(), esc_role, esc_content);
    }

  ret = fc_append_text_file(path, line);
  free(line);
  free(esc_content);
  return ret;
}

int fc_session_read_tail(const char *session_id, size_t max_bytes,
                         char *out, size_t out_len)
{
  char path[FC_PATH_LEN];
  int ret;

  ret = fc_session_path(session_id, path, sizeof(path));
  if (ret < 0)
    {
      return ret;
    }

  ret = fc_read_tail_file(path, max_bytes, out, out_len);
  if (ret == -ENOENT)
    {
      out[0] = '\0';
      return 0;
    }

  return ret;
}
