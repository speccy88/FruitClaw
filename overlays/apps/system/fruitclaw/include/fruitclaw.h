/* SPDX-License-Identifier: Apache-2.0 */

#ifndef __APPS_SYSTEM_FRUITCLAW_INCLUDE_FRUITCLAW_H
#define __APPS_SYSTEM_FRUITCLAW_INCLUDE_FRUITCLAW_H

#include <nuttx/config.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#ifndef CONFIG_FRUITCLAW_PROGNAME
#  define CONFIG_FRUITCLAW_PROGNAME "fruitclaw"
#endif

#ifndef CONFIG_FRUITCLAW_WORKER_STACKSIZE
#  define CONFIG_FRUITCLAW_WORKER_STACKSIZE 12288
#endif

#ifndef CONFIG_FRUITCLAW_SMALL_STACKSIZE
#  define CONFIG_FRUITCLAW_SMALL_STACKSIZE 4096
#endif

#ifndef CONFIG_FRUITCLAW_GUARD_STACKSIZE
#  define CONFIG_FRUITCLAW_GUARD_STACKSIZE 2048
#endif

#ifndef CONFIG_FRUITCLAW_BOOT_NETWORK_STACKSIZE
#  define CONFIG_FRUITCLAW_BOOT_NETWORK_STACKSIZE 8192
#endif

#ifndef CONFIG_INTERPRETERS_BERRY_STACKSIZE
#  define CONFIG_INTERPRETERS_BERRY_STACKSIZE 32768
#endif

#ifndef CONFIG_FRUITCLAW_DATA_DIR
#  define CONFIG_FRUITCLAW_DATA_DIR "/data/fruitclaw"
#endif

#ifndef CONFIG_FRUITCLAW_SD_DATA_DIR
#  define CONFIG_FRUITCLAW_SD_DATA_DIR "/mnt/sd0/fruitclaw"
#endif

#ifndef CONFIG_FRUITCLAW_MAX_EVENT_QUEUE
#  define CONFIG_FRUITCLAW_MAX_EVENT_QUEUE 16
#endif

#ifndef CONFIG_FRUITCLAW_MAX_EVENT_TEXT
#  define CONFIG_FRUITCLAW_MAX_EVENT_TEXT 1024
#endif

#ifndef CONFIG_FRUITCLAW_MAX_JSON
#  define CONFIG_FRUITCLAW_MAX_JSON 8192
#endif

#ifndef CONFIG_FRUITCLAW_MAX_HTTP_BODY
#  define CONFIG_FRUITCLAW_MAX_HTTP_BODY 65536
#endif

#ifndef CONFIG_FRUITCLAW_MCP_MAX_RESPONSE
#  define CONFIG_FRUITCLAW_MCP_MAX_RESPONSE 32768
#endif

#ifndef CONFIG_FRUITCLAW_MCP_TOOL_TIMEOUT_MS
#  define CONFIG_FRUITCLAW_MCP_TOOL_TIMEOUT_MS 30000
#endif

#ifndef CONFIG_FRUITCLAW_MCP_NOTIFY_STATUS_INTERVAL_MS
#  define CONFIG_FRUITCLAW_MCP_NOTIFY_STATUS_INTERVAL_MS 300000
#endif

#ifndef CONFIG_FRUITCLAW_MCP_NOTIFY_TOOL_INTERVAL_MS
#  define CONFIG_FRUITCLAW_MCP_NOTIFY_TOOL_INTERVAL_MS 10000
#endif

#ifndef CONFIG_FRUITCLAW_MAX_SESSION_MESSAGES
#  define CONFIG_FRUITCLAW_MAX_SESSION_MESSAGES 16
#endif

#ifndef CONFIG_FRUITCLAW_MAX_TOOL_ITERATIONS
#  define CONFIG_FRUITCLAW_MAX_TOOL_ITERATIONS 6
#endif

#ifndef CONFIG_FRUITCLAW_TLS_CA_CERT_PATH
#  define CONFIG_FRUITCLAW_TLS_CA_CERT_PATH "/data/fruitclaw/certs/roots.pem"
#endif

#ifndef CONFIG_FRUITCLAW_TELEGRAM_BOT_TOKEN_PATH
#  define CONFIG_FRUITCLAW_TELEGRAM_BOT_TOKEN_PATH \
    "/data/fruitclaw/secrets/telegram_token"
#endif

#ifndef CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_QUEUE_LEN
#  define CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_QUEUE_LEN 32
#endif

#ifndef CONFIG_FRUITCLAW_TELEGRAM_START_DELAY_MS
#  define CONFIG_FRUITCLAW_TELEGRAM_START_DELAY_MS 3000
#endif

#ifndef CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_BATCH_MAX
#  define CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_BATCH_MAX 6
#endif

#ifndef CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_COALESCE_MS
#  define CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_COALESCE_MS 1500
#endif

#ifndef CONFIG_FRUITCLAW_TELEGRAM_ALLOWED_CHAT_IDS_PATH
#  define CONFIG_FRUITCLAW_TELEGRAM_ALLOWED_CHAT_IDS_PATH \
    "/data/fruitclaw/telegram_allowed_chats.txt"
#endif

#ifndef CONFIG_FRUITCLAW_TELEGRAM_DEFAULT_ALLOWED_CHAT_ID
#  define CONFIG_FRUITCLAW_TELEGRAM_DEFAULT_ALLOWED_CHAT_ID "6216681418"
#endif

#ifndef CONFIG_FRUITCLAW_DEEPSEEK_API_KEY_PATH
#  define CONFIG_FRUITCLAW_DEEPSEEK_API_KEY_PATH \
    "/data/fruitclaw/secrets/deepseek_api_key"
#endif

#ifndef CONFIG_FRUITCLAW_DEEPSEEK_ENDPOINT
#  define CONFIG_FRUITCLAW_DEEPSEEK_ENDPOINT \
    "https://api.deepseek.com/chat/completions"
#endif

#ifndef CONFIG_FRUITCLAW_DEEPSEEK_MODEL
#  define CONFIG_FRUITCLAW_DEEPSEEK_MODEL "deepseek-chat"
#endif

#ifndef CONFIG_FRUITCLAW_DEEPSEEK_HTTP_TIMEOUT_SEC
#  define CONFIG_FRUITCLAW_DEEPSEEK_HTTP_TIMEOUT_SEC 15
#endif

#ifndef CONFIG_FRUITCLAW_HTTP_ALLOWLIST_PATH
#  define CONFIG_FRUITCLAW_HTTP_ALLOWLIST_PATH \
    "/data/fruitclaw/http_allowlist.txt"
#endif

#ifndef CONFIG_SYSTEM_NEOPIXELS_DEVPATH
#  define CONFIG_SYSTEM_NEOPIXELS_DEVPATH "/dev/leds0"
#endif

#ifndef CONFIG_FRUITCLAW_GUARD_DEVPATH
#  define CONFIG_FRUITCLAW_GUARD_DEVPATH "/dev/watchdog0"
#endif

#ifndef CONFIG_FRUITCLAW_GUARD_TIMEOUT_MS
#  define CONFIG_FRUITCLAW_GUARD_TIMEOUT_MS 12000
#endif

#ifndef CONFIG_FRUITCLAW_GUARD_ACQUIRE_TIMEOUT_MS
#  define CONFIG_FRUITCLAW_GUARD_ACQUIRE_TIMEOUT_MS 5000
#endif

#ifndef CONFIG_FRUITCLAW_SESSION_GUARD_TIMEOUT_MS
#  define CONFIG_FRUITCLAW_SESSION_GUARD_TIMEOUT_MS 600000
#endif

#ifndef CONFIG_FRUITCLAW_SESSION_GUARD_HW_TIMEOUT_MS
#  define CONFIG_FRUITCLAW_SESSION_GUARD_HW_TIMEOUT_MS 15000
#endif

#ifndef CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS
#  define CONFIG_FRUITCLAW_OPERATOR_GUARD_TIMEOUT_MS 0
#endif

#ifndef CONFIG_FRUITCLAW_LLM_GUARD_TIMEOUT_MS
#  define CONFIG_FRUITCLAW_LLM_GUARD_TIMEOUT_MS 60000
#endif

#ifndef CONFIG_FRUITCLAW_AGENT_GUARD_TIMEOUT_MS
#  define CONFIG_FRUITCLAW_AGENT_GUARD_TIMEOUT_MS 0
#endif

#ifndef CONFIG_FRUITCLAW_HTTP_GUARD_TIMEOUT_MS
#  define CONFIG_FRUITCLAW_HTTP_GUARD_TIMEOUT_MS 90000
#endif

#ifndef CONFIG_FRUITCLAW_CLI_GUARD_TIMEOUT_MS
#  define CONFIG_FRUITCLAW_CLI_GUARD_TIMEOUT_MS 60000
#endif

#ifndef CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS
#  define CONFIG_FRUITCLAW_MAX_UPTIME_GUARD_MS 0
#endif

#ifndef CONFIG_FRUITCLAW_TELEGRAM_POLL_TIMEOUT_SEC
#  define CONFIG_FRUITCLAW_TELEGRAM_POLL_TIMEOUT_SEC 2
#endif

#ifndef CONFIG_FRUITCLAW_TELEGRAM_POLL_IDLE_MS
#  define CONFIG_FRUITCLAW_TELEGRAM_POLL_IDLE_MS 750
#endif

#ifndef CONFIG_FRUITCLAW_TELEGRAM_MCP_YIELD_MS
#  define CONFIG_FRUITCLAW_TELEGRAM_MCP_YIELD_MS 15000
#endif

#ifndef CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_MAX_YIELD_MS
#  define CONFIG_FRUITCLAW_TELEGRAM_NOTIFY_MAX_YIELD_MS 5000
#endif

#ifndef CONFIG_FRUITCLAW_TELEGRAM_MCP_MAX_YIELD_MS
#  define CONFIG_FRUITCLAW_TELEGRAM_MCP_MAX_YIELD_MS 5000
#endif

#ifndef CONFIG_FRUITCLAW_TELEGRAM_POLL_MIN_IDLE_MS
#  define CONFIG_FRUITCLAW_TELEGRAM_POLL_MIN_IDLE_MS 10000
#endif

#ifndef CONFIG_FRUITCLAW_TELEGRAM_BOOT_QUIET_MS
#  define CONFIG_FRUITCLAW_TELEGRAM_BOOT_QUIET_MS 15000
#endif

#ifndef CONFIG_FRUITCLAW_TELEGRAM_HTTP_TIMEOUT_SEC
#  define CONFIG_FRUITCLAW_TELEGRAM_HTTP_TIMEOUT_SEC 8
#endif

#ifndef CONFIG_FRUITCLAW_TELEGRAM_HTTP_GUARD_TIMEOUT_MS
#  define CONFIG_FRUITCLAW_TELEGRAM_HTTP_GUARD_TIMEOUT_MS 0
#endif

#ifndef CONFIG_FRUITCLAW_TELEGRAM_FAIL_BACKOFF_MIN_MS
#  define CONFIG_FRUITCLAW_TELEGRAM_FAIL_BACKOFF_MIN_MS 10000
#endif

#ifndef CONFIG_FRUITCLAW_TELEGRAM_FAIL_BACKOFF_MAX_MS
#  define CONFIG_FRUITCLAW_TELEGRAM_FAIL_BACKOFF_MAX_MS 60000
#endif

#ifndef CONFIG_FRUITCLAW_WIFI_IFNAME
#  define CONFIG_FRUITCLAW_WIFI_IFNAME "wlan0"
#endif

#ifndef CONFIG_FRUITCLAW_WIFI_CONFIG_LEAF
#  define CONFIG_FRUITCLAW_WIFI_CONFIG_LEAF "wifi.conf"
#endif

#ifndef CONFIG_FRUITCLAW_WIFI_CONFIG_PATH
#  define CONFIG_FRUITCLAW_WIFI_CONFIG_PATH "/mnt/sd0/fruitclaw_wifi.conf"
#endif

#ifndef CONFIG_FRUITCLAW_WIFI_COMMAND_RETRIES
#  define CONFIG_FRUITCLAW_WIFI_COMMAND_RETRIES 4
#endif

#ifndef CONFIG_FRUITCLAW_WIFI_COMMAND_RETRY_DELAY_SEC
#  define CONFIG_FRUITCLAW_WIFI_COMMAND_RETRY_DELAY_SEC 2
#endif

#define FC_ID_LEN          40
#define FC_SOURCE_LEN      24
#define FC_TYPE_LEN        32
#define FC_CHANNEL_LEN     24
#define FC_CHAT_ID_LEN     48
#define FC_SESSION_ID_LEN  64
#define FC_PATH_LEN        192
#define FC_TOOL_NAME_LEN   64
#define FC_HTTP_HEADER_MAX 8
#define FC_MAX_TOOL_CALLS  4
#define FC_MAX_SCHEDULES   12
#define FC_TERMINAL_MAX_COMMAND 192

#define FC_BOOTGUARD_MAGIC        0x464a4247 /* "FJBG" */
#define FC_GUARD_STAGE_BERRY      0x46434245 /* "FCBE" */
#define FC_GUARD_STAGE_TERMINAL   0x4643544d /* "FCTM" */
#define FC_GUARD_STAGE_TEST       0x46435453 /* "FCTS" */
#define FC_GUARD_STAGE_SESSION    0x46435347 /* "FCSG" */
#define FC_GUARD_STAGE_BOOT       0x46434254 /* "FCBT" */
#define FC_GUARD_STAGE_MCP        0x46434d43 /* "FCMC" */
#define FC_GUARD_STAGE_NEOPIXELS  0x46434e50 /* "FCNP" */
#define FC_GUARD_STAGE_DEVICE     0x46434456 /* "FCDV" */
#define FC_GUARD_STAGE_WIFI       0x46435746 /* "FCWF" */
#define FC_GUARD_STAGE_WIFI_START 0x46435753 /* "FCWS" */
#define FC_GUARD_STAGE_WIFI_IFUP  0x46435749 /* "FCWI" */
#define FC_GUARD_STAGE_WIFI_PSK   0x46435750 /* "FCWP" */
#define FC_GUARD_STAGE_WIFI_ESSID 0x46435745 /* "FCWE" */
#define FC_GUARD_STAGE_WIFI_DHCP  0x46435744 /* "FCWD" */
#define FC_GUARD_STAGE_NETREC     0x46434e52 /* "FCNR" */
#define FC_GUARD_STAGE_LLM        0x46434c4d /* "FCLM" */
#define FC_GUARD_STAGE_AGENT      0x46434147 /* "FCAG" */
#define FC_GUARD_STAGE_CLI        0x46434c49 /* "FCLI" */
#define FC_GUARD_STAGE_UPTIME     0x46435550 /* "FCUP" */
#define FC_GUARD_STAGE_HTTP       0x46434854 /* "FCHT" */
#define FC_GUARD_STAGE_HTTP_TOOL  0x46434855 /* "FCHU" */
#define FC_GUARD_STAGE_TELEGRAM   0x46435447 /* "FCTG" */
#define FC_GUARD_STAGE_SCHED      0x46435343 /* "FCSC" */
#define FC_GUARD_STAGE_FILE       0x4643464c /* "FCFL" */
#define FC_GUARD_STAGE_OPERATOR   0x46434f50 /* "FCOP" */

#define FC_MCP_DISPATCH_RESPONSE              0
#define FC_MCP_DISPATCH_NOTIFICATION_ACCEPTED 1
#define FC_MCP_DISPATCH_HTTP_ERROR            2

#define FC_LOGE(fmt, ...) fprintf(stderr, "fruitclaw[E] " fmt "\n", ##__VA_ARGS__)
#define FC_LOGW(fmt, ...) fprintf(stderr, "fruitclaw[W] " fmt "\n", ##__VA_ARGS__)
#define FC_LOGI(fmt, ...) fprintf(stdout, "fruitclaw[I] " fmt "\n", ##__VA_ARGS__)
#define FC_LOGD(fmt, ...) do { } while (0)

typedef struct fc_event_s
{
  char id[FC_ID_LEN];
  char source[FC_SOURCE_LEN];
  char type[FC_TYPE_LEN];
  char channel[FC_CHANNEL_LEN];
  char chat_id[FC_CHAT_ID_LEN];
  char session_id[FC_SESSION_ID_LEN];
  bool owner_mode;
  char text[CONFIG_FRUITCLAW_MAX_EVENT_TEXT];
  char payload_json[CONFIG_FRUITCLAW_MAX_JSON];
  int64_t ts_ms;
} fc_event_t;

typedef struct fc_queue_s
{
  pthread_mutex_t lock;
  pthread_cond_t cond;
  fc_event_t *items;
  unsigned int capacity;
  unsigned int head;
  unsigned int tail;
  unsigned int count;
  bool initialized;
} fc_queue_t;

typedef struct fc_tool_context_s
{
  char source[FC_SOURCE_LEN];
  char channel[FC_CHANNEL_LEN];
  char chat_id[FC_CHAT_ID_LEN];
  char session_id[FC_SESSION_ID_LEN];
  bool owner_mode;
  bool guarded;
} fc_tool_context_t;

typedef int (*fc_cap_exec_fn)(const fc_tool_context_t *ctx,
                              const char *args_json, char *out_json,
                              size_t out_json_len);

typedef struct fc_cap_s
{
  const char *id;
  const char *name;
  const char *description;
  const char *input_schema_json;
  bool visible_to_llm;
  bool requires_confirmation;
  fc_cap_exec_fn execute;
} fc_cap_t;

typedef struct fc_http_header_s
{
  const char *name;
  const char *value;
} fc_http_header_t;

typedef struct fc_http_headers_s
{
  fc_http_header_t items[FC_HTTP_HEADER_MAX];
  unsigned int count;
} fc_http_headers_t;

typedef struct fc_guard_long_s
{
  pthread_t thread;
  volatile bool stop;
  bool started;
  bool expired;
  bool reentrant;
  int fd;
  uint32_t stage;
  uint32_t timeout_ms;
  int64_t deadline_ms;
} fc_guard_long_t;

typedef struct fc_tool_call_s
{
  char id[96];
  char name[FC_TOOL_NAME_LEN];
  char args_json[CONFIG_FRUITCLAW_MAX_JSON];
} fc_tool_call_t;

typedef enum fc_schedule_type_e
{
  FC_SCHED_INTERVAL = 0,
  FC_SCHED_ONCE,
  FC_SCHED_CRON,
  FC_SCHED_BOOT
} fc_schedule_type_t;

typedef struct fc_schedule_s
{
  bool used;
  bool enabled;
  fc_schedule_type_t type;
  char id[48];
  char expr[48];
  char prompt[CONFIG_FRUITCLAW_MAX_EVENT_TEXT];
  char channel[FC_CHANNEL_LEN];
  char chat_id[FC_CHAT_ID_LEN];
  char session_id[FC_SESSION_ID_LEN];
  bool owner_mode;
  uint32_t every_sec;
  int64_t at_epoch;
  int64_t created_ms;
  int64_t next_ms;
  int last_minute_key;
} fc_schedule_t;

int64_t fc_time_ms(void);
int64_t fc_mono_ms(void);
void fc_make_id(char *out, size_t out_len, const char *prefix);
int fc_strlcpy(char *dst, const char *src, size_t dst_len);
void fc_trim(char *s);
int fc_mkdir_p(const char *path);
int fc_init_data_dir(void);
const char *fc_data_dir(void);
int fc_read_text_file(const char *path, char *out, size_t out_len,
                      bool trim);
int fc_write_text_file_atomic(const char *path, const char *text);
int fc_append_text_file(const char *path, const char *text);
int fc_data_path(const char *leaf, char *out, size_t out_len);
int fc_secret_path(const char *leaf, char *out, size_t out_len);
int fc_tls_ca_cert_path(char *out, size_t out_len);
int fc_guard_arm(uint32_t stage, int *guard_fd);
int fc_guard_disarm(int guard_fd);
int fc_guard_long_start(uint32_t stage, uint32_t timeout_ms,
                        fc_guard_long_t *guard);
void fc_guard_long_set_stage(fc_guard_long_t *guard, uint32_t stage);
int fc_guard_long_stop(fc_guard_long_t *guard);
int fc_guard_uptime_start(void);
int fc_guard_session_start(void);
void fc_guard_session_heartbeat(const char *source);
void fc_guard_session_stop(void);
int fc_guard_prepare_controlled_reboot(void);
int fc_guard_force_recovery(uint32_t stage);
int fc_guard_status(char *out, size_t out_len);
void fc_bootstrap_note(const char *stage, int ret);
void fc_operator_progress_mark(const char *source);
int64_t fc_operator_progress_age_ms(char *source, size_t source_len);
int fc_operator_progress_status_format(char *out, size_t out_len);
bool fc_path_has_parent_ref(const char *path);
bool fc_path_is_secret(const char *path);
bool fc_url_host_allowed(const char *url);
int fc_json_escape(const char *in, char *out, size_t out_len);
int fc_web_home_read(char *out, size_t out_len, bool *custom);
int fc_web_home_write(const char *markdown);
int fc_web_register_http(void);
int fc_web_httpd_status_format(char *out, size_t out_len);
int fc_web_httpd_status_json(char *out, size_t out_len);
int fc_web_httpd_activity_snapshot(unsigned long *sends,
                                   unsigned long *sent,
                                   unsigned long *send_fail,
                                   unsigned long *send_fail_streak,
                                   int *last_send_errno,
                                   int64_t *last_age_ms);

int fc_queue_init(fc_queue_t *q);
int fc_queue_publish(fc_queue_t *q, const fc_event_t *ev);
int fc_queue_receive(fc_queue_t *q, fc_event_t *ev, int timeout_ms);
unsigned int fc_queue_count(fc_queue_t *q);
fc_queue_t *fc_main_queue(void);
fc_queue_t *fc_agent_queue(void);

int fc_cap_init(void);
int fc_cap_register(const fc_cap_t *cap);
const fc_cap_t *fc_cap_find(const char *id);
int fc_cap_execute(const char *id, const char *args_json, char *out_json,
                   size_t out_len);
int fc_cap_execute_ctx(const fc_tool_context_t *ctx, const char *id,
                       const char *args_json, char *out_json,
                       size_t out_len);
int fc_cap_build_openai_tools_json(char *out_json, size_t out_len,
                                   bool llm_visible_only);
int fc_cap_build_mcp_tools_json(char *out_json, size_t out_len,
                                bool visible_only);
unsigned int fc_cap_count(bool visible_only);
void fc_cap_list(FILE *out);
void fc_cap_status(FILE *out);
void fc_cap_clear_status(void);
int fc_tool_name_to_openai(const char *id, char *out, size_t out_len);
int fc_tool_name_from_openai(const char *name, char *out, size_t out_len);
int fc_register_builtin_caps(void);
void fc_tool_context_from_event(const fc_event_t *ev, fc_tool_context_t *ctx);
void fc_tool_context_local(fc_tool_context_t *ctx);

int fc_memory_append(const char *source, const char *text);
int fc_memory_read_tail(size_t max_bytes, char *out, size_t out_len);
int fc_session_append(const char *session_id, const char *role,
                      const char *name, const char *content);
int fc_session_read_tail(const char *session_id, size_t max_bytes,
                         char *out, size_t out_len);
int fc_session_safe_filename(const char *session_id, char *out,
                             size_t out_len);

int fc_router_route_event(const fc_event_t *ev);
int fc_router_worker_start(void);
int fc_router_status_format(char *out, size_t out_len);
void fc_router_status(FILE *out);

int fc_agent_handle_event(const fc_event_t *ev, char *reply,
                          size_t reply_len);
int fc_agent_worker_start(void);
int fc_agent_status_format(char *out, size_t out_len);
void fc_agent_status(FILE *out);

int fc_http_get(const char *url, const fc_http_headers_t *headers,
                char *response, size_t response_len, long *status_code);
int fc_http_get_unlocked(const char *url, const fc_http_headers_t *headers,
                         char *response, size_t response_len,
                         long *status_code);
int fc_http_get_unlocked_timeout(uint32_t request_timeout_sec,
                                 const char *url,
                                 const fc_http_headers_t *headers,
                                 char *response, size_t response_len,
                                 long *status_code);
int fc_http_post_json(const char *url, const fc_http_headers_t *headers,
                      const char *json_body, char *response,
                      size_t response_len, long *status_code);
int fc_http_post_json_unlocked_timeout(uint32_t request_timeout_sec,
                                       const char *url,
                                       const fc_http_headers_t *headers,
                                       const char *json_body,
                                       char *response, size_t response_len,
                                       long *status_code);
int fc_http_get_guarded(uint32_t stage, uint32_t timeout_ms,
                        const char *url, const fc_http_headers_t *headers,
                        char *response, size_t response_len,
                        long *status_code);
int fc_http_get_guarded_timeout(uint32_t stage, uint32_t guard_timeout_ms,
                                uint32_t request_timeout_sec,
                                const char *url,
                                const fc_http_headers_t *headers,
                                char *response, size_t response_len,
                                long *status_code);
int fc_http_post_json_guarded(uint32_t stage, uint32_t timeout_ms,
                              const char *url,
                              const fc_http_headers_t *headers,
                              const char *json_body, char *response,
                              size_t response_len, long *status_code);
int fc_http_post_json_guarded_timeout(uint32_t stage,
                                      uint32_t guard_timeout_ms,
                                      uint32_t request_timeout_sec,
                                      const char *url,
                                      const fc_http_headers_t *headers,
                                      const char *json_body,
                                      char *response, size_t response_len,
                                      long *status_code);

int fc_telegram_poll_once(void);
int fc_telegram_send_message(const char *chat_id, const char *text);
int fc_telegram_notify_owner_async(const char *text);
int fc_telegram_inject_text(const char *chat_id, const char *text);
int fc_telegram_worker_start(void);
int fc_telegram_first_allowed_chat(char *out, size_t out_len);
int fc_telegram_discover_chats(char *out, size_t out_len);
bool fc_telegram_should_ack_offset(bool ignorable_update, int publish_ret);
bool fc_telegram_poll_stale(int64_t now_ms, int64_t timeout_ms);
int64_t fc_telegram_poll_age_ms(bool *active);
int64_t fc_telegram_last_success_age_ms(void);
int fc_telegram_status_format(char *out, size_t out_len);
void fc_telegram_status(FILE *out);

int fc_deepseek_chat(void *messages_array, void **out_message);
int fc_deepseek_test(char *out, size_t out_len);
int fc_deepseek_status_format(char *out, size_t out_len);
void fc_deepseek_status(FILE *out);

int fc_scheduler_load(void);
int fc_scheduler_save(void);
int fc_scheduler_add_interval(const char *id, uint32_t seconds,
                              const char *prompt);
int fc_scheduler_add_interval_ctx(const char *id, uint32_t seconds,
                                  const char *prompt,
                                  const fc_tool_context_t *ctx);
int fc_scheduler_add_cron(const char *id, const char *expr,
                          const char *prompt);
int fc_scheduler_add_cron_ctx(const char *id, const char *expr,
                              const char *prompt,
                              const fc_tool_context_t *ctx);
int fc_scheduler_add_once(const char *id, int64_t epoch,
                          const char *prompt);
int fc_scheduler_add_once_ctx(const char *id, int64_t epoch,
                              const char *prompt,
                              const fc_tool_context_t *ctx);
int fc_scheduler_add_boot(const char *id, const char *prompt);
int fc_scheduler_add_boot_ctx(const char *id, const char *prompt,
                              const fc_tool_context_t *ctx);
int fc_scheduler_remove(const char *id);
int fc_scheduler_list(char *out, size_t out_len);
int fc_scheduler_list_json(char *out, size_t out_len);
int fc_scheduler_worker_start(void);
int fc_scheduler_status_format(char *out, size_t out_len);
void fc_scheduler_status(FILE *out);
int fc_cron_validate(const char *expr);
bool fc_cron_matches(const char *expr, const struct tm *tm);

int fc_berry_run_file(const fc_tool_context_t *ctx, const char *path,
                      const char *args_json, char *out, size_t out_len);
int fc_berry_check_file(const char *path, char *out, size_t out_len);
int fc_berry_status_format(char *out, size_t out_len);
void fc_berry_clear_status(void);
void fc_berry_status(FILE *out);

int fc_builtin_terminal_run(const fc_tool_context_t *ctx,
                            const char *args_json, char *out,
                            size_t out_len);
int fc_builtin_neopixels_set(const fc_tool_context_t *ctx,
                             const char *args_json, char *out,
                             size_t out_len);

int fc_mcp_register_http(void);
int fc_mcp_status(FILE *out);
int fc_mcp_status_format(char *out, size_t out_len);
void fc_mcp_clear_status(void);
void fc_mcp_mark_activity(void);
bool fc_mcp_recently_active(int64_t now_ms, int64_t quiet_ms);
int fc_mcp_jsonrpc_dispatch(const char *body, size_t body_len, char *out,
                            size_t out_len, int *dispatch_status,
                            int *http_status,
                            const fc_tool_context_t *ctx);

int fc_selftest_main(void);
bool fc_operator_progress_stale(int64_t runtime_start_ms, int64_t now_ms,
                                int64_t progress_age_ms,
                                uint32_t timeout_ms);
int fc_network_recovery_status_format(char *out, size_t out_len);
int fc_webserver_status_format(char *out, size_t out_len);
int fc_services_status_format(char *out, size_t out_len);
int fc_services_status_json(char *out, size_t out_len);
int fc_service_control(const char *service_name, const char *action,
                       char *out, size_t out_len);
int fc_runtime_start(void);

#endif /* __APPS_SYSTEM_FRUITCLAW_INCLUDE_FRUITCLAW_H */
