/****************************************************************************
 * include/nuttx/usb/rp23xx_pio_usbhost.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

#ifndef __INCLUDE_NUTTX_USB_RP23XX_PIO_USBHOST_H
#define __INCLUDE_NUTTX_USB_RP23XX_PIO_USBHOST_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/usb/usbhost.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

#define RP23XX_PIO_USBHOST_INFO_PORTS 7
#define RP23XX_PIO_USBHOST_INFO_EPS 16
#define RP23XX_PIO_USBHOST_INFO_XFER_BYTES 8

struct rp23xx_pio_usbhost_epinfo_s
{
  bool allocated;
  bool pending;
  bool async;
  bool in;
  uint8_t epaddr;
  uint8_t xfrtype;
  uint8_t interval;
  uint8_t ll_ep;
  uint8_t funcaddr;
  uint8_t speed;
  uint16_t maxpacket;
  size_t buflen;
  ssize_t result;
  uint8_t last_in_len;
  uint8_t last_in_data[RP23XX_PIO_USBHOST_INFO_XFER_BYTES];

  bool ll_valid;
  bool ll_is_tx;
  bool ll_has_transfer;
  uint8_t ll_dev_addr;
  uint8_t ll_ep_num;
  uint8_t ll_attr;
  uint8_t ll_data_id;
  uint8_t ll_failed_count;
  uint16_t ll_size;
  uint16_t ll_actual_len;
  uint16_t ll_total_len;
};

struct rp23xx_pio_usbhost_info_s
{
  bool initialized;
  bool timer_started;
  bool frame_thread_started;
  bool frame_pending;
  bool frame_active;
  bool connected;

  bool root_initialized;
  bool root_connected;
  bool root_fullspeed;
  bool root_suspended;
  uint8_t root_event;
  uint8_t root_line_state;
  uint8_t root_pin_dp;
  uint8_t root_pin_dm;
  uint32_t root_ints;
  uint32_t root_ep_complete;
  uint32_t root_ep_error;
  uint32_t root_ep_stalled;
  uint32_t frame_number;

  bool change_pending;
  bool hport_connected;
  bool hport_has_class;
  uint8_t hport_funcaddr;
  uint8_t hport_speed;
  uint32_t enum_count;
  int last_enum_ret;

  uint8_t last_ctrl_dirin;
  uint8_t last_ctrl_type;
  uint8_t last_ctrl_req;
  uint8_t last_ctrl_phase;
  uint16_t last_ctrl_value;
  uint16_t last_ctrl_index;
  uint16_t last_ctrl_len;
  int last_ctrl_ret;
  uint8_t last_ctrl_data_len;
  uint8_t last_ctrl_data[8];
  uint8_t last_ctrl_funcaddr;
  uint8_t last_ctrl_speed;
  uint8_t last_ctrl_parent;
  uint8_t last_ctrl_ll_ep;

  uint8_t prev_ctrl_dirin;
  uint8_t prev_ctrl_type;
  uint8_t prev_ctrl_req;
  uint8_t prev_ctrl_phase;
  uint16_t prev_ctrl_value;
  uint16_t prev_ctrl_index;
  uint16_t prev_ctrl_len;
  int prev_ctrl_ret;

  int addr0_probe_ret;
  int addr0_setaddr_ret;
  int addr1_retry_ret;
  uint8_t addr0_probe_len;
  uint8_t addr0_probe_data[8];

  int setaddr_status_ret;
  int32_t setaddr_in_result;
  uint32_t setaddr_in_pid;
  uint32_t setaddr_in_expected_pid;
  uint32_t setaddr_setup_handshake;
  uint32_t setaddr_tx_len;
  uint32_t setaddr_tx_wait_stage;
  uint32_t setaddr_tx_timeout_count;
  uint32_t setaddr_tx_irq;
  uint32_t setaddr_tx_fdebug;
  uint32_t setaddr_tx_flevel;
  uint32_t setaddr_tx_pc;

  uint8_t hub_nports;
  uint16_t hub_characteristics;
  uint8_t hub_lpsm;
  bool hub_compound;
  uint8_t hub_ocmode;
  bool hub_indicator;
  uint16_t hub_pwrondelay_ms;
  uint8_t hub_ctrlcurrent;
  uint8_t hub_devattached;
  uint8_t hub_pwrctrlmask;
  uint8_t hub_port_valid;
  uint8_t hub_port_power_valid;
  uint16_t hub_port_status[RP23XX_PIO_USBHOST_INFO_PORTS];
  uint16_t hub_port_change[RP23XX_PIO_USBHOST_INFO_PORTS];
  int hub_port_power_ret[RP23XX_PIO_USBHOST_INFO_PORTS];

  uint8_t ep_count;
  struct rp23xx_pio_usbhost_epinfo_s ep[RP23XX_PIO_USBHOST_INFO_EPS];

  uint32_t ll_last_tx_len;
  uint32_t ll_last_tx_wait_stage;
  uint32_t ll_tx_timeout_count;
  uint32_t ll_last_tx_irq;
  uint32_t ll_last_tx_fdebug;
  uint32_t ll_last_tx_flevel;
  uint32_t ll_last_tx_pc;
  uint32_t ll_last_rx_start_ok;
  uint32_t ll_last_rx_start_irq;
  uint32_t ll_rx_start_timeout_count;
  uint32_t ll_last_setup_fail;
  uint32_t ll_last_setup_handshake;
  uint32_t ll_last_setup_rx0;
  uint32_t ll_last_setup_rx1;
  uint32_t ll_last_setup_failed_count;
  int32_t ll_last_in_result;
  uint32_t ll_last_in_pid;
  uint32_t ll_last_in_expected_pid;
  uint32_t ll_last_in_rx0;
  uint32_t ll_last_in_rx1;
  uint32_t ll_last_in_failed_count;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

#ifdef CONFIG_RP23XX_PIO_USBHOST
int rp23xx_pio_usbhost_initialize(FAR struct usbhost_connection_s **conn);
int rp23xx_pio_usbhost_vbus_cycle(void);
int rp23xx_pio_usbhost_info(FAR struct rp23xx_pio_usbhost_info_s *info);
int rp23xx_pio_usbhost_hub_portstatus(uint8_t port, bool setpower,
                                       FAR uint16_t *status,
                                       FAR uint16_t *change);
#endif

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __INCLUDE_NUTTX_USB_RP23XX_PIO_USBHOST_H */
