/****************************************************************************
 * include/nuttx/wireless/esp_hosted.h
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

#ifndef __INCLUDE_NUTTX_WIRELESS_ESP_HOSTED_H
#define __INCLUDE_NUTTX_WIRELESS_ESP_HOSTED_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/compiler.h>
#include <nuttx/irq.h>
#include <nuttx/spi/spi.h>

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP_HOSTED_SPI_MAX_FRAME        1600
#define ESP_HOSTED_MAX_FRAGMENT_PAYLOAD 8192
#define ESP_HOSTED_MAC_SIZE             6

#define ESP_HOSTED_RPC_EP_NAME_RSP      "RPCRsp"
#define ESP_HOSTED_RPC_EP_NAME_EVT      "RPCEvt"

#define ESP_HOSTED_HDR_IF_TYPE(h)       ((h)->if_type_num & 0x0f)
#define ESP_HOSTED_HDR_IF_NUM(h)        (((h)->if_type_num >> 4) & 0x0f)
#define ESP_HOSTED_HDR_SET_IF(h, t, n) \
  do \
    { \
      (h)->if_type_num = (((n) & 0x0f) << 4) | ((t) & 0x0f); \
    } \
  while (0)

#define ESP_HOSTED_FLAG_MORE_FRAGMENT       (1 << 0)
#define ESP_HOSTED_FLAG_WAKEUP_PKT          (1 << 1)
#define ESP_HOSTED_FLAG_POWER_SAVE_STARTED  (1 << 2)
#define ESP_HOSTED_FLAG_POWER_SAVE_STOPPED  (1 << 3)

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum esp_hosted_if_type_e
{
  ESP_HOSTED_INVALID_IF = 0,
  ESP_HOSTED_STA_IF     = 1,
  ESP_HOSTED_AP_IF      = 2,
  ESP_HOSTED_SERIAL_IF  = 3,
  ESP_HOSTED_HCI_IF     = 4,
  ESP_HOSTED_PRIV_IF    = 5,
  ESP_HOSTED_TEST_IF    = 6,
  ESP_HOSTED_ETH_IF     = 7,
  ESP_HOSTED_MAX_IF     = 8
};

enum esp_hosted_priv_packet_type_e
{
  ESP_HOSTED_PACKET_TYPE_EVENT = 0x33
};

enum esp_hosted_priv_event_type_e
{
  ESP_HOSTED_PRIV_EVENT_INIT = 0x22
};

enum esp_hosted_rpc_id_e
{
  ESP_HOSTED_RPC_GET_MAC_ADDRESS           = 257,
  ESP_HOSTED_RPC_SET_MODE                  = 260,
  ESP_HOSTED_RPC_WIFI_INIT                 = 278,
  ESP_HOSTED_RPC_WIFI_START                = 280,
  ESP_HOSTED_RPC_WIFI_CONNECT              = 282,
  ESP_HOSTED_RPC_WIFI_DISCONNECT           = 283,
  ESP_HOSTED_RPC_WIFI_SET_CONFIG           = 284,
  ESP_HOSTED_RPC_WIFI_SCAN_START           = 286,
  ESP_HOSTED_RPC_WIFI_SCAN_GET_AP_NUM      = 288,
  ESP_HOSTED_RPC_WIFI_SCAN_GET_AP_RECORDS  = 289,
  ESP_HOSTED_RPC_WIFI_STA_GET_AP_INFO      = 294,
  ESP_HOSTED_RPC_GET_COPROCESSOR_FWVERSION = 350,
  ESP_HOSTED_RPC_WIFI_STA_GET_RSSI         = 341
};

enum esp_hosted_rpc_response_id_e
{
  ESP_HOSTED_RPC_RESP_GET_MAC_ADDRESS           = 513,
  ESP_HOSTED_RPC_RESP_SET_MODE                  = 516,
  ESP_HOSTED_RPC_RESP_WIFI_INIT                 = 534,
  ESP_HOSTED_RPC_RESP_WIFI_START                = 536,
  ESP_HOSTED_RPC_RESP_WIFI_CONNECT              = 538,
  ESP_HOSTED_RPC_RESP_WIFI_DISCONNECT           = 539,
  ESP_HOSTED_RPC_RESP_WIFI_SET_CONFIG           = 540,
  ESP_HOSTED_RPC_RESP_WIFI_SCAN_START           = 542,
  ESP_HOSTED_RPC_RESP_WIFI_SCAN_GET_AP_NUM      = 544,
  ESP_HOSTED_RPC_RESP_WIFI_SCAN_GET_AP_RECORDS  = 545,
  ESP_HOSTED_RPC_RESP_GET_COPROCESSOR_FWVERSION = 606
};

enum esp_hosted_rpc_event_id_e
{
  ESP_HOSTED_RPC_EVENT_STA_SCAN_DONE    = 774,
  ESP_HOSTED_RPC_EVENT_STA_CONNECTED    = 775,
  ESP_HOSTED_RPC_EVENT_STA_DISCONNECTED = 776
};

begin_packed_struct struct esp_hosted_payload_header_s
{
  uint8_t  if_type_num;
  uint8_t  flags;
  uint16_t len;
  uint16_t offset;
  uint16_t checksum;
  uint16_t seq_num;
  uint8_t  throttle_reserved;
  union
  {
    uint8_t reserved3;
    uint8_t hci_pkt_type;
    uint8_t priv_pkt_type;
  } u;
} end_packed_struct;

struct esp_hosted_stats_s
{
  uint32_t reset_count;
  uint32_t dataready_irq_count;
  uint32_t rx_work_count;
  uint32_t rx_poll_count;
  uint32_t rx_idle_poll_count;
  uint32_t rx_forced_poll_count;
  uint32_t rx_work_busy_count;
  uint32_t rx_poll_busy_count;
  uint32_t pump_call_count;
  uint32_t pump_busy_count;
  uint32_t pump_forced_count;
  uint32_t pump_frame_count;
  uint32_t pump_error_count;
  int32_t  pump_last_ret;
  uint32_t last_tx_ticks;
  uint32_t last_rx_ticks;
  uint32_t last_pump_ticks;
  uint32_t current_dataready_seen;
  uint32_t current_rx_poll_budget;
  uint32_t current_rx_queue_depth;
  uint32_t rx_queue_max_depth;
  uint32_t spi_transaction_count;
  uint32_t rx_frame_count;
  uint32_t tx_frame_count;
  uint32_t tx_dummy_count;
  uint32_t rx_dummy_count;
  uint32_t rx_priv_count;
  uint32_t rx_init_event_count;
  uint32_t rx_control_count;
  uint32_t rx_sta_count;
  uint32_t rx_ap_count;
  uint32_t rx_unknown_count;
  uint32_t malformed_frame_count;
  uint32_t checksum_error_count;
  uint32_t control_timeout_count;
  uint32_t rpc_tlv_tx_count;
  uint32_t rpc_tlv_rx_count;
  uint32_t rpc_tlv_error_count;
  uint32_t rpc_request_count;
  uint32_t rpc_response_count;
  uint32_t rpc_event_count;
  uint32_t rpc_malformed_count;
  uint32_t rpc_fwversion_count;
  uint32_t rpc_mac_count;
  uint32_t rpc_last_request_id;
  uint32_t rpc_last_response_id;
  uint32_t rpc_last_uid;
  uint32_t wlan_register_count;
  uint32_t wlan_register_error_count;
  uint32_t wlan_control_start_count;
  uint32_t wlan_control_start_error_count;
  uint32_t wlan_connect_count;
  uint32_t wlan_disconnect_count;
  uint32_t wlan_scan_start_count;
  uint32_t wlan_scan_done_count;
  uint32_t wlan_scan_get_records_count;
  uint32_t wlan_scan_get_records_error_count;
  uint32_t wlan_scan_result_count;
  uint32_t wlan_link_up_count;
  uint32_t wlan_link_down_count;
  uint32_t wlan_current_ifup;
  uint32_t wlan_current_carrier_on;
  uint32_t netdev_tx_count;
  uint32_t netdev_tx_error_count;
  uint32_t netdev_tx_carrier_off_count;
  uint32_t netdev_tx_oversize_count;
  uint32_t netdev_tx_copy_error_count;
  uint32_t netdev_tx_spi_error_count;
  uint32_t netdev_tx_parse_error_count;
  uint32_t netdev_tx_last_len;
  int32_t  netdev_tx_last_ret;
  int32_t  netdev_tx_last_parse_ret;
  uint32_t netdev_rx_count;
  uint32_t netdev_rx_dropped_count;
  uint32_t netdev_rx_echo_dropped_count;
};

struct esp_hosted_gpio_ops_s
{
  CODE int (*reset)(FAR void *arg, bool asserted);
  CODE bool (*handshake_ready)(FAR void *arg);
  CODE bool (*data_ready)(FAR void *arg);
  CODE int (*attach_dataready)(FAR void *arg, xcpt_t handler,
                               FAR void *israrg);
  CODE int (*enable_dataready_irq)(FAR void *arg, bool enable);
};

struct esp_hosted_config_s
{
  FAR struct spi_dev_s *spi;
  uint32_t spi_devid;
  uint32_t spi_frequency;
  uint8_t  spi_mode;
  uint8_t  spi_bits;

  FAR const struct esp_hosted_gpio_ops_s *gpio;
  FAR void *gpio_arg;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int esp_hosted_spi_initialize(FAR const struct esp_hosted_config_s *config);
int esp_hosted_spi_recover(void);
int esp_hosted_spi_get_stats(FAR struct esp_hosted_stats_s *stats);
int esp_hosted_spi_pump(unsigned int budget, bool force);

#endif /* __INCLUDE_NUTTX_WIRELESS_ESP_HOSTED_H */
