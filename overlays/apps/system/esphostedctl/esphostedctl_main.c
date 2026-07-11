/****************************************************************************
 * apps/system/esphostedctl/esphostedctl_main.c
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

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <nuttx/wireless/esp_hosted.h>

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void esphostedctl_print_counter(FAR const char *name, uint32_t value)
{
  printf("  %-32s %" PRIu32 "\n", name, value);
}

static void esphostedctl_print_section(FAR const char *name)
{
  printf("%s:\n", name);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * esphostedctl_main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct esp_hosted_stats_s stats;
  int ret;

  ret = esp_hosted_spi_get_stats(&stats);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: esp_hosted_spi_get_stats failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  esphostedctl_print_section("state");
  esphostedctl_print_counter("dataready_seen",
                             stats.current_dataready_seen);
  esphostedctl_print_counter("rx_poll_budget",
                             stats.current_rx_poll_budget);
  esphostedctl_print_counter("rx_queue_depth",
                             stats.current_rx_queue_depth);
  esphostedctl_print_counter("wlan_ifup", stats.wlan_current_ifup);
  esphostedctl_print_counter("wlan_carrier_on",
                             stats.wlan_current_carrier_on);

  esphostedctl_print_section("transport");
  esphostedctl_print_counter("reset", stats.reset_count);
  esphostedctl_print_counter("dataready_irq", stats.dataready_irq_count);
  esphostedctl_print_counter("rx_work", stats.rx_work_count);
  esphostedctl_print_counter("rx_poll", stats.rx_poll_count);
  esphostedctl_print_counter("rx_idle_poll", stats.rx_idle_poll_count);
  esphostedctl_print_counter("rx_forced_poll",
                             stats.rx_forced_poll_count);
  esphostedctl_print_counter("rx_work_busy", stats.rx_work_busy_count);
  esphostedctl_print_counter("rx_poll_busy", stats.rx_poll_busy_count);
  esphostedctl_print_counter("pump_call", stats.pump_call_count);
  esphostedctl_print_counter("pump_busy", stats.pump_busy_count);
  esphostedctl_print_counter("pump_forced", stats.pump_forced_count);
  esphostedctl_print_counter("pump_frame", stats.pump_frame_count);
  esphostedctl_print_counter("pump_error", stats.pump_error_count);
  printf("  %-32s %" PRId32 "\n", "pump_last_ret",
         stats.pump_last_ret);
  esphostedctl_print_counter("last_tx_ticks", stats.last_tx_ticks);
  esphostedctl_print_counter("last_rx_ticks", stats.last_rx_ticks);
  esphostedctl_print_counter("last_pump_ticks", stats.last_pump_ticks);
  esphostedctl_print_counter("spi_transaction", stats.spi_transaction_count);
  esphostedctl_print_counter("tx_frame", stats.tx_frame_count);
  esphostedctl_print_counter("rx_frame", stats.rx_frame_count);
  esphostedctl_print_counter("tx_dummy", stats.tx_dummy_count);
  esphostedctl_print_counter("rx_dummy", stats.rx_dummy_count);
  esphostedctl_print_counter("control_timeout", stats.control_timeout_count);
  esphostedctl_print_counter("malformed_frame", stats.malformed_frame_count);
  esphostedctl_print_counter("checksum_error", stats.checksum_error_count);

  esphostedctl_print_section("rx");
  esphostedctl_print_counter("priv", stats.rx_priv_count);
  esphostedctl_print_counter("init_event", stats.rx_init_event_count);
  esphostedctl_print_counter("control", stats.rx_control_count);
  esphostedctl_print_counter("sta", stats.rx_sta_count);
  esphostedctl_print_counter("ap", stats.rx_ap_count);
  esphostedctl_print_counter("unknown", stats.rx_unknown_count);

  esphostedctl_print_section("rpc");
  esphostedctl_print_counter("tlv_tx", stats.rpc_tlv_tx_count);
  esphostedctl_print_counter("tlv_rx", stats.rpc_tlv_rx_count);
  esphostedctl_print_counter("tlv_error", stats.rpc_tlv_error_count);
  esphostedctl_print_counter("request", stats.rpc_request_count);
  esphostedctl_print_counter("response", stats.rpc_response_count);
  esphostedctl_print_counter("event", stats.rpc_event_count);
  esphostedctl_print_counter("malformed", stats.rpc_malformed_count);
  esphostedctl_print_counter("fwversion", stats.rpc_fwversion_count);
  esphostedctl_print_counter("mac", stats.rpc_mac_count);
  esphostedctl_print_counter("last_request_id", stats.rpc_last_request_id);
  esphostedctl_print_counter("last_response_id", stats.rpc_last_response_id);
  esphostedctl_print_counter("last_uid", stats.rpc_last_uid);

  esphostedctl_print_section("wlan");
  esphostedctl_print_counter("register", stats.wlan_register_count);
  esphostedctl_print_counter("register_error", stats.wlan_register_error_count);
  esphostedctl_print_counter("control_start", stats.wlan_control_start_count);
  esphostedctl_print_counter("control_start_error",
                             stats.wlan_control_start_error_count);
  esphostedctl_print_counter("connect", stats.wlan_connect_count);
  esphostedctl_print_counter("disconnect", stats.wlan_disconnect_count);
  esphostedctl_print_counter("scan_start", stats.wlan_scan_start_count);
  esphostedctl_print_counter("scan_done", stats.wlan_scan_done_count);
  esphostedctl_print_counter("scan_get_records",
                             stats.wlan_scan_get_records_count);
  esphostedctl_print_counter("scan_get_records_error",
                             stats.wlan_scan_get_records_error_count);
  esphostedctl_print_counter("scan_result", stats.wlan_scan_result_count);
  esphostedctl_print_counter("link_up", stats.wlan_link_up_count);
  esphostedctl_print_counter("link_down", stats.wlan_link_down_count);

  esphostedctl_print_section("netdev");
  esphostedctl_print_counter("tx", stats.netdev_tx_count);
  esphostedctl_print_counter("tx_error", stats.netdev_tx_error_count);
  esphostedctl_print_counter("tx_carrier_off",
                             stats.netdev_tx_carrier_off_count);
  esphostedctl_print_counter("tx_oversize", stats.netdev_tx_oversize_count);
  esphostedctl_print_counter("tx_copy_error",
                             stats.netdev_tx_copy_error_count);
  esphostedctl_print_counter("tx_spi_error", stats.netdev_tx_spi_error_count);
  esphostedctl_print_counter("tx_parse_error",
                             stats.netdev_tx_parse_error_count);
  esphostedctl_print_counter("tx_last_len", stats.netdev_tx_last_len);
  printf("  %-32s %" PRId32 "\n",
         "tx_last_ret", stats.netdev_tx_last_ret);
  printf("  %-32s %" PRId32 "\n",
         "tx_last_parse_ret", stats.netdev_tx_last_parse_ret);
  esphostedctl_print_counter("rx", stats.netdev_rx_count);
  esphostedctl_print_counter("rx_queue_max", stats.rx_queue_max_depth);
  esphostedctl_print_counter("rx_dropped", stats.netdev_rx_dropped_count);
  esphostedctl_print_counter("rx_echo_dropped",
                             stats.netdev_rx_echo_dropped_count);

  return EXIT_SUCCESS;
}
