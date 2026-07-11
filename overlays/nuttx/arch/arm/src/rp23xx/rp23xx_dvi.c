/****************************************************************************
 * arch/arm/src/rp23xx/rp23xx_dvi.c
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

#include <sys/types.h>
#include <sys/param.h>
#include <syslog.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/video/fb.h>
#include <nuttx/wqueue.h>

#include <arch/board/board.h>
#include <arch/rp23xx/irq.h>

#include "arm_internal.h"

#include "rp23xx_dmac.h"
#include "rp23xx_gpio.h"

#include "hardware/rp23xx_dma.h"
#include "hardware/rp23xx_dreq.h"
#include "hardware/rp23xx_hstx_ctrl.h"
#include "hardware/rp23xx_hstx_fifo.h"
#include "hardware/rp23xx_memorymap.h"
#include "hardware/rp23xx_pads_bank0.h"
#include "hardware/rp23xx_resets.h"
#include "hardware/rp23xx_watchdog.h"

#ifdef CONFIG_RP23XX_HSTX_DVI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* TMDS control symbols */

#define TMDS_CTRL_00                     0x354u
#define TMDS_CTRL_01                     0x0abu
#define TMDS_CTRL_10                     0x154u
#define TMDS_CTRL_11                     0x2abu

#define SYNC_V0_H0                       (TMDS_CTRL_00 | \
                                          (TMDS_CTRL_00 << 10) | \
                                          (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1                       (TMDS_CTRL_01 | \
                                          (TMDS_CTRL_00 << 10) | \
                                          (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0                       (TMDS_CTRL_10 | \
                                          (TMDS_CTRL_00 << 10) | \
                                          (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1                       (TMDS_CTRL_11 | \
                                          (TMDS_CTRL_00 << 10) | \
                                          (TMDS_CTRL_00 << 20))

#ifndef CONFIG_RP23XX_HSTX_DVI_PIXEL_CLOCK
#  define CONFIG_RP23XX_HSTX_DVI_PIXEL_CLOCK 25200000
#endif

/* HSTX command types */

#define HSTX_CMD_RAW                     (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT              (0x1u << 12)
#define HSTX_CMD_TMDS                    (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT             (0x3u << 12)
#define HSTX_CMD_NOP                     (0xfu << 12)

/* Logical framebuffer profiles and output timing */

#ifdef CONFIG_RP23XX_HSTX_DVI_FB_Y2_800X480
#  ifndef CONFIG_RP23XX_HSTX_DVI_SCANOUT_PSRAM
#    ifndef CONFIG_RP23XX_HSTX_DVI_SINGLE_SCANOUT_INTERNAL
#      error "800x480 Y2 HSTX DVI mode requires PSRAM or internal single scanout"
#    endif
#  endif
#  define RP23XX_DVI_FB_WIDTH            800
#  define RP23XX_DVI_FB_HEIGHT           480
#  define RP23XX_DVI_FB_BPP              2
#  define RP23XX_DVI_FB_FMT              FB_FMT_Y2
#  define RP23XX_DVI_FB_FORMAT_NAME      "Y2"
#  define RP23XX_DVI_SCANOUT_BPP         8
#  define RP23XX_DVI_SCANOUT_FORMAT_NAME "RGB332"
#  define RP23XX_DVI_OUTPUT_SCALING      1
#  ifdef CONFIG_RP23XX_HSTX_DVI_Y2_OUTPUT_VGA_640X480
#    define RP23XX_DVI_SCANOUT_WIDTH     640
#    define RP23XX_DVI_SCANOUT_HEIGHT    480

/* 640x480 @ 60 Hz timing. The app-visible framebuffer remains 800x480 Y2;
 * the RGB332 scanout is downsampled horizontally for monitor compatibility.
 */

#    define H_FRONT_PORCH                16
#    define H_SYNC_WIDTH                 96
#    define H_BACK_PORCH                 48
#    define H_ACTIVE_PIXELS              640

#    define V_FRONT_PORCH                10
#    define V_SYNC_WIDTH                 2
#    define V_BACK_PORCH                 33
#    define V_ACTIVE_LINES               480
#  else
#    define RP23XX_DVI_SCANOUT_WIDTH     800
#    define RP23XX_DVI_SCANOUT_HEIGHT    480

/* 800x480 @ 60 Hz tight timing for TRMNL-size output. The 840x500 total
 * keeps the 25.2 MHz pixel clock exact from the Fruit Jam 126 MHz HSTX
 * clock and was validated with both RGB565 and four-gray HSTX probes.
 */

#    define H_FRONT_PORCH                8
#    define H_SYNC_WIDTH                 16
#    define H_BACK_PORCH                 16
#    define H_ACTIVE_PIXELS              800

#    define V_FRONT_PORCH                5
#    define V_SYNC_WIDTH                 5
#    define V_BACK_PORCH                 10
#    define V_ACTIVE_LINES               480
#  endif
#else
#define RP23XX_DVI_FB_WIDTH              320
#define RP23XX_DVI_FB_HEIGHT             240
#define RP23XX_DVI_OUTPUT_SCALING        CONFIG_RP23XX_HSTX_DVI_OUTPUT_SCALING
#define RP23XX_DVI_FB_BPP                16
#define RP23XX_DVI_FB_FMT                FB_FMT_RGB16_565
#define RP23XX_DVI_FB_FORMAT_NAME        "RGB565"
#define RP23XX_DVI_SCANOUT_BPP           16
#define RP23XX_DVI_SCANOUT_FORMAT_NAME   "RGB565"
#define RP23XX_DVI_SCANOUT_WIDTH         RP23XX_DVI_FB_WIDTH
#define RP23XX_DVI_SCANOUT_HEIGHT        RP23XX_DVI_FB_HEIGHT

/* 640x480 @ 60 Hz timing */

#define H_FRONT_PORCH                    16
#define H_SYNC_WIDTH                     96
#define H_BACK_PORCH                     48
#define H_ACTIVE_PIXELS                  640

#define V_FRONT_PORCH                    10
#define V_SYNC_WIDTH                     2
#define V_BACK_PORCH                     33
#define V_ACTIVE_LINES                   480
#endif

#define RP23XX_DVI_SCANOUT_FRAMEBUFFER   0
#define RP23XX_DVI_SCANOUT_BLACK         1

#if RP23XX_DVI_OUTPUT_SCALING < 1 || RP23XX_DVI_OUTPUT_SCALING > 2
#  error "RP23XX HSTX DVI output scaling must be 1 or 2"
#endif

#define V_TOTAL_LINES                    (V_FRONT_PORCH + V_SYNC_WIDTH + \
                                          V_BACK_PORCH + V_ACTIVE_LINES)
#define RP23XX_DVI_FB_STRIDE             ((RP23XX_DVI_FB_WIDTH * \
                                          RP23XX_DVI_FB_BPP + 7) / 8)
#define RP23XX_DVI_FRAME_BYTES           (RP23XX_DVI_FB_STRIDE * \
                                          RP23XX_DVI_FB_HEIGHT)
#define RP23XX_DVI_SCANOUT_STRIDE        ((RP23XX_DVI_SCANOUT_WIDTH * \
                                          RP23XX_DVI_SCANOUT_BPP + 7) / 8)
#define RP23XX_DVI_SCANOUT_FRAME_BYTES   (RP23XX_DVI_SCANOUT_STRIDE * \
                                          RP23XX_DVI_SCANOUT_HEIGHT)
#define RP23XX_DVI_BPP                   RP23XX_DVI_FB_BPP
#define RP23XX_DVI_CONTENT_WIDTH         (RP23XX_DVI_SCANOUT_WIDTH * \
                                          RP23XX_DVI_OUTPUT_SCALING)

#define H_BORDER                         ((H_ACTIVE_PIXELS - \
                                          RP23XX_DVI_CONTENT_WIDTH) / 2)
#define V_BORDER                         ((V_ACTIVE_LINES - \
                                          RP23XX_DVI_SCANOUT_HEIGHT * \
                                          RP23XX_DVI_OUTPUT_SCALING) / 2)
#define RP23XX_DVI_CONTENT_LINES         (RP23XX_DVI_SCANOUT_HEIGHT * \
                                          RP23XX_DVI_OUTPUT_SCALING)

#define VSYNC_LEN                        6
#if H_BORDER > 0
#  define VACTIVE_PRE_LEN                11
#else
#  define VACTIVE_PRE_LEN                9
#endif
#define VACTIVE_POST_LEN                 2
#define VACTIVE_BLACK_LEN                10
#define DMA_CMD_BLOCK_WORDS              4
#ifdef CONFIG_RP23XX_HSTX_DVI_BLACKOUT
#  define DMA_CMD_BUF_WORDS              ((V_TOTAL_LINES * \
                                          DMA_CMD_BLOCK_WORDS) + \
                                          DMA_CMD_BLOCK_WORDS)
#else
#define DMA_CMD_CONTENT_WORDS            (2 * DMA_CMD_BLOCK_WORDS + \
                                          (H_BORDER > 0 ? \
                                           DMA_CMD_BLOCK_WORDS : 0))

/* Every scanline needs one control block. Content lines need two blocks when
 * the framebuffer fills the active width, or three when diagnostic centering
 * adds left/right borders. Add one terminating block to generate the
 * end-of-frame DMA interrupt.
 */

#define DMA_CMD_BUF_WORDS                (((V_TOTAL_LINES - \
                                          RP23XX_DVI_CONTENT_LINES) * \
                                          DMA_CMD_BLOCK_WORDS) + \
                                          (RP23XX_DVI_CONTENT_LINES * \
                                          DMA_CMD_CONTENT_WORDS) + \
                                          DMA_CMD_BLOCK_WORDS)
#endif

#define RP23XX_DVI_MAX_SCANOUT_BUFFERS   2
#ifdef CONFIG_RP23XX_HSTX_DVI_SINGLE_SCANOUT_INTERNAL
#  define RP23XX_DVI_SCANOUT_BUFFERS     1
#  define RP23XX_DVI_FRONT_BACK_BUFFERS  0
#else
#  define RP23XX_DVI_SCANOUT_BUFFERS     2
#  define RP23XX_DVI_FRONT_BACK_BUFFERS  1
#endif
#define RP23XX_DVI_BUFFER_COUNT          (1 + RP23XX_DVI_SCANOUT_BUFFERS)
#define RP23XX_DVI_NO_PENDING_BUFFER     UINT32_MAX
#define RP23XX_DVI_TIMERAWL              (RP23XX_TIMER0_BASE + 0x28)
#ifdef CONFIG_SCHED_LPWORK
#  define RP23XX_DVI_WORK_QUEUE          LPWORK
#elif defined(CONFIG_SCHED_HPWORK)
#  define RP23XX_DVI_WORK_QUEUE          HPWORK
#else
#  error "RP23XX HSTX DVI framebuffer refresh requires a kernel work queue"
#endif

#define RP23XX_DVI_PIXEL_CLOCK           CONFIG_RP23XX_HSTX_DVI_PIXEL_CLOCK
#define RP23XX_DVI_CLOCKDIV              ((BOARD_HSTX_FREQ + \
                                          (RP23XX_DVI_PIXEL_CLOCK / 2)) / \
                                          RP23XX_DVI_PIXEL_CLOCK)
#define RP23XX_DVI_ACTUAL_PIXEL_CLOCK    (BOARD_HSTX_FREQ / \
                                          RP23XX_DVI_CLOCKDIV)
#define RP23XX_DVI_TREQL_HSTX            RP23XX_DMA_DREQ_HSTX
#define RP23XX_DVI_COLORBAR_WIDTH        (RP23XX_DVI_FB_WIDTH / 8)
#define RP23XX_DVI_RESET_TIMEOUT         1000000u
#define RP23XX_DVIIOC_GETINFO            _FBIOC(0x00f0)

#ifdef CONFIG_RP23XX_HSTX_DVI_FB_Y2_800X480
#  if (RP23XX_DVI_SCANOUT_WIDTH % 4) != 0
#    error "Y2 RGB332 HSTX scanout width must be divisible by four"
#  endif
#  define RP23XX_DVI_PIXEL_DMA_SIZE      RP23XX_DMA_SIZE_WORD
#  define RP23XX_DVI_PIXEL_DMA_COUNT     (RP23XX_DVI_SCANOUT_WIDTH / 4)
#else
#  define RP23XX_DVI_PIXEL_DMA_SIZE      RP23XX_DMA_SIZE_HALFWORD
#  define RP23XX_DVI_PIXEL_DMA_COUNT     RP23XX_DVI_SCANOUT_WIDTH
#endif

#define RP23XX_DVI_STAGE_IDLE            0x44563030u /* "DV00" */
#define RP23XX_DVI_STAGE_START           0x44565354u /* "DVST" */
#define RP23XX_DVI_STAGE_HSTX_ENTER      0x44564830u /* "DVH0" */
#define RP23XX_DVI_STAGE_HSTX_DONE       0x44564831u /* "DVH1" */
#define RP23XX_DVI_STAGE_DMA_ENTER       0x44564430u /* "DVD0" */
#define RP23XX_DVI_STAGE_DMA_ALLOC_PIXEL 0x44564150u /* "DVAP" */
#define RP23XX_DVI_STAGE_DMA_ALLOC_CMD   0x44564143u /* "DVAC" */
#define RP23XX_DVI_STAGE_DMA_ABORT       0x44564142u /* "DVAB" */
#define RP23XX_DVI_STAGE_DMA_COMMANDS    0x4456434cu /* "DVCL" */
#define RP23XX_DVI_STAGE_DMA_IRQ         0x44564952u /* "DVIR" */
#define RP23XX_DVI_STAGE_DMA_KICK        0x44564b49u /* "DVKI" */
#define RP23XX_DVI_STAGE_DMA_DONE        0x44564431u /* "DVD1" */
#define RP23XX_DVI_STAGE_STARTED         0x44564f4bu /* "DVOK" */

/* HSTX register shifts not currently named by the generated header. */

#define HSTX_CSR_CLKDIV_SHIFT            28
#define HSTX_CSR_N_SHIFTS_SHIFT          16
#define HSTX_CSR_SHIFT_SHIFT             8
#define HSTX_BIT_SEL_N_SHIFT             8
#define HSTX_EXPAND_ENC_N_SHIFTS_SHIFT   24
#define HSTX_EXPAND_ENC_SHIFT_SHIFT      16
#define HSTX_EXPAND_RAW_N_SHIFTS_SHIFT   8
#define HSTX_EXPAND_TMDS_L2_NBITS_SHIFT  21
#define HSTX_EXPAND_TMDS_L2_ROT_SHIFT    16
#define HSTX_EXPAND_TMDS_L1_NBITS_SHIFT  13
#define HSTX_EXPAND_TMDS_L1_ROT_SHIFT    8
#define HSTX_EXPAND_TMDS_L0_NBITS_SHIFT  5

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rp23xx_dvi_s
{
  struct fb_vtable_s vtable;
  DMA_HANDLE pixel_dma;
  DMA_HANDLE command_dma;
  struct work_s refresh_work;
  unsigned int pixel_ch;
  unsigned int command_ch;
  volatile uint32_t frame_count;
  volatile uint32_t irq_count;
  volatile uint32_t error_count;
  volatile uint32_t frames_started;
  volatile uint32_t framebuffer_write_count;
  volatile uint32_t framebuffer_mmap_count;
  volatile uint32_t framebuffer_update_count;
  volatile uint32_t front_back_swaps;
  volatile uint32_t missed_swaps;
  volatile uint32_t copy_to_scanout_count;
  volatile uint32_t copy_to_scanout_busy_count;
  volatile uint32_t writes_to_active_buffer_detected;
  volatile uint32_t hstx_restart_count;
  volatile uint32_t hstx_reconfig_count;
  volatile uint32_t buffer_conflict_count;
  volatile uint32_t max_copy_time_us;
  volatile uint32_t max_frame_gap_us;
  volatile uint32_t active_scanout_buffer_index;
  volatile uint32_t writable_buffer_index;
  volatile uint32_t pending_scanout_buffer_index;
  volatile uint32_t switching_scanout_buffer_index;
  volatile uint32_t last_frame_time_us;
  int power;
  bool initialized;
  bool irq_attached;
  bool streaming;
  volatile bool copy_in_progress;
};

struct rp23xx_dvi_info_s
{
  uint32_t framebuffer_width;
  uint32_t framebuffer_height;
  uint32_t output_width;
  uint32_t output_height;
  uint32_t output_scaling;
  uint32_t bpp;
  uint32_t frame_bytes;
  uint32_t sys_clock;
  uint32_t peri_clock;
  uint32_t hstx_clock;
  uint32_t pixel_clock;
  uint32_t target_pixel_clock;
  uint32_t clkdiv;
  uint32_t scanout_mode;
  uint32_t power;
  uint32_t streaming;
  uint32_t dma_allocated;
  uint32_t pixel_ch;
  uint32_t command_ch;
  uint32_t irq_attached;
  uint32_t frame_count;
  uint32_t irq_count;
  uint32_t error_count;
  uint32_t hstx_csr;
  uint32_t hstx_fifo_stat;
  uint32_t dma_intr;
  uint32_t dma_inte2;
  uint32_t dma_ints2;
  uint32_t pixel_ctrl;
  uint32_t pixel_read_addr;
  uint32_t pixel_write_addr;
  uint32_t pixel_trans_count;
  uint32_t command_ctrl;
  uint32_t command_read_addr;
  uint32_t command_write_addr;
  uint32_t command_trans_count;
  uint32_t app_buffer_addr;
  uint32_t scanout_buffer_addr[RP23XX_DVI_MAX_SCANOUT_BUFFERS];
  uint32_t active_scanout_addr;
  uint32_t dma_source_addr;
  uint32_t buffer_count;
  uint32_t app_scanout_same;
  uint32_t front_back_buffering;
  uint32_t app_buffer_internal;
  uint32_t scanout_buffer_internal[RP23XX_DVI_MAX_SCANOUT_BUFFERS];
  uint32_t frames_started;
  uint32_t frames_completed;
  uint32_t dma_irq_count;
  uint32_t framebuffer_write_count;
  uint32_t framebuffer_mmap_count;
  uint32_t framebuffer_update_count;
  uint32_t front_back_swaps;
  uint32_t missed_swaps;
  uint32_t copy_to_scanout_count;
  uint32_t copy_to_scanout_busy_count;
  uint32_t writes_to_active_buffer_detected;
  uint32_t active_scanout_buffer_index;
  uint32_t writable_buffer_index;
  uint32_t pending_scanout_buffer_index;
  uint32_t hstx_restart_count;
  uint32_t hstx_reconfig_count;
  uint32_t dma_error_count;
  uint32_t buffer_conflict_count;
  uint32_t copy_in_progress;
  uint32_t max_copy_time_us;
  uint32_t max_frame_gap_us;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rp23xx_dvi_getvideoinfo(FAR struct fb_vtable_s *vtable,
                                   FAR struct fb_videoinfo_s *vinfo);
static int rp23xx_dvi_getplaneinfo(FAR struct fb_vtable_s *vtable,
                                   int planeno,
                                   FAR struct fb_planeinfo_s *pinfo);
static int rp23xx_dvi_open(FAR struct fb_vtable_s *vtable);
static int rp23xx_dvi_close(FAR struct fb_vtable_s *vtable);
#ifdef CONFIG_FB_UPDATE
static int rp23xx_dvi_updatearea(FAR struct fb_vtable_s *vtable,
                                 FAR const struct fb_area_s *area);
#endif
static int rp23xx_dvi_setframerate(FAR struct fb_vtable_s *vtable,
                                   int rate);
static int rp23xx_dvi_getframerate(FAR struct fb_vtable_s *vtable);
static int rp23xx_dvi_getpower(FAR struct fb_vtable_s *vtable);
static int rp23xx_dvi_setpower(FAR struct fb_vtable_s *vtable, int power);
static int rp23xx_dvi_ioctl(FAR struct fb_vtable_s *vtable, int cmd,
                            unsigned long arg);
static void rp23xx_dvi_refresh_worker(FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint32_t g_vblank_vsync_off[VSYNC_LEN] =
{
  HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH,
  SYNC_V1_H1,
  HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,
  SYNC_V1_H0,
  HSTX_CMD_RAW_REPEAT | (H_BACK_PORCH + H_ACTIVE_PIXELS),
  SYNC_V1_H1
};

static uint32_t g_vblank_vsync_on[VSYNC_LEN] =
{
  HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH,
  SYNC_V0_H1,
  HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,
  SYNC_V0_H0,
  HSTX_CMD_RAW_REPEAT | (H_BACK_PORCH + H_ACTIVE_PIXELS),
  SYNC_V0_H1
};

#ifndef CONFIG_RP23XX_HSTX_DVI_BLACKOUT
static uint32_t g_vactive_pre[VACTIVE_PRE_LEN] =
{
  HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH,
  SYNC_V1_H1,
  HSTX_CMD_NOP,
  HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,
  SYNC_V1_H0,
  HSTX_CMD_NOP,
  HSTX_CMD_RAW_REPEAT | H_BACK_PORCH,
  SYNC_V1_H1,
#if H_BORDER > 0
  HSTX_CMD_TMDS_REPEAT | H_BORDER,
  0x00000000,
#endif
  HSTX_CMD_TMDS | RP23XX_DVI_CONTENT_WIDTH
};

static uint32_t g_vactive_post[VACTIVE_POST_LEN] =
{
  HSTX_CMD_TMDS_REPEAT | H_BORDER,
  0x00000000
};
#endif

static uint32_t g_vactive_black[VACTIVE_BLACK_LEN] =
{
  HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH,
  SYNC_V1_H1,
  HSTX_CMD_NOP,
  HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,
  SYNC_V1_H0,
  HSTX_CMD_NOP,
  HSTX_CMD_RAW_REPEAT | H_BACK_PORCH,
  SYNC_V1_H1,
  HSTX_CMD_TMDS_REPEAT | H_ACTIVE_PIXELS,
  0x00000000
};

static uint32_t g_dma_commands[RP23XX_DVI_SCANOUT_BUFFERS]
                              [DMA_CMD_BUF_WORDS];
static volatile uint32_t g_dma_next_command_addr;
static FAR uint8_t *g_app_buffer;
static FAR uint8_t *g_scanout_buffer[RP23XX_DVI_MAX_SCANOUT_BUFFERS];

static FAR uint8_t *rp23xx_dvi_alloc_scanout_buffer(void)
{
#if defined(CONFIG_RP23XX_HSTX_DVI_SINGLE_SCANOUT_INTERNAL)
  return kmm_memalign(4, RP23XX_DVI_SCANOUT_FRAME_BYTES);
#elif defined(CONFIG_RP23XX_HSTX_DVI_SCANOUT_PSRAM)
  return kumm_memalign(4, RP23XX_DVI_SCANOUT_FRAME_BYTES);
#else
  return kmm_memalign(4, RP23XX_DVI_SCANOUT_FRAME_BYTES);
#endif
}

static void rp23xx_dvi_free_scanout_buffer(FAR uint8_t *buffer)
{
#if defined(CONFIG_RP23XX_HSTX_DVI_SINGLE_SCANOUT_INTERNAL)
  kmm_free(buffer);
#elif defined(CONFIG_RP23XX_HSTX_DVI_SCANOUT_PSRAM)
  kumm_free(buffer);
#else
  kmm_free(buffer);
#endif
}

static struct rp23xx_dvi_s g_dvi =
{
  .vtable =
    {
      .getvideoinfo = rp23xx_dvi_getvideoinfo,
      .getplaneinfo = rp23xx_dvi_getplaneinfo,
      .open         = rp23xx_dvi_open,
      .close        = rp23xx_dvi_close,
#ifdef CONFIG_FB_UPDATE
      .updatearea   = rp23xx_dvi_updatearea,
#endif
      .setframerate = rp23xx_dvi_setframerate,
      .getframerate = rp23xx_dvi_getframerate,
      .getpower     = rp23xx_dvi_getpower,
      .setpower     = rp23xx_dvi_setpower,
      .ioctl        = rp23xx_dvi_ioctl,
    },
  .power = 0,
  .pending_scanout_buffer_index = RP23XX_DVI_NO_PENDING_BUFFER,
  .switching_scanout_buffer_index = RP23XX_DVI_NO_PENDING_BUFFER,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static unsigned int rp23xx_dvi_dma_channel(DMA_HANDLE handle)
{
  uintptr_t addr = rp23xx_dma_register(handle, RP23XX_DMA_CTRL_TRIG_OFFSET);

  return (addr - RP23XX_DMA_CTRL_TRIG(0)) / 0x40;
}

static void rp23xx_dvi_stage(uint32_t stage)
{
  putreg32(stage, RP23XX_WATCHDOG_SCRATCH(2));
}

static void rp23xx_dvi_push_block(FAR uint32_t *commands,
                                  FAR uint32_t *cw, uint32_t ctrl,
                                  uint32_t write_addr, uint32_t count,
                                  uintptr_t read_addr)
{
  commands[(*cw)++] = ctrl;
  commands[(*cw)++] = write_addr;
  commands[(*cw)++] = count;
  commands[(*cw)++] = read_addr;
}

static void rp23xx_dvi_build_command_list(unsigned int scanout)
{
  FAR uint32_t *commands = g_dma_commands[scanout];
  const uint32_t dma_ctrl_base =
    (g_dvi.command_ch << RP23XX_DMA_CTRL_TRIG_CHAIN_TO_SHIFT) |
    (RP23XX_DVI_TREQL_HSTX << RP23XX_DMA_CTRL_TRIG_TREQ_SEL_SHIFT) |
    RP23XX_DMA_CTRL_TRIG_IRQ_QUIET |
    RP23XX_DMA_CTRL_TRIG_HIGH_PRIORITY |
    RP23XX_DMA_CTRL_TRIG_INCR_READ |
    RP23XX_DMA_CTRL_TRIG_EN;
#ifndef CONFIG_RP23XX_HSTX_DVI_BLACKOUT
  const uint32_t dma_pixel_ctrl =
    dma_ctrl_base |
    (RP23XX_DVI_PIXEL_DMA_SIZE <<
     RP23XX_DMA_CTRL_TRIG_DATA_SIZE_SHIFT);
#endif
  const uint32_t dma_ctrl =
    dma_ctrl_base |
    (RP23XX_DMA_SIZE_WORD << RP23XX_DMA_CTRL_TRIG_DATA_SIZE_SHIFT);
  const uint32_t dma_loop_ctrl =
    RP23XX_DMA_CTRL_TRIG_READ_ERROR |
    RP23XX_DMA_CTRL_TRIG_WRITE_ERROR |
    RP23XX_DMA_CTRL_TRIG_HIGH_PRIORITY |
    RP23XX_DMA_CTRL_TRIG_INCR_READ |
    (g_dvi.command_ch << RP23XX_DMA_CTRL_TRIG_CHAIN_TO_SHIFT) |
    RP23XX_DMA_CTRL_TRIG_TREQ_SEL_PERMANENT |
    (RP23XX_DMA_SIZE_WORD << RP23XX_DMA_CTRL_TRIG_DATA_SIZE_SHIFT) |
    RP23XX_DMA_CTRL_TRIG_EN;
  const uint32_t dma_write_addr = RP23XX_HSTX_FIFO_FIFO;

  const unsigned int vsync_start = 0;
  const unsigned int vsync_end = V_SYNC_WIDTH;
  const unsigned int backporch_end = vsync_end + V_BACK_PORCH;
  const unsigned int active_start = backporch_end;
  const unsigned int frontporch_start = V_TOTAL_LINES - V_FRONT_PORCH;
#ifndef CONFIG_RP23XX_HSTX_DVI_BLACKOUT
  const unsigned int content_start = active_start + V_BORDER;
  const unsigned int content_end =
    content_start + RP23XX_DVI_CONTENT_LINES;
#endif
  uint32_t cw = 0;
  unsigned int line;

  DEBUGASSERT(scanout < RP23XX_DVI_SCANOUT_BUFFERS);
  DEBUGASSERT(g_scanout_buffer[scanout] != NULL);

  for (line = 0; line < V_TOTAL_LINES; line++)
    {
      if (line >= vsync_start && line < vsync_end)
        {
          rp23xx_dvi_push_block(commands, &cw, dma_ctrl, dma_write_addr,
                                VSYNC_LEN, (uintptr_t)g_vblank_vsync_on);
        }
      else if (line >= active_start && line < frontporch_start)
        {
#ifdef CONFIG_RP23XX_HSTX_DVI_BLACKOUT
          rp23xx_dvi_push_block(commands, &cw, dma_ctrl, dma_write_addr,
                                VACTIVE_BLACK_LEN,
                                (uintptr_t)g_vactive_black);
#else
          if (line >= content_start && line < content_end)
            {
              const unsigned int row =
                (line - content_start) / RP23XX_DVI_OUTPUT_SCALING;

              rp23xx_dvi_push_block(commands, &cw, dma_ctrl, dma_write_addr,
                                    VACTIVE_PRE_LEN,
                                    (uintptr_t)g_vactive_pre);
              rp23xx_dvi_push_block(commands, &cw, dma_pixel_ctrl,
                                    dma_write_addr,
                                    RP23XX_DVI_PIXEL_DMA_COUNT,
                                    (uintptr_t)&g_scanout_buffer[scanout]
                                      [row * RP23XX_DVI_SCANOUT_STRIDE]);

              if (H_BORDER > 0)
                {
                  rp23xx_dvi_push_block(commands, &cw, dma_ctrl,
                                        dma_write_addr,
                                        VACTIVE_POST_LEN,
                                        (uintptr_t)g_vactive_post);
                }
            }
          else
            {
              rp23xx_dvi_push_block(commands, &cw, dma_ctrl, dma_write_addr,
                                    VACTIVE_BLACK_LEN,
                                    (uintptr_t)g_vactive_black);
            }
#endif
        }
      else
        {
          rp23xx_dvi_push_block(commands, &cw, dma_ctrl, dma_write_addr,
                                VSYNC_LEN, (uintptr_t)g_vblank_vsync_off);
        }
    }

  /* Keep scanout continuous in hardware.  The final pixel-channel block
   * rewinds the command channel's READ_ADDR, then chains to the command
   * channel for the next frame.  This avoids depending on interrupt latency
   * to preserve HDMI/DVI sync.
   */

  rp23xx_dvi_push_block(commands, &cw, dma_loop_ctrl,
                        RP23XX_DMA_READ_ADDR(g_dvi.command_ch), 1,
                        (uintptr_t)&g_dma_next_command_addr);

  DEBUGASSERT(cw == DMA_CMD_BUF_WORDS);
}

#ifdef CONFIG_RP23XX_HSTX_DVI_FB_Y2_800X480
static const uint8_t g_y2_to_rgb332[] =
{
  0x00, 0x49, 0xb6, 0xff
};

static void rp23xx_dvi_y2_set_pixel(FAR uint8_t *buffer,
                                    unsigned int x, unsigned int y,
                                    uint8_t value)
{
  FAR uint8_t *pixel;
  unsigned int shift;

  pixel = &buffer[y * RP23XX_DVI_FB_STRIDE + (x >> 2)];
  shift = (3 - (x & 3)) * 2;
  *pixel = (*pixel & ~(3u << shift)) | ((value & 3u) << shift);
}

#if !defined(CONFIG_RP23XX_HSTX_DVI_Y2_OUTPUT_VGA_640X480) && \
    (RP23XX_DVI_SCANOUT_WIDTH != RP23XX_DVI_FB_WIDTH || \
     RP23XX_DVI_SCANOUT_HEIGHT != RP23XX_DVI_FB_HEIGHT)
static uint8_t rp23xx_dvi_y2_get_pixel(FAR const uint8_t *buffer,
                                       unsigned int x, unsigned int y)
{
  FAR const uint8_t *pixel;
  unsigned int shift;

  pixel = &buffer[y * RP23XX_DVI_FB_STRIDE + (x >> 2)];
  shift = (3 - (x & 3)) * 2;
  return (*pixel >> shift) & 3u;
}
#endif

#ifdef CONFIG_RP23XX_HSTX_DVI_Y2_OUTPUT_VGA_640X480
static uint8_t rp23xx_dvi_y2_get_row_pixel(FAR const uint8_t *row,
                                           unsigned int x)
{
  FAR const uint8_t *pixel;
  unsigned int shift;

  pixel = &row[x >> 2];
  shift = (3 - (x & 3)) * 2;
  return (*pixel >> shift) & 3u;
}
#endif

static void rp23xx_dvi_expand_y2_to_scanout(FAR uint8_t *scanout)
{
#if RP23XX_DVI_SCANOUT_WIDTH == RP23XX_DVI_FB_WIDTH && \
    RP23XX_DVI_SCANOUT_HEIGHT == RP23XX_DVI_FB_HEIGHT
  unsigned int x;
  unsigned int y;

  for (y = 0; y < RP23XX_DVI_SCANOUT_HEIGHT; y++)
    {
      FAR const uint8_t *src = &g_app_buffer[y * RP23XX_DVI_FB_STRIDE];
      FAR uint8_t *dest = &scanout[y * RP23XX_DVI_SCANOUT_STRIDE];

      for (x = 0; x < RP23XX_DVI_SCANOUT_WIDTH; x += 4)
        {
          uint8_t packed = src[x >> 2];

          dest[x + 0] = g_y2_to_rgb332[(packed >> 6) & 3u];
          dest[x + 1] = g_y2_to_rgb332[(packed >> 4) & 3u];
          dest[x + 2] = g_y2_to_rgb332[(packed >> 2) & 3u];
          dest[x + 3] = g_y2_to_rgb332[packed & 3u];
        }
    }
#elif defined(CONFIG_RP23XX_HSTX_DVI_Y2_OUTPUT_VGA_640X480)
  unsigned int x;
  unsigned int y;

  for (y = 0; y < RP23XX_DVI_SCANOUT_HEIGHT; y++)
    {
      FAR const uint8_t *src = &g_app_buffer[y * RP23XX_DVI_FB_STRIDE];
      FAR uint8_t *dest = &scanout[y * RP23XX_DVI_SCANOUT_STRIDE];
      unsigned int src_x = 0;

      /* 800 -> 640 is a 5:4 horizontal downsample. Select source pixels
       * 0, 1, 3, 4 from each group of 5 to avoid a divide per output pixel.
       */

      for (x = 0; x < RP23XX_DVI_SCANOUT_WIDTH; x += 4)
        {
          dest[x + 0] =
            g_y2_to_rgb332[rp23xx_dvi_y2_get_row_pixel(src, src_x + 0)];
          dest[x + 1] =
            g_y2_to_rgb332[rp23xx_dvi_y2_get_row_pixel(src, src_x + 1)];
          dest[x + 2] =
            g_y2_to_rgb332[rp23xx_dvi_y2_get_row_pixel(src, src_x + 3)];
          dest[x + 3] =
            g_y2_to_rgb332[rp23xx_dvi_y2_get_row_pixel(src, src_x + 4)];
          src_x += 5;
        }
    }
#else
  unsigned int src_x;
  unsigned int src_y;
  unsigned int x;
  unsigned int y;

  for (y = 0; y < RP23XX_DVI_SCANOUT_HEIGHT; y++)
    {
      FAR uint8_t *dest = &scanout[y * RP23XX_DVI_SCANOUT_STRIDE];

      src_y = ((y * RP23XX_DVI_FB_HEIGHT) +
               (RP23XX_DVI_SCANOUT_HEIGHT / 2)) /
              RP23XX_DVI_SCANOUT_HEIGHT;
      if (src_y >= RP23XX_DVI_FB_HEIGHT)
        {
          src_y = RP23XX_DVI_FB_HEIGHT - 1;
        }

      for (x = 0; x < RP23XX_DVI_SCANOUT_WIDTH; x++)
        {
          src_x = ((x * RP23XX_DVI_FB_WIDTH) +
                   (RP23XX_DVI_SCANOUT_WIDTH / 2)) /
                  RP23XX_DVI_SCANOUT_WIDTH;
          if (src_x >= RP23XX_DVI_FB_WIDTH)
            {
              src_x = RP23XX_DVI_FB_WIDTH - 1;
            }

          dest[x] = g_y2_to_rgb332
            [rp23xx_dvi_y2_get_pixel(g_app_buffer, src_x, src_y)];
        }
    }
#endif
}
#endif

static void rp23xx_dvi_copy_to_scanout(FAR uint8_t *scanout)
{
#ifdef CONFIG_RP23XX_HSTX_DVI_FB_Y2_800X480
  rp23xx_dvi_expand_y2_to_scanout(scanout);
#else
  memcpy(scanout, g_app_buffer, RP23XX_DVI_SCANOUT_FRAME_BYTES);
#endif
}

static void rp23xx_dvi_fill_colorbars(FAR uint8_t *buffer)
{
#ifdef CONFIG_RP23XX_HSTX_DVI_FB_Y2_800X480
  static const uint8_t colors[] =
    {
      3, 3, 2, 2, 1, 1, 0, 0
    };
#else
  static const uint16_t colors[] =
    {
      0xffff, 0xffe0, 0x07ff, 0x07e0,
      0xf81f, 0xf800, 0x001f, 0x0000
    };
  FAR uint16_t *pixels = (FAR uint16_t *)buffer;
#endif
  unsigned int x;
  unsigned int y;

  for (y = 0; y < RP23XX_DVI_FB_HEIGHT; y++)
    {
      for (x = 0; x < RP23XX_DVI_FB_WIDTH; x++)
        {
          unsigned int bar = x / RP23XX_DVI_COLORBAR_WIDTH;

          if (bar >= nitems(colors))
            {
              bar = nitems(colors) - 1;
            }

#ifdef CONFIG_RP23XX_HSTX_DVI_FB_Y2_800X480
          rp23xx_dvi_y2_set_pixel(buffer, x, y, colors[bar]);
#else
          pixels[y * RP23XX_DVI_FB_WIDTH + x] = colors[bar];
#endif
        }
    }
}

static uint32_t rp23xx_dvi_time_us(void)
{
  return getreg32(RP23XX_DVI_TIMERAWL);
}

static bool rp23xx_dvi_internal_sram(FAR const void *addr)
{
  uintptr_t value = (uintptr_t)addr;

  return value >= CONFIG_RAM_START &&
         value < (CONFIG_RAM_START + CONFIG_RAM_SIZE);
}

static void rp23xx_dvi_schedule_refresh(void)
{
  int ret;

  if (!g_dvi.streaming)
    {
      return;
    }

  if (!work_available(&g_dvi.refresh_work))
    {
      g_dvi.copy_to_scanout_busy_count++;
      return;
    }

  ret = work_queue(RP23XX_DVI_WORK_QUEUE, &g_dvi.refresh_work,
                   rp23xx_dvi_refresh_worker, NULL, 0);
  if (ret < 0)
    {
      g_dvi.copy_to_scanout_busy_count++;
    }
}

static void rp23xx_dvi_refresh_worker(FAR void *arg)
{
  irqstate_t flags;
  uint32_t target;
  uint32_t start;
  uint32_t elapsed;

  UNUSED(arg);

  flags = enter_critical_section();
#ifdef CONFIG_RP23XX_HSTX_DVI_SINGLE_SCANOUT_INTERNAL
  if (!g_dvi.streaming || g_dvi.copy_in_progress)
    {
      g_dvi.copy_to_scanout_busy_count++;
      leave_critical_section(flags);
      return;
    }

  target = g_dvi.active_scanout_buffer_index;
  if (target >= RP23XX_DVI_SCANOUT_BUFFERS ||
      g_scanout_buffer[target] == NULL)
    {
      g_dvi.buffer_conflict_count++;
      leave_critical_section(flags);
      return;
    }

  g_dvi.copy_in_progress = true;
  leave_critical_section(flags);

  start = rp23xx_dvi_time_us();
  rp23xx_dvi_copy_to_scanout(g_scanout_buffer[target]);
  UP_DMB();
  elapsed = rp23xx_dvi_time_us() - start;

  flags = enter_critical_section();
  g_dvi.copy_in_progress = false;
  g_dvi.copy_to_scanout_count++;
  if (elapsed > g_dvi.max_copy_time_us)
    {
      g_dvi.max_copy_time_us = elapsed;
    }

  leave_critical_section(flags);
#else
  if (!g_dvi.streaming || g_dvi.copy_in_progress ||
      g_dvi.pending_scanout_buffer_index != RP23XX_DVI_NO_PENDING_BUFFER ||
      g_dvi.switching_scanout_buffer_index != RP23XX_DVI_NO_PENDING_BUFFER)
    {
      g_dvi.copy_to_scanout_busy_count++;
      leave_critical_section(flags);
      return;
    }

  target = 1 - g_dvi.active_scanout_buffer_index;
  g_dvi.writable_buffer_index = target;
  if (target >= RP23XX_DVI_SCANOUT_BUFFERS ||
      g_scanout_buffer[target] == NULL ||
      g_scanout_buffer[target] ==
        g_scanout_buffer[g_dvi.active_scanout_buffer_index])
    {
      g_dvi.buffer_conflict_count++;
      g_dvi.writes_to_active_buffer_detected++;
      leave_critical_section(flags);
      return;
    }

  g_dvi.copy_in_progress = true;
  leave_critical_section(flags);

  start = rp23xx_dvi_time_us();
  rp23xx_dvi_copy_to_scanout(g_scanout_buffer[target]);
  UP_DMB();
  elapsed = rp23xx_dvi_time_us() - start;

  flags = enter_critical_section();
  g_dvi.copy_in_progress = false;
  g_dvi.copy_to_scanout_count++;
  if (elapsed > g_dvi.max_copy_time_us)
    {
      g_dvi.max_copy_time_us = elapsed;
    }

  if (g_dvi.streaming &&
      target != g_dvi.active_scanout_buffer_index &&
      g_dvi.pending_scanout_buffer_index == RP23XX_DVI_NO_PENDING_BUFFER)
    {
      g_dvi.pending_scanout_buffer_index = target;
    }
  else
    {
      g_dvi.buffer_conflict_count++;
    }

  leave_critical_section(flags);
#endif
}

static int rp23xx_dvi_dma_interrupt(int irq, void *context, void *arg)
{
  const uint32_t bit = 1u << g_dvi.pixel_ch;
  const uint32_t errors = RP23XX_DMA_CTRL_TRIG_READ_ERROR |
                          RP23XX_DMA_CTRL_TRIG_WRITE_ERROR;
  uint32_t now;
  uint32_t gap;
  uint32_t active;
  uint32_t ctrl;

  g_dvi.irq_count++;
  now = rp23xx_dvi_time_us();
  if (g_dvi.last_frame_time_us != 0)
    {
      gap = now - g_dvi.last_frame_time_us;
      if (gap > g_dvi.max_frame_gap_us)
        {
          g_dvi.max_frame_gap_us = gap;
        }
    }

  g_dvi.last_frame_time_us = now;
  putreg32(bit, RP23XX_DMA_INTR);
  putreg32(bit, RP23XX_DMA_INTS2);

  ctrl = getreg32(RP23XX_DMA_CTRL_TRIG(g_dvi.pixel_ch));
  if ((ctrl & RP23XX_DMA_CTRL_TRIG_AHB_ERROR) != 0)
    {
      g_dvi.error_count++;
      putreg32(0, RP23XX_DMA_CTRL_TRIG(g_dvi.pixel_ch));
      putreg32(0, RP23XX_DMA_AL3_CTRL(g_dvi.command_ch));
      clrbits_reg32(bit, RP23XX_DMA_INTE2);
      setbits_reg32(errors, RP23XX_DMA_CTRL_TRIG(g_dvi.pixel_ch));
      g_dvi.power = 0;
      g_dvi.streaming = false;
      return OK;
    }

  active = g_dvi.active_scanout_buffer_index;
  if (g_dvi.switching_scanout_buffer_index != RP23XX_DVI_NO_PENDING_BUFFER)
    {
      active = g_dvi.switching_scanout_buffer_index;
      if (active < RP23XX_DVI_SCANOUT_BUFFERS)
        {
          g_dvi.active_scanout_buffer_index = active;
          g_dvi.writable_buffer_index = 1 - active;
          g_dvi.switching_scanout_buffer_index =
            RP23XX_DVI_NO_PENDING_BUFFER;
          g_dvi.front_back_swaps++;
        }
      else
        {
          g_dvi.switching_scanout_buffer_index =
            RP23XX_DVI_NO_PENDING_BUFFER;
          g_dvi.buffer_conflict_count++;
          active = g_dvi.active_scanout_buffer_index;
        }
    }
  else if (g_dvi.pending_scanout_buffer_index != RP23XX_DVI_NO_PENDING_BUFFER)
    {
      active = g_dvi.pending_scanout_buffer_index;
      if (active < RP23XX_DVI_SCANOUT_BUFFERS)
        {
          g_dma_next_command_addr = (uint32_t)(uintptr_t)
            g_dma_commands[active];
          g_dvi.switching_scanout_buffer_index = active;
          g_dvi.pending_scanout_buffer_index = RP23XX_DVI_NO_PENDING_BUFFER;
        }
      else
        {
          g_dvi.pending_scanout_buffer_index = RP23XX_DVI_NO_PENDING_BUFFER;
          g_dvi.buffer_conflict_count++;
          active = g_dvi.active_scanout_buffer_index;
        }
    }
  else
    {
      g_dvi.missed_swaps++;
    }

  g_dvi.frames_started++;
  g_dvi.frame_count++;

  return OK;
}

static void rp23xx_dvi_configure_access(void)
{
}

static int rp23xx_dvi_configure_hstx(void)
{
  const unsigned int pins[] =
    {
      BOARD_DVI_D0_PIN, BOARD_DVI_D1_PIN,
      BOARD_DVI_D2_PIN, BOARD_DVI_D3_PIN,
      BOARD_DVI_D4_PIN, BOARD_DVI_D5_PIN,
      BOARD_DVI_D6_PIN, BOARD_DVI_D7_PIN
    };
  const unsigned int lane_pin[] =
    {
      BOARD_DVI_D3_PIN, BOARD_DVI_D5_PIN, BOARD_DVI_D7_PIN
    };
  uint32_t clkdiv = RP23XX_DVI_CLOCKDIV;
  unsigned int i;
  unsigned int timeout;

  g_dvi.hstx_reconfig_count++;
  putreg32(0, RP23XX_HSTX_CTRL_CSR);
  setbits_reg32(RP23XX_RESETS_RESET_HSTX, RP23XX_RESETS_RESET);
  clrbits_reg32(RP23XX_RESETS_RESET_HSTX, RP23XX_RESETS_RESET);
  timeout = RP23XX_DVI_RESET_TIMEOUT;
  while ((getreg32(RP23XX_RESETS_RESET_DONE) &
          RP23XX_RESETS_RESET_HSTX) == 0)
    {
      if (timeout-- == 0)
        {
          return -ETIMEDOUT;
        }
    }

  if (clkdiv < 1)
    {
      clkdiv = 1;
    }
  else if (clkdiv > 0x0f)
    {
      clkdiv = 0x0f;
    }

#ifdef CONFIG_RP23XX_HSTX_DVI_FB_Y2_800X480
  putreg32((2u << HSTX_EXPAND_TMDS_L2_NBITS_SHIFT) |
           (0u << HSTX_EXPAND_TMDS_L2_ROT_SHIFT) |
           (2u << HSTX_EXPAND_TMDS_L1_NBITS_SHIFT) |
           (29u << HSTX_EXPAND_TMDS_L1_ROT_SHIFT) |
           (1u << HSTX_EXPAND_TMDS_L0_NBITS_SHIFT) |
           26u,
           RP23XX_HSTX_CTRL_EXPAND_TMDS);

  putreg32((4u << HSTX_EXPAND_ENC_N_SHIFTS_SHIFT) |
           (8u << HSTX_EXPAND_ENC_SHIFT_SHIFT) |
           (1u << HSTX_EXPAND_RAW_N_SHIFTS_SHIFT),
           RP23XX_HSTX_CTRL_EXPAND_SHIFT);
#else
  putreg32((4u << HSTX_EXPAND_TMDS_L2_NBITS_SHIFT) |
           (8u << HSTX_EXPAND_TMDS_L2_ROT_SHIFT) |
           (5u << HSTX_EXPAND_TMDS_L1_NBITS_SHIFT) |
           (3u << HSTX_EXPAND_TMDS_L1_ROT_SHIFT) |
           (4u << HSTX_EXPAND_TMDS_L0_NBITS_SHIFT) |
           29u,
           RP23XX_HSTX_CTRL_EXPAND_TMDS);

  putreg32((RP23XX_DVI_OUTPUT_SCALING <<
            HSTX_EXPAND_ENC_N_SHIFTS_SHIFT) |
           (16u << HSTX_EXPAND_ENC_SHIFT_SHIFT) |
           (1u << HSTX_EXPAND_RAW_N_SHIFTS_SHIFT),
           RP23XX_HSTX_CTRL_EXPAND_SHIFT);
#endif

  putreg32(0, RP23XX_HSTX_CTRL_CSR);
  putreg32(RP23XX_HSTX_CTRL_CSR_EXPAND_EN |
           (clkdiv << HSTX_CSR_CLKDIV_SHIFT) |
           (5u << HSTX_CSR_N_SHIFTS_SHIFT) |
           (2u << HSTX_CSR_SHIFT_SHIFT) |
           RP23XX_HSTX_CTRL_CSR_EN,
           RP23XX_HSTX_CTRL_CSR);

  i = BOARD_DVI_D1_PIN - BOARD_DVI_D0_PIN;
  putreg32(RP23XX_HSTX_CTRL_BIT_CLK_MASK, RP23XX_HSTX_CTRL_BIT(i));
  putreg32(RP23XX_HSTX_CTRL_BIT_CLK_MASK | RP23XX_HSTX_CTRL_BIT_INV_MASK,
           RP23XX_HSTX_CTRL_BIT(i ^ 1));

  for (i = 0; i < 3; i++)
    {
      const unsigned int bit = lane_pin[i] - BOARD_DVI_D0_PIN;
      const uint32_t sel = ((i * 10u) |
                            ((i * 10u + 1u) << HSTX_BIT_SEL_N_SHIFT));

      putreg32(sel, RP23XX_HSTX_CTRL_BIT(bit));
      putreg32(sel | RP23XX_HSTX_CTRL_BIT_INV_MASK,
               RP23XX_HSTX_CTRL_BIT(bit ^ 1));
    }

  for (i = 0; i < nitems(pins); i++)
    {
      rp23xx_gpio_set_function(pins[i], RP23XX_GPIO_FUNC_HSTX);
      rp23xx_gpio_set_drive_strength(pins[i],
                                     RP23XX_PADS_BANK0_GPIO_DRIVE_4MA);
    }

  return OK;
}

static int rp23xx_dvi_abort_dma(void)
{
  uint32_t bitmask;
  unsigned int timeout;

  if (g_dvi.pixel_dma == NULL || g_dvi.command_dma == NULL)
    {
      return OK;
    }

  rp23xx_dvi_stage(RP23XX_DVI_STAGE_DMA_ABORT);
  bitmask = (1u << g_dvi.pixel_ch) | (1u << g_dvi.command_ch);

  clrbits_reg32(bitmask, RP23XX_DMA_INTE0);
  clrbits_reg32(bitmask, RP23XX_DMA_INTE1);
  clrbits_reg32(bitmask, RP23XX_DMA_INTE2);
  clrbits_reg32(bitmask, RP23XX_DMA_INTE3);

  putreg32(bitmask, RP23XX_DMA_CHAN_ABORT);
  timeout = RP23XX_DVI_RESET_TIMEOUT;
  while ((getreg32(RP23XX_DMA_CHAN_ABORT) & bitmask) != 0)
    {
      if (timeout-- == 0)
        {
          return -ETIMEDOUT;
        }
    }

  putreg32(0, RP23XX_DMA_CTRL_TRIG(g_dvi.pixel_ch));
  putreg32(0, RP23XX_DMA_CTRL_TRIG(g_dvi.command_ch));
  putreg32(0, RP23XX_DMA_AL3_CTRL(g_dvi.command_ch));

  putreg32(bitmask, RP23XX_DMA_INTR);
  putreg32(bitmask, RP23XX_DMA_INTS0);
  putreg32(bitmask, RP23XX_DMA_INTS1);
  putreg32(bitmask, RP23XX_DMA_INTS2);
  putreg32(bitmask, RP23XX_DMA_INTS3);

  return OK;
}

static int rp23xx_dvi_configure_dma(void)
{
  uint32_t bitmask;
  uint32_t command_ctrl;
  int ret;

  rp23xx_dvi_stage(RP23XX_DVI_STAGE_DMA_ENTER);
  if (g_dvi.pixel_dma == NULL)
    {
      rp23xx_dvi_stage(RP23XX_DVI_STAGE_DMA_ALLOC_PIXEL);
      g_dvi.pixel_dma = rp23xx_dmachannel();
      if (g_dvi.pixel_dma == NULL)
        {
          return -ENOMEM;
        }

      g_dvi.pixel_ch = rp23xx_dvi_dma_channel(g_dvi.pixel_dma);
    }

  if (g_dvi.command_dma == NULL)
    {
      rp23xx_dvi_stage(RP23XX_DVI_STAGE_DMA_ALLOC_CMD);
      g_dvi.command_dma = rp23xx_dmachannel();
      if (g_dvi.command_dma == NULL)
        {
          rp23xx_dmafree(g_dvi.pixel_dma);
          g_dvi.pixel_dma = NULL;
          return -ENOMEM;
        }

      g_dvi.command_ch = rp23xx_dvi_dma_channel(g_dvi.command_dma);
    }

  bitmask = (1u << g_dvi.pixel_ch) | (1u << g_dvi.command_ch);
  clrbits_reg32(bitmask, RP23XX_DMA_INTE0);
  putreg32(bitmask, RP23XX_DMA_INTS0);

  ret = rp23xx_dvi_abort_dma();
  if (ret < 0)
    {
      return ret;
    }

  rp23xx_dvi_stage(RP23XX_DVI_STAGE_DMA_COMMANDS);
  for (unsigned int i = 0; i < RP23XX_DVI_SCANOUT_BUFFERS; i++)
    {
      rp23xx_dvi_build_command_list(i);
    }

  command_ctrl = RP23XX_DMA_CTRL_TRIG_READ_ERROR |
                 RP23XX_DMA_CTRL_TRIG_WRITE_ERROR |
                 RP23XX_DMA_CTRL_TRIG_INCR_READ |
                 RP23XX_DMA_CTRL_TRIG_INCR_WRITE |
                 RP23XX_DMA_CTRL_TRIG_HIGH_PRIORITY |
                 RP23XX_DMA_CTRL_TRIG_RING_SEL |
                 (4u << RP23XX_DMA_CTRL_TRIG_RING_SIZE_SHIFT) |
                 (RP23XX_DMA_SIZE_WORD <<
                  RP23XX_DMA_CTRL_TRIG_DATA_SIZE_SHIFT) |
                 (g_dvi.command_ch <<
                  RP23XX_DMA_CTRL_TRIG_CHAIN_TO_SHIFT) |
                 RP23XX_DMA_CTRL_TRIG_TREQ_SEL_PERMANENT |
                 RP23XX_DMA_CTRL_TRIG_EN;

  putreg32(command_ctrl, RP23XX_DMA_AL3_CTRL(g_dvi.command_ch));
  putreg32(RP23XX_DMA_AL3_CTRL(g_dvi.pixel_ch),
           RP23XX_DMA_AL3_WRITE_ADDR(g_dvi.command_ch));
  putreg32(4, RP23XX_DMA_AL3_TRANS_COUNT(g_dvi.command_ch));

  rp23xx_dvi_stage(RP23XX_DVI_STAGE_DMA_IRQ);
  putreg32(1u << g_dvi.pixel_ch, RP23XX_DMA_INTE2);
  if (!g_dvi.irq_attached)
    {
      ret = irq_attach(RP23XX_DMA_IRQ_2, rp23xx_dvi_dma_interrupt, NULL);
      if (ret < 0)
        {
          return ret;
        }

      g_dvi.irq_attached = true;
    }

  g_dvi.frame_count = 0;
  g_dvi.irq_count = 0;
  g_dvi.error_count = 0;
  g_dvi.frames_started = 0;
  g_dvi.front_back_swaps = 0;
  g_dvi.missed_swaps = 0;
  g_dvi.copy_to_scanout_count = 0;
  g_dvi.copy_to_scanout_busy_count = 0;
  g_dvi.writes_to_active_buffer_detected = 0;
  g_dvi.buffer_conflict_count = 0;
  g_dvi.max_copy_time_us = 0;
  g_dvi.max_frame_gap_us = 0;
  g_dvi.last_frame_time_us = 0;
  g_dvi.active_scanout_buffer_index = 0;
  g_dvi.writable_buffer_index = RP23XX_DVI_FRONT_BACK_BUFFERS ? 1 : 0;
  g_dvi.pending_scanout_buffer_index = RP23XX_DVI_NO_PENDING_BUFFER;
  g_dvi.switching_scanout_buffer_index = RP23XX_DVI_NO_PENDING_BUFFER;
  g_dvi.copy_in_progress = false;
  g_dma_next_command_addr = (uint32_t)(uintptr_t)g_dma_commands[0];

  up_enable_irq(RP23XX_DMA_IRQ_2);

  rp23xx_dvi_stage(RP23XX_DVI_STAGE_DMA_KICK);
  putreg32((uintptr_t)g_dma_commands[0],
           RP23XX_DMA_AL3_READ_ADDR_TRIG(g_dvi.command_ch));
  g_dvi.frames_started++;
  rp23xx_dvi_stage(RP23XX_DVI_STAGE_DMA_DONE);
  return OK;
}

static int rp23xx_dvi_stop(void)
{
  int ret;

  g_dvi.streaming = false;
  g_dvi.power = 0;
  work_cancel(RP23XX_DVI_WORK_QUEUE, &g_dvi.refresh_work);

  up_disable_irq(RP23XX_DMA_IRQ_2);
  ret = rp23xx_dvi_abort_dma();

  putreg32(0, RP23XX_HSTX_CTRL_CSR);

  return ret;
}

static int rp23xx_dvi_start(void)
{
  int ret;

  if (g_app_buffer == NULL || g_scanout_buffer[0] == NULL ||
#ifndef CONFIG_RP23XX_HSTX_DVI_SINGLE_SCANOUT_INTERNAL
      g_scanout_buffer[1] == NULL
#else
      false
#endif
      )
    {
      return -ENODEV;
    }

  rp23xx_dvi_stage(RP23XX_DVI_STAGE_START);
  if (g_dvi.streaming)
    {
      g_dvi.power = 1;
      return OK;
    }

  g_dvi.hstx_restart_count++;
  rp23xx_dvi_copy_to_scanout(g_scanout_buffer[0]);
#ifndef CONFIG_RP23XX_HSTX_DVI_SINGLE_SCANOUT_INTERNAL
  memcpy(g_scanout_buffer[1], g_scanout_buffer[0],
         RP23XX_DVI_SCANOUT_FRAME_BYTES);
#endif

  rp23xx_dvi_configure_access();
  rp23xx_dvi_stage(RP23XX_DVI_STAGE_HSTX_ENTER);
  ret = rp23xx_dvi_configure_hstx();
  if (ret < 0)
    {
      putreg32(0, RP23XX_HSTX_CTRL_CSR);
      g_dvi.power = 0;
      return ret;
    }

  rp23xx_dvi_stage(RP23XX_DVI_STAGE_HSTX_DONE);
  ret = rp23xx_dvi_configure_dma();
  if (ret < 0)
    {
      putreg32(0, RP23XX_HSTX_CTRL_CSR);
      g_dvi.power = 0;
      return ret;
    }

  g_dvi.streaming = true;
  g_dvi.power = 1;
  rp23xx_dvi_stage(RP23XX_DVI_STAGE_STARTED);
  return OK;
}

static int rp23xx_dvi_getvideoinfo(FAR struct fb_vtable_s *vtable,
                                   FAR struct fb_videoinfo_s *vinfo)
{
  if (vinfo == NULL)
    {
      return -EINVAL;
    }

  memset(vinfo, 0, sizeof(*vinfo));
  vinfo->fmt = RP23XX_DVI_FB_FMT;
  vinfo->xres = RP23XX_DVI_FB_WIDTH;
  vinfo->yres = RP23XX_DVI_FB_HEIGHT;
  vinfo->nplanes = 1;

  return OK;
}

static int rp23xx_dvi_getplaneinfo(FAR struct fb_vtable_s *vtable,
                                   int planeno,
                                   FAR struct fb_planeinfo_s *pinfo)
{
  if (pinfo == NULL)
    {
      return -EINVAL;
    }

  if (planeno != 0)
    {
      return -EINVAL;
    }

  memset(pinfo, 0, sizeof(*pinfo));
  if (g_app_buffer == NULL)
    {
      return -ENODEV;
    }

  g_dvi.framebuffer_mmap_count++;
  pinfo->fbmem = g_app_buffer;
  pinfo->fblen = RP23XX_DVI_FRAME_BYTES;
  pinfo->stride = RP23XX_DVI_FB_STRIDE;
  pinfo->display = 0;
  pinfo->bpp = RP23XX_DVI_BPP;
  pinfo->xres_virtual = RP23XX_DVI_FB_WIDTH;
  pinfo->yres_virtual = RP23XX_DVI_FB_HEIGHT;

  return OK;
}

static int rp23xx_dvi_open(FAR struct fb_vtable_s *vtable)
{
  return OK;
}

static int rp23xx_dvi_close(FAR struct fb_vtable_s *vtable)
{
  return OK;
}

#ifdef CONFIG_FB_UPDATE
static int rp23xx_dvi_updatearea(FAR struct fb_vtable_s *vtable,
                                 FAR const struct fb_area_s *area)
{
  g_dvi.framebuffer_update_count++;
  g_dvi.framebuffer_write_count++;
  rp23xx_dvi_schedule_refresh();
  return OK;
}
#endif

static int rp23xx_dvi_setframerate(FAR struct fb_vtable_s *vtable,
                                   int rate)
{
  return rate == 60 ? OK : -EINVAL;
}

static int rp23xx_dvi_getframerate(FAR struct fb_vtable_s *vtable)
{
  return 60;
}

static int rp23xx_dvi_getpower(FAR struct fb_vtable_s *vtable)
{
  return g_dvi.power;
}

static int rp23xx_dvi_setpower(FAR struct fb_vtable_s *vtable, int power)
{
  if (power > 0)
    {
      return rp23xx_dvi_start();
    }

  return rp23xx_dvi_stop();
}

static int rp23xx_dvi_ioctl(FAR struct fb_vtable_s *vtable, int cmd,
                            unsigned long arg)
{
  FAR struct rp23xx_dvi_info_s *info;

  if (cmd == RP23XX_DVIIOC_GETINFO)
    {
      info = (FAR struct rp23xx_dvi_info_s *)((uintptr_t)arg);
      if (info == NULL)
        {
          return -EINVAL;
        }

      memset(info, 0, sizeof(*info));
      info->framebuffer_width = RP23XX_DVI_FB_WIDTH;
      info->framebuffer_height = RP23XX_DVI_FB_HEIGHT;
      info->output_width = H_ACTIVE_PIXELS;
      info->output_height = V_ACTIVE_LINES;
      info->output_scaling = RP23XX_DVI_OUTPUT_SCALING;
      info->bpp = RP23XX_DVI_BPP;
      info->frame_bytes = RP23XX_DVI_FRAME_BYTES;
      info->sys_clock = BOARD_SYS_FREQ;
      info->peri_clock = BOARD_PERI_FREQ;
      info->hstx_clock = BOARD_HSTX_FREQ;
      info->pixel_clock = RP23XX_DVI_ACTUAL_PIXEL_CLOCK;
      info->target_pixel_clock = RP23XX_DVI_PIXEL_CLOCK;
      info->clkdiv = RP23XX_DVI_CLOCKDIV;
#ifdef CONFIG_RP23XX_HSTX_DVI_BLACKOUT
      info->scanout_mode = RP23XX_DVI_SCANOUT_BLACK;
#else
      info->scanout_mode = RP23XX_DVI_SCANOUT_FRAMEBUFFER;
#endif
      info->power = g_dvi.power;
      info->streaming = g_dvi.streaming ? 1 : 0;
      info->dma_allocated =
        g_dvi.pixel_dma != NULL && g_dvi.command_dma != NULL ? 1 : 0;
      info->pixel_ch = g_dvi.pixel_ch;
      info->command_ch = g_dvi.command_ch;
      info->irq_attached = g_dvi.irq_attached ? 1 : 0;
      info->frame_count = g_dvi.frame_count;
      info->irq_count = g_dvi.irq_count;
      info->error_count = g_dvi.error_count;
      info->app_buffer_addr = (uint32_t)(uintptr_t)g_app_buffer;
      info->scanout_buffer_addr[0] =
        (uint32_t)(uintptr_t)g_scanout_buffer[0];
      info->scanout_buffer_addr[1] =
        (uint32_t)(uintptr_t)g_scanout_buffer[1];
      info->active_scanout_addr =
        (uint32_t)(uintptr_t)g_scanout_buffer
          [g_dvi.active_scanout_buffer_index];
      info->dma_source_addr = info->active_scanout_addr;
      info->buffer_count = RP23XX_DVI_BUFFER_COUNT;
      info->app_scanout_same =
        (FAR const void *)g_app_buffer ==
        (FAR const void *)g_scanout_buffer[g_dvi.active_scanout_buffer_index];
      info->front_back_buffering = RP23XX_DVI_FRONT_BACK_BUFFERS;
      info->app_buffer_internal = rp23xx_dvi_internal_sram(g_app_buffer);
      info->scanout_buffer_internal[0] =
        rp23xx_dvi_internal_sram(g_scanout_buffer[0]);
      info->scanout_buffer_internal[1] =
        rp23xx_dvi_internal_sram(g_scanout_buffer[1]);
      info->frames_started = g_dvi.frames_started;
      info->frames_completed = g_dvi.frame_count;
      info->dma_irq_count = g_dvi.irq_count;
      info->framebuffer_write_count = g_dvi.framebuffer_write_count;
      info->framebuffer_mmap_count = g_dvi.framebuffer_mmap_count;
      info->framebuffer_update_count = g_dvi.framebuffer_update_count;
      info->front_back_swaps = g_dvi.front_back_swaps;
      info->missed_swaps = g_dvi.missed_swaps;
      info->copy_to_scanout_count = g_dvi.copy_to_scanout_count;
      info->copy_to_scanout_busy_count = g_dvi.copy_to_scanout_busy_count;
      info->writes_to_active_buffer_detected =
        g_dvi.writes_to_active_buffer_detected;
      info->active_scanout_buffer_index =
        g_dvi.active_scanout_buffer_index;
      info->writable_buffer_index = g_dvi.writable_buffer_index;
      info->pending_scanout_buffer_index =
        g_dvi.pending_scanout_buffer_index;
      info->hstx_restart_count = g_dvi.hstx_restart_count;
      info->hstx_reconfig_count = g_dvi.hstx_reconfig_count;
      info->dma_error_count = g_dvi.error_count;
      info->buffer_conflict_count = g_dvi.buffer_conflict_count;
      info->copy_in_progress = g_dvi.copy_in_progress ? 1 : 0;
      info->max_copy_time_us = g_dvi.max_copy_time_us;
      info->max_frame_gap_us = g_dvi.max_frame_gap_us;
      info->hstx_csr = getreg32(RP23XX_HSTX_CTRL_CSR);
      info->hstx_fifo_stat = getreg32(RP23XX_HSTX_FIFO_STAT);
      info->dma_intr = getreg32(RP23XX_DMA_INTR);
      info->dma_inte2 = getreg32(RP23XX_DMA_INTE2);
      info->dma_ints2 = getreg32(RP23XX_DMA_INTS2);

      if (g_dvi.pixel_dma != NULL && g_dvi.command_dma != NULL)
        {
          info->pixel_ctrl = getreg32(RP23XX_DMA_CTRL_TRIG(g_dvi.pixel_ch));
          info->pixel_read_addr =
            getreg32(RP23XX_DMA_READ_ADDR(g_dvi.pixel_ch));
          info->pixel_write_addr =
            getreg32(RP23XX_DMA_WRITE_ADDR(g_dvi.pixel_ch));
          info->pixel_trans_count =
            getreg32(RP23XX_DMA_TRANS_COUNT(g_dvi.pixel_ch));
          info->command_ctrl =
            getreg32(RP23XX_DMA_CTRL_TRIG(g_dvi.command_ch));
          info->command_read_addr =
            getreg32(RP23XX_DMA_READ_ADDR(g_dvi.command_ch));
          info->command_write_addr =
            getreg32(RP23XX_DMA_WRITE_ADDR(g_dvi.command_ch));
          info->command_trans_count =
            getreg32(RP23XX_DMA_TRANS_COUNT(g_dvi.command_ch));
        }

      return OK;
    }

  return -ENOTTY;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int up_fbinitialize(int display)
{
  unsigned int i;

  if (display != 0)
    {
      return -EINVAL;
    }

  if (g_dvi.initialized)
    {
      return OK;
    }

  g_app_buffer = kumm_memalign(4, RP23XX_DVI_FRAME_BYTES);
  if (g_app_buffer == NULL)
    {
      return -ENOMEM;
    }

  for (i = 0; i < RP23XX_DVI_SCANOUT_BUFFERS; i++)
    {
      g_scanout_buffer[i] = rp23xx_dvi_alloc_scanout_buffer();
      if (g_scanout_buffer[i] == NULL)
        {
          while (i > 0)
            {
              i--;
              rp23xx_dvi_free_scanout_buffer(g_scanout_buffer[i]);
              g_scanout_buffer[i] = NULL;
            }

          kumm_free(g_app_buffer);
          g_app_buffer = NULL;
          return -ENOMEM;
        }
    }

  memset(g_app_buffer, 0, RP23XX_DVI_FRAME_BYTES);
  rp23xx_dvi_fill_colorbars(g_app_buffer);
  for (i = 0; i < RP23XX_DVI_SCANOUT_BUFFERS; i++)
    {
      rp23xx_dvi_copy_to_scanout(g_scanout_buffer[i]);
    }

  g_dvi.initialized = true;
  g_dvi.power = 0;
  g_dvi.active_scanout_buffer_index = 0;
  g_dvi.writable_buffer_index = RP23XX_DVI_FRONT_BACK_BUFFERS ? 1 : 0;
  g_dvi.pending_scanout_buffer_index = RP23XX_DVI_NO_PENDING_BUFFER;
  g_dvi.switching_scanout_buffer_index = RP23XX_DVI_NO_PENDING_BUFFER;
  rp23xx_dvi_stage(RP23XX_DVI_STAGE_IDLE);
  syslog(LOG_INFO,
         "rp23xx_dvi: /dev/fb0 %ux%u %s bpp=%u stride=%u bytes=%u"
         " output=%ux%u scale=%u scanout=%s bytes=%u app=%p"
         " scanout0=%p scanout1=%p buffers=%u split=%s app_sram=%u"
         " scanout_sram=%u/%u\n",
         RP23XX_DVI_FB_WIDTH, RP23XX_DVI_FB_HEIGHT,
         RP23XX_DVI_FB_FORMAT_NAME, RP23XX_DVI_BPP, RP23XX_DVI_FB_STRIDE,
         RP23XX_DVI_FRAME_BYTES, H_ACTIVE_PIXELS, V_ACTIVE_LINES,
         RP23XX_DVI_OUTPUT_SCALING, RP23XX_DVI_SCANOUT_FORMAT_NAME,
         RP23XX_DVI_SCANOUT_FRAME_BYTES,
         g_app_buffer, g_scanout_buffer[0], g_scanout_buffer[1],
         RP23XX_DVI_BUFFER_COUNT,
         (FAR const void *)g_app_buffer == (FAR const void *)g_scanout_buffer[0]
           ? "no" : "yes",
         rp23xx_dvi_internal_sram(g_app_buffer) ? 1 : 0,
         rp23xx_dvi_internal_sram(g_scanout_buffer[0]) ? 1 : 0,
         rp23xx_dvi_internal_sram(g_scanout_buffer[1]) ? 1 : 0);
  return OK;
}

FAR struct fb_vtable_s *up_fbgetvplane(int display, int vplane)
{
  if (display != 0 || vplane != 0)
    {
      return NULL;
    }

  return &g_dvi.vtable;
}

void up_fbuninitialize(int display)
{
  unsigned int i;

  if (display != 0 || !g_dvi.initialized)
    {
      return;
    }

  rp23xx_dvi_stop();
  for (i = 0; i < RP23XX_DVI_SCANOUT_BUFFERS; i++)
    {
      rp23xx_dvi_free_scanout_buffer(g_scanout_buffer[i]);
      g_scanout_buffer[i] = NULL;
    }

  kumm_free(g_app_buffer);
  g_app_buffer = NULL;
  g_dvi.initialized = false;
}

#endif /* CONFIG_RP23XX_HSTX_DVI */
