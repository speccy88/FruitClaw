/* SPDX-License-Identifier: Apache-2.0 */

#include "fruitclaw.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_data_dir[FC_PATH_LEN];

int64_t fc_time_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
    {
      return 0;
    }

  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t fc_mono_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    {
      return fc_time_ms();
    }

  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void fc_make_id(char *out, size_t out_len, const char *prefix)
{
  static unsigned int seq;
  int64_t now;

  if (out == NULL || out_len == 0)
    {
      return;
    }

  now = fc_time_ms();
  snprintf(out, out_len, "%s-%lld-%u",
           prefix ? prefix : "ev", (long long)now, ++seq);
}

int fc_strlcpy(char *dst, const char *src, size_t dst_len)
{
  size_t len;

  if (dst == NULL || dst_len == 0)
    {
      return -EINVAL;
    }

  if (src == NULL)
    {
      dst[0] = '\0';
      return 0;
    }

  len = strnlen(src, dst_len - 1);
  memcpy(dst, src, len);
  dst[len] = '\0';

  return src[len] == '\0' ? 0 : -ENOSPC;
}

void fc_trim(char *s)
{
  size_t len;
  char *start;

  if (s == NULL)
    {
      return;
    }

  start = s;
  while (isspace((unsigned char)*start))
    {
      start++;
    }

  if (start != s)
    {
      memmove(s, start, strlen(start) + 1);
    }

  for (; ; )
    {
      len = strlen(s);
      while (len > 0 && isspace((unsigned char)s[len - 1]))
        {
          s[--len] = '\0';
        }

      /* Serial setup commands can write literal "\n"/"\r" suffixes. */
      if (len >= 2 && s[len - 2] == '\\' &&
          (s[len - 1] == 'n' || s[len - 1] == 'r'))
        {
          s[len - 2] = '\0';
          continue;
        }

      break;
    }
}

static bool fc_mkdir_skip_mount_root(const char *path)
{
  if (path == NULL)
    {
      return false;
    }

  return strcmp(path, "/data") == 0 ||
         strcmp(path, "/tmp") == 0 ||
         strcmp(path, "/scripts") == 0 ||
         strcmp(path, "/mnt") == 0 ||
         strcmp(path, "/mnt/sd0") == 0;
}

int fc_mkdir_p(const char *path)
{
  char tmp[FC_PATH_LEN];
  char *p;

  if (path == NULL || path[0] == '\0')
    {
      return -EINVAL;
    }

  if (fc_strlcpy(tmp, path, sizeof(tmp)) < 0)
    {
      return -ENAMETOOLONG;
    }

  for (p = tmp + 1; *p != '\0'; p++)
    {
      if (*p == '/')
        {
          *p = '\0';
          if (!fc_mkdir_skip_mount_root(tmp) &&
              mkdir(tmp, 0775) < 0 && errno != EEXIST)
            {
              return -errno;
            }

          *p = '/';
        }
    }

  if (!fc_mkdir_skip_mount_root(tmp) &&
      mkdir(tmp, 0775) < 0 && errno != EEXIST)
    {
      return -errno;
    }

  return 0;
}

static int fc_create_if_missing(const char *path, const char *text)
{
  int fd;

  fd = open(path, O_RDONLY);
  if (fd >= 0)
    {
      close(fd);
      return 0;
    }

  if (errno != ENOENT)
    {
      return -errno;
    }

  return fc_write_text_file_atomic(path, text);
}

#ifdef CONFIG_FRUITCLAW_PREFER_SD_DATA_DIR
static bool fc_dir_exists(const char *path)
{
  struct stat st;

  return path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool fc_sd_data_dir_ready(void)
{
  char marker[FC_PATH_LEN];
  int fd;

  if (snprintf(marker, sizeof(marker), "%s/.fruitclaw-ready",
               CONFIG_FRUITCLAW_SD_DATA_DIR) >= (int)sizeof(marker))
    {
      return false;
    }

  if (fc_dir_exists(CONFIG_FRUITCLAW_SD_DATA_DIR))
    {
      if (access(marker, F_OK) == 0)
        {
          return true;
        }
    }
  else if (fc_mkdir_p(CONFIG_FRUITCLAW_SD_DATA_DIR) < 0)
    {
      return false;
    }

  fd = open(marker, O_WRONLY | O_CREAT | O_TRUNC, 0664);
  if (fd < 0)
    {
      return false;
    }

  close(fd);
  return true;
}
#endif

const char *fc_data_dir(void)
{
  if (g_data_dir[0] == '\0')
    {
#ifdef CONFIG_FRUITCLAW_PREFER_SD_DATA_DIR
      if (fc_sd_data_dir_ready())
        {
          fc_strlcpy(g_data_dir, CONFIG_FRUITCLAW_SD_DATA_DIR,
                     sizeof(g_data_dir));
        }
      else
#endif
        {
          fc_strlcpy(g_data_dir, CONFIG_FRUITCLAW_DATA_DIR,
                     sizeof(g_data_dir));
        }
    }

  return g_data_dir;
}

static int fc_create_data_subdir(const char *leaf)
{
  char path[FC_PATH_LEN];

  if (snprintf(path, sizeof(path), "%s/%s", fc_data_dir(), leaf) >=
      (int)sizeof(path))
    {
      return -ENAMETOOLONG;
    }

  return fc_mkdir_p(path);
}

static int fc_migrate_from_fallback_if_missing(const char *leaf)
{
  char src[FC_PATH_LEN];
  char dst[FC_PATH_LEN];
  char *buf;
  int ret;

  if (leaf == NULL || strcmp(fc_data_dir(), CONFIG_FRUITCLAW_DATA_DIR) == 0)
    {
      return 0;
    }

  ret = fc_data_path(leaf, dst, sizeof(dst));
  if (ret < 0)
    {
      return ret;
    }

  if (access(dst, F_OK) == 0)
    {
      return 0;
    }

  if (snprintf(src, sizeof(src), "%s/%s", CONFIG_FRUITCLAW_DATA_DIR, leaf) >=
      (int)sizeof(src))
    {
      return -ENAMETOOLONG;
    }

  if (access(src, F_OK) < 0)
    {
      return 0;
    }

  buf = calloc(1, CONFIG_FRUITCLAW_MAX_JSON);
  if (buf == NULL)
    {
      return -ENOMEM;
    }

  ret = fc_read_text_file(src, buf, CONFIG_FRUITCLAW_MAX_JSON, false);
  if (ret == 0)
    {
      ret = fc_write_text_file_atomic(dst, buf);
    }

  free(buf);
  return ret;
}

int fc_init_data_dir(void)
{
  char path[FC_PATH_LEN];
  char allowed_chat_seed[FC_CHAT_ID_LEN + 2];
  int ret;

  fc_bootstrap_note("data-root", 0);
  ret = fc_mkdir_p(fc_data_dir());
  if (ret < 0)
    {
      fc_bootstrap_note("data-root-failed", ret);
      return ret;
    }

  fc_bootstrap_note("data-subdir-secrets", 0);
  if ((ret = fc_create_data_subdir("secrets")) < 0)
    {
      fc_bootstrap_note("data-subdir-failed", ret);
      return ret;
    }

  fc_bootstrap_note("data-subdir-sessions", 0);
  if ((ret = fc_create_data_subdir("sessions")) < 0)
    {
      fc_bootstrap_note("data-subdir-failed", ret);
      return ret;
    }

  fc_bootstrap_note("data-subdir-scripts", 0);
  if ((ret = fc_create_data_subdir("scripts")) < 0)
    {
      fc_bootstrap_note("data-subdir-failed", ret);
      return ret;
    }

  fc_bootstrap_note("data-subdir-generated-scripts", 0);
  if ((ret = fc_create_data_subdir("scripts/generated")) < 0)
    {
      fc_bootstrap_note("data-subdir-failed", ret);
      return ret;
    }

  fc_bootstrap_note("data-subdir-notes", 0);
  if ((ret = fc_create_data_subdir("notes")) < 0)
    {
      fc_bootstrap_note("data-subdir-failed", ret);
      return ret;
    }

  fc_bootstrap_note("data-subdir-www", 0);
  if ((ret = fc_create_data_subdir("www")) < 0)
    {
      fc_bootstrap_note("data-subdir-failed", ret);
      return ret;
    }

  fc_bootstrap_note("data-subdir-certs", 0);
  if ((ret = fc_create_data_subdir("certs")) < 0)
    {
      fc_bootstrap_note("data-subdir-failed", ret);
      return ret;
    }

  fc_bootstrap_note("data-subdir-tmp", 0);
  if ((ret = fc_create_data_subdir("tmp")) < 0)
    {
      fc_bootstrap_note("data-subdir-failed", ret);
      return ret;
    }

  fc_bootstrap_note("data-subdir-services", 0);
  if ((ret = fc_create_data_subdir("services")) < 0)
    {
      fc_bootstrap_note("data-subdir-failed", ret);
      return ret;
    }

  fc_bootstrap_note("data-migrate", 0);
  fc_migrate_from_fallback_if_missing("secrets/telegram_token");
  fc_migrate_from_fallback_if_missing("secrets/deepseek_api_key");
  fc_migrate_from_fallback_if_missing("telegram_allowed_chats.txt");
  fc_migrate_from_fallback_if_missing("telegram_offset");
  fc_migrate_from_fallback_if_missing(CONFIG_FRUITCLAW_WIFI_CONFIG_LEAF);
  fc_migrate_from_fallback_if_missing("certs/roots.pem");
  fc_migrate_from_fallback_if_missing("http_allowlist.txt");

  fc_bootstrap_note("data-system-md", 0);
  fc_data_path("system.md", path, sizeof(path));
  ret = fc_create_if_missing(path,
      "You are FruitClaw, a small embedded assistant running on a NuttX "
      "RP2350 device. You can answer the user and use available tools when "
      "helpful. Be concise. Do not claim capabilities that are not listed as "
      "tools. Never reveal secrets. Allowed Telegram chats and local CLI are "
      "the device owner; owner-mode tools may operate directly. For reusable "
	      "board automation, create generated Berry or NSH scripts with "
	      "script.write, validate them, run them, rework them from user feedback, "
	      "and schedule them with script.schedule for boot, once, interval, or "
	      "cron execution. script.list and script.read show last validation "
	      "status for generated scripts. Use validate_mode=syntax for "
	      "long-running Berry LVGL "
      "UI scripts so validation parses them without executing forever. Berry "
      "scripts can use the constrained claw helpers for memory, terminal, "
      "NeoPixels, RTTTL, services, scripts, schedules, Telegram replies, and "
      "LVGL when the lv module is compiled.\n");
  if (ret < 0)
    {
      fc_bootstrap_note("data-file-failed", ret);
      return ret;
    }

  fc_bootstrap_note("data-user-md", 0);
  fc_data_path("user.md", path, sizeof(path));
  if ((ret = fc_create_if_missing(path,
      "The user is the owner of this device.\n")) < 0)
    {
      fc_bootstrap_note("data-file-failed", ret);
      return ret;
    }

  fc_bootstrap_note("data-memory", 0);
  fc_data_path("memory.jsonl", path, sizeof(path));
  if ((ret = fc_create_if_missing(path, "")) < 0)
    {
      fc_bootstrap_note("data-file-failed", ret);
      return ret;
    }

  fc_bootstrap_note("data-schedules", 0);
  fc_data_path("schedules.json", path, sizeof(path));
  if ((ret = fc_create_if_missing(path, "[]\n")) < 0)
    {
      fc_bootstrap_note("data-file-failed", ret);
      return ret;
    }

  fc_bootstrap_note("data-router-rules", 0);
  fc_data_path("router_rules.json", path, sizeof(path));
  if ((ret = fc_create_if_missing(path, "[]\n")) < 0)
    {
      fc_bootstrap_note("data-file-failed", ret);
      return ret;
    }

  fc_bootstrap_note("data-allowed-chats", 0);
  fc_data_path("telegram_allowed_chats.txt", path, sizeof(path));
  if (CONFIG_FRUITCLAW_TELEGRAM_DEFAULT_ALLOWED_CHAT_ID[0] != '\0')
    {
      snprintf(allowed_chat_seed, sizeof(allowed_chat_seed), "%s\n",
               CONFIG_FRUITCLAW_TELEGRAM_DEFAULT_ALLOWED_CHAT_ID);
    }
  else
    {
      fc_strlcpy(allowed_chat_seed, "", sizeof(allowed_chat_seed));
    }

  if ((ret = fc_create_if_missing(path, allowed_chat_seed)) < 0)
    {
      fc_bootstrap_note("data-file-failed", ret);
      return ret;
    }

  fc_bootstrap_note("data-http-allow", 0);
  fc_data_path("http_allowlist.txt", path, sizeof(path));
  if ((ret = fc_create_if_missing(path,
      "api.deepseek.com\napi.telegram.org\n")) < 0)
    {
      fc_bootstrap_note("data-file-failed", ret);
      return ret;
    }

  fc_bootstrap_note("data-ready", 0);
  return 0;
}

int fc_read_text_file(const char *path, char *out, size_t out_len, bool trim)
{
  int fd;
  ssize_t nread;
  size_t off = 0;

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

  while (off < out_len - 1)
    {
      nread = read(fd, out + off, out_len - 1 - off);
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
  out[off] = '\0';
  if (trim)
    {
      fc_trim(out);
    }

  return 0;
}

int fc_write_text_file_atomic(const char *path, const char *text)
{
  char tmp[FC_PATH_LEN + 8];
  int fd;
  size_t len;
  ssize_t written;

  if (path == NULL || text == NULL)
    {
      return -EINVAL;
    }

  if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
    {
      return -ENAMETOOLONG;
    }

  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0664);
  if (fd < 0)
    {
      int err = -errno;
      fprintf(stderr, "fruitclaw: atomic write open failed tmp=%s "
              "path=%s err=%d\n", tmp, path, err);
      return err;
    }

  len = strlen(text);
  written = write(fd, text, len);
  if (written < 0 || (size_t)written != len)
    {
      int err = written < 0 ? -errno : -EIO;
      close(fd);
      unlink(tmp);
      fprintf(stderr, "fruitclaw: atomic write failed tmp=%s path=%s "
              "err=%d\n", tmp, path, err);
      return err;
    }

#ifdef CONFIG_FS_FSYNC
  fsync(fd);
#endif
  close(fd);

  if (rename(tmp, path) < 0)
    {
      int err = -errno;
      unlink(tmp);
      fprintf(stderr, "fruitclaw: atomic write rename failed tmp=%s "
              "path=%s err=%d\n", tmp, path, err);
      return err;
    }

  return 0;
}

int fc_append_text_file(const char *path, const char *text)
{
  int fd;
  size_t len;
  ssize_t written;

  if (path == NULL || text == NULL)
    {
      return -EINVAL;
    }

  fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0664);
  if (fd < 0)
    {
      return -errno;
    }

  len = strlen(text);
  written = write(fd, text, len);
  close(fd);

  if (written < 0)
    {
      return -errno;
    }

  return (size_t)written == len ? 0 : -EIO;
}

int fc_data_path(const char *leaf, char *out, size_t out_len)
{
  if (leaf == NULL || out == NULL || out_len == 0 ||
      fc_path_has_parent_ref(leaf))
    {
      return -EINVAL;
    }

  if (snprintf(out, out_len, "%s/%s", fc_data_dir(), leaf) >=
      (int)out_len)
    {
      return -ENAMETOOLONG;
    }

  return 0;
}

int fc_secret_path(const char *leaf, char *out, size_t out_len)
{
  char rel[FC_PATH_LEN];

  if (leaf == NULL || out == NULL || out_len == 0 ||
      fc_path_has_parent_ref(leaf))
    {
      return -EINVAL;
    }

  if (snprintf(rel, sizeof(rel), "secrets/%s", leaf) >= (int)sizeof(rel))
    {
      return -ENAMETOOLONG;
    }

  return fc_data_path(rel, out, out_len);
}

int fc_tls_ca_cert_path(char *out, size_t out_len)
{
  return fc_data_path("certs/roots.pem", out, out_len);
}

bool fc_path_has_parent_ref(const char *path)
{
  if (path == NULL)
    {
      return true;
    }

  return strstr(path, "../") != NULL || strstr(path, "/..") != NULL ||
         strcmp(path, "..") == 0;
}

bool fc_path_is_secret(const char *path)
{
  return path != NULL && strstr(path, "/secrets/") != NULL;
}

static bool fc_parse_url_host(const char *url, char *host, size_t host_len)
{
  const char *p;
  const char *end;
  size_t len;

  if (url == NULL || host == NULL || host_len == 0)
    {
      return false;
    }

  p = strstr(url, "://");
  if (p == NULL)
    {
      return false;
    }

  p += 3;
  end = p;
  while (*end != '\0' && *end != '/' && *end != ':' && *end != '?' &&
         *end != '#')
    {
      end++;
    }

  len = end - p;
  if (len == 0 || len >= host_len)
    {
      return false;
    }

  memcpy(host, p, len);
  host[len] = '\0';
  return true;
}

bool fc_url_host_allowed(const char *url)
{
  char host[96];
  char line[128];
  char path[FC_PATH_LEN];
  FILE *fp;

  if (!fc_parse_url_host(url, host, sizeof(host)))
    {
      return false;
    }

  if (fc_data_path("http_allowlist.txt", path, sizeof(path)) < 0)
    {
      return false;
    }

  fp = fopen(path, "r");
  if (fp == NULL)
    {
      return false;
    }

  while (fgets(line, sizeof(line), fp) != NULL)
    {
      fc_trim(line);
      if (line[0] == '\0' || line[0] == '#')
        {
          continue;
        }

      if (strcmp(line, host) == 0)
        {
          fclose(fp);
          return true;
        }
    }

  fclose(fp);
  return false;
}

int fc_json_escape(const char *in, char *out, size_t out_len)
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
