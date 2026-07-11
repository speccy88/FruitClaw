/****************************************************************************
 * apps/system/dvictrl/dvictrl_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_SYSTEM_DVICTRL_START_GUARD
#  include <arch/rp23xx/watchdog.h>
#  include <nuttx/timers/watchdog.h>
#endif

#include <nuttx/video/fb.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_SYSTEM_DVICTRL_DEVPATH
#  define CONFIG_SYSTEM_DVICTRL_DEVPATH "/dev/fb0"
#endif

#ifndef CONFIG_SYSTEM_DVICTRL_GUARD_DEVPATH
#  define CONFIG_SYSTEM_DVICTRL_GUARD_DEVPATH "/dev/watchdog0"
#endif

#ifndef CONFIG_SYSTEM_DVICTRL_GUARD_TIMEOUT_MS
#  define CONFIG_SYSTEM_DVICTRL_GUARD_TIMEOUT_MS 8000
#endif

#ifndef CONFIG_SYSTEM_DVICTRL_GUARD_SETTLE_MS
#  define CONFIG_SYSTEM_DVICTRL_GUARD_SETTLE_MS 3000
#endif

#define DVICTRL_BOOTGUARD_MAGIC 0x464a4247 /* "FJBG" */
#define DVICTRL_BOOTGUARD_STAGE_START 0x44564931 /* "DVI1" */
#define DVICTRL_USEC_PER_MSEC 1000u
#define DVICTRL_DEFAULT_STRESS_SECONDS 10u

#define RP23XX_DVIIOC_GETINFO _FBIOC(0x00f0)

/****************************************************************************
 * Private Types
 ****************************************************************************/

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
  uint32_t scanout_buffer_addr[2];
  uint32_t active_scanout_addr;
  uint32_t dma_source_addr;
  uint32_t buffer_count;
  uint32_t app_scanout_same;
  uint32_t front_back_buffering;
  uint32_t app_buffer_internal;
  uint32_t scanout_buffer_internal[2];
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
 * Private Functions
 ****************************************************************************/

static void dvictrl_usage(FAR const char *progname, int exitcode)
{
  fprintf(stderr,
          "Usage:\n"
          "  %s info\n"
          "  %s pattern colorbars\n"
          "  %s solid <rgb565>\n"
          "  %s stress static\n"
          "  %s stress full [seconds]\n"
          "  %s stress partial [seconds]\n"
          "  %s start [--unguarded] [--guard-timeout <ms>]"
          " [--guard-window <ms>]\n"
          "  %s stop\n",
          progname, progname, progname, progname, progname, progname,
          progname, progname);
  exit(exitcode);
}

static int dvictrl_open(void)
{
  int fd = open(CONFIG_SYSTEM_DVICTRL_DEVPATH, O_RDWR);

  if (fd < 0)
    {
      fprintf(stderr, "ERROR: open %s failed: %d\n",
              CONFIG_SYSTEM_DVICTRL_DEVPATH, errno);
    }

  return fd;
}

static int dvictrl_getinfo(int fd, FAR struct fb_videoinfo_s *vinfo,
                           FAR struct fb_planeinfo_s *pinfo)
{
  memset(vinfo, 0, sizeof(*vinfo));
  memset(pinfo, 0, sizeof(*pinfo));

  if (ioctl(fd, FBIOGET_VIDEOINFO, (unsigned long)((uintptr_t)vinfo)) < 0)
    {
      fprintf(stderr, "ERROR: FBIOGET_VIDEOINFO failed: %d\n", errno);
      return -errno;
    }

  pinfo->display = 0;
  if (ioctl(fd, FBIOGET_PLANEINFO, (unsigned long)((uintptr_t)pinfo)) < 0)
    {
      fprintf(stderr, "ERROR: FBIOGET_PLANEINFO failed: %d\n", errno);
      return -errno;
    }

  return 0;
}

static int dvictrl_print_info(int fd)
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  struct rp23xx_dvi_info_s dinfo;
  int power;
  int ret;

  ret = dvictrl_getinfo(fd, &vinfo, &pinfo);
  if (ret < 0)
    {
      return ret;
    }

  power = -1;
  ret = ioctl(fd, FBIOGET_POWER, (unsigned long)((uintptr_t)&power));
  if (ret < 0)
    {
      fprintf(stderr, "WARNING: FBIOGET_POWER failed: %d\n", errno);
    }

  printf("device: %s\n", CONFIG_SYSTEM_DVICTRL_DEVPATH);
  printf("video: %ux%u fmt=%u planes=%u\n",
         vinfo.xres, vinfo.yres, vinfo.fmt, vinfo.nplanes);
  printf("plane: bpp=%u stride=%u fblen=%zu virt=%" PRIu32 "x%" PRIu32
         "\n",
         pinfo.bpp, pinfo.stride, pinfo.fblen,
         (uint32_t)pinfo.xres_virtual, (uint32_t)pinfo.yres_virtual);
  printf("power: %d\n", power);

  memset(&dinfo, 0, sizeof(dinfo));
  ret = ioctl(fd, RP23XX_DVIIOC_GETINFO,
              (unsigned long)((uintptr_t)&dinfo));
  if (ret < 0)
    {
      printf("dvi: debug-info unavailable\n");
      return 0;
    }

  printf("dvi: fb=%" PRIu32 "x%" PRIu32 " output=%" PRIu32 "x%" PRIu32
         " scale=%" PRIu32 " bpp=%" PRIu32 " bytes=%" PRIu32 "\n",
         dinfo.framebuffer_width, dinfo.framebuffer_height,
         dinfo.output_width, dinfo.output_height, dinfo.output_scaling,
         dinfo.bpp, dinfo.frame_bytes);
  printf("clock: sys=%" PRIu32 " peri=%" PRIu32 " hstx=%" PRIu32
         " target_pixel=%" PRIu32 " actual_pixel=%" PRIu32
         " clkdiv=%" PRIu32 "\n",
         dinfo.sys_clock, dinfo.peri_clock, dinfo.hstx_clock,
         dinfo.target_pixel_clock, dinfo.pixel_clock, dinfo.clkdiv);
  printf("scanout: %s\n",
         dinfo.scanout_mode == 1 ? "black-only" : "framebuffer");
  printf("dma: allocated=%" PRIu32 " pixel_ch=%" PRIu32
         " command_ch=%" PRIu32 " irq_attached=%" PRIu32
         " streaming=%" PRIu32 "\n",
         dinfo.dma_allocated, dinfo.pixel_ch, dinfo.command_ch,
         dinfo.irq_attached, dinfo.streaming);
  printf("frames: frame=%" PRIu32 " irq=%" PRIu32
         " errors=%" PRIu32 "\n",
         dinfo.frame_count, dinfo.irq_count, dinfo.error_count);
  printf("hstx: csr=0x%08" PRIx32 " fifo_stat=0x%08" PRIx32 "\n",
         dinfo.hstx_csr, dinfo.hstx_fifo_stat);
  printf("dmairq: intr=0x%08" PRIx32 " inte2=0x%08" PRIx32
         " ints2=0x%08" PRIx32 "\n",
         dinfo.dma_intr, dinfo.dma_inte2, dinfo.dma_ints2);
  printf("pixel: ctrl=0x%08" PRIx32 " read=0x%08" PRIx32
         " write=0x%08" PRIx32 " count=%" PRIu32 "\n",
         dinfo.pixel_ctrl, dinfo.pixel_read_addr, dinfo.pixel_write_addr,
         dinfo.pixel_trans_count);
  printf("command: ctrl=0x%08" PRIx32 " read=0x%08" PRIx32
         " write=0x%08" PRIx32 " count=%" PRIu32 "\n",
         dinfo.command_ctrl, dinfo.command_read_addr,
         dinfo.command_write_addr, dinfo.command_trans_count);
  printf("buffers: count=%" PRIu32 " split=%s frontback=%" PRIu32
         " app=0x%08" PRIx32 " active=0x%08" PRIx32
         " dma=0x%08" PRIx32 "\n",
         dinfo.buffer_count, dinfo.app_scanout_same ? "no" : "yes",
         dinfo.front_back_buffering, dinfo.app_buffer_addr,
         dinfo.active_scanout_addr, dinfo.dma_source_addr);
  printf("scanout: [0]=0x%08" PRIx32 " sram=%" PRIu32
         " [1]=0x%08" PRIx32 " sram=%" PRIu32
         " app_sram=%" PRIu32 "\n",
         dinfo.scanout_buffer_addr[0], dinfo.scanout_buffer_internal[0],
         dinfo.scanout_buffer_addr[1], dinfo.scanout_buffer_internal[1],
         dinfo.app_buffer_internal);
  printf("ownership: active=%" PRIu32 " writable=%" PRIu32
         " pending=%" PRIu32 " copying=%" PRIu32 "\n",
         dinfo.active_scanout_buffer_index, dinfo.writable_buffer_index,
         dinfo.pending_scanout_buffer_index, dinfo.copy_in_progress);
  printf("refresh: copies=%" PRIu32 " swaps=%" PRIu32
         " missed=%" PRIu32 " busy=%" PRIu32
         " conflicts=%" PRIu32 " active_writes=%" PRIu32 "\n",
         dinfo.copy_to_scanout_count, dinfo.front_back_swaps,
         dinfo.missed_swaps, dinfo.copy_to_scanout_busy_count,
         dinfo.buffer_conflict_count,
         dinfo.writes_to_active_buffer_detected);
  printf("counters: started=%" PRIu32 " completed=%" PRIu32
         " dma_irq=%" PRIu32 " fb_write=%" PRIu32
         " fb_mmap=%" PRIu32 " fb_update=%" PRIu32 "\n",
         dinfo.frames_started, dinfo.frames_completed,
         dinfo.dma_irq_count, dinfo.framebuffer_write_count,
         dinfo.framebuffer_mmap_count, dinfo.framebuffer_update_count);
  printf("timing: max_copy=%" PRIu32 " us max_frame_gap=%" PRIu32
         " us hstx_start=%" PRIu32 " hstx_reconfig=%" PRIu32
         " dma_errors=%" PRIu32 "\n",
         dinfo.max_copy_time_us, dinfo.max_frame_gap_us,
         dinfo.hstx_restart_count, dinfo.hstx_reconfig_count,
         dinfo.dma_error_count);

  return 0;
}

static int dvictrl_set_power(int fd, int power)
{
  if (ioctl(fd, FBIOSET_POWER, (unsigned long)power) < 0)
    {
      fprintf(stderr, "ERROR: FBIOSET_POWER(%d) failed: %d\n",
              power, errno);
      return -errno;
    }

  return 0;
}

static int dvictrl_parse_u32(FAR const char *arg, FAR uint32_t *value)
{
  FAR char *endptr;
  unsigned long parsed;

  errno = 0;
  parsed = strtoul(arg, &endptr, 0);
  if (errno != 0 || *endptr != '\0' || parsed > UINT32_MAX)
    {
      return -EINVAL;
    }

  *value = parsed;
  return 0;
}

#ifdef CONFIG_SYSTEM_DVICTRL_START_GUARD
static int dvictrl_guard_open(void)
{
  int fd = open(CONFIG_SYSTEM_DVICTRL_GUARD_DEVPATH, O_RDONLY);

  if (fd < 0)
    {
      fprintf(stderr, "ERROR: open %s failed: %d\n",
              CONFIG_SYSTEM_DVICTRL_GUARD_DEVPATH, errno);
    }

  return fd;
}

static int dvictrl_guard_ioctl(int fd, int cmd, unsigned long arg,
                               FAR const char *name)
{
  int ret;

  ret = ioctl(fd, cmd, arg);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: %s failed: %d\n", name, errno);
      return -errno;
    }

  return 0;
}

static int dvictrl_guard_arm(FAR int *guardfd, uint32_t timeout)
{
  int fd;
  int ret;

  fd = dvictrl_guard_open();
  if (fd < 0)
    {
      return -errno;
    }

  ret = dvictrl_guard_ioctl(fd, WDIOC_STOP, 0, "WDIOC_STOP");
  if (ret < 0)
    {
      goto errout;
    }

  ret = dvictrl_guard_ioctl(fd, WDIOC_SET_SCRATCH0,
                            DVICTRL_BOOTGUARD_MAGIC,
                            "WDIOC_SET_SCRATCH0");
  if (ret < 0)
    {
      goto errout;
    }

  ret = dvictrl_guard_ioctl(fd, WDIOC_SET_SCRATCH1, timeout,
                            "WDIOC_SET_SCRATCH1");
  if (ret < 0)
    {
      goto errout;
    }

  ret = dvictrl_guard_ioctl(fd, WDIOC_SET_SCRATCH2,
                            DVICTRL_BOOTGUARD_STAGE_START,
                            "WDIOC_SET_SCRATCH2");
  if (ret < 0)
    {
      goto errout;
    }

  ret = dvictrl_guard_ioctl(fd, WDIOC_SETTIMEOUT, timeout,
                            "WDIOC_SETTIMEOUT");
  if (ret < 0)
    {
      goto errout;
    }

  ret = dvictrl_guard_ioctl(fd, WDIOC_START, 0, "WDIOC_START");
  if (ret < 0)
    {
      goto errout;
    }

  *guardfd = fd;
  printf("bootguard: armed for %" PRIu32 " ms\n", timeout);
  return 0;

errout:
  close(fd);
  return ret;
}

static int dvictrl_guard_disarm(int fd)
{
  int ret = 0;

  if (fd < 0)
    {
      return 0;
    }

  if (dvictrl_guard_ioctl(fd, WDIOC_SET_SCRATCH0, 0,
                          "WDIOC_SET_SCRATCH0") < 0)
    {
      ret = -EIO;
    }

  if (dvictrl_guard_ioctl(fd, WDIOC_SET_SCRATCH1, 0,
                          "WDIOC_SET_SCRATCH1") < 0)
    {
      ret = -EIO;
    }

  if (dvictrl_guard_ioctl(fd, WDIOC_SET_SCRATCH2, 0,
                          "WDIOC_SET_SCRATCH2") < 0)
    {
      ret = -EIO;
    }

  if (dvictrl_guard_ioctl(fd, WDIOC_STOP, 0, "WDIOC_STOP") < 0)
    {
      ret = -EIO;
    }

  printf("bootguard: disarmed\n");
  return ret;
}

static int dvictrl_start(int fd, int argc, FAR char *argv[])
{
  uint32_t timeout = CONFIG_SYSTEM_DVICTRL_GUARD_TIMEOUT_MS;
  uint32_t settle = CONFIG_SYSTEM_DVICTRL_GUARD_SETTLE_MS;
  bool guarded = true;
  int guardfd = -1;
  int ret;
  int i;

  for (i = 2; i < argc; i++)
    {
      if (strcmp(argv[i], "--unguarded") == 0 ||
          strcmp(argv[i], "unguarded") == 0)
        {
          guarded = false;
        }
      else if (strcmp(argv[i], "--guard-timeout") == 0)
        {
          if (++i >= argc || dvictrl_parse_u32(argv[i], &timeout) < 0 ||
              timeout == 0)
            {
              dvictrl_usage(argv[0], EXIT_FAILURE);
            }
        }
      else if (strcmp(argv[i], "--guard-window") == 0)
        {
          if (++i >= argc || dvictrl_parse_u32(argv[i], &settle) < 0)
            {
              dvictrl_usage(argv[0], EXIT_FAILURE);
            }
        }
      else
        {
          dvictrl_usage(argv[0], EXIT_FAILURE);
        }
    }

  if (guarded)
    {
      if (timeout <= settle)
        {
          fprintf(stderr,
                  "ERROR: guard timeout must be greater than guard window\n");
          return -EINVAL;
        }

      ret = dvictrl_guard_arm(&guardfd, timeout);
      if (ret < 0)
        {
          fprintf(stderr,
                  "ERROR: DVI start guard unavailable; use --unguarded"
                  " only for hands-on tests\n");
          return ret;
        }
    }

  ret = dvictrl_set_power(fd, 1);
  if (ret < 0)
    {
      if (guarded)
        {
          dvictrl_guard_disarm(guardfd);
          close(guardfd);
        }

      return ret;
    }

  if (guarded && settle > 0)
    {
      printf("bootguard: holding for %" PRIu32 " ms\n", settle);
      usleep(settle * DVICTRL_USEC_PER_MSEC);
    }

  if (guarded)
    {
      ret = dvictrl_guard_disarm(guardfd);
      close(guardfd);
      if (ret < 0)
        {
          return ret;
        }
    }

  return 0;
}
#else
static int dvictrl_start(int fd, int argc, FAR char *argv[])
{
  int i;

  for (i = 2; i < argc; i++)
    {
      if (strcmp(argv[i], "--unguarded") != 0 &&
          strcmp(argv[i], "unguarded") != 0)
        {
          dvictrl_usage(argv[0], EXIT_FAILURE);
        }
    }

  return dvictrl_set_power(fd, 1);
}
#endif

static int dvictrl_map_framebuffer(int fd,
                                   FAR struct fb_videoinfo_s *vinfo,
                                   FAR struct fb_planeinfo_s *pinfo,
                                   FAR uint8_t **fbmem)
{
  FAR void *map;
  int ret;

  ret = dvictrl_getinfo(fd, vinfo, pinfo);
  if (ret < 0)
    {
      return ret;
    }

  if (!((vinfo->fmt == FB_FMT_RGB16_565 && pinfo->bpp == 16) ||
        (vinfo->fmt == FB_FMT_Y2 && pinfo->bpp == 2)))
    {
      fprintf(stderr, "ERROR: unsupported framebuffer format/bpp: %u/%u\n",
              vinfo->fmt, pinfo->bpp);
      return -EINVAL;
    }

  map = mmap(NULL, pinfo->fblen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED)
    {
      fprintf(stderr, "ERROR: mmap failed: %d\n", errno);
      return -errno;
    }

  *fbmem = map;
  return 0;
}

static uint8_t dvictrl_rgb565_to_y2(uint16_t color)
{
  uint32_t r = (color >> 11) & 0x1f;
  uint32_t g = (color >> 5) & 0x3f;
  uint32_t b = color & 0x1f;
  uint32_t y;

  r = (r << 1) | (r >> 4);
  b = (b << 1) | (b >> 4);
  y = (r * 30 + g * 59 + b * 11) / 100;

  return (uint8_t)((y * 3 + 31) / 63);
}

static void dvictrl_set_y2_pixel(FAR uint8_t *fbmem,
                                 FAR const struct fb_planeinfo_s *pinfo,
                                 unsigned int x, unsigned int y,
                                 uint8_t value)
{
  FAR uint8_t *pixel;
  unsigned int shift;

  pixel = &fbmem[y * pinfo->stride + (x >> 2)];
  shift = (3 - (x & 3)) * 2;
  *pixel = (*pixel & ~(3u << shift)) | ((value & 3u) << shift);
}

static void dvictrl_set_pixel(FAR uint8_t *fbmem,
                              FAR const struct fb_videoinfo_s *vinfo,
                              FAR const struct fb_planeinfo_s *pinfo,
                              unsigned int x, unsigned int y,
                              uint16_t color)
{
  if (vinfo->fmt == FB_FMT_Y2 && pinfo->bpp == 2)
    {
      dvictrl_set_y2_pixel(fbmem, pinfo, x, y,
                           dvictrl_rgb565_to_y2(color));
      return;
    }

  ((FAR uint16_t *)(fbmem + y * pinfo->stride))[x] = color;
}

static void dvictrl_fill_solid(FAR uint8_t *fbmem,
                               FAR const struct fb_videoinfo_s *vinfo,
                               FAR const struct fb_planeinfo_s *pinfo,
                               uint16_t color)
{
  unsigned int x;
  unsigned int y;

  for (y = 0; y < vinfo->yres; y++)
    {
      for (x = 0; x < vinfo->xres; x++)
        {
          dvictrl_set_pixel(fbmem, vinfo, pinfo, x, y, color);
        }
    }
}

static int dvictrl_update_full(int fd, FAR const struct fb_videoinfo_s *vinfo)
{
#ifdef CONFIG_FB_UPDATE
  struct fb_area_s area;

  area.x = 0;
  area.y = 0;
  area.w = vinfo->xres;
  area.h = vinfo->yres;

  if (ioctl(fd, FBIO_UPDATE, (uintptr_t)&area) < 0)
    {
      return -errno;
    }
#else
  UNUSED(fd);
  UNUSED(vinfo);
#endif

  return 0;
}

static void dvictrl_fill_colorbars(FAR uint8_t *fbmem,
                                   FAR const struct fb_videoinfo_s *vinfo,
                                   FAR const struct fb_planeinfo_s *pinfo)
{
  static const uint16_t colors[] =
    {
      0xffff, 0xffe0, 0x07ff, 0x07e0,
      0xf81f, 0xf800, 0x001f, 0x0000
    };
  unsigned int bar_width = MAX(1, vinfo->xres / nitems(colors));
  unsigned int x;
  unsigned int y;

  for (y = 0; y < vinfo->yres; y++)
    {
      for (x = 0; x < vinfo->xres; x++)
        {
          unsigned int bar = x / bar_width;

          if (bar >= nitems(colors))
            {
              bar = nitems(colors) - 1;
            }

          dvictrl_set_pixel(fbmem, vinfo, pinfo, x, y, colors[bar]);
        }
    }
}

static int dvictrl_pattern_colorbars(int fd)
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  FAR uint8_t *fbmem;
  int ret;

  ret = dvictrl_map_framebuffer(fd, &vinfo, &pinfo, &fbmem);
  if (ret < 0)
    {
      return ret;
    }

  dvictrl_fill_colorbars(fbmem, &vinfo, &pinfo);
  ret = dvictrl_update_full(fd, &vinfo);
  munmap(fbmem, pinfo.fblen);
  if (ret < 0)
    {
      return ret;
    }

  printf("filled colorbars\n");
  return 0;
}

static int dvictrl_solid(int fd, FAR const char *arg)
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  FAR uint8_t *fbmem;
  FAR char *endptr;
  unsigned long value;
  int ret;

  errno = 0;
  value = strtoul(arg, &endptr, 0);
  if (errno != 0 || *endptr != '\0' || value > 0xffff)
    {
      fprintf(stderr, "ERROR: invalid RGB565 color: %s\n", arg);
      return -EINVAL;
    }

  ret = dvictrl_map_framebuffer(fd, &vinfo, &pinfo, &fbmem);
  if (ret < 0)
    {
      return ret;
    }

  dvictrl_fill_solid(fbmem, &vinfo, &pinfo, (uint16_t)value);
  ret = dvictrl_update_full(fd, &vinfo);
  munmap(fbmem, pinfo.fblen);
  if (ret < 0)
    {
      return ret;
    }

  printf("filled solid 0x%04lx\n", value);
  return 0;
}

static uint64_t dvictrl_time_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    {
      return 0;
    }

  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint32_t dvictrl_stress_seconds(int argc, FAR char *argv[])
{
  uint32_t seconds = DVICTRL_DEFAULT_STRESS_SECONDS;

  if (argc >= 4 && dvictrl_parse_u32(argv[3], &seconds) < 0)
    {
      dvictrl_usage(argv[0], EXIT_FAILURE);
    }

  return seconds;
}

static void dvictrl_draw_rect(FAR uint8_t *fbmem,
                              FAR const struct fb_videoinfo_s *vinfo,
                              FAR const struct fb_planeinfo_s *pinfo,
                              unsigned int x, unsigned int y,
                              unsigned int w, unsigned int h,
                              uint16_t color)
{
  unsigned int xend = MIN(x + w, vinfo->xres);
  unsigned int yend = MIN(y + h, vinfo->yres);
  unsigned int yy;
  unsigned int xx;

  for (yy = y; yy < yend; yy++)
    {
      for (xx = x; xx < xend; xx++)
        {
          dvictrl_set_pixel(fbmem, vinfo, pinfo, xx, yy, color);
        }
    }
}

static void dvictrl_fill_moving(FAR uint8_t *fbmem,
                                FAR const struct fb_videoinfo_s *vinfo,
                                FAR const struct fb_planeinfo_s *pinfo,
                                uint32_t frame)
{
  static const uint16_t colors[] =
    {
      0xf800, 0xffe0, 0x07e0, 0x07ff,
      0x001f, 0xf81f, 0xffff, 0x0000
    };
  unsigned int x;
  unsigned int y;

  for (y = 0; y < vinfo->yres; y++)
    {
      for (x = 0; x < vinfo->xres; x++)
        {
          unsigned int bar = ((x + frame * 4) / 20 + y / 30) %
                             nitems(colors);

          dvictrl_set_pixel(fbmem, vinfo, pinfo, x, y, colors[bar]);
        }
    }
}

static int dvictrl_stress_static(int fd)
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  FAR uint8_t *fbmem;
  int ret;

  ret = dvictrl_map_framebuffer(fd, &vinfo, &pinfo, &fbmem);
  if (ret < 0)
    {
      return ret;
    }

  dvictrl_fill_colorbars(fbmem, &vinfo, &pinfo);
  ret = dvictrl_update_full(fd, &vinfo);
  munmap(fbmem, pinfo.fblen);
  if (ret < 0)
    {
      return ret;
    }

  printf("stress static: filled once and stopped writing\n");
  return 0;
}

static int dvictrl_stress_full(int fd, uint32_t seconds)
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  FAR uint8_t *fbmem;
  uint64_t end;
  uint32_t frames = 0;
  int ret;

  ret = dvictrl_map_framebuffer(fd, &vinfo, &pinfo, &fbmem);
  if (ret < 0)
    {
      return ret;
    }

  end = dvictrl_time_ms() + (uint64_t)seconds * 1000u;
  while (seconds == 0 || dvictrl_time_ms() < end)
    {
      dvictrl_fill_moving(fbmem, &vinfo, &pinfo, frames);
      dvictrl_update_full(fd, &vinfo);
      frames++;
      if ((frames & 3) == 0)
        {
          usleep(1000);
        }
    }

  munmap(fbmem, pinfo.fblen);
  printf("stress full: wrote %" PRIu32 " frames in %" PRIu32 " seconds\n",
         frames, seconds);
  return 0;
}

static int dvictrl_stress_partial(int fd, uint32_t seconds)
{
  static const uint16_t colors[] =
    {
      0xf800, 0xffe0, 0x07e0, 0x07ff,
      0x001f, 0xf81f, 0xffff, 0x0000
    };
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  FAR uint8_t *fbmem;
  uint64_t end;
  uint32_t frames = 0;
  unsigned int oldx = 0;
  unsigned int oldy = 0;
  unsigned int xspan;
  unsigned int yspan;
  int ret;

  ret = dvictrl_map_framebuffer(fd, &vinfo, &pinfo, &fbmem);
  if (ret < 0)
    {
      return ret;
    }

  dvictrl_fill_solid(fbmem, &vinfo, &pinfo, 0x0000);
  xspan = vinfo.xres > 48 ? vinfo.xres - 48 : 1;
  yspan = vinfo.yres > 32 ? vinfo.yres - 32 : 1;
  end = dvictrl_time_ms() + (uint64_t)seconds * 1000u;
  while (seconds == 0 || dvictrl_time_ms() < end)
    {
      unsigned int x = (frames * 7) % xspan;
      unsigned int y = (frames * 5) % yspan;

      dvictrl_draw_rect(fbmem, &vinfo, &pinfo, oldx, oldy, 48, 32, 0x0000);
      dvictrl_draw_rect(fbmem, &vinfo, &pinfo, x, y, 48, 32,
                        colors[frames % nitems(colors)]);
      dvictrl_update_full(fd, &vinfo);
      oldx = x;
      oldy = y;
      frames++;
      usleep(1000);
    }

  munmap(fbmem, pinfo.fblen);
  printf("stress partial: wrote %" PRIu32 " rectangles in %" PRIu32
         " seconds\n",
         frames, seconds);
  return 0;
}

static int dvictrl_stress(int fd, int argc, FAR char *argv[])
{
  uint32_t seconds;

  if (argc < 3 || argc > 4)
    {
      dvictrl_usage(argv[0], EXIT_FAILURE);
    }

  if (strcmp(argv[2], "static") == 0)
    {
      if (argc != 3)
        {
          dvictrl_usage(argv[0], EXIT_FAILURE);
        }

      return dvictrl_stress_static(fd);
    }

  seconds = dvictrl_stress_seconds(argc, argv);
  if (strcmp(argv[2], "full") == 0)
    {
      return dvictrl_stress_full(fd, seconds);
    }

  if (strcmp(argv[2], "partial") == 0)
    {
      return dvictrl_stress_partial(fd, seconds);
    }

  dvictrl_usage(argv[0], EXIT_FAILURE);
  return -EINVAL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int fd;
  int ret;

  if (argc < 2)
    {
      dvictrl_usage(argv[0], EXIT_FAILURE);
    }

  fd = dvictrl_open();
  if (fd < 0)
    {
      return EXIT_FAILURE;
    }

  if (strcmp(argv[1], "info") == 0)
    {
      ret = dvictrl_print_info(fd);
    }
  else if (strcmp(argv[1], "start") == 0)
    {
      ret = dvictrl_start(fd, argc, argv);
    }
  else if (strcmp(argv[1], "stop") == 0)
    {
      ret = dvictrl_set_power(fd, 0);
    }
  else if (strcmp(argv[1], "pattern") == 0)
    {
      if (argc != 3 || strcmp(argv[2], "colorbars") != 0)
        {
          close(fd);
          dvictrl_usage(argv[0], EXIT_FAILURE);
        }

      ret = dvictrl_pattern_colorbars(fd);
    }
  else if (strcmp(argv[1], "solid") == 0)
    {
      if (argc != 3)
        {
          close(fd);
          dvictrl_usage(argv[0], EXIT_FAILURE);
        }

      ret = dvictrl_solid(fd, argv[2]);
    }
  else if (strcmp(argv[1], "stress") == 0)
    {
      ret = dvictrl_stress(fd, argc, argv);
    }
  else
    {
      close(fd);
      dvictrl_usage(argv[0], EXIT_FAILURE);
    }

  close(fd);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
