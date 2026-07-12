/****************************************************************************
 * drivers/wireless/esp_hosted.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <net/if_arp.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/debug.h>
#include <nuttx/mm/iob.h>
#include <nuttx/mutex.h>
#include <nuttx/net/ethernet.h>
#include <nuttx/net/netdev_lowerhalf.h>
#include <nuttx/sched.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>
#include <nuttx/wireless/esp_hosted.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP_HOSTED_RESET_ASSERT_MS       10
#define ESP_HOSTED_RESET_RELEASE_MS      250
#define ESP_HOSTED_HANDSHAKE_POLL_US     1000
#define ESP_HOSTED_HANDSHAKE_POLLS       100
#define ESP_HOSTED_RX_WORK_DRAIN_LIMIT \
  CONFIG_ESP_HOSTED_RX_WORK_DRAIN_LIMIT
#define ESP_HOSTED_RX_POLL_DELAY_MS \
  CONFIG_ESP_HOSTED_RX_POLL_DELAY_MS
/* The ESP32-C6 data-ready IRQ is not always enough on the Fruit Jam SPI
 * transport: live gateway pings showed late ICMP replies and high loss while
 * TX reported success.  Keep a low-rate background RX poll so Telnet, HTTP,
 * MCP, and Telegram replies do not wait on a missed edge.
 */

#define ESP_HOSTED_RX_IDLE_POLL_DELAY_MS \
  CONFIG_ESP_HOSTED_RX_IDLE_POLL_DELAY_MS
#define ESP_HOSTED_RX_POLL_IRQ_BUDGET    4
#define ESP_HOSTED_RX_POLL_TX_BUDGET     4
#define ESP_HOSTED_RX_POLL_LINK_BUDGET   8
#define ESP_HOSTED_PUMP_DEFAULT_BUDGET   1
#define ESP_HOSTED_PUMP_MAX_BUDGET       4
#define ESP_HOSTED_NETDEV_PRIORITY       225

#define ESP_HOSTED_HEADER_LEN            \
  (sizeof(struct esp_hosted_payload_header_s))

#define ESP_HOSTED_MAX_PAYLOAD           \
  (ESP_HOSTED_SPI_MAX_FRAME - ESP_HOSTED_HEADER_LEN)

#define ESP_HOSTED_PRIV_EVENT_HDR_LEN    2

#define ESP_HOSTED_RPC_TYPE_REQ          1
#define ESP_HOSTED_RPC_TYPE_RESP         2
#define ESP_HOSTED_RPC_TYPE_EVENT        3

#define ESP_HOSTED_PB_WIRE_VARINT        0
#define ESP_HOSTED_PB_WIRE_64BIT         1
#define ESP_HOSTED_PB_WIRE_LENGTH        2
#define ESP_HOSTED_PB_WIRE_32BIT         5

#define ESP_HOSTED_RPC_FIELD_MSG_TYPE    1
#define ESP_HOSTED_RPC_FIELD_MSG_ID      2
#define ESP_HOSTED_RPC_FIELD_UID         3

#define ESP_HOSTED_PSER_TLV_TYPE_EPNAME  1
#define ESP_HOSTED_PSER_TLV_TYPE_DATA    2
#define ESP_HOSTED_PSER_TLV_LEN_SIZE     2
#define ESP_HOSTED_PSER_TLV_MAX_EP       16
#define ESP_HOSTED_PSER_TLV_OVERHEAD     \
  (1 + ESP_HOSTED_PSER_TLV_LEN_SIZE + ESP_HOSTED_PSER_TLV_MAX_EP + \
   1 + ESP_HOSTED_PSER_TLV_LEN_SIZE)

#define ESP_HOSTED_WIFI_IF_STA           0
#define ESP_HOSTED_RPC_PAYLOAD_MAX       512
#define ESP_HOSTED_RPC_TLV_PAYLOAD_MAX   \
  (ESP_HOSTED_RPC_PAYLOAD_MAX + ESP_HOSTED_PSER_TLV_OVERHEAD)
#define ESP_HOSTED_IDF_TARGET_MAX        16
#define ESP_HOSTED_NETDEV_RX_QUOTA       64
#define ESP_HOSTED_NETDEV_TX_QUOTA       8

/* Leave room for one synchronous full-duplex TX burst after RX polling
 * reaches its high-water mark.
 */

#define ESP_HOSTED_RX_QUEUE_HIGH_WATER   16
#define ESP_HOSTED_WIFI_PASSWORD_MAX     64
#define ESP_HOSTED_WIFI_MODE_STA         1
#define ESP_HOSTED_WIFI_INIT_MAGIC       0x1f2f3f4f
#define ESP_HOSTED_WIFI_INIT_STATIC_RX   10
#define ESP_HOSTED_WIFI_INIT_DYNAMIC_RX  32
#define ESP_HOSTED_WIFI_INIT_TX_TYPE     1
#define ESP_HOSTED_WIFI_INIT_DYNAMIC_TX  32
#define ESP_HOSTED_WIFI_INIT_RX_MGMT_NUM 5
#define ESP_HOSTED_WIFI_INIT_AMPDU_RX    1
#define ESP_HOSTED_WIFI_INIT_AMPDU_TX    1
#define ESP_HOSTED_WIFI_INIT_NVS         1
#define ESP_HOSTED_WIFI_INIT_RX_BA_WIN   6
#define ESP_HOSTED_WIFI_INIT_BEACON_MAX  752
#define ESP_HOSTED_WIFI_INIT_MGMT_SBUF   32
#define ESP_HOSTED_WIFI_INIT_ESPNOW_MAX  7
#define ESP_HOSTED_WIFI_INIT_TX_HETB_NUM 3
#define ESP_HOSTED_WIFI_AUTH_OPEN        0
#define ESP_HOSTED_WIFI_AUTH_WPA_PSK     2
#define ESP_HOSTED_WIFI_AUTH_WPA2_PSK    3
#define ESP_HOSTED_WIFI_AUTH_WPA_WPA2    4
#define ESP_HOSTED_WIFI_AUTH_WPA3_PSK    6
#define ESP_HOSTED_WIFI_AUTH_WPA2_WPA3   7
#define ESP_HOSTED_SCAN_MAX_APS          12

#ifdef CONFIG_ESP_HOSTED_LPWORK
#  define ESP_HOSTED_WORK_QUEUE          LPWORK
#else
#  define ESP_HOSTED_WORK_QUEUE          HPWORK
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct esp_hosted_rpc_message_s
{
  uint32_t msg_type;
  uint32_t msg_id;
  uint32_t uid;
  uint32_t payload_field;
  FAR const uint8_t *payload;
  size_t payload_len;
};

struct esp_hosted_fwversion_s
{
  int32_t resp;
  uint32_t major;
  uint32_t minor;
  uint32_t patch;
  int32_t revision;
  int32_t prerelease;
  int32_t build;
  uint32_t chip_id;
  char idf_target[ESP_HOSTED_IDF_TARGET_MAX];
};

#ifdef CONFIG_ESP_HOSTED_WLAN
struct esp_hosted_scan_record_s
{
  uint8_t bssid[ESP_HOSTED_MAC_SIZE];
  char ssid[IW_ESSID_MAX_SIZE + 1];
  uint8_t ssid_len;
  uint8_t channel;
  int32_t rssi;
  int32_t authmode;
};
#endif

struct esp_hosted_driver_s
{
#ifdef CONFIG_ESP_HOSTED_WLAN
  struct netdev_lowerhalf_s lower;
#endif
  struct esp_hosted_config_s config;
  struct esp_hosted_stats_s stats;
  struct work_s rx_work;
  struct work_s poll_work;
  mutex_t lock;
#ifdef CONFIG_ESP_HOSTED_WLAN
  netpkt_queue_t rx_queue;
  spinlock_t rx_lock;
#endif
  struct esp_hosted_fwversion_s fwversion;
  uint32_t rpc_uid;
  volatile bool dataready_seen;
  bool init_seen;
  bool recovering;
  bool startup_probe_pending;
  bool startup_probe_sent;
  bool have_mac;
  bool have_fwversion;
#ifdef CONFIG_ESP_HOSTED_WLAN
  bool mac_probe_sent;
  bool wlan_registered;
  bool ifup;
  bool carrier_on;
  bool wlan_control_start_pending;
  bool wlan_control_started;
  uint8_t rx_poll_budget;
  bool rx_force_poll;
  bool have_ssid;
  bool scan_in_progress;
  bool scan_done;
  bool scan_records_valid;
  uint32_t auth_wpa;
  uint32_t cipher_pairwise;
  uint32_t cipher_group;
  uint32_t scan_status;
  uint16_t scan_ap_available;
  uint16_t scan_result_count;
  uint16_t scan_request_count;
  uint8_t ssid_len;
  uint8_t passphrase_len;
  char ssid[IW_ESSID_MAX_SIZE + 1];
  char passphrase[ESP_HOSTED_WIFI_PASSWORD_MAX + 1];
  struct esp_hosted_scan_record_s scan_records[ESP_HOSTED_SCAN_MAX_APS];
#endif
  uint16_t seq_num;
  uint8_t mac[ESP_HOSTED_MAC_SIZE];
  uint8_t rpc_payload[ESP_HOSTED_RPC_PAYLOAD_MAX];
  uint8_t rpc_tlv_payload[ESP_HOSTED_RPC_TLV_PAYLOAD_MAX];
#ifdef CONFIG_ESP_HOSTED_WLAN
  uint8_t netdev_tx_payload[ESP_HOSTED_MAX_PAYLOAD];
#endif
  uint8_t tx_frame[ESP_HOSTED_SPI_MAX_FRAME];
  uint8_t rx_frame[ESP_HOSTED_SPI_MAX_FRAME];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct esp_hosted_driver_s g_esp_hosted;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void esp_hosted_rx_work(FAR void *arg);
static void esp_hosted_rx_poll_work(FAR void *arg);
static int esp_hosted_enable_irq(FAR struct esp_hosted_driver_s *priv,
                                 bool enable);
static int esp_hosted_send_rpc_request(FAR struct esp_hosted_driver_s *priv,
                                       uint32_t request_id);

#ifdef CONFIG_ESP_HOSTED_WLAN
static int esp_hosted_run_wlan_control_start(
  FAR struct esp_hosted_driver_s *priv);
static int esp_hosted_wlan_ifup(FAR struct netdev_lowerhalf_s *dev);
static int esp_hosted_wlan_ifdown(FAR struct netdev_lowerhalf_s *dev);
static int esp_hosted_wlan_transmit(FAR struct netdev_lowerhalf_s *dev,
                                    FAR netpkt_t *pkt);
static FAR netpkt_t *esp_hosted_wlan_receive(
  FAR struct netdev_lowerhalf_s *dev);
#  ifdef CONFIG_NETDEV_IOCTL
static int esp_hosted_wlan_ioctl(FAR struct netdev_lowerhalf_s *dev,
                                 int cmd, unsigned long arg);
#  endif
static void esp_hosted_wlan_reclaim(FAR struct netdev_lowerhalf_s *dev);
static int esp_hosted_wlan_connect(FAR struct netdev_lowerhalf_s *dev);
static int esp_hosted_wlan_disconnect(FAR struct netdev_lowerhalf_s *dev);
static int esp_hosted_wlan_essid(FAR struct netdev_lowerhalf_s *dev,
                                 FAR struct iwreq *iwr, bool set);
static int esp_hosted_wlan_passwd(FAR struct netdev_lowerhalf_s *dev,
                                  FAR struct iwreq *iwr, bool set);
static int esp_hosted_wlan_auth(FAR struct netdev_lowerhalf_s *dev,
                                FAR struct iwreq *iwr, bool set);
static int esp_hosted_wlan_scan(FAR struct netdev_lowerhalf_s *dev,
                                FAR struct iwreq *iwr, bool set);
static int esp_hosted_wlan_range(FAR struct netdev_lowerhalf_s *dev,
                                 FAR struct iwreq *iwr);
static void esp_hosted_arm_rx_poll(FAR struct esp_hosted_driver_s *priv,
                                   uint8_t budget);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifdef CONFIG_ESP_HOSTED_WLAN
static const struct netdev_ops_s g_esp_hosted_netdev_ops =
{
  .ifup    = esp_hosted_wlan_ifup,
  .ifdown  = esp_hosted_wlan_ifdown,
  .transmit = esp_hosted_wlan_transmit,
  .receive = esp_hosted_wlan_receive,
#  ifdef CONFIG_NETDEV_IOCTL
  .ioctl   = esp_hosted_wlan_ioctl,
#  endif
  .reclaim = esp_hosted_wlan_reclaim,
};

static const struct wireless_ops_s g_esp_hosted_wireless_ops =
{
  .connect    = esp_hosted_wlan_connect,
  .disconnect = esp_hosted_wlan_disconnect,
  .essid      = esp_hosted_wlan_essid,
  .passwd     = esp_hosted_wlan_passwd,
  .auth       = esp_hosted_wlan_auth,
  .scan       = esp_hosted_wlan_scan,
  .range      = esp_hosted_wlan_range,
};
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int esp_hosted_pb_put_varint(FAR uint8_t *buf, size_t buflen,
                                    uint64_t value)
{
  size_t offset = 0;

  do
    {
      uint8_t byte;

      if (offset >= buflen)
        {
          return -ENOSPC;
        }

      byte = value & 0x7f;
      value >>= 7;

      if (value != 0)
        {
          byte |= 0x80;
        }

      buf[offset++] = byte;
    }
  while (value != 0);

  return offset;
}

static int esp_hosted_pb_append_varint(FAR uint8_t *buf, size_t buflen,
                                       FAR size_t *offset, uint64_t value)
{
  int ret;

  ret = esp_hosted_pb_put_varint(buf + *offset, buflen - *offset, value);
  if (ret < 0)
    {
      return ret;
    }

  *offset += ret;
  return OK;
}

static int esp_hosted_pb_append_key(FAR uint8_t *buf, size_t buflen,
                                    FAR size_t *offset, uint32_t field,
                                    uint8_t wire_type)
{
  return esp_hosted_pb_append_varint(buf, buflen, offset,
                                     ((uint64_t)field << 3) | wire_type);
}

static int esp_hosted_pb_append_varint_field(FAR uint8_t *buf, size_t buflen,
                                             FAR size_t *offset,
                                             uint32_t field, uint64_t value)
{
  int ret;

  ret = esp_hosted_pb_append_key(buf, buflen, offset, field,
                                 ESP_HOSTED_PB_WIRE_VARINT);
  if (ret < 0)
    {
      return ret;
    }

  return esp_hosted_pb_append_varint(buf, buflen, offset, value);
}

static int esp_hosted_pb_append_bytes_field(FAR uint8_t *buf, size_t buflen,
                                            FAR size_t *offset,
                                            uint32_t field,
                                            FAR const uint8_t *value,
                                            size_t value_len)
{
  int ret;

  ret = esp_hosted_pb_append_key(buf, buflen, offset, field,
                                 ESP_HOSTED_PB_WIRE_LENGTH);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint(buf, buflen, offset, value_len);
  if (ret < 0)
    {
      return ret;
    }

  if (value_len > buflen - *offset)
    {
      return -ENOSPC;
    }

  if (value_len > 0)
    {
      memcpy(buf + *offset, value, value_len);
      *offset += value_len;
    }

  return OK;
}

static int esp_hosted_pb_get_varint(FAR const uint8_t *buf, size_t len,
                                    FAR size_t *offset,
                                    FAR uint64_t *value)
{
  uint64_t result = 0;
  unsigned int shift = 0;

  while (*offset < len && shift < 64)
    {
      uint8_t byte = buf[(*offset)++];

      result |= ((uint64_t)(byte & 0x7f)) << shift;
      if ((byte & 0x80) == 0)
        {
          *value = result;
          return OK;
        }

      shift += 7;
    }

  return -EINVAL;
}

static int esp_hosted_pb_skip_field(FAR const uint8_t *buf, size_t len,
                                    FAR size_t *offset, uint8_t wire_type)
{
  uint64_t value;
  int ret;

  switch (wire_type)
    {
      case ESP_HOSTED_PB_WIRE_VARINT:
        return esp_hosted_pb_get_varint(buf, len, offset, &value);

      case ESP_HOSTED_PB_WIRE_64BIT:
        if (len - *offset < 8)
          {
            return -EINVAL;
          }

        *offset += 8;
        return OK;

      case ESP_HOSTED_PB_WIRE_LENGTH:
        ret = esp_hosted_pb_get_varint(buf, len, offset, &value);
        if (ret < 0 || value > len - *offset)
          {
            return -EINVAL;
          }

        *offset += value;
        return OK;

      case ESP_HOSTED_PB_WIRE_32BIT:
        if (len - *offset < 4)
          {
            return -EINVAL;
          }

        *offset += 4;
        return OK;

      default:
        return -EINVAL;
    }
}

static int esp_hosted_pb_get_bytes(FAR const uint8_t *buf, size_t len,
                                   FAR size_t *offset,
                                   FAR const uint8_t **value,
                                   FAR size_t *value_len)
{
  uint64_t field_len;
  int ret;

  ret = esp_hosted_pb_get_varint(buf, len, offset, &field_len);
  if (ret < 0 || field_len > len - *offset)
    {
      return -EINVAL;
    }

  *value = buf + *offset;
  *value_len = field_len;
  *offset += field_len;
  return OK;
}

static int32_t esp_hosted_pb_int32(uint64_t value)
{
  return (int32_t)(uint32_t)value;
}

static void esp_hosted_put_le16(FAR uint8_t *buf, uint16_t value)
{
  buf[0] = value & 0xff;
  buf[1] = value >> 8;
}

static uint16_t esp_hosted_get_le16(FAR const uint8_t *buf)
{
  return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static int esp_hosted_pserial_wrap(FAR struct esp_hosted_driver_s *priv,
                                   FAR const char *epname,
                                   FAR const uint8_t *payload,
                                   size_t payload_len,
                                   FAR uint8_t *out,
                                   size_t out_size,
                                   FAR size_t *out_len)
{
  size_t ep_len;
  size_t total_len;
  size_t offset = 0;

  ep_len = strlen(epname);
  if (ep_len == 0 || ep_len > UINT16_MAX ||
      ep_len > ESP_HOSTED_PSER_TLV_MAX_EP || payload_len > UINT16_MAX)
    {
      priv->stats.rpc_tlv_error_count++;
      return -EINVAL;
    }

  total_len = 1 + ESP_HOSTED_PSER_TLV_LEN_SIZE + ep_len +
              1 + ESP_HOSTED_PSER_TLV_LEN_SIZE + payload_len;
  if (total_len > out_size)
    {
      priv->stats.rpc_tlv_error_count++;
      return -ENOSPC;
    }

  out[offset++] = ESP_HOSTED_PSER_TLV_TYPE_EPNAME;
  esp_hosted_put_le16(&out[offset], ep_len);
  offset += ESP_HOSTED_PSER_TLV_LEN_SIZE;
  memcpy(&out[offset], epname, ep_len);
  offset += ep_len;

  out[offset++] = ESP_HOSTED_PSER_TLV_TYPE_DATA;
  esp_hosted_put_le16(&out[offset], payload_len);
  offset += ESP_HOSTED_PSER_TLV_LEN_SIZE;
  memcpy(&out[offset], payload, payload_len);
  offset += payload_len;

  *out_len = offset;
  priv->stats.rpc_tlv_tx_count++;
  return OK;
}

static int esp_hosted_pserial_unwrap(FAR struct esp_hosted_driver_s *priv,
                                     FAR const uint8_t *payload,
                                     size_t payload_len,
                                     FAR const uint8_t **rpc_payload,
                                     FAR size_t *rpc_len)
{
  FAR const uint8_t *data = NULL;
  char epname[ESP_HOSTED_PSER_TLV_MAX_EP + 1];
  size_t data_len = 0;
  size_t offset = 0;
  bool have_epname = false;
  bool have_data = false;

  memset(epname, 0, sizeof(epname));

  while (offset < payload_len)
    {
      uint8_t type;
      uint16_t len;

      if (payload_len - offset < 1 + ESP_HOSTED_PSER_TLV_LEN_SIZE)
        {
          priv->stats.rpc_tlv_error_count++;
          return -EINVAL;
        }

      type = payload[offset++];
      len = esp_hosted_get_le16(&payload[offset]);
      offset += ESP_HOSTED_PSER_TLV_LEN_SIZE;

      if (len > payload_len - offset)
        {
          priv->stats.rpc_tlv_error_count++;
          return -EINVAL;
        }

      switch (type)
        {
          case ESP_HOSTED_PSER_TLV_TYPE_EPNAME:
            if (len == 0 || len > ESP_HOSTED_PSER_TLV_MAX_EP)
              {
                priv->stats.rpc_tlv_error_count++;
                return -EINVAL;
              }

            memcpy(epname, &payload[offset], len);
            epname[len] = '\0';
            have_epname = true;
            break;

          case ESP_HOSTED_PSER_TLV_TYPE_DATA:
            data = &payload[offset];
            data_len = len;
            have_data = true;
            break;

          default:
            priv->stats.rpc_tlv_error_count++;
            return -EINVAL;
        }

      offset += len;
    }

  if (!have_epname || !have_data || data_len == 0 ||
      (strcmp(epname, ESP_HOSTED_RPC_EP_NAME_RSP) != 0 &&
       strcmp(epname, ESP_HOSTED_RPC_EP_NAME_EVT) != 0))
    {
      priv->stats.rpc_tlv_error_count++;
      nwarn("ESP-Hosted invalid pserial TLV: ep=%s have_data=%d len=%zu\n",
            have_epname ? epname : "<none>", have_data, data_len);
      return -EINVAL;
    }

  *rpc_payload = data;
  *rpc_len = data_len;
  priv->stats.rpc_tlv_rx_count++;
  return OK;
}

static uint32_t esp_hosted_next_rpc_uid(FAR struct esp_hosted_driver_s *priv)
{
  priv->rpc_uid++;
  if (priv->rpc_uid == 0)
    {
      priv->rpc_uid++;
    }

  return priv->rpc_uid;
}

#ifdef CONFIG_ESP_HOSTED_WLAN
static uint32_t esp_hosted_wlan_authmode(
  FAR const struct esp_hosted_driver_s *priv)
{
  if (priv->passphrase_len == 0 ||
      priv->auth_wpa == IW_AUTH_WPA_VERSION_DISABLED)
    {
      return ESP_HOSTED_WIFI_AUTH_OPEN;
    }

  if ((priv->auth_wpa & IW_AUTH_WPA_VERSION_WPA3) != 0 &&
      (priv->auth_wpa & IW_AUTH_WPA_VERSION_WPA2) != 0)
    {
      return ESP_HOSTED_WIFI_AUTH_WPA2_WPA3;
    }

  if ((priv->auth_wpa & IW_AUTH_WPA_VERSION_WPA3) != 0)
    {
      return ESP_HOSTED_WIFI_AUTH_WPA3_PSK;
    }

  if ((priv->auth_wpa & IW_AUTH_WPA_VERSION_WPA2) != 0 &&
      (priv->auth_wpa & IW_AUTH_WPA_VERSION_WPA) != 0)
    {
      return ESP_HOSTED_WIFI_AUTH_WPA_WPA2;
    }

  if ((priv->auth_wpa & IW_AUTH_WPA_VERSION_WPA) != 0)
    {
      return ESP_HOSTED_WIFI_AUTH_WPA_PSK;
    }

  return ESP_HOSTED_WIFI_AUTH_WPA2_PSK;
}

static int esp_hosted_build_wifi_init_payload(FAR uint8_t *payload,
                                              size_t payload_size,
                                              FAR size_t *payload_len)
{
  uint8_t cfg_payload[128];
  size_t cfg_len = 0;
  size_t offset = 0;
  int ret;

  ret = esp_hosted_pb_append_varint_field(cfg_payload, sizeof(cfg_payload),
                                          &cfg_len, 1,
                                          ESP_HOSTED_WIFI_INIT_STATIC_RX);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(cfg_payload, sizeof(cfg_payload),
                                          &cfg_len, 2,
                                          ESP_HOSTED_WIFI_INIT_DYNAMIC_RX);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(cfg_payload, sizeof(cfg_payload),
                                          &cfg_len, 3,
                                          ESP_HOSTED_WIFI_INIT_TX_TYPE);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(cfg_payload, sizeof(cfg_payload),
                                          &cfg_len, 5,
                                          ESP_HOSTED_WIFI_INIT_DYNAMIC_TX);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(cfg_payload, sizeof(cfg_payload),
                                          &cfg_len, 7, 0);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(cfg_payload, sizeof(cfg_payload),
                                          &cfg_len, 8,
                                          ESP_HOSTED_WIFI_INIT_AMPDU_RX);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(cfg_payload, sizeof(cfg_payload),
                                          &cfg_len, 9,
                                          ESP_HOSTED_WIFI_INIT_AMPDU_TX);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(cfg_payload, sizeof(cfg_payload),
                                          &cfg_len, 11,
                                          ESP_HOSTED_WIFI_INIT_NVS);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(cfg_payload, sizeof(cfg_payload),
                                          &cfg_len, 13,
                                          ESP_HOSTED_WIFI_INIT_RX_BA_WIN);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(cfg_payload, sizeof(cfg_payload),
                                          &cfg_len, 15,
                                          ESP_HOSTED_WIFI_INIT_BEACON_MAX);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(cfg_payload, sizeof(cfg_payload),
                                          &cfg_len, 16,
                                          ESP_HOSTED_WIFI_INIT_MGMT_SBUF);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(cfg_payload, sizeof(cfg_payload),
                                          &cfg_len, 18, 1);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(cfg_payload, sizeof(cfg_payload),
                                          &cfg_len, 19,
                                          ESP_HOSTED_WIFI_INIT_ESPNOW_MAX);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(cfg_payload, sizeof(cfg_payload),
                                          &cfg_len, 20,
                                          ESP_HOSTED_WIFI_INIT_MAGIC);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(cfg_payload, sizeof(cfg_payload),
                                          &cfg_len, 21, 0);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(cfg_payload, sizeof(cfg_payload),
                                          &cfg_len, 22,
                                          ESP_HOSTED_WIFI_INIT_RX_MGMT_NUM);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(cfg_payload, sizeof(cfg_payload),
                                          &cfg_len, 23,
                                          ESP_HOSTED_WIFI_INIT_TX_HETB_NUM);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_bytes_field(payload, payload_size, &offset, 1,
                                         cfg_payload, cfg_len);
  if (ret < 0)
    {
      return ret;
    }

  *payload_len = offset;
  return OK;
}

static int esp_hosted_build_wifi_set_config_payload(
  FAR struct esp_hosted_driver_s *priv, FAR uint8_t *payload,
  size_t payload_size, FAR size_t *payload_len)
{
  uint8_t sta_payload[160];
  uint8_t config_payload[192];
  uint8_t threshold_payload[16];
  size_t sta_len = 0;
  size_t config_len = 0;
  size_t threshold_len = 0;
  size_t offset = 0;
  int ret;

  if (!priv->have_ssid || priv->ssid_len == 0)
    {
      return -ENOTCONN;
    }

  ret = esp_hosted_pb_append_bytes_field(sta_payload, sizeof(sta_payload),
                                         &sta_len, 1,
                                         (FAR const uint8_t *)priv->ssid,
                                         priv->ssid_len);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->passphrase_len > 0)
    {
      ret = esp_hosted_pb_append_bytes_field(sta_payload,
                                             sizeof(sta_payload), &sta_len,
                                             2,
                                             (FAR const uint8_t *)
                                             priv->passphrase,
                                             priv->passphrase_len);
      if (ret < 0)
        {
          return ret;
        }
    }

  ret = esp_hosted_pb_append_varint_field(sta_payload, sizeof(sta_payload),
                                          &sta_len, 13, 3);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(threshold_payload,
                                          sizeof(threshold_payload),
                                          &threshold_len, 2,
                                          esp_hosted_wlan_authmode(priv));
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_bytes_field(sta_payload, sizeof(sta_payload),
                                         &sta_len, 9, threshold_payload,
                                         threshold_len);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_bytes_field(config_payload,
                                         sizeof(config_payload),
                                         &config_len, 2,
                                         sta_payload, sta_len);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(payload, payload_size, &offset, 1,
                                          ESP_HOSTED_WIFI_IF_STA);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_bytes_field(payload, payload_size, &offset, 2,
                                         config_payload, config_len);
  if (ret < 0)
    {
      return ret;
    }

  *payload_len = offset;
  return OK;
}
#endif

#ifdef CONFIG_ESP_HOSTED_WLAN
static void esp_hosted_wlan_clear_rx_queue(
  FAR struct esp_hosted_driver_s *priv)
{
  FAR netpkt_t *pkt;
  irqstate_t flags;

  flags = spin_lock_irqsave(&priv->rx_lock);
  while ((pkt = netpkt_remove_queue(&priv->rx_queue)) != NULL)
    {
      if (priv->stats.current_rx_queue_depth > 0)
        {
          priv->stats.current_rx_queue_depth--;
        }

      spin_unlock_irqrestore(&priv->rx_lock, flags);
      netpkt_free(&priv->lower, pkt, NETPKT_RX);
      flags = spin_lock_irqsave(&priv->rx_lock);
    }

  priv->stats.current_rx_queue_depth = 0;
  spin_unlock_irqrestore(&priv->rx_lock, flags);
}

static uint32_t esp_hosted_rx_queue_depth(
  FAR struct esp_hosted_driver_s *priv)
{
  irqstate_t flags;
  uint32_t depth;

  flags = spin_lock_irqsave(&priv->rx_lock);
  depth = priv->stats.current_rx_queue_depth;
  spin_unlock_irqrestore(&priv->rx_lock, flags);
  return depth;
}

static int esp_hosted_try_register_wlan(FAR struct esp_hosted_driver_s *priv)
{
  FAR struct net_driver_s *netdev;
  int ret;

  if (priv->wlan_registered || !priv->have_mac || !priv->have_fwversion)
    {
      return OK;
    }

  priv->lower.ops = &g_esp_hosted_netdev_ops;
#ifdef CONFIG_NETDEV_WIRELESS_HANDLER
  priv->lower.iw_ops = &g_esp_hosted_wireless_ops;
#endif
  priv->lower.quota[NETPKT_RX] = ESP_HOSTED_NETDEV_RX_QUOTA;
  priv->lower.quota[NETPKT_TX] = ESP_HOSTED_NETDEV_TX_QUOTA;
  priv->lower.rxtype = NETDEV_RX_THREAD;
  priv->lower.priority = ESP_HOSTED_NETDEV_PRIORITY;

  netdev = &priv->lower.netdev;
  memcpy(netdev->d_mac.ether.ether_addr_octet, priv->mac,
         ESP_HOSTED_MAC_SIZE);

  ret = netdev_lower_register(&priv->lower, NET_LL_IEEE80211);
  if (ret < 0)
    {
      priv->stats.wlan_register_error_count++;
      nwarn("ESP-Hosted wlan netdev register failed: %d\n", ret);
      return ret;
    }

  priv->wlan_registered = true;
  priv->wlan_control_start_pending = true;
  priv->stats.wlan_register_count++;

  netdev_lower_carrier_off(&priv->lower);

  ninfo("ESP-Hosted wlan netdev registered: "
        "%02x:%02x:%02x:%02x:%02x:%02x\n",
        priv->mac[0], priv->mac[1], priv->mac[2],
        priv->mac[3], priv->mac[4], priv->mac[5]);

  return OK;
}

static int esp_hosted_queue_sta_frame(FAR struct esp_hosted_driver_s *priv,
                                      FAR const uint8_t *payload,
                                      uint16_t len)
{
  FAR netpkt_t *pkt;
  irqstate_t flags;
  int ret;

  if (!priv->wlan_registered)
    {
      priv->stats.netdev_rx_dropped_count++;
      return OK;
    }

  if (len >= ETH_HDRLEN &&
      memcmp(((FAR const struct eth_hdr_s *)payload)->src, priv->mac,
             ESP_HOSTED_MAC_SIZE) == 0)
    {
      priv->stats.netdev_rx_echo_dropped_count++;
      return OK;
    }

  pkt = netpkt_alloc(&priv->lower, NETPKT_RX);
  if (pkt == NULL)
    {
      priv->stats.netdev_rx_dropped_count++;
      return -ENOMEM;
    }

  ret = netpkt_copyin(&priv->lower, pkt, payload, len, 0);
  if (ret < 0)
    {
      priv->stats.netdev_rx_dropped_count++;
      netpkt_free(&priv->lower, pkt, NETPKT_RX);
      return ret;
    }

  flags = spin_lock_irqsave(&priv->rx_lock);
  ret = netpkt_tryadd_queue(pkt, &priv->rx_queue);
  if (ret >= 0)
    {
      priv->stats.current_rx_queue_depth++;
      if (priv->stats.current_rx_queue_depth >
          priv->stats.rx_queue_max_depth)
        {
          priv->stats.rx_queue_max_depth =
            priv->stats.current_rx_queue_depth;
        }
    }

  spin_unlock_irqrestore(&priv->rx_lock, flags);
  if (ret < 0)
    {
      priv->stats.netdev_rx_dropped_count++;
      netpkt_free(&priv->lower, pkt, NETPKT_RX);
      return ret;
    }

  priv->stats.netdev_rx_count++;
  priv->stats.last_rx_ticks = clock_systime_ticks();
  netdev_lower_rxready(&priv->lower);
  return OK;
}
#endif

static int esp_hosted_parse_rpc_message(FAR const uint8_t *payload,
                                        uint16_t len,
                                        FAR struct esp_hosted_rpc_message_s
                                        *msg)
{
  size_t offset = 0;
  int ret;

  memset(msg, 0, sizeof(*msg));

  while (offset < len)
    {
      uint64_t key;
      uint32_t field;
      uint8_t wire_type;

      ret = esp_hosted_pb_get_varint(payload, len, &offset, &key);
      if (ret < 0 || key == 0)
        {
          return -EINVAL;
        }

      field = key >> 3;
      wire_type = key & 0x07;

      switch (field)
        {
          case ESP_HOSTED_RPC_FIELD_MSG_TYPE:
          case ESP_HOSTED_RPC_FIELD_MSG_ID:
          case ESP_HOSTED_RPC_FIELD_UID:
            {
              uint64_t value;

              if (wire_type != ESP_HOSTED_PB_WIRE_VARINT)
                {
                  return -EINVAL;
                }

              ret = esp_hosted_pb_get_varint(payload, len, &offset, &value);
              if (ret < 0)
                {
                  return ret;
                }

              if (field == ESP_HOSTED_RPC_FIELD_MSG_TYPE)
                {
                  msg->msg_type = value;
                }
              else if (field == ESP_HOSTED_RPC_FIELD_MSG_ID)
                {
                  msg->msg_id = value;
                }
              else
                {
                  msg->uid = value;
                }
            }
            break;

          default:
            if (wire_type == ESP_HOSTED_PB_WIRE_LENGTH && field >= 256)
              {
                uint64_t value_len;

                ret = esp_hosted_pb_get_varint(payload, len, &offset,
                                               &value_len);
                if (ret < 0 || value_len > len - offset)
                  {
                    return -EINVAL;
                  }

                msg->payload_field = field;
                msg->payload = payload + offset;
                msg->payload_len = value_len;
                offset += value_len;
            }
            else
              {
                ret = esp_hosted_pb_skip_field(payload, len, &offset,
                                               wire_type);
                if (ret < 0)
                  {
                    return ret;
                  }
              }
            break;
        }
    }

  return OK;
}

static int esp_hosted_parse_mac_response(FAR struct esp_hosted_driver_s *priv,
                                         FAR const uint8_t *payload,
                                         size_t len)
{
  size_t offset = 0;
  int32_t resp = 0;
  bool have_mac = false;
  uint8_t mac[ESP_HOSTED_MAC_SIZE];
  int ret;

  memset(mac, 0, sizeof(mac));

  while (offset < len)
    {
      uint64_t key;
      uint32_t field;
      uint8_t wire_type;

      ret = esp_hosted_pb_get_varint(payload, len, &offset, &key);
      if (ret < 0 || key == 0)
        {
          return -EINVAL;
        }

      field = key >> 3;
      wire_type = key & 0x07;

      switch (field)
        {
          case 1:
            {
              uint64_t value_len;

              if (wire_type != ESP_HOSTED_PB_WIRE_LENGTH)
                {
                  return -EINVAL;
                }

              ret = esp_hosted_pb_get_varint(payload, len, &offset,
                                             &value_len);
              if (ret < 0 || value_len > len - offset ||
                  value_len < ESP_HOSTED_MAC_SIZE)
                {
                  return -EINVAL;
                }

              memcpy(mac, payload + offset, ESP_HOSTED_MAC_SIZE);
              offset += value_len;
              have_mac = true;
            }
            break;

          case 2:
            {
              uint64_t value;

              if (wire_type != ESP_HOSTED_PB_WIRE_VARINT)
                {
                  return -EINVAL;
                }

              ret = esp_hosted_pb_get_varint(payload, len, &offset, &value);
              if (ret < 0)
                {
                  return ret;
                }

              resp = esp_hosted_pb_int32(value);
            }
            break;

          default:
            ret = esp_hosted_pb_skip_field(payload, len, &offset, wire_type);
            if (ret < 0)
              {
                return ret;
              }
            break;
        }
    }

  if (!have_mac)
    {
      if (resp != 0)
        {
          nwarn("ESP-Hosted MAC response failed: resp=%" PRId32 "\n", resp);
#ifdef CONFIG_ESP_HOSTED_WLAN
          priv->mac_probe_sent = false;
#endif
          return -EIO;
        }

      return -EINVAL;
    }

  if (resp != 0)
    {
      nwarn("ESP-Hosted MAC response failed: resp=%" PRId32 "\n", resp);
#ifdef CONFIG_ESP_HOSTED_WLAN
      priv->mac_probe_sent = false;
#endif
      return -EIO;
    }

  memcpy(priv->mac, mac, sizeof(priv->mac));
  priv->have_mac = true;
  priv->stats.rpc_mac_count++;

  ninfo("ESP-Hosted STA MAC: %02x:%02x:%02x:%02x:%02x:%02x resp=%" PRId32
        "\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], resp);

#ifdef CONFIG_ESP_HOSTED_WLAN
  priv->mac_probe_sent = true;
  esp_hosted_try_register_wlan(priv);
#endif

  return OK;
}

static int esp_hosted_parse_fwversion_response(
  FAR struct esp_hosted_driver_s *priv, FAR const uint8_t *payload,
  size_t len)
{
  struct esp_hosted_fwversion_s version;
  size_t offset = 0;
  int ret;

  memset(&version, 0, sizeof(version));
  version.revision = -1;
  version.prerelease = -1;
  version.build = -1;

  while (offset < len)
    {
      uint64_t key;
      uint32_t field;
      uint8_t wire_type;

      ret = esp_hosted_pb_get_varint(payload, len, &offset, &key);
      if (ret < 0 || key == 0)
        {
          return -EINVAL;
        }

      field = key >> 3;
      wire_type = key & 0x07;

      switch (field)
        {
          case 1:
          case 5:
          case 6:
          case 7:
            {
              uint64_t value;

              if (wire_type != ESP_HOSTED_PB_WIRE_VARINT)
                {
                  return -EINVAL;
                }

              ret = esp_hosted_pb_get_varint(payload, len, &offset, &value);
              if (ret < 0)
                {
                  return ret;
                }

              if (field == 1)
                {
                  version.resp = esp_hosted_pb_int32(value);
                }
              else if (field == 5 && value != 0)
                {
                  version.revision = esp_hosted_pb_int32(value);
                }
              else if (field == 6 && value != 0)
                {
                  version.prerelease = esp_hosted_pb_int32(value);
                }
              else if (field == 7 && value != 0)
                {
                  version.build = esp_hosted_pb_int32(value);
                }
            }
            break;

          case 2:
          case 3:
          case 4:
          case 8:
            {
              uint64_t value;

              if (wire_type != ESP_HOSTED_PB_WIRE_VARINT)
                {
                  return -EINVAL;
                }

              ret = esp_hosted_pb_get_varint(payload, len, &offset, &value);
              if (ret < 0)
                {
                  return ret;
                }

              if (field == 2)
                {
                  version.major = value;
                }
              else if (field == 3)
                {
                  version.minor = value;
                }
              else if (field == 4)
                {
                  version.patch = value;
                }
              else
                {
                  version.chip_id = value;
                }
            }
            break;

          case 9:
            {
              uint64_t value_len;
              size_t copy_len;

              if (wire_type != ESP_HOSTED_PB_WIRE_LENGTH)
                {
                  return -EINVAL;
                }

              ret = esp_hosted_pb_get_varint(payload, len, &offset,
                                             &value_len);
              if (ret < 0 || value_len > len - offset)
                {
                  return -EINVAL;
                }

              copy_len = value_len;
              if (copy_len >= sizeof(version.idf_target))
                {
                  copy_len = sizeof(version.idf_target) - 1;
                }

              if (copy_len > 0)
                {
                  memcpy(version.idf_target, payload + offset, copy_len);
                  version.idf_target[copy_len] = '\0';
                }

              offset += value_len;
            }
            break;

          default:
            ret = esp_hosted_pb_skip_field(payload, len, &offset, wire_type);
            if (ret < 0)
              {
                return ret;
              }
            break;
        }
    }

  priv->fwversion = version;
  priv->have_fwversion = true;
  priv->stats.rpc_fwversion_count++;

  ninfo("ESP-Hosted firmware: %" PRIu32 ".%" PRIu32 ".%" PRIu32
        " target=%s chip=%" PRIu32 " resp=%" PRId32 "\n",
        version.major, version.minor, version.patch, version.idf_target,
        version.chip_id, version.resp);

#ifdef CONFIG_ESP_HOSTED_WLAN
  if (!priv->wlan_control_started)
    {
      priv->wlan_control_start_pending = true;
    }

  esp_hosted_try_register_wlan(priv);
#endif

  return OK;
}

static int esp_hosted_parse_response_status(FAR const uint8_t *payload,
                                            size_t len,
                                            FAR int32_t *resp)
{
  size_t offset = 0;
  int ret;

  *resp = 0;

  while (offset < len)
    {
      uint64_t key;
      uint32_t field;
      uint8_t wire_type;

      ret = esp_hosted_pb_get_varint(payload, len, &offset, &key);
      if (ret < 0 || key == 0)
        {
          return -EINVAL;
        }

      field = key >> 3;
      wire_type = key & 0x07;

      if (field == 1)
        {
          uint64_t value;

          if (wire_type != ESP_HOSTED_PB_WIRE_VARINT)
            {
              return -EINVAL;
            }

          ret = esp_hosted_pb_get_varint(payload, len, &offset, &value);
          if (ret < 0)
            {
              return ret;
            }

          *resp = esp_hosted_pb_int32(value);
        }
      else
        {
          ret = esp_hosted_pb_skip_field(payload, len, &offset, wire_type);
          if (ret < 0)
            {
              return ret;
            }
        }
    }

  return OK;
}

#ifdef CONFIG_ESP_HOSTED_WLAN
static int esp_hosted_parse_scan_done_payload(FAR const uint8_t *payload,
                                              size_t len,
                                              FAR uint32_t *status,
                                              FAR uint32_t *number,
                                              FAR uint32_t *scan_id)
{
  size_t offset = 0;
  int ret;

  *status = 0;
  *number = 0;
  *scan_id = 0;

  while (offset < len)
    {
      uint64_t key;
      uint64_t value;
      uint32_t field;
      uint8_t wire_type;

      ret = esp_hosted_pb_get_varint(payload, len, &offset, &key);
      if (ret < 0 || key == 0)
        {
          return -EINVAL;
        }

      field = key >> 3;
      wire_type = key & 0x07;

      switch (field)
        {
          case 1:
          case 2:
          case 3:
            if (wire_type != ESP_HOSTED_PB_WIRE_VARINT)
              {
                return -EINVAL;
              }

            ret = esp_hosted_pb_get_varint(payload, len, &offset, &value);
            if (ret < 0)
              {
                return ret;
              }

            if (field == 1)
              {
                *status = value;
              }
            else if (field == 2)
              {
                *number = value;
              }
            else
              {
                *scan_id = value;
              }
            break;

          default:
            ret = esp_hosted_pb_skip_field(payload, len, &offset, wire_type);
            if (ret < 0)
              {
                return ret;
              }
            break;
        }
    }

  return OK;
}

static int esp_hosted_parse_sta_scan_done_event(
  FAR struct esp_hosted_driver_s *priv, FAR const uint8_t *payload,
  size_t len)
{
  FAR const uint8_t *scan_payload = NULL;
  size_t scan_len = 0;
  size_t offset = 0;
  int32_t resp = 0;
  uint32_t status = 0;
  uint32_t number = 0;
  uint32_t scan_id = 0;
  int ret;

  while (offset < len)
    {
      uint64_t key;
      uint64_t value;
      uint32_t field;
      uint8_t wire_type;

      ret = esp_hosted_pb_get_varint(payload, len, &offset, &key);
      if (ret < 0 || key == 0)
        {
          return -EINVAL;
        }

      field = key >> 3;
      wire_type = key & 0x07;

      switch (field)
        {
          case 1:
            if (wire_type != ESP_HOSTED_PB_WIRE_VARINT)
              {
                return -EINVAL;
              }

            ret = esp_hosted_pb_get_varint(payload, len, &offset, &value);
            if (ret < 0)
              {
                return ret;
              }

            resp = esp_hosted_pb_int32(value);
            break;

          case 2:
            if (wire_type != ESP_HOSTED_PB_WIRE_LENGTH)
              {
                return -EINVAL;
              }

            ret = esp_hosted_pb_get_bytes(payload, len, &offset,
                                          &scan_payload, &scan_len);
            if (ret < 0)
              {
                return ret;
              }
            break;

          default:
            ret = esp_hosted_pb_skip_field(payload, len, &offset, wire_type);
            if (ret < 0)
              {
                return ret;
              }
            break;
        }
    }

  if (scan_payload != NULL)
    {
      ret = esp_hosted_parse_scan_done_payload(scan_payload, scan_len,
                                               &status, &number, &scan_id);
      if (ret < 0)
        {
          return ret;
        }
    }

  priv->scan_in_progress = false;
  priv->scan_done = true;
  priv->scan_records_valid = false;
  priv->scan_status = resp != 0 ? (uint32_t)resp : status;
  priv->scan_ap_available = number > ESP_HOSTED_SCAN_MAX_APS ?
                            ESP_HOSTED_SCAN_MAX_APS : number;
  priv->scan_result_count = 0;
  priv->stats.wlan_scan_done_count++;

  ninfo("ESP-Hosted scan done: resp=%" PRId32 " status=%" PRIu32
        " aps=%" PRIu32 " scan_id=%" PRIu32 "\n",
        resp, status, number, scan_id);
  return OK;
}

static int esp_hosted_parse_ap_record(
  FAR struct esp_hosted_scan_record_s *record, FAR const uint8_t *payload,
  size_t len)
{
  size_t offset = 0;
  bool have_bssid = false;
  int ret;

  memset(record, 0, sizeof(*record));

  while (offset < len)
    {
      FAR const uint8_t *value_ptr;
      uint64_t key;
      uint64_t value;
      size_t value_len;
      size_t copy_len;
      uint32_t field;
      uint8_t wire_type;

      ret = esp_hosted_pb_get_varint(payload, len, &offset, &key);
      if (ret < 0 || key == 0)
        {
          return -EINVAL;
        }

      field = key >> 3;
      wire_type = key & 0x07;

      switch (field)
        {
          case 1:
            if (wire_type != ESP_HOSTED_PB_WIRE_LENGTH)
              {
                return -EINVAL;
              }

            ret = esp_hosted_pb_get_bytes(payload, len, &offset,
                                          &value_ptr, &value_len);
            if (ret < 0 || value_len < ESP_HOSTED_MAC_SIZE)
              {
                return -EINVAL;
              }

            memcpy(record->bssid, value_ptr, ESP_HOSTED_MAC_SIZE);
            have_bssid = true;
            break;

          case 2:
            if (wire_type != ESP_HOSTED_PB_WIRE_LENGTH)
              {
                return -EINVAL;
              }

            ret = esp_hosted_pb_get_bytes(payload, len, &offset,
                                          &value_ptr, &value_len);
            if (ret < 0)
              {
                return ret;
              }

            copy_len = value_len;
            if (copy_len > 0 && value_ptr[copy_len - 1] == '\0')
              {
                copy_len--;
              }

            if (copy_len > IW_ESSID_MAX_SIZE)
              {
                copy_len = IW_ESSID_MAX_SIZE;
              }

            if (copy_len > 0)
              {
                memcpy(record->ssid, value_ptr, copy_len);
              }

            record->ssid[copy_len] = '\0';
            record->ssid_len = copy_len;
            break;

          case 3:
          case 5:
          case 6:
            if (wire_type != ESP_HOSTED_PB_WIRE_VARINT)
              {
                return -EINVAL;
              }

            ret = esp_hosted_pb_get_varint(payload, len, &offset, &value);
            if (ret < 0)
              {
                return ret;
              }

            if (field == 3)
              {
                record->channel = value > UINT8_MAX ? UINT8_MAX : value;
              }
            else if (field == 5)
              {
                record->rssi = esp_hosted_pb_int32(value);
              }
            else
              {
                record->authmode = esp_hosted_pb_int32(value);
              }
            break;

          default:
            ret = esp_hosted_pb_skip_field(payload, len, &offset, wire_type);
            if (ret < 0)
              {
                return ret;
              }
            break;
        }
    }

  return have_bssid ? OK : -EINVAL;
}

static int esp_hosted_parse_scan_records_response(
  FAR struct esp_hosted_driver_s *priv, FAR const uint8_t *payload,
  size_t len)
{
  struct esp_hosted_scan_record_s records[ESP_HOSTED_SCAN_MAX_APS];
  size_t offset = 0;
  int32_t resp = 0;
  uint32_t number = 0;
  uint16_t count = 0;
  int ret;

  memset(records, 0, sizeof(records));

  while (offset < len)
    {
      FAR const uint8_t *value_ptr;
      uint64_t key;
      uint64_t value;
      size_t value_len;
      uint32_t field;
      uint8_t wire_type;

      ret = esp_hosted_pb_get_varint(payload, len, &offset, &key);
      if (ret < 0 || key == 0)
        {
          return -EINVAL;
        }

      field = key >> 3;
      wire_type = key & 0x07;

      switch (field)
        {
          case 1:
          case 2:
            if (wire_type != ESP_HOSTED_PB_WIRE_VARINT)
              {
                return -EINVAL;
              }

            ret = esp_hosted_pb_get_varint(payload, len, &offset, &value);
            if (ret < 0)
              {
                return ret;
              }

            if (field == 1)
              {
                resp = esp_hosted_pb_int32(value);
              }
            else
              {
                number = value;
              }
            break;

          case 3:
            if (wire_type != ESP_HOSTED_PB_WIRE_LENGTH)
              {
                return -EINVAL;
              }

            ret = esp_hosted_pb_get_bytes(payload, len, &offset,
                                          &value_ptr, &value_len);
            if (ret < 0)
              {
                return ret;
              }

            if (count < ESP_HOSTED_SCAN_MAX_APS)
              {
                ret = esp_hosted_parse_ap_record(&records[count],
                                                 value_ptr, value_len);
                if (ret < 0)
                  {
                    return ret;
                  }

                count++;
              }
            break;

          default:
            ret = esp_hosted_pb_skip_field(payload, len, &offset, wire_type);
            if (ret < 0)
              {
                return ret;
              }
            break;
        }
    }

  if (resp != 0)
    {
      priv->stats.wlan_scan_get_records_error_count++;
      nwarn("ESP-Hosted scan records failed: resp=%" PRId32 "\n", resp);
      return -EIO;
    }

  memcpy(priv->scan_records, records, sizeof(records));
  priv->scan_result_count = count;
  priv->scan_ap_available = number > ESP_HOSTED_SCAN_MAX_APS ?
                            ESP_HOSTED_SCAN_MAX_APS : number;
  priv->scan_records_valid = true;
  priv->scan_done = true;
  priv->scan_in_progress = false;
  priv->stats.wlan_scan_get_records_count++;
  priv->stats.wlan_scan_result_count += count;

  ninfo("ESP-Hosted scan records: reported=%" PRIu32 " cached=%u\n",
        number, count);
  return OK;
}

static int esp_hosted_parse_sta_connected_event(
  FAR struct esp_hosted_driver_s *priv, FAR const uint8_t *payload,
  size_t len)
{
  int32_t resp;
  int ret;

  ret = esp_hosted_parse_response_status(payload, len, &resp);
  if (ret < 0)
    {
      return ret;
    }

  if (resp == 0 && priv->wlan_registered)
    {
      priv->carrier_on = true;
      priv->stats.wlan_link_up_count++;
      netdev_lower_carrier_on(&priv->lower);
      netdev_lower_rxready(&priv->lower);
      esp_hosted_arm_rx_poll(priv, ESP_HOSTED_RX_POLL_LINK_BUDGET);
    }

  ninfo("ESP-Hosted STA connected event: resp=%" PRId32 "\n", resp);
  return OK;
}

static int esp_hosted_parse_sta_disconnected_event(
  FAR struct esp_hosted_driver_s *priv, FAR const uint8_t *payload,
  size_t len)
{
  int32_t resp;
  int ret;

  ret = esp_hosted_parse_response_status(payload, len, &resp);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->wlan_registered)
    {
      priv->carrier_on = false;
      priv->rx_poll_budget = 0;
      priv->rx_force_poll = false;
      priv->stats.wlan_link_down_count++;
      netdev_lower_carrier_off(&priv->lower);
    }

  ninfo("ESP-Hosted STA disconnected event: resp=%" PRId32 "\n", resp);
  return OK;
}
#endif

static int esp_hosted_parse_control_frame(FAR struct esp_hosted_driver_s *priv,
                                          FAR const uint8_t *payload,
                                          uint16_t len)
{
  FAR const uint8_t *rpc_payload;
  size_t rpc_len;
  struct esp_hosted_rpc_message_s msg;
  int ret;

  ret = esp_hosted_pserial_unwrap(priv, payload, len, &rpc_payload,
                                  &rpc_len);
  if (ret < 0)
    {
      priv->stats.rpc_malformed_count++;
      return ret;
    }

  ret = esp_hosted_parse_rpc_message(rpc_payload, rpc_len, &msg);
  if (ret < 0)
    {
      priv->stats.rpc_malformed_count++;
      return ret;
    }

  priv->stats.rpc_last_uid = msg.uid;

  if (msg.msg_type == ESP_HOSTED_RPC_TYPE_RESP)
    {
      priv->stats.rpc_response_count++;
      priv->stats.rpc_last_response_id = msg.msg_id;

      if (msg.payload_field != msg.msg_id)
        {
          priv->stats.rpc_malformed_count++;
          nwarn("ESP-Hosted RPC response payload mismatch: id=%" PRIu32
                " payload=%" PRIu32 "\n",
                msg.msg_id, msg.payload_field);
          return -EINVAL;
        }

      switch (msg.msg_id)
        {
          int32_t resp;

          case ESP_HOSTED_RPC_RESP_GET_MAC_ADDRESS:
            return esp_hosted_parse_mac_response(priv, msg.payload,
                                                 msg.payload_len);

          case ESP_HOSTED_RPC_RESP_GET_COPROCESSOR_FWVERSION:
            return esp_hosted_parse_fwversion_response(priv, msg.payload,
                                                       msg.payload_len);

          case ESP_HOSTED_RPC_RESP_SET_MODE:
          case ESP_HOSTED_RPC_RESP_WIFI_INIT:
          case ESP_HOSTED_RPC_RESP_WIFI_START:
          case ESP_HOSTED_RPC_RESP_WIFI_CONNECT:
          case ESP_HOSTED_RPC_RESP_WIFI_DISCONNECT:
          case ESP_HOSTED_RPC_RESP_WIFI_SET_CONFIG:
          case ESP_HOSTED_RPC_RESP_WIFI_SCAN_START:
          case ESP_HOSTED_RPC_RESP_WIFI_SCAN_GET_AP_NUM:
            ret = esp_hosted_parse_response_status(msg.payload,
                                                   msg.payload_len, &resp);
            if (ret < 0)
              {
                return ret;
              }

            if (resp != 0)
              {
                priv->stats.rpc_malformed_count++;
                nwarn("ESP-Hosted RPC response failed: id=%" PRIu32
                      " resp=%" PRId32 "\n",
                      msg.msg_id, resp);
                return -EIO;
              }

            ninfo("ESP-Hosted RPC response OK: id=%" PRIu32
                  " uid=%" PRIu32 "\n",
                  msg.msg_id, msg.uid);
            return OK;

          case ESP_HOSTED_RPC_RESP_WIFI_SCAN_GET_AP_RECORDS:
#ifdef CONFIG_ESP_HOSTED_WLAN
            return esp_hosted_parse_scan_records_response(priv, msg.payload,
                                                          msg.payload_len);
#else
            return OK;
#endif

          default:
            ninfo("ESP-Hosted RPC response: id=%" PRIu32 " uid=%" PRIu32
                  " len=%zu\n",
                  msg.msg_id, msg.uid, msg.payload_len);
            return OK;
        }
    }
  else if (msg.msg_type == ESP_HOSTED_RPC_TYPE_EVENT)
    {
      priv->stats.rpc_event_count++;

      switch (msg.msg_id)
        {
#ifdef CONFIG_ESP_HOSTED_WLAN
          case ESP_HOSTED_RPC_EVENT_STA_SCAN_DONE:
            return esp_hosted_parse_sta_scan_done_event(priv, msg.payload,
                                                        msg.payload_len);

          case ESP_HOSTED_RPC_EVENT_STA_CONNECTED:
            return esp_hosted_parse_sta_connected_event(priv, msg.payload,
                                                        msg.payload_len);

          case ESP_HOSTED_RPC_EVENT_STA_DISCONNECTED:
            return esp_hosted_parse_sta_disconnected_event(priv, msg.payload,
                                                           msg.payload_len);
#endif

          default:
            ninfo("ESP-Hosted RPC event: id=%" PRIu32 " uid=%" PRIu32
                  " len=%zu\n",
                  msg.msg_id, msg.uid, msg.payload_len);
            return OK;
        }
    }

  priv->stats.rpc_malformed_count++;
  nwarn("ESP-Hosted unexpected RPC message: type=%" PRIu32 " id=%" PRIu32
        " uid=%" PRIu32 "\n",
        msg.msg_type, msg.msg_id, msg.uid);

  return -EINVAL;
}

static int esp_hosted_build_rpc_request(FAR struct esp_hosted_driver_s *priv,
                                        uint32_t request_id,
                                        FAR uint8_t *payload,
                                        size_t payload_size,
                                        FAR size_t *payload_len)
{
  uint8_t request_payload[256];
  size_t request_len = 0;
  size_t offset = 0;
  uint32_t uid;
  int ret;

  switch (request_id)
    {
      case ESP_HOSTED_RPC_GET_MAC_ADDRESS:
        ret = esp_hosted_pb_append_varint_field(request_payload,
                                                sizeof(request_payload),
                                                &request_len, 1,
                                                ESP_HOSTED_WIFI_IF_STA);
        if (ret < 0)
          {
            return ret;
          }
        break;

      case ESP_HOSTED_RPC_GET_COPROCESSOR_FWVERSION:
        break;

#ifdef CONFIG_ESP_HOSTED_WLAN
      case ESP_HOSTED_RPC_SET_MODE:
        ret = esp_hosted_pb_append_varint_field(request_payload,
                                                sizeof(request_payload),
                                                &request_len, 1,
                                                ESP_HOSTED_WIFI_MODE_STA);
        if (ret < 0)
          {
            return ret;
          }
        break;

      case ESP_HOSTED_RPC_WIFI_INIT:
        ret = esp_hosted_build_wifi_init_payload(request_payload,
                                                sizeof(request_payload),
                                                &request_len);
        if (ret < 0)
          {
            return ret;
          }
        break;

      case ESP_HOSTED_RPC_WIFI_START:
      case ESP_HOSTED_RPC_WIFI_CONNECT:
      case ESP_HOSTED_RPC_WIFI_DISCONNECT:
        break;

      case ESP_HOSTED_RPC_WIFI_SET_CONFIG:
        ret = esp_hosted_build_wifi_set_config_payload(priv, request_payload,
                                                       sizeof(request_payload),
                                                       &request_len);
        if (ret < 0)
          {
            return ret;
          }
        break;

      case ESP_HOSTED_RPC_WIFI_SCAN_START:
        break;

      case ESP_HOSTED_RPC_WIFI_SCAN_GET_AP_RECORDS:
        ret = esp_hosted_pb_append_varint_field(request_payload,
                                                sizeof(request_payload),
                                                &request_len, 1,
                                                priv->scan_request_count);
        if (ret < 0)
          {
            return ret;
          }
        break;
#endif

      default:
        return -EINVAL;
    }

  uid = esp_hosted_next_rpc_uid(priv);

  ret = esp_hosted_pb_append_varint_field(payload, payload_size, &offset,
                                          ESP_HOSTED_RPC_FIELD_MSG_TYPE,
                                          ESP_HOSTED_RPC_TYPE_REQ);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(payload, payload_size, &offset,
                                          ESP_HOSTED_RPC_FIELD_MSG_ID,
                                          request_id);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_varint_field(payload, payload_size, &offset,
                                          ESP_HOSTED_RPC_FIELD_UID, uid);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_pb_append_bytes_field(payload, payload_size, &offset,
                                         request_id, request_payload,
                                         request_len);
  if (ret < 0)
    {
      return ret;
    }

  *payload_len = offset;
  priv->stats.rpc_last_uid = uid;
  return OK;
}

static uint16_t esp_hosted_checksum(FAR const uint8_t *buf, uint16_t len)
{
  uint16_t checksum = 0;
  uint16_t i;

  for (i = 0; i < len; i++)
    {
      checksum += buf[i];
    }

  return checksum;
}

static uint16_t esp_hosted_frame_checksum(FAR uint8_t *buf, uint16_t len)
{
  FAR struct esp_hosted_payload_header_s *header;
  uint16_t saved;
  uint16_t checksum;

  header = (FAR struct esp_hosted_payload_header_s *)buf;
  saved = header->checksum;
  header->checksum = 0;
  checksum = esp_hosted_checksum(buf, len);
  header->checksum = saved;

  return checksum;
}

static void esp_hosted_build_header(FAR struct esp_hosted_driver_s *priv,
                                    FAR uint8_t *frame,
                                    enum esp_hosted_if_type_e if_type,
                                    uint8_t if_num, uint8_t flags,
                                    FAR const uint8_t *payload,
                                    uint16_t payload_len)
{
  FAR struct esp_hosted_payload_header_s *header;
  uint16_t frame_len;

  DEBUGASSERT(payload_len <= ESP_HOSTED_MAX_PAYLOAD);

  memset(frame, 0, ESP_HOSTED_SPI_MAX_FRAME);

  header = (FAR struct esp_hosted_payload_header_s *)frame;
  ESP_HOSTED_HDR_SET_IF(header, if_type, if_num);
  header->flags = flags;

  if (payload_len > 0)
    {
      memcpy(frame + ESP_HOSTED_HEADER_LEN, payload, payload_len);
      header->len = htole16(payload_len);
      header->offset = htole16(ESP_HOSTED_HEADER_LEN);
      header->seq_num = htole16(priv->seq_num++);

      frame_len = ESP_HOSTED_HEADER_LEN + payload_len;
      header->checksum = htole16(esp_hosted_checksum(frame, frame_len));
      priv->stats.tx_frame_count++;
      priv->stats.last_tx_ticks = clock_systime_ticks();
    }
}

static void esp_hosted_build_dummy(FAR struct esp_hosted_driver_s *priv,
                                   FAR uint8_t *frame)
{
  esp_hosted_build_header(priv, frame, ESP_HOSTED_MAX_IF, 0, 0, NULL, 0);
  priv->stats.tx_dummy_count++;
}

static int esp_hosted_parse_priv_frame(FAR struct esp_hosted_driver_s *priv,
                                       FAR const uint8_t *payload,
                                       uint16_t len)
{
  uint8_t event_type;
  uint8_t event_len;

  priv->stats.rx_priv_count++;

  if (len < ESP_HOSTED_PRIV_EVENT_HDR_LEN)
    {
      return -EINVAL;
    }

  event_type = payload[0];
  event_len = payload[1];

  if (event_len + ESP_HOSTED_PRIV_EVENT_HDR_LEN > len)
    {
      return -EINVAL;
    }

  if (event_type == ESP_HOSTED_PRIV_EVENT_INIT)
    {
      priv->stats.rx_init_event_count++;
      priv->init_seen = true;
      if (!priv->startup_probe_sent)
        {
          priv->startup_probe_pending = true;
        }

      ninfo("ESP-Hosted INIT event received: len=%u\n", event_len);
    }
  else
    {
      ninfo("ESP-Hosted private event received: type=%02x len=%u\n",
            event_type, event_len);
    }

  return OK;
}

static int esp_hosted_parse_rx_frame(FAR struct esp_hosted_driver_s *priv,
                                     FAR uint8_t *frame)
{
  FAR struct esp_hosted_payload_header_s *header;
  FAR uint8_t *payload;
  enum esp_hosted_if_type_e if_type;
  uint16_t len;
  uint16_t offset;
  uint16_t checksum;
  uint16_t expected;
  int ret = OK;

  header = (FAR struct esp_hosted_payload_header_s *)frame;
  if_type = ESP_HOSTED_HDR_IF_TYPE(header);
  len = le16toh(header->len);
  offset = le16toh(header->offset);

  if (len == 0)
    {
      priv->stats.rx_dummy_count++;
      return OK;
    }

  if (len > ESP_HOSTED_MAX_PAYLOAD || offset != ESP_HOSTED_HEADER_LEN ||
      offset + len > ESP_HOSTED_SPI_MAX_FRAME)
    {
      priv->stats.malformed_frame_count++;
      nwarn("ESP-Hosted malformed frame: if=%u len=%u offset=%u\n",
            if_type, len, offset);
      return -EINVAL;
    }

  checksum = le16toh(header->checksum);
  expected = esp_hosted_frame_checksum(frame, offset + len);
  if (checksum != expected)
    {
      priv->stats.checksum_error_count++;
      nwarn("ESP-Hosted checksum mismatch: rx=%04x expected=%04x\n",
            checksum, expected);
      return -EIO;
    }

  priv->stats.rx_frame_count++;
  priv->stats.last_rx_ticks = clock_systime_ticks();
  payload = frame + offset;

  switch (if_type)
    {
      case ESP_HOSTED_STA_IF:
        priv->stats.rx_sta_count++;
#ifdef CONFIG_ESP_HOSTED_WLAN
        ret = esp_hosted_queue_sta_frame(priv, payload, len);
#endif
        break;

      case ESP_HOSTED_AP_IF:
        priv->stats.rx_ap_count++;
        break;

      case ESP_HOSTED_SERIAL_IF:
        priv->stats.rx_control_count++;
        ret = esp_hosted_parse_control_frame(priv, payload, len);
        if (ret < 0)
          {
            priv->stats.malformed_frame_count++;
          }
        break;

      case ESP_HOSTED_PRIV_IF:
        ret = esp_hosted_parse_priv_frame(priv, payload, len);
        if (ret < 0)
          {
            priv->stats.malformed_frame_count++;
          }
        break;

      default:
        priv->stats.rx_unknown_count++;
        nwarn("ESP-Hosted unsupported frame: if=%u len=%u\n", if_type, len);
        break;
    }

  return ret;
}

static int esp_hosted_validate_config(
  FAR const struct esp_hosted_config_s *config)
{
  if (config == NULL || config->spi == NULL)
    {
      return -EINVAL;
    }

  if (config->spi_frequency == 0 || config->spi_bits == 0)
    {
      return -EINVAL;
    }

  if (config->gpio == NULL || config->gpio->reset == NULL ||
      config->gpio->handshake_ready == NULL ||
      config->gpio->data_ready == NULL)
    {
      return -EINVAL;
    }

  return OK;
}

static int esp_hosted_schedule_rx_work_delay(
  FAR struct esp_hosted_driver_s *priv, clock_t delay, bool idle)
{
  int ret;

  if (delay > 0)
    {
      if (!work_available(&priv->poll_work))
        {
          priv->stats.rx_poll_busy_count++;
          return -EBUSY;
        }

      ret = work_queue(ESP_HOSTED_WORK_QUEUE, &priv->poll_work,
                       esp_hosted_rx_poll_work, priv, delay);
      if (ret >= 0)
        {
          if (idle)
            {
              priv->stats.rx_idle_poll_count++;
            }
          else
            {
              priv->stats.rx_poll_count++;
            }
        }

      return ret;
    }

  if (!work_available(&priv->rx_work))
    {
      priv->stats.rx_work_busy_count++;
    }

  ret = work_queue(ESP_HOSTED_WORK_QUEUE, &priv->rx_work,
                   esp_hosted_rx_work, priv, 0);
  if (ret >= 0)
    {
      priv->stats.rx_work_count++;
    }

  return ret;
}

static int esp_hosted_schedule_rx_work(FAR struct esp_hosted_driver_s *priv)
{
  return esp_hosted_schedule_rx_work_delay(priv, 0, false);
}

#ifdef CONFIG_ESP_HOSTED_WLAN
static void esp_hosted_arm_rx_poll(FAR struct esp_hosted_driver_s *priv,
                                   uint8_t budget)
{
  if (budget > priv->rx_poll_budget)
    {
      priv->rx_poll_budget = budget;
    }
}
#endif

static void esp_hosted_schedule_rx_poll(FAR struct esp_hosted_driver_s *priv)
{
#ifdef CONFIG_ESP_HOSTED_WLAN
  if (priv->wlan_registered && priv->ifup && priv->carrier_on)
    {
      bool burst = priv->rx_poll_budget > 0;
      int ret;

      if (burst)
        {
          priv->rx_force_poll = true;
          if (!work_available(&priv->poll_work))
            {
              (void)work_cancel(ESP_HOSTED_WORK_QUEUE, &priv->poll_work);
            }
        }

      /* Burst polls follow TX, IRQ, or link-up activity and need a short,
       * sustained polling window.  esp_hosted_schedule_rx_work_delay() skips
       * retiming an already queued delayed poll, so repeated TX packets cannot
       * keep pushing the receive worker into the future.
       */

      ret = esp_hosted_schedule_rx_work_delay(priv,
        MSEC2TICK(burst ? ESP_HOSTED_RX_POLL_DELAY_MS :
                          ESP_HOSTED_RX_IDLE_POLL_DELAY_MS),
        !burst);
      if (ret >= 0 && burst)
        {
          priv->rx_poll_budget--;
        }
      else if (ret < 0 && burst)
        {
          esp_hosted_schedule_rx_work(priv);
        }
    }
#endif
}

static int esp_hosted_dataready_isr(int irq, FAR void *context, FAR void *arg)
{
  FAR struct esp_hosted_driver_s *priv = arg;

  UNUSED(irq);
  UNUSED(context);

  if (priv->recovering)
    {
      return OK;
    }

  priv->dataready_seen = true;
  priv->stats.dataready_irq_count++;

#ifdef CONFIG_ESP_HOSTED_WLAN
  esp_hosted_arm_rx_poll(priv, ESP_HOSTED_RX_POLL_IRQ_BUDGET);
#endif
  esp_hosted_schedule_rx_work(priv);

  return OK;
}

static int esp_hosted_wait_handshake(FAR struct esp_hosted_driver_s *priv)
{
  unsigned int retries;

  for (retries = 0; retries < ESP_HOSTED_HANDSHAKE_POLLS; retries++)
    {
      if (priv->config.gpio->handshake_ready(priv->config.gpio_arg))
        {
          return OK;
        }

      nxsched_usleep(ESP_HOSTED_HANDSHAKE_POLL_US);
    }

  priv->stats.control_timeout_count++;
  return -ETIMEDOUT;
}

static void esp_hosted_spi_configure(FAR struct esp_hosted_driver_s *priv)
{
  FAR struct spi_dev_s *spi = priv->config.spi;

  SPI_SETFREQUENCY(spi, priv->config.spi_frequency);
  SPI_SETMODE(spi, priv->config.spi_mode);
  SPI_SETBITS(spi, priv->config.spi_bits);
}

static int esp_hosted_spi_exchange_frame(FAR struct esp_hosted_driver_s *priv,
                                         FAR const void *txbuf,
                                         FAR void *rxbuf)
{
  FAR struct spi_dev_s *spi = priv->config.spi;
  int ret;

  ret = esp_hosted_wait_handshake(priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = SPI_LOCK(spi, true);
  if (ret < 0)
    {
      return ret;
    }

  esp_hosted_spi_configure(priv);

  SPI_SELECT(spi, priv->config.spi_devid, true);
  SPI_EXCHANGE(spi, txbuf, rxbuf, ESP_HOSTED_SPI_MAX_FRAME);
  SPI_SELECT(spi, priv->config.spi_devid, false);

  SPI_LOCK(spi, false);

  priv->stats.spi_transaction_count++;
  return OK;
}

#ifdef CONFIG_ESP_HOSTED_WLAN
static int esp_hosted_wlan_ifup(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct esp_hosted_driver_s *priv =
    (FAR struct esp_hosted_driver_s *)dev;

  priv->ifup = true;

  if (!priv->carrier_on)
    {
      netdev_lower_carrier_off(&priv->lower);
    }
  else
    {
      netdev_lower_carrier_on(&priv->lower);
      netdev_lower_rxready(&priv->lower);
      esp_hosted_arm_rx_poll(priv, ESP_HOSTED_RX_POLL_LINK_BUDGET);
      esp_hosted_schedule_rx_poll(priv);
    }

  return OK;
}

static int esp_hosted_wlan_ifdown(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct esp_hosted_driver_s *priv =
    (FAR struct esp_hosted_driver_s *)dev;

  priv->ifup = false;
  priv->carrier_on = false;
  priv->rx_poll_budget = 0;
  priv->rx_force_poll = false;
  netdev_lower_carrier_off(&priv->lower);
  esp_hosted_wlan_clear_rx_queue(priv);
  return OK;
}

static int esp_hosted_wlan_transmit(FAR struct netdev_lowerhalf_s *dev,
                                    FAR netpkt_t *pkt)
{
  FAR struct esp_hosted_driver_s *priv =
    (FAR struct esp_hosted_driver_s *)dev;
  unsigned int len;
  int parse_ret = OK;
  int ret;

  len = netpkt_getdatalen(dev, pkt);
  priv->stats.netdev_tx_last_len = len;
  priv->stats.netdev_tx_last_ret = OK;
  priv->stats.netdev_tx_last_parse_ret = OK;

  if (!priv->ifup)
    {
      priv->stats.netdev_tx_error_count++;
      priv->stats.netdev_tx_last_ret = -ENETDOWN;
      return -ENETDOWN;
    }

  if (!priv->carrier_on)
    {
      priv->stats.netdev_tx_error_count++;
      priv->stats.netdev_tx_carrier_off_count++;
      priv->stats.netdev_tx_last_ret = -ENETDOWN;
      netdev_lower_carrier_off(&priv->lower);
      return -ENETDOWN;
    }

  if (len > ESP_HOSTED_MAX_PAYLOAD)
    {
      priv->stats.netdev_tx_error_count++;
      priv->stats.netdev_tx_oversize_count++;
      priv->stats.netdev_tx_last_ret = -EMSGSIZE;
      return -EMSGSIZE;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      priv->stats.netdev_tx_error_count++;
      priv->stats.netdev_tx_last_ret = ret;
      return ret;
    }

  ret = netpkt_copyout(dev, priv->netdev_tx_payload, pkt, len, 0);
  if (ret < 0)
    {
      nxmutex_unlock(&priv->lock);
      priv->stats.netdev_tx_error_count++;
      priv->stats.netdev_tx_copy_error_count++;
      priv->stats.netdev_tx_last_ret = ret;
      return ret;
    }

  esp_hosted_build_header(priv, priv->tx_frame, ESP_HOSTED_STA_IF, 0, 0,
                          priv->netdev_tx_payload, len);
  memset(priv->rx_frame, 0, sizeof(priv->rx_frame));

  ret = esp_hosted_spi_exchange_frame(priv, priv->tx_frame, priv->rx_frame);
  if (ret >= 0)
    {
      parse_ret = esp_hosted_parse_rx_frame(priv, priv->rx_frame);
      priv->stats.netdev_tx_last_parse_ret = parse_ret;
      if (parse_ret < 0)
        {
          priv->stats.netdev_tx_parse_error_count++;
          nwarn("ESP-Hosted STA TX side RX parse failed: %d\n", parse_ret);
        }
    }
  else
    {
      priv->stats.netdev_tx_last_ret = ret;
    }

  nxmutex_unlock(&priv->lock);

  if (ret < 0)
    {
      priv->stats.netdev_tx_error_count++;
      priv->stats.netdev_tx_spi_error_count++;
      return ret;
    }

  priv->stats.netdev_tx_count++;
  priv->stats.netdev_tx_last_ret = OK;
  netpkt_free(dev, pkt, NETPKT_TX);
  netdev_lower_txdone(dev);
  esp_hosted_arm_rx_poll(priv, ESP_HOSTED_RX_POLL_TX_BUDGET);
  esp_hosted_schedule_rx_poll(priv);
  return OK;
}

static FAR netpkt_t *esp_hosted_wlan_receive(
  FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct esp_hosted_driver_s *priv =
    (FAR struct esp_hosted_driver_s *)dev;
  FAR netpkt_t *pkt;
  irqstate_t flags;

  flags = spin_lock_irqsave(&priv->rx_lock);
  pkt = netpkt_remove_queue(&priv->rx_queue);
  if (pkt != NULL && priv->stats.current_rx_queue_depth > 0)
    {
      priv->stats.current_rx_queue_depth--;
    }

  spin_unlock_irqrestore(&priv->rx_lock, flags);

  return pkt;
}

#ifdef CONFIG_NETDEV_IOCTL
static int esp_hosted_wlan_ioctl(FAR struct netdev_lowerhalf_s *dev,
                                 int cmd, unsigned long arg)
{
  UNUSED(dev);
  UNUSED(cmd);
  UNUSED(arg);

  return -ENOTTY;
}
#endif

static void esp_hosted_wlan_reclaim(FAR struct netdev_lowerhalf_s *dev)
{
  UNUSED(dev);
}

static int esp_hosted_wlan_connect(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct esp_hosted_driver_s *priv =
    (FAR struct esp_hosted_driver_s *)dev;
  int ret;

  if (!priv->have_ssid)
    {
      return -ENOTCONN;
    }

  ret = esp_hosted_run_wlan_control_start(priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_send_rpc_request(priv, ESP_HOSTED_RPC_WIFI_SET_CONFIG);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_send_rpc_request(priv, ESP_HOSTED_RPC_WIFI_CONNECT);
  if (ret < 0)
    {
      return ret;
    }

  priv->stats.wlan_connect_count++;
  return OK;
}

static int esp_hosted_wlan_disconnect(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct esp_hosted_driver_s *priv =
    (FAR struct esp_hosted_driver_s *)dev;
  int ret;

  ret = esp_hosted_send_rpc_request(priv, ESP_HOSTED_RPC_WIFI_DISCONNECT);
  if (ret < 0)
    {
      return ret;
    }

  priv->carrier_on = false;
  priv->rx_poll_budget = 0;
  priv->rx_force_poll = false;
  priv->stats.wlan_disconnect_count++;
  netdev_lower_carrier_off(&priv->lower);
  return OK;
}

static int esp_hosted_wlan_essid(FAR struct netdev_lowerhalf_s *dev,
                                 FAR struct iwreq *iwr, bool set)
{
  FAR struct esp_hosted_driver_s *priv =
    (FAR struct esp_hosted_driver_s *)dev;
  size_t len;
  int ret;

  if (iwr == NULL || iwr->u.essid.pointer == NULL)
    {
      return -EINVAL;
    }

  if (!set)
    {
      len = priv->ssid_len;
      if (iwr->u.essid.length < len + 1)
        {
          return -ENOSPC;
        }

      memcpy(iwr->u.essid.pointer, priv->ssid, len);
      ((FAR char *)iwr->u.essid.pointer)[len] = '\0';
      iwr->u.essid.length = len;
      iwr->u.essid.flags = priv->have_ssid ? IW_ESSID_ON : IW_ESSID_OFF;
      return OK;
    }

  len = iwr->u.essid.length;
  if (len > 0 && ((FAR const char *)iwr->u.essid.pointer)[len - 1] == '\0')
    {
      len--;
    }

  if (len > IW_ESSID_MAX_SIZE)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  memcpy(priv->ssid, iwr->u.essid.pointer, len);
  priv->ssid[len] = '\0';
  priv->ssid_len = len;
  priv->have_ssid = len > 0;
  nxmutex_unlock(&priv->lock);

  return OK;
}

static int esp_hosted_wlan_passwd(FAR struct netdev_lowerhalf_s *dev,
                                  FAR struct iwreq *iwr, bool set)
{
  FAR struct esp_hosted_driver_s *priv =
    (FAR struct esp_hosted_driver_s *)dev;
  FAR struct iw_encode_ext *ext;
  int ret;

  if (iwr == NULL || iwr->u.encoding.pointer == NULL)
    {
      return -EINVAL;
    }

  if (iwr->u.encoding.length < sizeof(struct iw_encode_ext))
    {
      return -EINVAL;
    }

  ext = (FAR struct iw_encode_ext *)iwr->u.encoding.pointer;
  if (!set)
    {
      ext->alg = priv->passphrase_len > 0 ? IW_ENCODE_ALG_CCMP :
                                            IW_ENCODE_ALG_NONE;
      ext->key_len = 0;
      iwr->u.encoding.flags = IW_ENCODE_NOKEY;
      return OK;
    }

  if ((iwr->u.encoding.flags & IW_ENCODE_DISABLED) != 0 ||
      ext->alg == IW_ENCODE_ALG_NONE || ext->key_len == 0)
    {
      ret = nxmutex_lock(&priv->lock);
      if (ret < 0)
        {
          return ret;
        }

      memset(priv->passphrase, 0, sizeof(priv->passphrase));
      priv->passphrase_len = 0;
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  if (ext->key_len > ESP_HOSTED_WIFI_PASSWORD_MAX ||
      iwr->u.encoding.length < sizeof(*ext) + ext->key_len)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  memcpy(priv->passphrase, ext->key, ext->key_len);
  priv->passphrase[ext->key_len] = '\0';
  priv->passphrase_len = ext->key_len;
  nxmutex_unlock(&priv->lock);

  return OK;
}

static int esp_hosted_wlan_auth(FAR struct netdev_lowerhalf_s *dev,
                                FAR struct iwreq *iwr, bool set)
{
  FAR struct esp_hosted_driver_s *priv =
    (FAR struct esp_hosted_driver_s *)dev;
  uint32_t index;

  if (iwr == NULL)
    {
      return -EINVAL;
    }

  index = iwr->u.param.flags & IW_AUTH_INDEX;
  if (set)
    {
      switch (index)
        {
          case IW_AUTH_WPA_VERSION:
            priv->auth_wpa = iwr->u.param.value;
            break;

          case IW_AUTH_CIPHER_PAIRWISE:
            priv->cipher_pairwise = iwr->u.param.value;
            break;

          case IW_AUTH_CIPHER_GROUP:
            priv->cipher_group = iwr->u.param.value;
            break;

          default:
            break;
        }
    }
  else
    {
      switch (index)
        {
          case IW_AUTH_WPA_VERSION:
            iwr->u.param.value = priv->auth_wpa;
            break;

          case IW_AUTH_CIPHER_PAIRWISE:
            iwr->u.param.value = priv->cipher_pairwise;
            break;

          case IW_AUTH_CIPHER_GROUP:
            iwr->u.param.value = priv->cipher_group;
            break;

          default:
            iwr->u.param.value = 0;
            break;
        }
    }

  return OK;
}

static size_t esp_hosted_wlan_scan_result_len(
  FAR const struct esp_hosted_driver_s *priv)
{
  size_t len = 0;
  uint16_t i;

  for (i = 0; i < priv->scan_result_count; i++)
    {
      len += IW_EV_LEN(ap_addr) + IW_EV_LEN(qual) +
             IW_EV_LEN(freq) + IW_EV_LEN(data) + IW_EV_LEN(essid) +
             ((priv->scan_records[i].ssid_len + 3) & ~3);
    }

  return len;
}

static int esp_hosted_wlan_format_scan_results(
  FAR struct esp_hosted_driver_s *priv, FAR struct iwreq *iwr)
{
  FAR struct esp_hosted_scan_record_s *record;
  FAR struct iw_event *iwe;
  FAR char *pointer;
  size_t need_len;
  uint16_t i;

  if (iwr == NULL)
    {
      return -EINVAL;
    }

  if (priv->scan_result_count == 0)
    {
      iwr->u.data.length = 0;
      return OK;
    }

  need_len = esp_hosted_wlan_scan_result_len(priv);
  if (iwr->u.data.pointer == NULL || iwr->u.data.length < need_len)
    {
      iwr->u.data.length = need_len;
      return -E2BIG;
    }

  pointer = iwr->u.data.pointer;

  for (i = 0; i < priv->scan_result_count; i++)
    {
      record = &priv->scan_records[i];

      iwe = (FAR struct iw_event *)pointer;
      iwe->cmd = SIOCGIWAP;
      iwe->u.ap_addr.sa_family = ARPHRD_ETHER;
      memcpy(iwe->u.ap_addr.sa_data, record->bssid, ESP_HOSTED_MAC_SIZE);
      iwe->len = IW_EV_LEN(ap_addr);
      pointer += iwe->len;

      iwe = (FAR struct iw_event *)pointer;
      iwe->cmd = SIOCGIWESSID;
      iwe->u.essid.flags = 0;
      iwe->u.essid.length = record->ssid_len;
      iwe->u.essid.pointer = (FAR void *)sizeof(iwe->u.essid);
      memcpy(&iwe->u.essid + 1, record->ssid, iwe->u.essid.length);
      iwe->len = IW_EV_LEN(essid) + ((iwe->u.essid.length + 3) & ~3);
      pointer += iwe->len;

      iwe = (FAR struct iw_event *)pointer;
      iwe->cmd = IWEVQUAL;
      iwe->u.qual.qual = 0;
      iwe->u.qual.level = record->rssi;
      iwe->u.qual.noise = 0;
      iwe->u.qual.updated = IW_QUAL_DBM | IW_QUAL_ALL_UPDATED;
      iwe->len = IW_EV_LEN(qual);
      pointer += iwe->len;

      iwe = (FAR struct iw_event *)pointer;
      iwe->cmd = SIOCGIWFREQ;
      iwe->u.freq.e = 0;
      iwe->u.freq.m = record->channel;
      iwe->len = IW_EV_LEN(freq);
      pointer += iwe->len;

      iwe = (FAR struct iw_event *)pointer;
      iwe->cmd = SIOCGIWENCODE;
      iwe->u.data.flags = record->authmode == ESP_HOSTED_WIFI_AUTH_OPEN ?
                          IW_ENCODE_DISABLED :
                          IW_ENCODE_ENABLED | IW_ENCODE_NOKEY;
      iwe->u.data.length = 0;
      iwe->u.data.pointer = NULL;
      iwe->len = IW_EV_LEN(data);
      pointer += iwe->len;
    }

  iwr->u.data.length = pointer - (FAR char *)iwr->u.data.pointer;
  return OK;
}

static int esp_hosted_wlan_scan(FAR struct netdev_lowerhalf_s *dev,
                                FAR struct iwreq *iwr, bool set)
{
  FAR struct esp_hosted_driver_s *priv =
    (FAR struct esp_hosted_driver_s *)dev;
  int ret;

  if (!set)
    {
      if (iwr == NULL)
        {
          return -EINVAL;
        }

      if (priv->scan_in_progress || !priv->scan_done)
        {
          return -EAGAIN;
        }

      if (priv->scan_status != 0)
        {
          iwr->u.data.length = 0;
          return -EIO;
        }

      if (!priv->scan_records_valid)
        {
          if (priv->scan_ap_available == 0)
            {
              priv->scan_records_valid = true;
              priv->scan_result_count = 0;
            }
          else
            {
              priv->scan_request_count = priv->scan_ap_available;
              ret = esp_hosted_send_rpc_request(priv,
                ESP_HOSTED_RPC_WIFI_SCAN_GET_AP_RECORDS);
              if (ret < 0)
                {
                  return ret;
                }

              if (!priv->scan_records_valid)
                {
                  return -EAGAIN;
                }
            }
        }

      return esp_hosted_wlan_format_scan_results(priv, iwr);
    }

  ret = esp_hosted_run_wlan_control_start(priv);
  if (ret < 0)
    {
      return ret;
    }

  priv->scan_in_progress = true;
  priv->scan_done = false;
  priv->scan_records_valid = false;
  priv->scan_status = 0;
  priv->scan_ap_available = 0;
  priv->scan_result_count = 0;
  priv->scan_request_count = 0;
  memset(priv->scan_records, 0, sizeof(priv->scan_records));

  ret = esp_hosted_send_rpc_request(priv, ESP_HOSTED_RPC_WIFI_SCAN_START);
  if (ret < 0)
    {
      priv->scan_in_progress = false;
      return ret;
    }

  priv->stats.wlan_scan_start_count++;
  return OK;
}

static int esp_hosted_wlan_range(FAR struct netdev_lowerhalf_s *dev,
                                 FAR struct iwreq *iwr)
{
  UNUSED(dev);
  UNUSED(iwr);

  return -ENOSYS;
}
#endif

static int esp_hosted_send_rpc_request(FAR struct esp_hosted_driver_s *priv,
                                       uint32_t request_id)
{
  size_t payload_len;
  int parse_ret;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_build_rpc_request(priv, request_id, priv->rpc_payload,
                                     sizeof(priv->rpc_payload),
                                     &payload_len);
  if (ret < 0)
    {
      priv->stats.rpc_malformed_count++;
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  ret = esp_hosted_pserial_wrap(priv, ESP_HOSTED_RPC_EP_NAME_RSP,
                                priv->rpc_payload, payload_len,
                                priv->rpc_tlv_payload,
                                sizeof(priv->rpc_tlv_payload),
                                &payload_len);
  if (ret < 0)
    {
      priv->stats.rpc_malformed_count++;
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  esp_hosted_build_header(priv, priv->tx_frame, ESP_HOSTED_SERIAL_IF, 0, 0,
                          priv->rpc_tlv_payload, payload_len);
  memset(priv->rx_frame, 0, sizeof(priv->rx_frame));

  ret = esp_hosted_spi_exchange_frame(priv, priv->tx_frame, priv->rx_frame);
  if (ret < 0)
    {
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  priv->stats.rpc_request_count++;
  priv->stats.rpc_last_request_id = request_id;

  ninfo("ESP-Hosted RPC request: id=%" PRIu32 " uid=%" PRIu32
        " len=%zu\n",
        request_id, priv->stats.rpc_last_uid, payload_len);

  parse_ret = esp_hosted_parse_rx_frame(priv, priv->rx_frame);
  nxmutex_unlock(&priv->lock);
  return parse_ret;
}

#ifdef CONFIG_ESP_HOSTED_WLAN
static int esp_hosted_run_wlan_control_start(
  FAR struct esp_hosted_driver_s *priv)
{
  int ret;

  if (priv->wlan_control_started)
    {
      return OK;
    }

  if (!priv->have_fwversion)
    {
      return -EAGAIN;
    }

  priv->wlan_control_start_pending = false;

  ret = esp_hosted_send_rpc_request(priv, ESP_HOSTED_RPC_WIFI_INIT);
  if (ret < 0)
    {
      priv->stats.wlan_control_start_error_count++;
      return ret;
    }

  ret = esp_hosted_send_rpc_request(priv, ESP_HOSTED_RPC_SET_MODE);
  if (ret < 0)
    {
      priv->stats.wlan_control_start_error_count++;
      return ret;
    }

  ret = esp_hosted_send_rpc_request(priv, ESP_HOSTED_RPC_WIFI_START);
  if (ret < 0)
    {
      priv->stats.wlan_control_start_error_count++;
      return ret;
    }

  priv->wlan_control_started = true;
  priv->stats.wlan_control_start_count++;

  ninfo("ESP-Hosted STA control path start requested\n");
  return OK;
}
#endif

static void esp_hosted_run_startup_probe(FAR struct esp_hosted_driver_s *priv)
{
  int ret;

  if (!priv->startup_probe_pending || priv->startup_probe_sent)
    {
      return;
    }

  priv->startup_probe_pending = false;
  priv->startup_probe_sent = true;

  ret = esp_hosted_send_rpc_request(priv,
                                    ESP_HOSTED_RPC_GET_COPROCESSOR_FWVERSION);
  if (ret < 0)
    {
      nwarn("ESP-Hosted firmware-version RPC failed: %d\n", ret);
    }
}

static int esp_hosted_exchange_dummy_locked(
  FAR struct esp_hosted_driver_s *priv)
{
  int ret;
  int parse_ret;

  esp_hosted_build_dummy(priv, priv->tx_frame);
  memset(priv->rx_frame, 0, sizeof(priv->rx_frame));

  ret = esp_hosted_spi_exchange_frame(priv, priv->tx_frame, priv->rx_frame);
  if (ret < 0)
    {
      return ret;
    }

  parse_ret = esp_hosted_parse_rx_frame(priv, priv->rx_frame);
  return parse_ret;
}

static int esp_hosted_exchange_dummy(FAR struct esp_hosted_driver_s *priv)
{
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_exchange_dummy_locked(priv);
  nxmutex_unlock(&priv->lock);
  return ret;
}

static int esp_hosted_drain_rx(FAR struct esp_hosted_driver_s *priv,
                               bool force_once, unsigned int limit,
                               bool nonblocking)
{
  unsigned int i;
  int drained = 0;
  int ret;

  if (limit == 0 || limit > ESP_HOSTED_RX_WORK_DRAIN_LIMIT)
    {
      limit = ESP_HOSTED_RX_WORK_DRAIN_LIMIT;
    }

  for (i = 0; i < limit; i++)
    {
      bool had_dataready = priv->dataready_seen;

#ifdef CONFIG_ESP_HOSTED_WLAN
      /* Leave frames in the coprocessor when the NuttX network thread is
       * temporarily blocked on the network lock.  Pulling more frames into
       * IOB chains here only exhausts the shared pool and drops TCP data.
       */

      if (priv->wlan_registered &&
          esp_hosted_rx_queue_depth(priv) >=
            ESP_HOSTED_RX_QUEUE_HIGH_WATER)
        {
          break;
        }
#endif

      if (!had_dataready &&
          !priv->config.gpio->data_ready(priv->config.gpio_arg))
        {
          if (!force_once)
            {
              break;
            }

          force_once = false;
          priv->stats.rx_forced_poll_count++;
        }

      ret = nonblocking ? nxmutex_trylock(&priv->lock) :
                          nxmutex_lock(&priv->lock);
      if (ret < 0)
        {
          return drained > 0 ? drained : ret;
        }

      if (had_dataready)
        {
          priv->dataready_seen = false;
        }

      ret = esp_hosted_exchange_dummy_locked(priv);
      nxmutex_unlock(&priv->lock);
      if (ret < 0)
        {
          nwarn("ESP-Hosted RX drain failed: %d\n", ret);
          return drained > 0 ? drained : ret;
        }

      drained++;
    }

  return drained;
}

static void esp_hosted_rx_work(FAR void *arg)
{
  FAR struct esp_hosted_driver_s *priv = arg;
  bool force_poll;
#ifdef CONFIG_ESP_HOSTED_WLAN
  int ret;
#endif

  if (priv->recovering)
    {
      return;
    }

  force_poll = priv->rx_force_poll;
  priv->rx_force_poll = false;

  (void)esp_hosted_drain_rx(priv, force_poll,
                            ESP_HOSTED_RX_WORK_DRAIN_LIMIT, false);
  esp_hosted_run_startup_probe(priv);
  (void)esp_hosted_drain_rx(priv, false,
                            ESP_HOSTED_RX_WORK_DRAIN_LIMIT, false);

#ifdef CONFIG_ESP_HOSTED_WLAN
  if (priv->wlan_control_start_pending)
    {
      ret = esp_hosted_run_wlan_control_start(priv);
      if (ret < 0 && ret != -EAGAIN)
        {
          nwarn("ESP-Hosted STA control start failed: %d\n", ret);
        }
    }

  (void)esp_hosted_drain_rx(priv, false,
                            ESP_HOSTED_RX_WORK_DRAIN_LIMIT, false);

  if (priv->wlan_control_started && !priv->have_mac &&
      !priv->mac_probe_sent)
    {
      priv->mac_probe_sent = true;
      ret = esp_hosted_send_rpc_request(priv, ESP_HOSTED_RPC_GET_MAC_ADDRESS);
      if (ret < 0)
        {
          priv->mac_probe_sent = false;
          nwarn("ESP-Hosted MAC RPC failed: %d\n", ret);
        }
    }

  (void)esp_hosted_drain_rx(priv, false,
                            ESP_HOSTED_RX_WORK_DRAIN_LIMIT, false);
#endif

  if (priv->recovering)
    {
      return;
    }

#ifdef CONFIG_ESP_HOSTED_WLAN
  if (priv->wlan_registered &&
      esp_hosted_rx_queue_depth(priv) >= ESP_HOSTED_RX_QUEUE_HIGH_WATER)
    {
      /* Recheck after a bounded delay so the higher-priority netdev thread
       * can consume the queued packets before this producer drains more.
       */

      esp_hosted_arm_rx_poll(priv, ESP_HOSTED_RX_POLL_IRQ_BUDGET);
      esp_hosted_schedule_rx_poll(priv);
      return;
    }
#endif

  if (priv->config.gpio->data_ready(priv->config.gpio_arg))
    {
      int schedule_ret;

      priv->dataready_seen = true;
#ifdef CONFIG_ESP_HOSTED_WLAN
      esp_hosted_arm_rx_poll(priv, ESP_HOSTED_RX_POLL_IRQ_BUDGET);
#endif
      schedule_ret = esp_hosted_schedule_rx_work(priv);
      if (schedule_ret >= 0)
        {
          return;
        }
    }

  esp_hosted_schedule_rx_poll(priv);
}

static void esp_hosted_rx_poll_work(FAR void *arg)
{
  FAR struct esp_hosted_driver_s *priv = arg;

#ifdef CONFIG_ESP_HOSTED_WLAN
  if (priv->wlan_registered && priv->ifup && priv->carrier_on)
    {
      priv->rx_force_poll = true;
      esp_hosted_schedule_rx_work(priv);
    }
#else
  esp_hosted_schedule_rx_work(priv);
#endif
}

static int esp_hosted_attach_irq(FAR struct esp_hosted_driver_s *priv)
{
  int ret;

  if (priv->config.gpio->attach_dataready == NULL)
    {
      return OK;
    }

  ret = priv->config.gpio->attach_dataready(priv->config.gpio_arg,
                                            esp_hosted_dataready_isr,
                                            priv);
  if (ret < 0)
    {
      return ret;
    }

  return OK;
}

static int esp_hosted_enable_irq(FAR struct esp_hosted_driver_s *priv,
                                 bool enable)
{
  if (priv->config.gpio->enable_dataready_irq == NULL)
    {
      return OK;
    }

  return priv->config.gpio->enable_dataready_irq(priv->config.gpio_arg,
                                                 enable);
}

static void esp_hosted_reset_coprocessor(FAR struct esp_hosted_driver_s *priv)
{
  priv->stats.reset_count++;

  priv->config.gpio->reset(priv->config.gpio_arg, true);
  up_mdelay(ESP_HOSTED_RESET_ASSERT_MS);
  priv->config.gpio->reset(priv->config.gpio_arg, false);
  up_mdelay(ESP_HOSTED_RESET_RELEASE_MS);
}

static void esp_hosted_prime_transport(FAR struct esp_hosted_driver_s *priv)
{
  int ret;

  if (!priv->config.gpio->handshake_ready(priv->config.gpio_arg))
    {
      ninfo("ESP-Hosted handshake is not ready after reset\n");
      return;
    }

  ret = esp_hosted_exchange_dummy(priv);
  if (ret < 0)
    {
      nwarn("ESP-Hosted initial SPI exchange failed: %d\n", ret);
    }
  else
    {
      ninfo("ESP-Hosted initial SPI exchange completed\n");
    }

  if (priv->config.gpio->data_ready(priv->config.gpio_arg))
    {
      priv->dataready_seen = true;
      esp_hosted_schedule_rx_work(priv);
    }
  else if (priv->startup_probe_pending
#ifdef CONFIG_ESP_HOSTED_WLAN
           || priv->wlan_control_start_pending
#endif
          )
    {
      esp_hosted_schedule_rx_work(priv);
    }
}

static void esp_hosted_reset_runtime_state(FAR struct esp_hosted_driver_s *priv)
{
  priv->dataready_seen = false;
  priv->init_seen = false;
  priv->startup_probe_pending = true;
  priv->startup_probe_sent = false;
  priv->have_fwversion = false;
  memset(&priv->fwversion, 0, sizeof(priv->fwversion));

#ifdef CONFIG_ESP_HOSTED_WLAN
  priv->ifup = false;
  priv->carrier_on = false;
  priv->rx_poll_budget = 0;
  priv->rx_force_poll = false;
  priv->wlan_control_start_pending = false;
  priv->wlan_control_started = false;
  priv->mac_probe_sent = false;
  priv->scan_in_progress = false;
  priv->scan_done = false;
  priv->scan_records_valid = false;
  priv->scan_status = 0;
  priv->scan_ap_available = 0;
  priv->scan_result_count = 0;
  priv->scan_request_count = 0;
  esp_hosted_wlan_clear_rx_queue(priv);

  if (priv->wlan_registered)
    {
      netdev_lower_carrier_off(&priv->lower);
    }
#endif
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp_hosted_spi_initialize(FAR const struct esp_hosted_config_s *config)
{
  int ret;

  ret = esp_hosted_validate_config(config);
  if (ret < 0)
    {
      return ret;
    }

  memset(&g_esp_hosted, 0, sizeof(g_esp_hosted));
  g_esp_hosted.config = *config;
  nxmutex_init(&g_esp_hosted.lock);

#ifdef CONFIG_ESP_HOSTED_WLAN
  spin_lock_init(&g_esp_hosted.rx_lock);
  IOB_QINIT(&g_esp_hosted.rx_queue);
  g_esp_hosted.auth_wpa = IW_AUTH_WPA_VERSION_WPA2;
  g_esp_hosted.cipher_pairwise = IW_AUTH_CIPHER_CCMP;
  g_esp_hosted.cipher_group = IW_AUTH_CIPHER_CCMP;
#endif

  /* Do not register wlan0 until the C6 has answered identity RPCs. */

  ret = esp_hosted_attach_irq(&g_esp_hosted);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_enable_irq(&g_esp_hosted, false);
  if (ret < 0)
    {
      return ret;
    }

  esp_hosted_reset_coprocessor(&g_esp_hosted);
  esp_hosted_prime_transport(&g_esp_hosted);

  ret = esp_hosted_enable_irq(&g_esp_hosted, true);
  if (ret < 0)
    {
      return ret;
    }

  if (g_esp_hosted.config.gpio->data_ready(g_esp_hosted.config.gpio_arg))
    {
      g_esp_hosted.dataready_seen = true;
      esp_hosted_schedule_rx_work(&g_esp_hosted);
    }
  else if (g_esp_hosted.startup_probe_pending)
    {
      esp_hosted_schedule_rx_work(&g_esp_hosted);
    }

  ninfo("ESP-Hosted SPI scaffold ready: frame=%u checksum=%04" PRIx16 "\n",
        ESP_HOSTED_SPI_MAX_FRAME,
        esp_hosted_checksum((FAR const uint8_t *)ESP_HOSTED_RPC_EP_NAME_RSP,
                            sizeof(ESP_HOSTED_RPC_EP_NAME_RSP) - 1));

  return OK;
}

int esp_hosted_spi_recover(void)
{
  FAR struct esp_hosted_driver_s *priv = &g_esp_hosted;
  int irq_ret;
  int ret;

  ret = esp_hosted_validate_config(&priv->config);
  if (ret < 0)
    {
      return ret;
    }

  irq_ret = esp_hosted_enable_irq(priv, false);
  priv->recovering = true;
  (void)work_cancel_sync(ESP_HOSTED_WORK_QUEUE, &priv->rx_work);
  (void)work_cancel_sync(ESP_HOSTED_WORK_QUEUE, &priv->poll_work);
  (void)work_cancel(ESP_HOSTED_WORK_QUEUE, &priv->rx_work);
  (void)work_cancel(ESP_HOSTED_WORK_QUEUE, &priv->poll_work);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      priv->recovering = false;
      return ret;
    }

  esp_hosted_reset_runtime_state(priv);
  esp_hosted_reset_coprocessor(priv);
  nxmutex_unlock(&priv->lock);

  priv->recovering = false;
  esp_hosted_prime_transport(priv);
  esp_hosted_run_startup_probe(priv);
  (void)esp_hosted_drain_rx(priv, false,
                            ESP_HOSTED_RX_WORK_DRAIN_LIMIT, false);

  ret = esp_hosted_enable_irq(priv, true);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->config.gpio->data_ready(priv->config.gpio_arg))
    {
      priv->dataready_seen = true;
      esp_hosted_schedule_rx_work(priv);
    }
  else if (priv->startup_probe_pending)
    {
      esp_hosted_schedule_rx_work(priv);
    }

  return irq_ret < 0 ? irq_ret : OK;
}

int esp_hosted_spi_pump(unsigned int budget, bool force)
{
  FAR struct esp_hosted_driver_s *priv = &g_esp_hosted;
  int ret;

  ret = esp_hosted_validate_config(&priv->config);
  if (ret < 0)
    {
      return ret;
    }

  if (budget == 0)
    {
      budget = ESP_HOSTED_PUMP_DEFAULT_BUDGET;
    }
  else if (budget > ESP_HOSTED_PUMP_MAX_BUDGET)
    {
      budget = ESP_HOSTED_PUMP_MAX_BUDGET;
    }

  priv->stats.pump_call_count++;
  if (force)
    {
      priv->stats.pump_forced_count++;
    }

  priv->stats.last_pump_ticks = clock_systime_ticks();
  ret = esp_hosted_drain_rx(priv, force, budget, true);
  priv->stats.pump_last_ret = ret;
  if (ret == -EAGAIN)
    {
      priv->stats.pump_busy_count++;
    }
  else if (ret < 0)
    {
      priv->stats.pump_error_count++;
    }
  else
    {
      priv->stats.pump_frame_count += ret;
    }

  if (ret > 0)
    {
      esp_hosted_schedule_rx_poll(priv);
    }

  return ret;
}

int esp_hosted_spi_get_stats(FAR struct esp_hosted_stats_s *stats)
{
  if (stats == NULL)
    {
      return -EINVAL;
    }

  *stats = g_esp_hosted.stats;
  stats->current_dataready_seen = g_esp_hosted.dataready_seen ? 1 : 0;
#ifdef CONFIG_ESP_HOSTED_WLAN
  stats->current_rx_poll_budget = g_esp_hosted.rx_poll_budget;
  stats->current_rx_queue_depth =
    g_esp_hosted.stats.current_rx_queue_depth;
  stats->wlan_current_ifup = g_esp_hosted.ifup ? 1 : 0;
  stats->wlan_current_carrier_on = g_esp_hosted.carrier_on ? 1 : 0;
#endif
  return OK;
}
