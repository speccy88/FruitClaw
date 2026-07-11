/****************************************************************************
 * arch/arm/src/rp23xx/rp23xx_pio_usbhost.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttX USB host-controller wrapper for Pico-PIO-USB on RP23XX.
 * The bit-level PIO transport is from Pico-PIO-USB:
 *
 *   Copyright (c) 2021 sekigon-gonnoc
 *                      Ha Thach (thach@tinyusb.org)
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/kthread.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/usb/hub.h>
#include <nuttx/usb/usb.h>
#include <nuttx/usb/usbhost.h>
#include <nuttx/usb/usbhost_devaddr.h>
#include <nuttx/wqueue.h>

#include <arch/board/board.h>
#include <arch/irq.h>

#include "arm_internal.h"
#include "hardware/rp23xx_timer.h"
#include "rp23xx_gpio.h"
#include "rp23xx_pio_usbhost.h"
#include "pio_usb/pio_usb.h"
#include "pio_usb/pio_usb_ll.h"

#undef RP23XX_TIMER_BASE
#define RP23XX_TIMER_BASE RP23XX_TIMER0_BASE

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_RP23XX_PIO_USBHOST_DESCSIZE
#  define CONFIG_RP23XX_PIO_USBHOST_DESCSIZE 128
#endif

#ifndef CONFIG_RP23XX_PIO_USBHOST_XFER_TIMEOUT_MS
#  define CONFIG_RP23XX_PIO_USBHOST_XFER_TIMEOUT_MS 5000
#endif

#ifndef CONFIG_RP23XX_PIO_USBHOST_FRAME_PRIO
#  define CONFIG_RP23XX_PIO_USBHOST_FRAME_PRIO 90
#endif

#ifndef CONFIG_RP23XX_PIO_USBHOST_FRAME_STACKSIZE
#  define CONFIG_RP23XX_PIO_USBHOST_FRAME_STACKSIZE 2048
#endif

#ifndef CONFIG_RP23XX_PIO_USBHOST_ENUM_DELAY_MS
#  define CONFIG_RP23XX_PIO_USBHOST_ENUM_DELAY_MS 100
#endif

#ifndef CONFIG_RP23XX_PIO_USBHOST_RESET_MS
#  define CONFIG_RP23XX_PIO_USBHOST_RESET_MS 20
#endif

#ifndef CONFIG_RP23XX_PIO_USBHOST_POST_RESET_MS
#  define CONFIG_RP23XX_PIO_USBHOST_POST_RESET_MS 50
#endif

#ifndef CONFIG_RP23XX_PIO_USBHOST_SETADDR_DELAY_MS
#  define CONFIG_RP23XX_PIO_USBHOST_SETADDR_DELAY_MS 10
#endif

#ifndef CONFIG_RP23XX_PIO_USBHOST_VBUS_OFF_MS
#  define CONFIG_RP23XX_PIO_USBHOST_VBUS_OFF_MS 100
#endif

#ifndef CONFIG_RP23XX_PIO_USBHOST_VBUS_ON_MS
#  define CONFIG_RP23XX_PIO_USBHOST_VBUS_ON_MS 250
#endif

#ifndef CONFIG_RP23XX_PIO_USBHOST_CTRL_GAP_MS
#  define CONFIG_RP23XX_PIO_USBHOST_CTRL_GAP_MS 1
#endif

#ifndef CONFIG_RP23XX_PIO_USBHOST_CTRL_RETRIES
#  define CONFIG_RP23XX_PIO_USBHOST_CTRL_RETRIES 2
#endif

#ifndef CONFIG_RP23XX_PIO_USBHOST_ENUM_ATTEMPTS
#  define CONFIG_RP23XX_PIO_USBHOST_ENUM_ATTEMPTS 3
#endif

#define RP23XX_PIOUSB_ROOT_INDEX    0
#define RP23XX_PIOUSB_EP0_INDEX     0
#define RP23XX_PIOUSB_NO_LL_EP      0xff
#define RP23XX_PIOUSB_CHANGE_QUEUE_SIZE 8

#define RP23XX_PIOUSB_TIMEOUT \
  MSEC2TICK(CONFIG_RP23XX_PIO_USBHOST_XFER_TIMEOUT_MS)

#if CONFIG_RP23XX_PIO_USBHOST_TIMER_ALARM == 0
#  define RP23XX_PIOUSB_TIMER_IRQ  RP23XX_TIMER0_IRQ_0
#  define RP23XX_PIOUSB_TIMER_ALARM RP23XX_TIMER_ALARM0
#  define RP23XX_PIOUSB_TIMER_MASK RP23XX_TIMER_INTR_ALARM_0_MASK
#elif CONFIG_RP23XX_PIO_USBHOST_TIMER_ALARM == 1
#  define RP23XX_PIOUSB_TIMER_IRQ  RP23XX_TIMER0_IRQ_1
#  define RP23XX_PIOUSB_TIMER_ALARM RP23XX_TIMER_ALARM1
#  define RP23XX_PIOUSB_TIMER_MASK RP23XX_TIMER_INTR_ALARM_1_MASK
#elif CONFIG_RP23XX_PIO_USBHOST_TIMER_ALARM == 2
#  define RP23XX_PIOUSB_TIMER_IRQ  RP23XX_TIMER0_IRQ_2
#  define RP23XX_PIOUSB_TIMER_ALARM RP23XX_TIMER_ALARM2
#  define RP23XX_PIOUSB_TIMER_MASK RP23XX_TIMER_INTR_ALARM_2_MASK
#else
#  define RP23XX_PIOUSB_TIMER_IRQ  RP23XX_TIMER0_IRQ_3
#  define RP23XX_PIOUSB_TIMER_ALARM RP23XX_TIMER_ALARM3
#  define RP23XX_PIOUSB_TIMER_MASK RP23XX_TIMER_INTR_ALARM_3_MASK
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rp23xx_pio_usbhost_ep_s
{
  bool allocated;
  bool pending;
  bool async;
  bool in;
  uint8_t funcaddr;
  uint8_t epaddr;
  uint8_t xfrtype;
  uint8_t interval;
  uint8_t ll_ep;
  uint16_t maxpacket;

  FAR struct usbhost_hubport_s *hport;
  FAR uint8_t *buffer;
  size_t buflen;
  ssize_t result;
  uint8_t last_in_len;
  uint8_t last_in_data[RP23XX_PIO_USBHOST_INFO_XFER_BYTES];

  sem_t waitsem;

#ifdef CONFIG_USBHOST_ASYNCH
  usbhost_asynch_t callback;
  FAR void *arg;
  struct work_s cbwork;
#endif
};

struct rp23xx_pio_usbhost_s
{
  struct usbhost_driver_s drvr;
  struct usbhost_connection_s conn;
  struct usbhost_roothubport_s rhport;
  struct usbhost_devaddr_s devgen;

  mutex_t lock;
  sem_t eventsem;
  sem_t framesem;

  FAR struct usbhost_hubport_s *change[RP23XX_PIOUSB_CHANGE_QUEUE_SIZE];
  uint8_t change_head;
  uint8_t change_tail;
  uint8_t change_count;
  bool initialized;
  bool timer_started;
  bool connected;
  bool frame_thread_started;
  volatile bool frame_pending;
  volatile bool frame_active;
  pid_t frame_pid;
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

  struct rp23xx_pio_usbhost_ep_s ep[PIO_USB_EP_POOL_CNT];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int piousb_wait(FAR struct usbhost_connection_s *conn,
                       FAR struct usbhost_hubport_s **hport);
static int piousb_enumerate(FAR struct usbhost_connection_s *conn,
                            FAR struct usbhost_hubport_s *hport);

static int piousb_ep0configure(FAR struct usbhost_driver_s *drvr,
                               usbhost_ep_t ep0, uint8_t funcaddr,
                               uint8_t speed, uint16_t maxpacketsize);
static int piousb_epalloc(FAR struct usbhost_driver_s *drvr,
                          FAR const struct usbhost_epdesc_s *epdesc,
                          FAR usbhost_ep_t *ep);
static int piousb_epfree(FAR struct usbhost_driver_s *drvr,
                         usbhost_ep_t ep);
static int piousb_alloc(FAR struct usbhost_driver_s *drvr,
                        FAR uint8_t **buffer, FAR size_t *maxlen);
static int piousb_free(FAR struct usbhost_driver_s *drvr,
                       FAR uint8_t *buffer);
static int piousb_ioalloc(FAR struct usbhost_driver_s *drvr,
                          FAR uint8_t **buffer, size_t buflen);
static int piousb_iofree(FAR struct usbhost_driver_s *drvr,
                         FAR uint8_t *buffer);
static int piousb_ctrlin(FAR struct usbhost_driver_s *drvr,
                         usbhost_ep_t ep0,
                         FAR const struct usb_ctrlreq_s *req,
                         FAR uint8_t *buffer);
static int piousb_ctrlout(FAR struct usbhost_driver_s *drvr,
                          usbhost_ep_t ep0,
                          FAR const struct usb_ctrlreq_s *req,
                          FAR const uint8_t *buffer);
static int piousb_ctrl_wait_stage(FAR struct rp23xx_pio_usbhost_ep_s *ep);
static int piousb_ctrlin_once(FAR struct rp23xx_pio_usbhost_ep_s *ep,
                              FAR const struct usb_ctrlreq_s *req,
                              FAR uint8_t *buffer, uint16_t buflen);
static int piousb_set_address_once(FAR struct rp23xx_pio_usbhost_ep_s *ep,
                                   uint8_t funcaddr);
static void piousb_ctrl_recover(FAR struct rp23xx_pio_usbhost_ep_s *ep,
                                int result);
static ssize_t piousb_transfer(FAR struct usbhost_driver_s *drvr,
                               usbhost_ep_t ep, FAR uint8_t *buffer,
                               size_t buflen);
#ifdef CONFIG_USBHOST_ASYNCH
static int piousb_asynch(FAR struct usbhost_driver_s *drvr,
                         usbhost_ep_t ep, FAR uint8_t *buffer,
                         size_t buflen, usbhost_asynch_t callback,
                         FAR void *arg);
#endif
static int piousb_cancel(FAR struct usbhost_driver_s *drvr,
                         usbhost_ep_t ep);
#ifdef CONFIG_USBHOST_HUB
static int piousb_connect(FAR struct usbhost_driver_s *drvr,
                          FAR struct usbhost_hubport_s *hport,
                          bool connected);
#endif
static void piousb_disconnect(FAR struct usbhost_driver_s *drvr,
                              FAR struct usbhost_hubport_s *hport);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rp23xx_pio_usbhost_s g_piousb;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint16_t piousb_getle16(FAR const uint8_t *value)
{
  return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static void piousb_putle16(FAR uint8_t *dest, uint16_t value)
{
  dest[0] = value & 0xff;
  dest[1] = value >> 8;
}

static uint32_t piousb_time_us(void)
{
  return getreg32(RP23XX_TIMER_TIMERAWL);
}

static FAR struct rp23xx_pio_usbhost_s *
piousb_from_connection(FAR struct usbhost_connection_s *conn)
{
  return (FAR struct rp23xx_pio_usbhost_s *)
    ((uintptr_t)conn - offsetof(struct rp23xx_pio_usbhost_s, conn));
}

static void piousb_timer_arm_next(void)
{
  putreg32(piousb_time_us() + 1000, RP23XX_PIOUSB_TIMER_ALARM);
}

#ifndef CONFIG_RP23XX_PIO_USBHOST_FRAME_IRQ
static int piousb_frame_thread(int argc, FAR char *argv[])
{
  FAR struct rp23xx_pio_usbhost_s *priv = &g_piousb;
  irqstate_t flags;

  for (; ; )
    {
      nxsem_wait_uninterruptible(&priv->framesem);

      if (!priv->timer_started)
        {
          flags = enter_critical_section();
          priv->frame_pending = false;
          leave_critical_section(flags);
          continue;
        }

      priv->frame_active = true;
      pio_usb_host_frame();
      priv->frame_active = false;

      flags = enter_critical_section();
      priv->frame_pending = false;
      leave_critical_section(flags);
    }

  return OK;
}
#endif

static int piousb_timer_isr(int irq, FAR void *context, FAR void *arg)
{
  FAR struct rp23xx_pio_usbhost_s *priv = &g_piousb;

  putreg32(RP23XX_PIOUSB_TIMER_MASK, RP23XX_TIMER_INTR);

  if (priv->timer_started)
    {
#ifdef CONFIG_RP23XX_PIO_USBHOST_FRAME_IRQ
      priv->frame_pending = true;
      priv->frame_active = true;
      piousb_timer_arm_next();
      pio_usb_host_frame();
      priv->frame_active = false;
      priv->frame_pending = false;
#else
      if (!priv->frame_pending)
        {
          priv->frame_pending = true;
          nxsem_post(&priv->framesem);
        }

      piousb_timer_arm_next();
#endif
    }

  return OK;
}

static int piousb_timer_start(FAR struct rp23xx_pio_usbhost_s *priv)
{
  int ret;

  if (priv->timer_started)
    {
      return OK;
    }

#ifndef CONFIG_RP23XX_PIO_USBHOST_FRAME_IRQ
  if (!priv->frame_thread_started)
    {
      ret = kthread_create("piousbfrm",
                           CONFIG_RP23XX_PIO_USBHOST_FRAME_PRIO,
                           CONFIG_RP23XX_PIO_USBHOST_FRAME_STACKSIZE,
                           piousb_frame_thread, NULL);
      if (ret < 0)
        {
          return ret;
        }

      priv->frame_pid = ret;
      priv->frame_thread_started = true;
    }
#endif

  ret = irq_attach(RP23XX_PIOUSB_TIMER_IRQ, piousb_timer_isr, NULL);
  if (ret < 0)
    {
      return ret;
    }

  putreg32(RP23XX_PIOUSB_TIMER_MASK, RP23XX_TIMER_INTR);
  setbits_reg32(RP23XX_PIOUSB_TIMER_MASK, RP23XX_TIMER_INTE);
  piousb_timer_arm_next();
  priv->timer_started = true;
  up_enable_irq(RP23XX_PIOUSB_TIMER_IRQ);

  return OK;
}

static uint8_t piousb_find_ll_ep(uint8_t root_idx, uint8_t funcaddr,
                                 uint8_t epaddr)
{
  int i;

  for (i = 0; i < PIO_USB_EP_POOL_CNT; i++)
    {
      FAR endpoint_t *ll = PIO_USB_ENDPOINT(i);

      if (ll->size != 0 && ll->root_idx == root_idx &&
          ll->dev_addr == funcaddr &&
          (ll->ep_num == epaddr ||
           (((epaddr & USB_EPNO_MASK) == 0) &&
            ((ll->ep_num & USB_EPNO_MASK) == 0))))
        {
          return i;
        }
    }

  return RP23XX_PIOUSB_NO_LL_EP;
}

static void piousb_epdesc(uint8_t desc[7], uint8_t epaddr, uint8_t xfrtype,
                          uint16_t maxpacket, uint8_t interval)
{
  desc[0] = 7;
  desc[1] = USB_DESC_TYPE_ENDPOINT;
  desc[2] = epaddr;
  desc[3] = xfrtype & USB_EP_ATTR_XFERTYPE_MASK;
  desc[4] = maxpacket & 0xff;
  desc[5] = maxpacket >> 8;
  desc[6] = interval;
}

static bool piousb_need_pre(FAR struct usbhost_hubport_s *hport)
{
#ifdef CONFIG_USBHOST_HUB
  return hport != NULL && hport->parent != NULL &&
         hport->speed == USB_SPEED_LOW;
#else
  return false;
#endif
}

static int piousb_open_ep(FAR struct rp23xx_pio_usbhost_ep_s *ep)
{
  uint8_t desc[7];
  uint8_t funcaddr;

  DEBUGASSERT(ep != NULL && ep->hport != NULL);

  funcaddr = ep->funcaddr;
  piousb_epdesc(desc, ep->epaddr, ep->xfrtype, ep->maxpacket, ep->interval);

  if (!pio_usb_host_endpoint_open(RP23XX_PIOUSB_ROOT_INDEX, funcaddr, desc,
                                  piousb_need_pre(ep->hport)))
    {
      return -ENOMEM;
    }

  ep->ll_ep = piousb_find_ll_ep(RP23XX_PIOUSB_ROOT_INDEX, funcaddr,
                                ep->epaddr);
  if (ep->ll_ep == RP23XX_PIOUSB_NO_LL_EP)
    {
      return -ENOENT;
    }

  return OK;
}

static int piousb_reconfigure_ep(FAR struct rp23xx_pio_usbhost_ep_s *ep)
{
  uint8_t desc[7];
  FAR endpoint_t *ll;

  DEBUGASSERT(ep != NULL && ep->hport != NULL);

  if (ep->ll_ep == RP23XX_PIOUSB_NO_LL_EP)
    {
      return piousb_open_ep(ep);
    }

  ll = PIO_USB_ENDPOINT(ep->ll_ep);
  if (ll->size == 0)
    {
      ep->ll_ep = RP23XX_PIOUSB_NO_LL_EP;
      return piousb_open_ep(ep);
    }

  piousb_epdesc(desc, ep->epaddr, ep->xfrtype, ep->maxpacket, ep->interval);
  pio_usb_ll_configure_endpoint(ll, desc);
  ll->root_idx = RP23XX_PIOUSB_ROOT_INDEX;
  ll->dev_addr = ep->funcaddr;
  ll->need_pre = piousb_need_pre(ep->hport);
  ll->is_tx = (ep->epaddr & 0x80) ? false : true;

  return OK;
}

static void piousb_ctrl_save_prev(void)
{
  g_piousb.prev_ctrl_dirin = g_piousb.last_ctrl_dirin;
  g_piousb.prev_ctrl_type = g_piousb.last_ctrl_type;
  g_piousb.prev_ctrl_req = g_piousb.last_ctrl_req;
  g_piousb.prev_ctrl_phase = g_piousb.last_ctrl_phase;
  g_piousb.prev_ctrl_value = g_piousb.last_ctrl_value;
  g_piousb.prev_ctrl_index = g_piousb.last_ctrl_index;
  g_piousb.prev_ctrl_len = g_piousb.last_ctrl_len;
  g_piousb.prev_ctrl_ret = g_piousb.last_ctrl_ret;
}

static void piousb_record_hub_ctrlin(FAR const struct usb_ctrlreq_s *req,
                                     FAR const uint8_t *buffer,
                                     uint16_t buflen)
{
  uint16_t index;
  uint8_t nports;

  if (buffer == NULL)
    {
      return;
    }

  if (req->type == (USB_REQ_DIR_IN | USBHUB_REQ_TYPE_HUB) &&
      req->req == USBHUB_REQ_GETDESCRIPTOR &&
      buflen >= 3 && buffer[1] == USB_DESC_TYPE_HUB)
    {
      nports = buffer[2];
      if (nports > RP23XX_PIO_USBHOST_INFO_PORTS)
        {
          nports = RP23XX_PIO_USBHOST_INFO_PORTS;
        }

      g_piousb.hub_nports = nports;
      if (buflen >= USB_SIZEOF_HUBDESC)
        {
          uint16_t hubchar = piousb_getle16(&buffer[3]);

          g_piousb.hub_characteristics = hubchar;
          g_piousb.hub_lpsm = (hubchar & USBHUB_CHAR_LPSM_MASK) >>
                              USBHUB_CHAR_LPSM_SHIFT;
          g_piousb.hub_compound =
            (hubchar & USBHUB_CHAR_COMPOUND) != 0;
          g_piousb.hub_ocmode = (hubchar & USBHUB_CHAR_OCPM_MASK) >>
                                USBHUB_CHAR_OCPM_SHIFT;
          g_piousb.hub_indicator =
            (hubchar & USBHUB_CHAR_PORTIND) != 0;
          g_piousb.hub_pwrondelay_ms = 2 * buffer[5];
          g_piousb.hub_ctrlcurrent = buffer[6];
          g_piousb.hub_devattached = buffer[7];
          g_piousb.hub_pwrctrlmask = buffer[8];
          g_piousb.hub_port_power_valid = 0;
        }

      return;
    }

  if (req->type == (USB_REQ_DIR_IN | USBHUB_REQ_TYPE_PORT) &&
      req->req == USBHUB_REQ_GETSTATUS && buflen >= 4)
    {
      index = piousb_getle16(req->index);
      if (index >= 1 && index <= RP23XX_PIO_USBHOST_INFO_PORTS)
        {
          index--;
          g_piousb.hub_port_status[index] =
            ((uint16_t)buffer[1] << 8) | buffer[0];
          g_piousb.hub_port_change[index] =
            ((uint16_t)buffer[3] << 8) | buffer[2];
          g_piousb.hub_port_valid |= (1u << index);
        }
    }
}

static bool piousb_can_ignore_status_stage(FAR const struct usb_ctrlreq_s *req)
{
  if (req->req == USB_REQ_SETADDRESS)
    {
      return false;
    }

  return true;
}

static int piousb_start_setup(FAR struct rp23xx_pio_usbhost_ep_s *ep,
                              FAR const struct usb_ctrlreq_s *req)
{
  irqstate_t flags;

  DEBUGASSERT(ep != NULL && req != NULL);

  if (ep->pending)
    {
      return -EBUSY;
    }

  nxsem_reset(&ep->waitsem, 0);

  flags = enter_critical_section();
  ep->buffer = (FAR uint8_t *)req;
  ep->buflen = USB_SIZEOF_CTRLREQ;
  ep->result = -EINPROGRESS;
  ep->async = false;
  ep->pending = true;
  ep->in = false;
  leave_critical_section(flags);

  if (!pio_usb_host_send_setup(RP23XX_PIOUSB_ROOT_INDEX,
                               ep->funcaddr,
                               (FAR const uint8_t *)req))
    {
      flags = enter_critical_section();
      ep->pending = false;
      ep->result = -EIO;
      leave_critical_section(flags);
      return -EIO;
    }

  return OK;
}

static int piousb_start_transfer(FAR struct rp23xx_pio_usbhost_ep_s *ep,
                                 FAR uint8_t *buffer, size_t buflen,
                                 bool async)
{
  uint8_t epaddr;
  irqstate_t flags;

  DEBUGASSERT(ep != NULL && ep->hport != NULL);

  if (ep->pending)
    {
      return -EBUSY;
    }

  if (!async)
    {
      nxsem_reset(&ep->waitsem, 0);
    }

  epaddr = ep->epaddr;
  if (ep->xfrtype == USB_EP_ATTR_XFER_CONTROL)
    {
      epaddr = ep->in ? 0x80 : 0x00;
    }

  flags = enter_critical_section();
  ep->buffer = buffer;
  ep->buflen = buflen;
  ep->result = -EINPROGRESS;
  ep->async = async;
  ep->pending = true;
  leave_critical_section(flags);

  if (!pio_usb_host_endpoint_transfer(RP23XX_PIOUSB_ROOT_INDEX,
                                      ep->funcaddr, epaddr,
                                      buffer, buflen))
    {
      flags = enter_critical_section();
      ep->pending = false;
      ep->result = -EIO;
      leave_critical_section(flags);
      return -EIO;
    }

  return OK;
}

static void piousb_record_setaddr_status(int ret)
{
  g_piousb.setaddr_status_ret = ret;
  g_piousb.setaddr_in_result = pio_usb_debug.last_in_result;
  g_piousb.setaddr_in_pid = pio_usb_debug.last_in_pid;
  g_piousb.setaddr_in_expected_pid = pio_usb_debug.last_in_expected_pid;
  g_piousb.setaddr_setup_handshake = pio_usb_debug.last_setup_handshake;
  g_piousb.setaddr_tx_len = pio_usb_debug.last_tx_len;
  g_piousb.setaddr_tx_wait_stage = pio_usb_debug.last_tx_wait_stage;
  g_piousb.setaddr_tx_timeout_count = pio_usb_debug.tx_timeout_count;
  g_piousb.setaddr_tx_irq = pio_usb_debug.last_tx_irq;
  g_piousb.setaddr_tx_fdebug = pio_usb_debug.last_tx_fdebug;
  g_piousb.setaddr_tx_flevel = pio_usb_debug.last_tx_flevel;
  g_piousb.setaddr_tx_pc = pio_usb_debug.last_tx_pc;
}

static int
piousb_setaddr_status_newaddr(FAR struct rp23xx_pio_usbhost_ep_s *ep,
                              uint8_t newaddr)
{
  uint8_t saveaddr;
  int ret;

  if (newaddr == 0)
    {
      return -EINVAL;
    }

  saveaddr = ep->funcaddr;
  piousb_ctrl_recover(ep, -EIO);

  ep->funcaddr = newaddr;
  ret = piousb_reconfigure_ep(ep);
  if (ret >= 0)
    {
      ep->in = true;
      ret = piousb_start_transfer(ep, NULL, 0, false);
      if (ret >= 0)
        {
          ret = piousb_ctrl_wait_stage(ep);
        }
    }

  ep->funcaddr = saveaddr;
  piousb_reconfigure_ep(ep);
  return ret;
}

static int piousb_use_ep0_address(FAR struct rp23xx_pio_usbhost_ep_s *ep,
                                  uint8_t funcaddr)
{
  ep->funcaddr = funcaddr;
  ep->hport->funcaddr = funcaddr;
  return piousb_reconfigure_ep(ep);
}

static void piousb_restore_ep0_address(FAR struct rp23xx_pio_usbhost_ep_s *ep,
                                       uint8_t funcaddr,
                                       uint8_t hportaddr)
{
  ep->funcaddr = funcaddr;
  ep->hport->funcaddr = hportaddr;
  piousb_reconfigure_ep(ep);
}

static int piousb_probe_device_descriptor_addr(
                                  FAR struct rp23xx_pio_usbhost_ep_s *ep,
                                  uint8_t funcaddr,
                                  FAR uint8_t *buffer, uint16_t buflen)
{
  struct usb_ctrlreq_s req;
  uint8_t saveaddr;
  uint8_t savehportaddr;
  int ret;

  saveaddr = ep->funcaddr;
  savehportaddr = ep->hport->funcaddr;

  memset(&req, 0, sizeof(req));
  req.type = USB_REQ_DIR_IN | USB_REQ_RECIPIENT_DEVICE;
  req.req = USB_REQ_GETDESCRIPTOR;
  piousb_putle16(req.value, USB_DESC_TYPE_DEVICE << 8);
  piousb_putle16(req.len, buflen);

  ret = piousb_use_ep0_address(ep, funcaddr);
  if (ret >= 0)
    {
      ret = piousb_ctrlin_once(ep, &req, buffer, buflen);
    }

  piousb_restore_ep0_address(ep, saveaddr, savehportaddr);
  return ret;
}

static int piousb_verify_set_address(FAR struct rp23xx_pio_usbhost_ep_s *ep,
                                     uint8_t newaddr)
{
  uint8_t buffer[8];
  uint8_t saveaddr;
  uint8_t savehportaddr;
  int ret;
  int proberet;
  int setret;

  if (newaddr == 0 || newaddr >= 128)
    {
      return -EINVAL;
    }

  saveaddr = ep->funcaddr;
  savehportaddr = ep->hport->funcaddr;
  g_piousb.addr0_probe_ret = -ENODATA;
  g_piousb.addr0_setaddr_ret = -ENODATA;
  g_piousb.addr1_retry_ret = -ENODATA;
  g_piousb.addr0_probe_len = 0;
  memset(g_piousb.addr0_probe_data, 0, sizeof(g_piousb.addr0_probe_data));

  ret = piousb_probe_device_descriptor_addr(ep, newaddr, buffer,
                                            sizeof(buffer));
  g_piousb.addr1_retry_ret = ret;
  if (ret >= 0)
    {
      return OK;
    }

  proberet = piousb_probe_device_descriptor_addr(ep, 0, buffer,
                                                 sizeof(buffer));
  g_piousb.addr0_probe_ret = proberet;
  if (proberet < 0)
    {
      return ret;
    }

  memcpy(g_piousb.addr0_probe_data, buffer, sizeof(buffer));
  g_piousb.addr0_probe_len = sizeof(buffer);

  ret = piousb_use_ep0_address(ep, 0);
  if (ret < 0)
    {
      piousb_restore_ep0_address(ep, saveaddr, savehportaddr);
      return ret;
    }

  setret = piousb_set_address_once(ep, newaddr);
  g_piousb.addr0_setaddr_ret = setret;
  piousb_restore_ep0_address(ep, saveaddr, savehportaddr);
  if (setret < 0)
    {
      return setret;
    }

  ret = piousb_probe_device_descriptor_addr(ep, newaddr, buffer,
                                            sizeof(buffer));
  g_piousb.addr1_retry_ret = ret;
  return ret;
}

static int piousb_wait_transfer(FAR struct rp23xx_pio_usbhost_ep_s *ep)
{
  int ret;

  for (; ; )
    {
      ret = nxsem_tickwait_uninterruptible(&ep->waitsem,
                                           RP23XX_PIOUSB_TIMEOUT);
      if (ret < 0)
        {
          pio_usb_host_endpoint_abort_transfer(RP23XX_PIOUSB_ROOT_INDEX,
                                               ep->funcaddr, ep->epaddr);
          ep->pending = false;
          return ret == -ETIMEDOUT ? -ETIMEDOUT : ret;
        }

      if (!ep->pending || ep->result != -EINPROGRESS)
        {
          break;
        }
    }

  return ep->result < 0 ? (int)ep->result : OK;
}

static int piousb_ctrl_wait_stage(FAR struct rp23xx_pio_usbhost_ep_s *ep)
{
  int ret = piousb_wait_transfer(ep);

  if (ret < 0)
    {
      return ret;
    }

  return OK;
}

static int piousb_ctrlin_once(FAR struct rp23xx_pio_usbhost_ep_s *ep,
                              FAR const struct usb_ctrlreq_s *req,
                              FAR uint8_t *buffer, uint16_t buflen)
{
  int phase = 1;
  int ret;

  ret = piousb_start_setup(ep, req);
  if (ret >= 0)
    {
      ret = piousb_ctrl_wait_stage(ep);
    }

  if (ret >= 0 && buflen > 0)
    {
      phase = 2;
      ep->in = true;
      ret = piousb_start_transfer(ep, buffer, buflen, false);
      if (ret >= 0)
        {
          ret = piousb_ctrl_wait_stage(ep);
        }
    }

  if (ret >= 0)
    {
      phase = 3;
      ep->in = false;
      ret = piousb_start_transfer(ep, NULL, 0, false);
      if (ret >= 0)
        {
          ret = piousb_ctrl_wait_stage(ep);
        }
    }

  if (ret < 0 && phase == 3 && piousb_can_ignore_status_stage(req))
    {
      ret = OK;
    }

  return ret;
}

static int piousb_set_address_once(FAR struct rp23xx_pio_usbhost_ep_s *ep,
                                   uint8_t funcaddr)
{
  struct usb_ctrlreq_s req;
  int ret;

  memset(&req, 0, sizeof(req));
  req.type = USB_REQ_DIR_OUT | USB_REQ_RECIPIENT_DEVICE;
  req.req = USB_REQ_SETADDRESS;
  piousb_putle16(req.value, funcaddr);

  ret = piousb_start_setup(ep, &req);
  if (ret >= 0)
    {
      ret = piousb_ctrl_wait_stage(ep);
    }

  if (ret >= 0)
    {
      ep->in = true;
      ret = piousb_start_transfer(ep, NULL, 0, false);
      if (ret >= 0)
        {
          ret = piousb_ctrl_wait_stage(ep);
        }

      piousb_record_setaddr_status(ret);
    }

  if (ret >= 0)
    {
      nxsched_usleep(CONFIG_RP23XX_PIO_USBHOST_SETADDR_DELAY_MS * 1000);
    }

  return ret;
}

static void piousb_ctrl_recover(FAR struct rp23xx_pio_usbhost_ep_s *ep,
                                int result)
{
  if (ep->pending)
    {
      pio_usb_host_endpoint_abort_transfer(RP23XX_PIOUSB_ROOT_INDEX,
                                           ep->funcaddr, ep->epaddr);
      ep->pending = false;
    }

  ep->result = result;
}

#ifdef CONFIG_USBHOST_ASYNCH
static void piousb_async_worker(FAR void *arg)
{
  FAR struct rp23xx_pio_usbhost_ep_s *ep = arg;
  usbhost_asynch_t callback;
  FAR void *cbarg;
  ssize_t result;

  callback = ep->callback;
  cbarg = ep->arg;
  result = ep->result;

  ep->callback = NULL;
  ep->arg = NULL;

  if (callback != NULL)
    {
      callback(cbarg, result);
    }
}
#endif

static FAR struct rp23xx_pio_usbhost_ep_s *
piousb_find_wrapper_by_ll(uint8_t ll_ep)
{
  FAR struct rp23xx_pio_usbhost_ep_s *fallback = NULL;
  FAR endpoint_t *ll;
  int i;

  if (ll_ep >= PIO_USB_EP_POOL_CNT)
    {
      return NULL;
    }

  ll = PIO_USB_ENDPOINT(ll_ep);

  for (i = 0; i < PIO_USB_EP_POOL_CNT; i++)
    {
      if (g_piousb.ep[i].allocated && g_piousb.ep[i].ll_ep == ll_ep)
        {
          FAR struct rp23xx_pio_usbhost_ep_s *ep = &g_piousb.ep[i];

          if (fallback == NULL)
            {
              fallback = ep;
            }

          if (ep->funcaddr == ll->dev_addr &&
              (ep->epaddr == ll->ep_num ||
               (((ep->epaddr & USB_EPNO_MASK) == 0) &&
                ((ll->ep_num & USB_EPNO_MASK) == 0))))
            {
              return ep;
            }
        }
    }

  return fallback;
}

static void piousb_complete_ep(uint8_t ll_ep, ssize_t result)
{
  FAR struct rp23xx_pio_usbhost_ep_s *ep;
  FAR endpoint_t *llep;

  ep = piousb_find_wrapper_by_ll(ll_ep);
  if (ep == NULL || !ep->pending)
    {
      return;
    }

  llep = PIO_USB_ENDPOINT(ll_ep);
  if (result >= 0)
    {
      result = llep->actual_len;
      if (ep->in && ep->buffer != NULL && result > 0)
        {
          size_t copylen = result < sizeof(ep->last_in_data) ?
                           result : sizeof(ep->last_in_data);

          memcpy(ep->last_in_data, ep->buffer, copylen);
          ep->last_in_len = copylen;
        }
    }
  else if (result == -EIO &&
           (ep->xfrtype & USB_EP_ATTR_XFERTYPE_MASK) ==
           USB_EP_ATTR_XFER_INT)
    {
      result = -EAGAIN;
    }

  ep->result = result;
  ep->pending = false;

#ifdef CONFIG_USBHOST_ASYNCH
  if (ep->async)
    {
      work_queue(HPWORK, &ep->cbwork, piousb_async_worker, ep, 0);
      return;
    }
#endif

  nxsem_post(&ep->waitsem);
}

static void piousb_wake_connection(FAR struct usbhost_hubport_s *hport)
{
  FAR struct rp23xx_pio_usbhost_s *priv = &g_piousb;
  irqstate_t flags;
  uint8_t index;
  uint8_t count;

  flags = enter_critical_section();
  index = priv->change_head;
  for (count = 0; count < priv->change_count; count++)
    {
      if (priv->change[index] == hport)
        {
          leave_critical_section(flags);
          return;
        }

      index++;
      if (index >= RP23XX_PIOUSB_CHANGE_QUEUE_SIZE)
        {
          index = 0;
        }
    }

  if (priv->change_count < RP23XX_PIOUSB_CHANGE_QUEUE_SIZE)
    {
      priv->change[priv->change_tail] = hport;
      priv->change_tail++;
      if (priv->change_tail >= RP23XX_PIOUSB_CHANGE_QUEUE_SIZE)
        {
          priv->change_tail = 0;
        }

      priv->change_count++;
      nxsem_post(&priv->eventsem);
    }

  leave_critical_section(flags);
}

static void piousb_root_disconnect(FAR struct rp23xx_pio_usbhost_s *priv)
{
  FAR struct usbhost_hubport_s *hport = &priv->rhport.hport;

  if (hport->devclass != NULL)
    {
      CLASS_DISCONNECTED(hport->devclass);
      hport->devclass = NULL;
    }

  PIO_USB_ROOT_PORT(RP23XX_PIOUSB_ROOT_INDEX)->connected = false;
  PIO_USB_ROOT_PORT(RP23XX_PIOUSB_ROOT_INDEX)->suspended = true;
  pio_usb_host_close_device(RP23XX_PIOUSB_ROOT_INDEX, hport->funcaddr);
  hport->funcaddr = 0;
  hport->speed = USB_SPEED_UNKNOWN;
  hport->connected = false;
  priv->connected = false;

  piousb_wake_connection(hport);
}

static int piousb_root_prepare_enumerate(FAR struct usbhost_hubport_s *hport)
{
  FAR root_port_t *root = PIO_USB_ROOT_PORT(RP23XX_PIOUSB_ROOT_INDEX);

  nxsched_usleep(CONFIG_RP23XX_PIO_USBHOST_ENUM_DELAY_MS * 1000);
  if (!hport->connected)
    {
      return -ENODEV;
    }

  hport->funcaddr = 0;

  pio_usb_host_port_reset_start(RP23XX_PIOUSB_ROOT_INDEX);
  nxsched_usleep(CONFIG_RP23XX_PIO_USBHOST_RESET_MS * 1000);
  pio_usb_host_port_reset_end(RP23XX_PIOUSB_ROOT_INDEX);
  nxsched_usleep(CONFIG_RP23XX_PIO_USBHOST_POST_RESET_MS * 1000);

  hport->speed = root->is_fullspeed ? USB_SPEED_FULL : USB_SPEED_LOW;
  return OK;
}

/****************************************************************************
 * Name: pio_usb_host_irq_handler
 *
 * Description:
 *   Strong override for Pico-PIO-USB's weak default host IRQ handler.
 *
 ****************************************************************************/

void pio_usb_host_irq_handler(uint8_t root_id)
{
  FAR root_port_t *root = PIO_USB_ROOT_PORT(root_id);
  uint32_t ints = root->ints;
  uint32_t bits;
  int i;

  if (root_id != RP23XX_PIOUSB_ROOT_INDEX)
    {
      root->ints = 0;
      return;
    }

  if ((ints & PIO_USB_INTS_CONNECT_BITS) != 0)
    {
      FAR struct usbhost_hubport_s *hport = &g_piousb.rhport.hport;

      hport->speed = root->is_fullspeed ? USB_SPEED_FULL : USB_SPEED_LOW;
      hport->connected = true;
      g_piousb.connected = true;
      piousb_wake_connection(hport);
    }

  if ((ints & PIO_USB_INTS_DISCONNECT_BITS) != 0)
    {
      piousb_root_disconnect(&g_piousb);
    }

  bits = root->ep_complete;
  for (i = 0; i < PIO_USB_EP_POOL_CNT; i++)
    {
      if ((bits & (1u << i)) != 0)
        {
          piousb_complete_ep(i, OK);
        }
    }

  bits = root->ep_error;
  for (i = 0; i < PIO_USB_EP_POOL_CNT; i++)
    {
      if ((bits & (1u << i)) != 0)
        {
          piousb_complete_ep(i, -EIO);
        }
    }

  bits = root->ep_stalled;
  for (i = 0; i < PIO_USB_EP_POOL_CNT; i++)
    {
      if ((bits & (1u << i)) != 0)
        {
          piousb_complete_ep(i, -EPIPE);
        }
    }

  root->ep_complete = 0;
  root->ep_error = 0;
  root->ep_stalled = 0;
  root->ints = 0;
}

static int piousb_wait(FAR struct usbhost_connection_s *conn,
                       FAR struct usbhost_hubport_s **hport)
{
  FAR struct rp23xx_pio_usbhost_s *priv = piousb_from_connection(conn);
  irqstate_t flags;
  int ret;

  for (; ; )
    {
      flags = enter_critical_section();
      if (priv->change_count > 0)
        {
          *hport = priv->change[priv->change_head];
          priv->change[priv->change_head] = NULL;
          priv->change_head++;
          if (priv->change_head >= RP23XX_PIOUSB_CHANGE_QUEUE_SIZE)
            {
              priv->change_head = 0;
            }

          priv->change_count--;
          leave_critical_section(flags);
          return OK;
        }

      leave_critical_section(flags);
      ret = nxsem_wait_uninterruptible(&priv->eventsem);
      if (ret < 0)
        {
          return ret;
        }
    }
}

static int piousb_enumerate(FAR struct usbhost_connection_s *conn,
                            FAR struct usbhost_hubport_s *hport)
{
  FAR struct rp23xx_pio_usbhost_s *priv = piousb_from_connection(conn);
  bool roothub = false;
  int attempts = CONFIG_RP23XX_PIO_USBHOST_ENUM_ATTEMPTS;
  int attempt;
  int ret;

  DEBUGASSERT(priv != NULL && hport != NULL);

#ifdef CONFIG_USBHOST_HUB
  if (hport->parent == NULL)
#endif
    {
      roothub = true;
      attempts = CONFIG_RP23XX_PIO_USBHOST_ENUM_ATTEMPTS;
    }

  for (attempt = 1; attempt <= attempts; attempt++)
    {
      if (roothub)
        {
          ret = piousb_root_prepare_enumerate(hport);
          if (ret < 0)
            {
              break;
            }
        }

      ret = usbhost_enumerate(hport, &hport->devclass);
      priv->enum_count++;
      priv->last_enum_ret = ret;
      if (ret >= 0)
        {
          return ret;
        }

      uerr("ERROR: PIO USB enumeration attempt %d/%d failed: %d\n",
           attempt, attempts, ret);

      hport->funcaddr = 0;
      nxsched_usleep(CONFIG_RP23XX_PIO_USBHOST_POST_RESET_MS * 1000);
    }

  if (ret < 0)
    {
      uerr("ERROR: PIO USB enumeration failed: %d\n", ret);
      if (roothub)
        {
          piousb_root_disconnect(priv);
        }
      else
        {
          hport->connected = false;
          if (hport->ep0 != NULL)
            {
              piousb_epfree(&priv->drvr, hport->ep0);
              hport->ep0 = NULL;
            }
        }
    }

  return ret;
}

static int piousb_ep0configure(FAR struct usbhost_driver_s *drvr,
                               usbhost_ep_t ep0, uint8_t funcaddr,
                               uint8_t speed, uint16_t maxpacketsize)
{
  FAR struct rp23xx_pio_usbhost_ep_s *ep = ep0;
  int ret;

  DEBUGASSERT(drvr != NULL && ep != NULL && ep->hport != NULL);
  DEBUGASSERT(maxpacketsize <= PIO_USB_EP_SIZE);

  ep->funcaddr = funcaddr;
  ep->hport->funcaddr = funcaddr;
  ep->hport->speed = speed;
  ep->maxpacket = maxpacketsize;
  ep->epaddr = 0;
  ep->xfrtype = USB_EP_ATTR_XFER_CONTROL;
  ep->interval = 0;
  ep->in = false;

  ret = piousb_reconfigure_ep(ep);
  if (ret < 0)
    {
      uerr("ERROR: failed to open EP0 addr %u: %d\n", funcaddr, ret);
    }

  return ret;
}

static int piousb_epalloc(FAR struct usbhost_driver_s *drvr,
                          FAR const struct usbhost_epdesc_s *epdesc,
                          FAR usbhost_ep_t *ep)
{
  FAR struct rp23xx_pio_usbhost_s *priv =
    (FAR struct rp23xx_pio_usbhost_s *)drvr;
  FAR struct rp23xx_pio_usbhost_ep_s *priv_ep = NULL;
  int i;
  int ret;

  DEBUGASSERT(priv != NULL && epdesc != NULL && ep != NULL);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (epdesc->addr == 0 &&
      epdesc->xfrtype == USB_EP_ATTR_XFER_CONTROL &&
      epdesc->hport->funcaddr == 0)
    {
      for (i = 1; i < PIO_USB_EP_POOL_CNT; i++)
        {
          if (priv->ep[i].allocated &&
              priv->ep[i].funcaddr == 0 &&
              priv->ep[i].epaddr == 0 &&
              priv->ep[i].xfrtype == USB_EP_ATTR_XFER_CONTROL)
            {
              nxmutex_unlock(&priv->lock);
              return -EBUSY;
            }
        }
    }

  for (i = 1; i < PIO_USB_EP_POOL_CNT; i++)
    {
      if (!priv->ep[i].allocated)
        {
          priv_ep = &priv->ep[i];
          memset(priv_ep, 0, sizeof(*priv_ep));
          nxsem_init(&priv_ep->waitsem, 0, 0);
          priv_ep->allocated = true;
          priv_ep->ll_ep = RP23XX_PIOUSB_NO_LL_EP;
          break;
        }
    }

  if (priv_ep == NULL)
    {
      nxmutex_unlock(&priv->lock);
      return -ENOMEM;
    }

  priv_ep->hport = epdesc->hport;
  priv_ep->funcaddr = epdesc->hport->funcaddr;
  priv_ep->epaddr = epdesc->addr | (epdesc->in ? 0x80 : 0x00);
  priv_ep->in = epdesc->in;
  priv_ep->xfrtype = epdesc->xfrtype;
  priv_ep->interval = epdesc->interval;
  priv_ep->maxpacket = epdesc->mxpacketsize;

  ret = piousb_open_ep(priv_ep);
  if (ret < 0)
    {
      priv_ep->allocated = false;
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  *ep = priv_ep;
  nxmutex_unlock(&priv->lock);
  return OK;
}

static int piousb_epfree(FAR struct usbhost_driver_s *drvr, usbhost_ep_t ep)
{
  FAR struct rp23xx_pio_usbhost_ep_s *priv_ep = ep;

  if (priv_ep == NULL || !priv_ep->allocated)
    {
      return -EINVAL;
    }

  if (priv_ep->pending)
    {
      piousb_cancel(drvr, ep);
    }

  pio_usb_host_endpoint_close(RP23XX_PIOUSB_ROOT_INDEX,
                              priv_ep->funcaddr, priv_ep->epaddr);
  priv_ep->allocated = false;
  priv_ep->pending = false;
  priv_ep->async = false;
  priv_ep->hport = NULL;
  priv_ep->buffer = NULL;
  priv_ep->buflen = 0;
  priv_ep->result = 0;
  priv_ep->last_in_len = 0;
#ifdef CONFIG_USBHOST_ASYNCH
  priv_ep->callback = NULL;
  priv_ep->arg = NULL;
#endif
  priv_ep->ll_ep = RP23XX_PIOUSB_NO_LL_EP;

  return OK;
}

static int piousb_alloc(FAR struct usbhost_driver_s *drvr,
                        FAR uint8_t **buffer, FAR size_t *maxlen)
{
  FAR uint8_t *alloc;

  DEBUGASSERT(buffer != NULL && maxlen != NULL);

  alloc = kmm_malloc(CONFIG_RP23XX_PIO_USBHOST_DESCSIZE);
  if (alloc == NULL)
    {
      return -ENOMEM;
    }

  *buffer = alloc;
  *maxlen = CONFIG_RP23XX_PIO_USBHOST_DESCSIZE;
  return OK;
}

static int piousb_free(FAR struct usbhost_driver_s *drvr,
                       FAR uint8_t *buffer)
{
  kmm_free(buffer);
  return OK;
}

static int piousb_ioalloc(FAR struct usbhost_driver_s *drvr,
                          FAR uint8_t **buffer, size_t buflen)
{
  FAR uint8_t *alloc;

  DEBUGASSERT(buffer != NULL);

  alloc = kmm_malloc(buflen);
  if (alloc == NULL)
    {
      return -ENOMEM;
    }

  *buffer = alloc;
  return OK;
}

static int piousb_iofree(FAR struct usbhost_driver_s *drvr,
                         FAR uint8_t *buffer)
{
  kmm_free(buffer);
  return OK;
}

static int piousb_ctrlin(FAR struct usbhost_driver_s *drvr,
                         usbhost_ep_t ep0,
                         FAR const struct usb_ctrlreq_s *req,
                         FAR uint8_t *buffer)
{
  FAR struct rp23xx_pio_usbhost_ep_s *ep = ep0;
  uint16_t buflen;
  uint16_t copylen;
  int attempt;
  int ret;

  DEBUGASSERT(ep != NULL && req != NULL);

  buflen = piousb_getle16(req->len);
  piousb_ctrl_save_prev();
  g_piousb.last_ctrl_dirin = 1;
  g_piousb.last_ctrl_type = req->type;
  g_piousb.last_ctrl_req = req->req;
  g_piousb.last_ctrl_value = piousb_getle16(req->value);
  g_piousb.last_ctrl_index = piousb_getle16(req->index);
  g_piousb.last_ctrl_len = buflen;
  g_piousb.last_ctrl_phase = 1;
  g_piousb.last_ctrl_ret = -EINPROGRESS;
  g_piousb.last_ctrl_data_len = 0;
  memset(g_piousb.last_ctrl_data, 0, sizeof(g_piousb.last_ctrl_data));
  g_piousb.last_ctrl_funcaddr = ep->funcaddr;
  g_piousb.last_ctrl_speed = ep->hport->speed;
  g_piousb.last_ctrl_parent = ep->hport->parent != NULL ?
                              ep->hport->parent->funcaddr : 0;
  g_piousb.last_ctrl_ll_ep = ep->ll_ep;
  g_piousb.addr0_probe_ret = -ENODATA;
  g_piousb.addr0_setaddr_ret = -ENODATA;
  g_piousb.addr1_retry_ret = -ENODATA;
  g_piousb.addr0_probe_len = 0;
  memset(g_piousb.addr0_probe_data, 0, sizeof(g_piousb.addr0_probe_data));

  ret = nxmutex_lock(&g_piousb.lock);
  if (ret < 0)
    {
      return ret;
    }

  for (attempt = 0; attempt <= CONFIG_RP23XX_PIO_USBHOST_CTRL_RETRIES;
       attempt++)
    {
      ret = piousb_start_setup(ep, req);
      if (ret >= 0)
        {
          g_piousb.last_ctrl_phase = 1;
          ret = piousb_ctrl_wait_stage(ep);
        }

      if (ret >= 0 && buflen > 0)
        {
          ep->in = true;
          g_piousb.last_ctrl_phase = 2;
          ret = piousb_start_transfer(ep, buffer, buflen, false);
          if (ret >= 0)
            {
              ret = piousb_ctrl_wait_stage(ep);
            }
        }

      if (ret >= 0)
        {
          ep->in = false;
          g_piousb.last_ctrl_phase = 3;
          ret = piousb_start_transfer(ep, NULL, 0, false);
          if (ret >= 0)
            {
              ret = piousb_ctrl_wait_stage(ep);
            }
        }

      if (ret < 0 && g_piousb.last_ctrl_phase == 3 &&
          piousb_can_ignore_status_stage(req))
        {
          ret = OK;
        }

      if (ret >= 0 || attempt >= CONFIG_RP23XX_PIO_USBHOST_CTRL_RETRIES)
        {
          break;
        }

      piousb_ctrl_recover(ep, ret);
      nxsched_usleep(CONFIG_RP23XX_PIO_USBHOST_CTRL_GAP_MS * 1000);
    }

  g_piousb.last_ctrl_ret = ret;
  if (ret < 0 && ep->funcaddr != 0 &&
      req->req == USB_REQ_GETDESCRIPTOR &&
      piousb_getle16(req->value) == (USB_DESC_TYPE_CONFIG << 8) &&
      buflen >= USB_SIZEOF_CFGDESC)
    {
      uint8_t saveaddr = ep->funcaddr;
      uint8_t savehportaddr = ep->hport->funcaddr;
      uint8_t probelen = buflen < sizeof(g_piousb.addr0_probe_data) ?
                         buflen : sizeof(g_piousb.addr0_probe_data);
      int proberet;
      int setret;
      int retryret;

      ep->funcaddr = 0;
      ep->hport->funcaddr = 0;
      setret = piousb_reconfigure_ep(ep);
      if (setret >= 0)
        {
          setret = piousb_set_address_once(ep, saveaddr);
        }

      g_piousb.addr0_setaddr_ret = setret;
      if (setret >= 0)
        {
          ep->funcaddr = saveaddr;
          ep->hport->funcaddr = savehportaddr;
          piousb_reconfigure_ep(ep);
          retryret = piousb_ctrlin_once(ep, req, buffer, buflen);
          g_piousb.addr1_retry_ret = retryret;
          ret = retryret;
        }

      if (ret < 0)
        {
          ep->funcaddr = 0;
          ep->hport->funcaddr = 0;
          proberet = piousb_reconfigure_ep(ep);
          if (proberet >= 0)
            {
              proberet = piousb_ctrlin_once(ep, req, buffer, buflen);
            }

          g_piousb.addr0_probe_ret = proberet;
          if (proberet >= 0)
            {
              memcpy(g_piousb.addr0_probe_data, buffer, probelen);
              g_piousb.addr0_probe_len = probelen;
            }
        }

      ep->funcaddr = saveaddr;
      ep->hport->funcaddr = savehportaddr;
      piousb_reconfigure_ep(ep);
    }

  g_piousb.last_ctrl_ret = ret;
  if (ret >= 0 && buffer != NULL && buflen > 0)
    {
      copylen = buflen < sizeof(g_piousb.last_ctrl_data) ?
                buflen : sizeof(g_piousb.last_ctrl_data);
      memcpy(g_piousb.last_ctrl_data, buffer, copylen);
      g_piousb.last_ctrl_data_len = copylen;
      piousb_record_hub_ctrlin(req, buffer, buflen);
    }

  if (ret >= 0 && CONFIG_RP23XX_PIO_USBHOST_CTRL_GAP_MS > 0)
    {
      nxsched_usleep(CONFIG_RP23XX_PIO_USBHOST_CTRL_GAP_MS * 1000);
    }

  nxmutex_unlock(&g_piousb.lock);
  return ret;
}

static int piousb_ctrlout(FAR struct usbhost_driver_s *drvr,
                          usbhost_ep_t ep0,
                          FAR const struct usb_ctrlreq_s *req,
                          FAR const uint8_t *buffer)
{
  FAR struct rp23xx_pio_usbhost_ep_s *ep = ep0;
  uint16_t buflen;
  int attempt;
  int ret;

  DEBUGASSERT(ep != NULL && req != NULL);

  buflen = piousb_getle16(req->len);
  piousb_ctrl_save_prev();
  g_piousb.last_ctrl_dirin = 0;
  g_piousb.last_ctrl_type = req->type;
  g_piousb.last_ctrl_req = req->req;
  g_piousb.last_ctrl_value = piousb_getle16(req->value);
  g_piousb.last_ctrl_index = piousb_getle16(req->index);
  g_piousb.last_ctrl_len = buflen;
  g_piousb.last_ctrl_phase = 1;
  g_piousb.last_ctrl_ret = -EINPROGRESS;
  g_piousb.last_ctrl_data_len = 0;
  memset(g_piousb.last_ctrl_data, 0, sizeof(g_piousb.last_ctrl_data));
  g_piousb.last_ctrl_funcaddr = ep->funcaddr;
  g_piousb.last_ctrl_speed = ep->hport->speed;
  g_piousb.last_ctrl_parent = ep->hport->parent != NULL ?
                              ep->hport->parent->funcaddr : 0;
  g_piousb.last_ctrl_ll_ep = ep->ll_ep;

  ret = nxmutex_lock(&g_piousb.lock);
  if (ret < 0)
    {
      return ret;
    }

  for (attempt = 0; attempt <= CONFIG_RP23XX_PIO_USBHOST_CTRL_RETRIES;
       attempt++)
    {
      ret = piousb_start_setup(ep, req);
      if (ret >= 0)
        {
          g_piousb.last_ctrl_phase = 1;
          ret = piousb_ctrl_wait_stage(ep);
        }

      if (ret >= 0 && buflen > 0)
        {
          ep->in = false;
          g_piousb.last_ctrl_phase = 2;
          ret = piousb_start_transfer(ep, (FAR uint8_t *)buffer, buflen,
                                      false);
          if (ret >= 0)
            {
              ret = piousb_ctrl_wait_stage(ep);
            }
        }

      if (ret >= 0)
        {
          ep->in = true;
          g_piousb.last_ctrl_phase = 3;
          ret = piousb_start_transfer(ep, NULL, 0, false);
          if (ret >= 0)
            {
              ret = piousb_ctrl_wait_stage(ep);
            }

          if (req->req == USB_REQ_SETADDRESS)
            {
              uint16_t newaddr = piousb_getle16(req->value);

              piousb_record_setaddr_status(ret);
              if (ret < 0 && newaddr < 128)
                {
                  ret = piousb_setaddr_status_newaddr(ep,
                                                      (uint8_t)newaddr);
                  piousb_record_setaddr_status(ret);
                }
            }
        }

      if (ret < 0 && g_piousb.last_ctrl_phase == 3 &&
          piousb_can_ignore_status_stage(req))
        {
          ret = OK;
        }

      if (ret >= 0 || attempt >= CONFIG_RP23XX_PIO_USBHOST_CTRL_RETRIES)
        {
          break;
        }

      piousb_ctrl_recover(ep, ret);
      nxsched_usleep(CONFIG_RP23XX_PIO_USBHOST_CTRL_GAP_MS * 1000);
    }

  if (ret >= 0 && req->req == USB_REQ_SETADDRESS)
    {
      uint16_t newaddr = piousb_getle16(req->value);

      nxsched_usleep(CONFIG_RP23XX_PIO_USBHOST_SETADDR_DELAY_MS * 1000);
      ret = piousb_verify_set_address(ep, (uint8_t)newaddr);
    }

  g_piousb.last_ctrl_ret = ret;
  if (req->type == USBHUB_REQ_TYPE_PORT &&
      (req->req == USBHUB_REQ_SETFEATURE ||
       req->req == USBHUB_REQ_CLEARFEATURE) &&
      piousb_getle16(req->value) == USBHUB_PORT_FEAT_POWER)
    {
      uint16_t index = piousb_getle16(req->index);

      if (index >= 1 && index <= RP23XX_PIO_USBHOST_INFO_PORTS)
        {
          index--;
          g_piousb.hub_port_power_ret[index] = ret;
          g_piousb.hub_port_power_valid |= (1u << index);
        }
    }

  if (ret >= 0 && CONFIG_RP23XX_PIO_USBHOST_CTRL_GAP_MS > 0)
    {
      nxsched_usleep(CONFIG_RP23XX_PIO_USBHOST_CTRL_GAP_MS * 1000);
    }

  nxmutex_unlock(&g_piousb.lock);
  return ret;
}

static ssize_t piousb_transfer(FAR struct usbhost_driver_s *drvr,
                               usbhost_ep_t ep, FAR uint8_t *buffer,
                               size_t buflen)
{
  FAR struct rp23xx_pio_usbhost_ep_s *priv_ep = ep;
  ssize_t nbytes;
  int ret;

  DEBUGASSERT(priv_ep != NULL);

  ret = nxmutex_lock(&g_piousb.lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = piousb_start_transfer(priv_ep, buffer, buflen, false);
  if (ret >= 0)
    {
      ret = piousb_wait_transfer(priv_ep);
    }

  nbytes = ret < 0 ? ret : priv_ep->result;
  nxmutex_unlock(&g_piousb.lock);
  return nbytes;
}

#ifdef CONFIG_USBHOST_ASYNCH
static int piousb_asynch(FAR struct usbhost_driver_s *drvr,
                         usbhost_ep_t ep, FAR uint8_t *buffer,
                         size_t buflen, usbhost_asynch_t callback,
                         FAR void *arg)
{
  FAR struct rp23xx_pio_usbhost_ep_s *priv_ep = ep;
  int ret;

  DEBUGASSERT(priv_ep != NULL && callback != NULL);

  ret = nxmutex_lock(&g_piousb.lock);
  if (ret < 0)
    {
      return ret;
    }

  priv_ep->callback = callback;
  priv_ep->arg = arg;
  ret = piousb_start_transfer(priv_ep, buffer, buflen, true);

  nxmutex_unlock(&g_piousb.lock);
  return ret;
}
#endif

static int piousb_cancel(FAR struct usbhost_driver_s *drvr, usbhost_ep_t ep)
{
  FAR struct rp23xx_pio_usbhost_ep_s *priv_ep = ep;

  if (priv_ep == NULL || !priv_ep->pending)
    {
      return OK;
    }

  pio_usb_host_endpoint_abort_transfer(RP23XX_PIOUSB_ROOT_INDEX,
                                       priv_ep->funcaddr, priv_ep->epaddr);
  priv_ep->result = -ESHUTDOWN;
  priv_ep->pending = false;

#ifdef CONFIG_USBHOST_ASYNCH
  if (priv_ep->async)
    {
      work_queue(HPWORK, &priv_ep->cbwork, piousb_async_worker, priv_ep, 0);
      return OK;
    }
#endif

  nxsem_post(&priv_ep->waitsem);
  return OK;
}

#ifdef CONFIG_USBHOST_HUB
static int piousb_connect(FAR struct usbhost_driver_s *drvr,
                          FAR struct usbhost_hubport_s *hport,
                          bool connected)
{
  DEBUGASSERT(hport != NULL);

  if (connected && hport->connected)
    {
      return OK;
    }

  hport->connected = connected;
  piousb_wake_connection(hport);
  return OK;
}
#endif

static void piousb_disconnect(FAR struct usbhost_driver_s *drvr,
                              FAR struct usbhost_hubport_s *hport)
{
  DEBUGASSERT(hport != NULL);
  hport->devclass = NULL;
}

static void piousb_vbus_enable(void)
{
#if CONFIG_RP23XX_PIO_USBHOST_VBUS_GPIO >= 0
  rp23xx_gpio_init(CONFIG_RP23XX_PIO_USBHOST_VBUS_GPIO);
  rp23xx_gpio_setdir(CONFIG_RP23XX_PIO_USBHOST_VBUS_GPIO, true);
  rp23xx_gpio_put(CONFIG_RP23XX_PIO_USBHOST_VBUS_GPIO, true);
  nxsched_usleep(CONFIG_RP23XX_PIO_USBHOST_VBUS_ON_MS * 1000);
#endif
}

static void piousb_vbus_cycle_gpio(void)
{
#if CONFIG_RP23XX_PIO_USBHOST_VBUS_GPIO >= 0
  rp23xx_gpio_init(CONFIG_RP23XX_PIO_USBHOST_VBUS_GPIO);
  rp23xx_gpio_setdir(CONFIG_RP23XX_PIO_USBHOST_VBUS_GPIO, true);
  rp23xx_gpio_put(CONFIG_RP23XX_PIO_USBHOST_VBUS_GPIO, false);
  nxsched_usleep(CONFIG_RP23XX_PIO_USBHOST_VBUS_OFF_MS * 1000);
  rp23xx_gpio_put(CONFIG_RP23XX_PIO_USBHOST_VBUS_GPIO, true);
  nxsched_usleep(CONFIG_RP23XX_PIO_USBHOST_VBUS_ON_MS * 1000);
#endif
}

int rp23xx_pio_usbhost_vbus_cycle(void)
{
#if CONFIG_RP23XX_PIO_USBHOST_VBUS_GPIO >= 0
  FAR struct rp23xx_pio_usbhost_s *priv = &g_piousb;
  FAR root_port_t *root = PIO_USB_ROOT_PORT(RP23XX_PIOUSB_ROOT_INDEX);
  irqstate_t flags;

  if (priv->initialized)
    {
      piousb_root_disconnect(priv);
    }

  flags = enter_critical_section();
  root->connected = false;
  root->suspended = true;
  root->ints = 0;
  root->ep_complete = 0;
  root->ep_error = 0;
  root->ep_stalled = 0;
  leave_critical_section(flags);

  piousb_vbus_cycle_gpio();
  return OK;
#else
  return -ENOTSUP;
#endif
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rp23xx_pio_usbhost_initialize(FAR struct usbhost_connection_s **conn)
{
  FAR struct rp23xx_pio_usbhost_s *priv = &g_piousb;
  pio_usb_configuration_t config;
  int i;
  int ret;

  DEBUGASSERT(conn != NULL);

  if (priv->initialized)
    {
      *conn = &priv->conn;
      return OK;
    }

  memset(priv, 0, sizeof(*priv));
  priv->setaddr_status_ret = -ENODATA;

  priv->drvr.ep0configure = piousb_ep0configure;
  priv->drvr.epalloc      = piousb_epalloc;
  priv->drvr.epfree       = piousb_epfree;
  priv->drvr.alloc        = piousb_alloc;
  priv->drvr.free         = piousb_free;
  priv->drvr.ioalloc      = piousb_ioalloc;
  priv->drvr.iofree       = piousb_iofree;
  priv->drvr.ctrlin       = piousb_ctrlin;
  priv->drvr.ctrlout      = piousb_ctrlout;
  priv->drvr.transfer     = piousb_transfer;
#ifdef CONFIG_USBHOST_ASYNCH
  priv->drvr.asynch       = piousb_asynch;
#endif
  priv->drvr.cancel       = piousb_cancel;
#ifdef CONFIG_USBHOST_HUB
  priv->drvr.connect      = piousb_connect;
#endif
  priv->drvr.disconnect   = piousb_disconnect;

  priv->conn.wait         = piousb_wait;
  priv->conn.enumerate    = piousb_enumerate;

  nxmutex_init(&priv->lock);
  nxsem_init(&priv->eventsem, 0, 0);
  nxsem_init(&priv->framesem, 0, 0);

  usbhost_devaddr_initialize(&priv->devgen);
  priv->devgen.next = CONFIG_RP23XX_PIO_USBHOST_FIRST_ADDRESS;

  priv->rhport.hport.drvr = &priv->drvr;
  priv->rhport.hport.ep0 = &priv->ep[RP23XX_PIOUSB_EP0_INDEX];
  priv->rhport.hport.port = 0;
  priv->rhport.hport.speed = USB_SPEED_UNKNOWN;
  priv->rhport.pdevgen = &priv->devgen;

  for (i = 0; i < PIO_USB_EP_POOL_CNT; i++)
    {
      nxsem_init(&priv->ep[i].waitsem, 0, 0);
      priv->ep[i].ll_ep = RP23XX_PIOUSB_NO_LL_EP;
    }

  priv->ep[RP23XX_PIOUSB_EP0_INDEX].allocated = true;
  priv->ep[RP23XX_PIOUSB_EP0_INDEX].hport = &priv->rhport.hport;
  priv->ep[RP23XX_PIOUSB_EP0_INDEX].epaddr = 0;
  priv->ep[RP23XX_PIOUSB_EP0_INDEX].xfrtype = USB_EP_ATTR_XFER_CONTROL;
  priv->ep[RP23XX_PIOUSB_EP0_INDEX].maxpacket = 8;

  piousb_vbus_enable();

  config.pin_dp = CONFIG_RP23XX_PIO_USBHOST_DP_GPIO;
  config.pio_tx_num = CONFIG_RP23XX_PIO_USBHOST_PIO;
  config.sm_tx = CONFIG_RP23XX_PIO_USBHOST_TX_SM;
  config.tx_ch = CONFIG_RP23XX_PIO_USBHOST_DMA_CHANNEL;
  config.pio_rx_num = CONFIG_RP23XX_PIO_USBHOST_PIO;
  config.sm_rx = CONFIG_RP23XX_PIO_USBHOST_RX_SM;
  config.sm_eop = CONFIG_RP23XX_PIO_USBHOST_EOP_SM;
  config.alarm_pool = NULL;
  config.debug_pin_rx = -1;
  config.debug_pin_eop = -1;
  config.skip_alarm_pool = true;
  config.pinout = PIO_USB_PINOUT_DPDM;

  pio_usb_host_init(&config);

  ret = piousb_timer_start(priv);
  if (ret < 0)
    {
      uerr("ERROR: failed to start PIO USB frame timer: %d\n", ret);
      return ret;
    }

  priv->initialized = true;
  *conn = &priv->conn;

  return OK;
}

int rp23xx_pio_usbhost_info(FAR struct rp23xx_pio_usbhost_info_s *info)
{
  FAR struct rp23xx_pio_usbhost_s *priv = &g_piousb;
  FAR root_port_t *root;
  int i;

  if (info == NULL)
    {
      return -EINVAL;
    }

  memset(info, 0, sizeof(*info));

  info->initialized = priv->initialized;
  info->timer_started = priv->timer_started;
  info->frame_thread_started = priv->frame_thread_started;
  info->frame_pending = priv->frame_pending;
  info->frame_active = priv->frame_active;
  info->connected = priv->connected;
  info->frame_number = pio_usb_host_get_frame_number();

  root = PIO_USB_ROOT_PORT(RP23XX_PIOUSB_ROOT_INDEX);
  info->root_initialized = root->initialized;
  info->root_connected = root->connected;
  info->root_fullspeed = root->is_fullspeed;
  info->root_suspended = root->suspended;
  info->root_event = root->event;
  info->root_pin_dp = root->pin_dp;
  info->root_pin_dm = root->pin_dm;
  info->root_ints = root->ints;
  info->root_ep_complete = root->ep_complete;
  info->root_ep_error = root->ep_error;
  info->root_ep_stalled = root->ep_stalled;
  info->change_pending = priv->change_count > 0;
  info->hport_connected = priv->rhport.hport.connected;
  info->hport_has_class = priv->rhport.hport.devclass != NULL;
  info->hport_funcaddr = priv->rhport.hport.funcaddr;
  info->hport_speed = priv->rhport.hport.speed;
  info->enum_count = priv->enum_count;
  info->last_enum_ret = priv->last_enum_ret;
  info->last_ctrl_dirin = priv->last_ctrl_dirin;
  info->last_ctrl_type = priv->last_ctrl_type;
  info->last_ctrl_req = priv->last_ctrl_req;
  info->last_ctrl_phase = priv->last_ctrl_phase;
  info->last_ctrl_value = priv->last_ctrl_value;
  info->last_ctrl_index = priv->last_ctrl_index;
  info->last_ctrl_len = priv->last_ctrl_len;
  info->last_ctrl_ret = priv->last_ctrl_ret;
  info->last_ctrl_data_len = priv->last_ctrl_data_len;
  memcpy(info->last_ctrl_data, priv->last_ctrl_data,
         sizeof(info->last_ctrl_data));
  info->last_ctrl_funcaddr = priv->last_ctrl_funcaddr;
  info->last_ctrl_speed = priv->last_ctrl_speed;
  info->last_ctrl_parent = priv->last_ctrl_parent;
  info->last_ctrl_ll_ep = priv->last_ctrl_ll_ep;
  info->prev_ctrl_dirin = priv->prev_ctrl_dirin;
  info->prev_ctrl_type = priv->prev_ctrl_type;
  info->prev_ctrl_req = priv->prev_ctrl_req;
  info->prev_ctrl_phase = priv->prev_ctrl_phase;
  info->prev_ctrl_value = priv->prev_ctrl_value;
  info->prev_ctrl_index = priv->prev_ctrl_index;
  info->prev_ctrl_len = priv->prev_ctrl_len;
  info->prev_ctrl_ret = priv->prev_ctrl_ret;
  info->addr0_probe_ret = priv->addr0_probe_ret;
  info->addr0_setaddr_ret = priv->addr0_setaddr_ret;
  info->addr1_retry_ret = priv->addr1_retry_ret;
  info->addr0_probe_len = priv->addr0_probe_len;
  memcpy(info->addr0_probe_data, priv->addr0_probe_data,
         sizeof(info->addr0_probe_data));
  info->setaddr_status_ret = priv->setaddr_status_ret;
  info->setaddr_in_result = priv->setaddr_in_result;
  info->setaddr_in_pid = priv->setaddr_in_pid;
  info->setaddr_in_expected_pid = priv->setaddr_in_expected_pid;
  info->setaddr_setup_handshake = priv->setaddr_setup_handshake;
  info->setaddr_tx_len = priv->setaddr_tx_len;
  info->setaddr_tx_wait_stage = priv->setaddr_tx_wait_stage;
  info->setaddr_tx_timeout_count = priv->setaddr_tx_timeout_count;
  info->setaddr_tx_irq = priv->setaddr_tx_irq;
  info->setaddr_tx_fdebug = priv->setaddr_tx_fdebug;
  info->setaddr_tx_flevel = priv->setaddr_tx_flevel;
  info->setaddr_tx_pc = priv->setaddr_tx_pc;
  info->hub_nports = priv->hub_nports;
  info->hub_characteristics = priv->hub_characteristics;
  info->hub_lpsm = priv->hub_lpsm;
  info->hub_compound = priv->hub_compound;
  info->hub_ocmode = priv->hub_ocmode;
  info->hub_indicator = priv->hub_indicator;
  info->hub_pwrondelay_ms = priv->hub_pwrondelay_ms;
  info->hub_ctrlcurrent = priv->hub_ctrlcurrent;
  info->hub_devattached = priv->hub_devattached;
  info->hub_pwrctrlmask = priv->hub_pwrctrlmask;
  info->hub_port_valid = priv->hub_port_valid;
  info->hub_port_power_valid = priv->hub_port_power_valid;
  memcpy(info->hub_port_status, priv->hub_port_status,
         sizeof(info->hub_port_status));
  memcpy(info->hub_port_change, priv->hub_port_change,
         sizeof(info->hub_port_change));
  memcpy(info->hub_port_power_ret, priv->hub_port_power_ret,
         sizeof(info->hub_port_power_ret));
  info->ep_count = PIO_USB_EP_POOL_CNT < RP23XX_PIO_USBHOST_INFO_EPS ?
                   PIO_USB_EP_POOL_CNT : RP23XX_PIO_USBHOST_INFO_EPS;
  for (i = 0; i < info->ep_count; i++)
    {
      FAR struct rp23xx_pio_usbhost_ep_s *ep = &priv->ep[i];

      info->ep[i].allocated = ep->allocated;
      info->ep[i].pending = ep->pending;
      info->ep[i].async = ep->async;
      info->ep[i].in = ep->in;
      info->ep[i].epaddr = ep->epaddr;
      info->ep[i].xfrtype = ep->xfrtype;
      info->ep[i].interval = ep->interval;
      info->ep[i].ll_ep = ep->ll_ep;
      info->ep[i].funcaddr = ep->funcaddr;
      info->ep[i].speed = ep->hport != NULL ? ep->hport->speed : 0;
      info->ep[i].maxpacket = ep->maxpacket;
      info->ep[i].buflen = ep->buflen;
      info->ep[i].result = ep->result;
      info->ep[i].last_in_len = ep->last_in_len;
      memcpy(info->ep[i].last_in_data, ep->last_in_data,
             sizeof(info->ep[i].last_in_data));

      if (ep->ll_ep != RP23XX_PIOUSB_NO_LL_EP)
        {
          FAR endpoint_t *ll = PIO_USB_ENDPOINT(ep->ll_ep);

          if (ll->size != 0)
            {
              info->ep[i].ll_valid = true;
              info->ep[i].ll_is_tx = ll->is_tx;
              info->ep[i].ll_has_transfer = ll->has_transfer;
              info->ep[i].ll_dev_addr = ll->dev_addr;
              info->ep[i].ll_ep_num = ll->ep_num;
              info->ep[i].ll_attr = ll->attr;
              info->ep[i].ll_data_id = ll->data_id;
              info->ep[i].ll_failed_count = ll->failed_count;
              info->ep[i].ll_size = ll->size;
              info->ep[i].ll_actual_len = ll->actual_len;
              info->ep[i].ll_total_len = ll->total_len;
            }
        }
    }

  info->ll_last_tx_len = pio_usb_debug.last_tx_len;
  info->ll_last_tx_wait_stage = pio_usb_debug.last_tx_wait_stage;
  info->ll_tx_timeout_count = pio_usb_debug.tx_timeout_count;
  info->ll_last_tx_irq = pio_usb_debug.last_tx_irq;
  info->ll_last_tx_fdebug = pio_usb_debug.last_tx_fdebug;
  info->ll_last_tx_flevel = pio_usb_debug.last_tx_flevel;
  info->ll_last_tx_pc = pio_usb_debug.last_tx_pc;
  info->ll_last_rx_start_ok = pio_usb_debug.last_rx_start_ok;
  info->ll_last_rx_start_irq = pio_usb_debug.last_rx_start_irq;
  info->ll_rx_start_timeout_count = pio_usb_debug.rx_start_timeout_count;
  info->ll_last_setup_fail = pio_usb_debug.last_setup_fail;
  info->ll_last_setup_handshake = pio_usb_debug.last_setup_handshake;
  info->ll_last_setup_rx0 = pio_usb_debug.last_setup_rx0;
  info->ll_last_setup_rx1 = pio_usb_debug.last_setup_rx1;
  info->ll_last_setup_failed_count = pio_usb_debug.last_setup_failed_count;
  info->ll_last_in_result = pio_usb_debug.last_in_result;
  info->ll_last_in_pid = pio_usb_debug.last_in_pid;
  info->ll_last_in_expected_pid = pio_usb_debug.last_in_expected_pid;
  info->ll_last_in_rx0 = pio_usb_debug.last_in_rx0;
  info->ll_last_in_rx1 = pio_usb_debug.last_in_rx1;
  info->ll_last_in_failed_count = pio_usb_debug.last_in_failed_count;

  if (root->initialized)
    {
      info->root_line_state = pio_usb_bus_get_line_state(root);
    }

  return OK;
}

int rp23xx_pio_usbhost_hub_portstatus(uint8_t port, bool setpower,
                                       FAR uint16_t *status,
                                       FAR uint16_t *change)
{
#ifdef CONFIG_USBHOST_HUB
  FAR struct rp23xx_pio_usbhost_s *priv = &g_piousb;
  FAR struct usbhost_hubport_s *hport = &priv->rhport.hport;
  struct usb_ctrlreq_s req;
  struct usb_portstatus_s portsts;
  int ret;

  if (status == NULL || change == NULL || port == 0 ||
      port > RP23XX_PIO_USBHOST_INFO_PORTS)
    {
      return -EINVAL;
    }

  if (!priv->initialized || !hport->connected || hport->devclass == NULL ||
      hport->ep0 == NULL || hport->funcaddr == 0)
    {
      return -ENODEV;
    }

  if (priv->hub_nports != 0 && port > priv->hub_nports)
    {
      return -ERANGE;
    }

  if (setpower)
    {
      memset(&req, 0, sizeof(req));
      req.type = USB_REQ_DIR_OUT | USBHUB_REQ_TYPE_PORT;
      req.req = USBHUB_REQ_SETFEATURE;
      piousb_putle16(req.value, USBHUB_PORT_FEAT_POWER);
      piousb_putle16(req.index, port);

      ret = piousb_ctrlout(&priv->drvr, hport->ep0, &req, NULL);
      if (ret < 0)
        {
          return ret;
        }

      nxsched_usleep(150 * 1000);
    }

  memset(&portsts, 0, sizeof(portsts));
  memset(&req, 0, sizeof(req));
  req.type = USB_REQ_DIR_IN | USBHUB_REQ_TYPE_PORT;
  req.req = USBHUB_REQ_GETSTATUS;
  piousb_putle16(req.index, port);
  piousb_putle16(req.len, USB_SIZEOF_PORTSTS);

  ret = piousb_ctrlin(&priv->drvr, hport->ep0, &req, (FAR uint8_t *)&portsts);
  if (ret < 0)
    {
      return ret;
    }

  *status = piousb_getle16(portsts.status);
  *change = piousb_getle16(portsts.change);
  return OK;
#else
  return -ENOTSUP;
#endif
}
