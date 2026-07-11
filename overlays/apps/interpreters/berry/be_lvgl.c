/****************************************************************************
 * apps/interpreters/berry/be_lvgl.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include <nuttx/video/fb.h>
#ifdef CONFIG_INTERPRETERS_BERRY_LVGL_MOUSE
#  include <nuttx/input/mouse.h>
#endif

#include <lvgl/lvgl.h>
#include <lvgl/src/drivers/nuttx/lv_nuttx_entry.h>

#include "berry.h"
#include "be_exec.h"
#include "be_object.h"
#include "be_vm.h"

#define BE_LVGL_FB_PATH      "/dev/fb0"
#define BE_LVGL_ROOTS_NAME   "__lv_roots"
#define BE_LVGL_MOUSE_RETRY  1000
#define BE_LVGL_MOUSE_WHEEL  12

struct be_lvgl_style_s
{
  lv_style_t style;
  bool initialized;
  struct be_lvgl_style_s *flink;
};

struct be_lvgl_event_s
{
  bvm *vm;
  bvalue cb;
  bvalue obj;
  struct be_lvgl_event_s *flink;
};

#ifdef CONFIG_INTERPRETERS_BERRY_LVGL_MOUSE
struct be_lvgl_mouse_s
{
  int fd;
  uint8_t buttons;
  int16_t wheel;
  uint32_t last_open;
  bool have_wheel;
  lv_point_t last;
  lv_indev_t *indev;
};
#endif

static lv_nuttx_result_t g_lvgl_result;
static bvm *g_lvgl_owner;
static bool g_lvgl_started;
static bool g_lvgl_external;
static struct be_lvgl_style_s *g_lvgl_styles;
static struct be_lvgl_event_s *g_lvgl_events;

#ifdef CONFIG_INTERPRETERS_BERRY_LVGL_MOUSE
static struct be_lvgl_mouse_s g_lvgl_mouse =
{
  .fd = -1
};
#endif

static int be_lvgl_raise(bvm *vm, const char *msg)
{
  be_raise(vm, "lv_error", msg);
}

static int be_lvgl_return_self(bvm *vm)
{
  be_pushvalue(vm, 1);
  be_return(vm);
}

static void be_lvgl_require_initialized(bvm *vm)
{
  if (!lv_is_initialized())
    {
      be_lvgl_raise(vm, "call lv.start() first");
    }
}

static void be_lvgl_clear_roots(bvm *vm)
{
  be_pushnil(vm);
  be_setglobal(vm, BE_LVGL_ROOTS_NAME);
  be_pop(vm, 1);
}

static void be_lvgl_root_value(bvm *vm, int index)
{
  index = be_absindex(vm, index);

  if (!be_getglobal(vm, BE_LVGL_ROOTS_NAME) || be_isnil(vm, -1))
    {
      be_pop(vm, 1);
      be_newlist(vm);
      be_pushvalue(vm, -1);
      be_setglobal(vm, BE_LVGL_ROOTS_NAME);
      be_pop(vm, 1);
    }

  be_pushvalue(vm, index);
  be_data_push(vm, -2);
  be_pop(vm, 2);
}

static lv_display_t *be_lvgl_display(void)
{
  if (g_lvgl_result.disp != NULL)
    {
      return g_lvgl_result.disp;
    }

  return lv_display_get_default();
}

static int32_t be_lvgl_intarg(bvm *vm, int index, int32_t defval)
{
  if (be_top(vm) >= index && be_isnumber(vm, index))
    {
      return (int32_t)be_toint(vm, index);
    }

  return defval;
}

static lv_color_t be_lvgl_colorarg(bvm *vm, int index)
{
  uint32_t rgb = (uint32_t)be_lvgl_intarg(vm, index, 0);
  return lv_color_hex(rgb & 0x00ffffff);
}

static lv_obj_t *be_lvgl_objarg(bvm *vm, int index, bool screen_default)
{
  lv_obj_t *obj = NULL;

  if (be_top(vm) < index || be_isnil(vm, index))
    {
      return screen_default ? lv_screen_active() : NULL;
    }

  if (be_getmember(vm, index, ".p"))
    {
      if (be_iscomptr(vm, -1))
        {
          obj = (lv_obj_t *)be_tocomptr(vm, -1);
        }
    }

  be_pop(vm, 1);
  return obj;
}

static struct be_lvgl_style_s *be_lvgl_stylearg(bvm *vm, int index)
{
  struct be_lvgl_style_s *style = NULL;

  if (be_top(vm) < index || be_isnil(vm, index))
    {
      return NULL;
    }

  if (be_getmember(vm, index, ".p"))
    {
      if (be_iscomptr(vm, -1))
        {
          style = (struct be_lvgl_style_s *)be_tocomptr(vm, -1);
        }
    }

  be_pop(vm, 1);
  return style;
}

static lv_obj_t *be_lvgl_self_obj(bvm *vm)
{
  lv_obj_t *obj = be_lvgl_objarg(vm, 1, false);

  if (obj == NULL)
    {
      be_lvgl_raise(vm, "invalid LVGL object");
    }

  return obj;
}

static struct be_lvgl_style_s *be_lvgl_self_style(bvm *vm)
{
  struct be_lvgl_style_s *style = be_lvgl_stylearg(vm, 1);

  if (style == NULL || !style->initialized)
    {
      be_lvgl_raise(vm, "invalid LVGL style");
    }

  return style;
}

static int be_lvgl_obj_set_text(bvm *vm);
static int be_lvgl_obj_center(bvm *vm);
static int be_lvgl_obj_align(bvm *vm);
static int be_lvgl_obj_set_pos(bvm *vm);
static int be_lvgl_obj_set_size(bvm *vm);
static int be_lvgl_obj_set_width(bvm *vm);
static int be_lvgl_obj_set_height(bvm *vm);
static int be_lvgl_obj_add_style(bvm *vm);
static int be_lvgl_obj_add_event_cb(bvm *vm);
static int be_lvgl_obj_set_scroll_dir(bvm *vm);
static int be_lvgl_obj_scroll_by(bvm *vm);
static int be_lvgl_obj_remove_style_all(bvm *vm);
static int be_lvgl_obj_add_flag(bvm *vm);
static int be_lvgl_obj_remove_flag(bvm *vm);
static int be_lvgl_obj_set_style_bg_color(bvm *vm);
static int be_lvgl_obj_set_style_bg_opa(bvm *vm);
static int be_lvgl_obj_set_style_text_color(bvm *vm);
static int be_lvgl_obj_set_style_border_color(bvm *vm);
static int be_lvgl_obj_set_style_border_width(bvm *vm);
static int be_lvgl_obj_set_style_radius(bvm *vm);
static int be_lvgl_obj_set_style_pad_all(bvm *vm);
static int be_lvgl_style_set_bg_color(bvm *vm);
static int be_lvgl_style_set_bg_opa(bvm *vm);
static int be_lvgl_style_set_text_color(bvm *vm);
static int be_lvgl_style_set_border_color(bvm *vm);
static int be_lvgl_style_set_border_width(bvm *vm);
static int be_lvgl_style_set_radius(bvm *vm);
static int be_lvgl_style_set_pad_all(bvm *vm);

static void be_lvgl_push_obj(bvm *vm, lv_obj_t *obj)
{
  static const bnfuncinfo members[] =
    {
      { ".p", NULL },
      { "set_text", be_lvgl_obj_set_text },
      { "center", be_lvgl_obj_center },
      { "align", be_lvgl_obj_align },
      { "set_pos", be_lvgl_obj_set_pos },
      { "set_size", be_lvgl_obj_set_size },
      { "set_width", be_lvgl_obj_set_width },
      { "set_height", be_lvgl_obj_set_height },
      { "add_style", be_lvgl_obj_add_style },
      { "add_event_cb", be_lvgl_obj_add_event_cb },
      { "set_scroll_dir", be_lvgl_obj_set_scroll_dir },
      { "scroll_by", be_lvgl_obj_scroll_by },
      { "remove_style_all", be_lvgl_obj_remove_style_all },
      { "add_flag", be_lvgl_obj_add_flag },
      { "remove_flag", be_lvgl_obj_remove_flag },
      { "set_style_bg_color", be_lvgl_obj_set_style_bg_color },
      { "set_style_bg_opa", be_lvgl_obj_set_style_bg_opa },
      { "set_style_text_color", be_lvgl_obj_set_style_text_color },
      { "set_style_border_color", be_lvgl_obj_set_style_border_color },
      { "set_style_border_width", be_lvgl_obj_set_style_border_width },
      { "set_style_radius", be_lvgl_obj_set_style_radius },
      { "set_style_pad_all", be_lvgl_obj_set_style_pad_all },
      { NULL, NULL }
    };

  be_pushclass(vm, "lv_obj", members);
  be_call(vm, 0);
  be_pushcomptr(vm, obj);
  be_setmember(vm, -2, ".p");
  be_pop(vm, 1);
  be_lvgl_root_value(vm, -1);
}

static void be_lvgl_push_style(bvm *vm, struct be_lvgl_style_s *style)
{
  static const bnfuncinfo members[] =
    {
      { ".p", NULL },
      { "set_bg_color", be_lvgl_style_set_bg_color },
      { "set_bg_opa", be_lvgl_style_set_bg_opa },
      { "set_text_color", be_lvgl_style_set_text_color },
      { "set_border_color", be_lvgl_style_set_border_color },
      { "set_border_width", be_lvgl_style_set_border_width },
      { "set_radius", be_lvgl_style_set_radius },
      { "set_pad_all", be_lvgl_style_set_pad_all },
      { NULL, NULL }
    };

  be_pushclass(vm, "lv_style", members);
  be_call(vm, 0);
  be_pushcomptr(vm, style);
  be_setmember(vm, -2, ".p");
  be_pop(vm, 1);
  be_lvgl_root_value(vm, -1);
}

static void be_lvgl_free_events(void)
{
  struct be_lvgl_event_s *event = g_lvgl_events;

  while (event != NULL)
    {
      struct be_lvgl_event_s *next = event->flink;
      free(event);
      event = next;
    }

  g_lvgl_events = NULL;
}

static void be_lvgl_free_styles(bool reset)
{
  struct be_lvgl_style_s *style = g_lvgl_styles;

  while (style != NULL)
    {
      struct be_lvgl_style_s *next = style->flink;
      if (reset && style->initialized)
        {
          lv_style_reset(&style->style);
        }

      free(style);
      style = next;
    }

  g_lvgl_styles = NULL;
}

static void be_lvgl_event_trampoline(lv_event_t *e)
{
  struct be_lvgl_event_s *event;
  int top;
  int ret;

  event = (struct be_lvgl_event_s *)lv_event_get_user_data(e);
  if (event == NULL || event->vm == NULL)
    {
      return;
    }

  top = be_top(event->vm);
  var_setval(event->vm->top, &event->cb);
  be_incrtop(event->vm);
  var_setval(event->vm->top, &event->obj);
  be_incrtop(event->vm);
  be_pushint(event->vm, (bint)lv_event_get_code(e));

  ret = be_pcall(event->vm, 2);
  if (ret != BE_OK)
    {
      be_dumpexcept(event->vm);
    }

  if (be_top(event->vm) > top)
    {
      be_pop(event->vm, be_top(event->vm) - top);
    }
}

#ifdef CONFIG_INTERPRETERS_BERRY_LVGL_MOUSE
static int32_t be_lvgl_clip_axis(int32_t value, int32_t limit)
{
  if (value < 0)
    {
      return 0;
    }

  if (value >= limit)
    {
      return limit - 1;
    }

  return value;
}

static void be_lvgl_mouse_open(struct be_lvgl_mouse_s *mouse)
{
  mouse->last_open = lv_tick_get();
  mouse->fd = open(CONFIG_INTERPRETERS_BERRY_LVGL_MOUSE_DEVPATH,
                   O_RDONLY | O_NONBLOCK);
  if (mouse->fd >= 0)
    {
      mouse->have_wheel = false;
      LV_LOG_INFO("Berry LVGL mouse input attached to %s",
                  CONFIG_INTERPRETERS_BERRY_LVGL_MOUSE_DEVPATH);
    }
}

#ifdef CONFIG_INPUT_MOUSE_WHEEL
static lv_obj_t *be_lvgl_mouse_find_scrollable(lv_obj_t *obj)
{
  while (obj != NULL)
    {
      if ((lv_obj_get_scroll_dir(obj) & LV_DIR_VER) != 0 &&
          (lv_obj_get_scroll_top(obj) > 0 ||
           lv_obj_get_scroll_bottom(obj) > 0))
        {
          return obj;
        }

      obj = lv_obj_get_parent(obj);
    }

  return NULL;
}

static void be_lvgl_mouse_scroll(struct be_lvgl_mouse_s *mouse, int16_t diff)
{
  lv_obj_t *obj;

  if (diff == 0)
    {
      return;
    }

  obj = lv_indev_search_obj(lv_screen_active(), &mouse->last);
  obj = be_lvgl_mouse_find_scrollable(obj);
  if (obj != NULL)
    {
      lv_obj_scroll_by_bounded(obj, 0,
                               -(int32_t)diff * BE_LVGL_MOUSE_WHEEL,
                               LV_ANIM_OFF);
    }
}
#endif

static void be_lvgl_mouse_read(lv_indev_t *indev, lv_indev_data_t *data)
{
  struct be_lvgl_mouse_s *mouse = lv_indev_get_driver_data(indev);
  lv_display_t *disp;
  struct mouse_report_s report;
  ssize_t nread;

  if (mouse->fd < 0 &&
      lv_tick_get() - mouse->last_open >= BE_LVGL_MOUSE_RETRY)
    {
      be_lvgl_mouse_open(mouse);
    }

  while (mouse->fd >= 0)
    {
      nread = read(mouse->fd, &report, sizeof(report));
      if (nread != sizeof(report))
        {
          if (nread < 0 && errno != EAGAIN && errno != EINTR)
            {
              close(mouse->fd);
              mouse->fd = -1;
              mouse->last_open = lv_tick_get();
            }

          break;
        }

      disp = lv_indev_get_display(indev);
      mouse->last.x =
        be_lvgl_clip_axis(report.x,
                          lv_display_get_horizontal_resolution(disp));
      mouse->last.y =
        be_lvgl_clip_axis(report.y,
                          lv_display_get_vertical_resolution(disp));
      mouse->buttons = report.buttons;

#ifdef CONFIG_INPUT_MOUSE_WHEEL
      if (mouse->have_wheel)
        {
          be_lvgl_mouse_scroll(mouse, report.wheel - mouse->wheel);
        }

      mouse->wheel = report.wheel;
      mouse->have_wheel = true;
#endif
    }

  data->point = mouse->last;
  data->state = (mouse->buttons & MOUSE_BUTTON_1) != 0 ?
                LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void be_lvgl_mouse_init(lv_display_t *disp)
{
  struct be_lvgl_mouse_s *mouse = &g_lvgl_mouse;
  lv_obj_t *cursor;

  mouse->last.x = lv_display_get_horizontal_resolution(disp) / 2;
  mouse->last.y = lv_display_get_vertical_resolution(disp) / 2;

  mouse->indev = lv_indev_create();
  if (mouse->indev == NULL)
    {
      LV_LOG_WARN("Failed to create Berry LVGL mouse input");
      return;
    }

  lv_indev_set_type(mouse->indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(mouse->indev, be_lvgl_mouse_read);
  lv_indev_set_driver_data(mouse->indev, mouse);
  lv_indev_set_display(mouse->indev, disp);

  cursor = lv_obj_create(lv_display_get_layer_sys(disp));
  lv_obj_remove_style_all(cursor);
  lv_obj_remove_flag(cursor, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(cursor, 7, 7);
  lv_obj_set_style_radius(cursor, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(cursor, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(cursor, lv_color_black(), 0);
  lv_obj_set_style_border_width(cursor, 1, 0);
  lv_obj_set_style_border_color(cursor, lv_color_white(), 0);
  lv_indev_set_cursor(mouse->indev, cursor);

  be_lvgl_mouse_open(mouse);
}

static void be_lvgl_mouse_deinit(void)
{
  struct be_lvgl_mouse_s *mouse = &g_lvgl_mouse;

  if (mouse->fd >= 0)
    {
      close(mouse->fd);
      mouse->fd = -1;
    }

  if (mouse->indev != NULL)
    {
      lv_indev_delete(mouse->indev);
      mouse->indev = NULL;
    }
}
#else
static void be_lvgl_mouse_init(lv_display_t *disp)
{
  (void)disp;
}

static void be_lvgl_mouse_deinit(void)
{
}
#endif

static void be_lvgl_warn_power(void)
{
  int fd;
  int power = 0;

  fd = open(BE_LVGL_FB_PATH, O_RDWR);
  if (fd < 0)
    {
      return;
    }

  if (ioctl(fd, FBIOGET_POWER, (unsigned long)((uintptr_t)&power)) >= 0 &&
      power == 0)
    {
      printf("lv: framebuffer is powered off; run 'dvictrl start' for DVI\n");
    }

  close(fd);
}

static void be_lvgl_stop_internal(bvm *vm)
{
  if (g_lvgl_owner != vm)
    {
      return;
    }

  be_lvgl_mouse_deinit();
  be_lvgl_free_events();

  if (g_lvgl_started && !g_lvgl_external)
    {
      be_lvgl_free_styles(true);
      lv_nuttx_deinit(&g_lvgl_result);
      lv_deinit();
    }
  else
    {
      be_lvgl_free_styles(false);
    }

  memset(&g_lvgl_result, 0, sizeof(g_lvgl_result));
  g_lvgl_started = false;
  g_lvgl_external = false;
  g_lvgl_owner = NULL;
  be_lvgl_clear_roots(vm);
}

static int be_lvgl_start(bvm *vm)
{
  lv_nuttx_dsc_t info;

  if (g_lvgl_started)
    {
      if (g_lvgl_owner == vm)
        {
          be_pushbool(vm, true);
          be_return(vm);
        }

      be_lvgl_raise(vm, "LVGL is already owned by another Berry VM");
    }

  if (lv_is_initialized())
    {
      g_lvgl_result.disp = lv_display_get_default();
      if (g_lvgl_result.disp == NULL)
        {
          be_lvgl_raise(vm, "LVGL is initialized but has no display");
        }

      g_lvgl_owner = vm;
      g_lvgl_started = true;
      g_lvgl_external = true;
      be_pushbool(vm, true);
      be_return(vm);
    }

  be_lvgl_warn_power();

  lv_init();
  lv_nuttx_dsc_init(&info);
  info.fb_path = BE_LVGL_FB_PATH;
  info.input_path = NULL;
  info.utouch_path = NULL;

  memset(&g_lvgl_result, 0, sizeof(g_lvgl_result));
  lv_nuttx_init(&info, &g_lvgl_result);
  if (g_lvgl_result.disp == NULL)
    {
      lv_deinit();
      be_lvgl_raise(vm, "lv_nuttx_init failed for /dev/fb0");
    }

  g_lvgl_owner = vm;
  g_lvgl_started = true;
  g_lvgl_external = false;
  be_lvgl_mouse_init(g_lvgl_result.disp);

  be_pushbool(vm, true);
  be_return(vm);
}

static int be_lvgl_stop(bvm *vm)
{
  be_lvgl_stop_internal(vm);
  be_return_nil(vm);
}

static int be_lvgl_task_handler(bvm *vm)
{
  uint32_t idle;

  be_lvgl_require_initialized(vm);
  idle = lv_timer_handler();
  be_pushint(vm, (bint)idle);
  be_return(vm);
}

static int be_lvgl_run(bvm *vm)
{
  int32_t duration = be_lvgl_intarg(vm, 1, -1);
  uint32_t start = lv_tick_get();

  be_lvgl_require_initialized(vm);

  do
    {
      uint32_t idle = lv_timer_handler();

      if (idle == 0)
        {
          idle = 1;
        }

      usleep(idle * 1000);
    }
  while (duration < 0 || (int32_t)(lv_tick_get() - start) < duration);

  be_return_nil(vm);
}

static int be_lvgl_millis(bvm *vm)
{
  struct timeval tv;
  uint64_t ms;

  gettimeofday(&tv, NULL);
  ms = (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
  be_pushint(vm, (bint)(ms & 0x7fffffff));
  be_return(vm);
}

static int be_lvgl_sleep_ms(bvm *vm)
{
  int32_t ms = be_lvgl_intarg(vm, 1, 0);

  if (ms > 0)
    {
      usleep((useconds_t)ms * 1000);
    }

  be_return_nil(vm);
}

static int be_lvgl_get_hor_res(bvm *vm)
{
  lv_display_t *disp;

  be_lvgl_require_initialized(vm);
  disp = be_lvgl_display();
  be_pushint(vm, disp == NULL ? 0 : lv_display_get_horizontal_resolution(disp));
  be_return(vm);
}

static int be_lvgl_get_ver_res(bvm *vm)
{
  lv_display_t *disp;

  be_lvgl_require_initialized(vm);
  disp = be_lvgl_display();
  be_pushint(vm, disp == NULL ? 0 : lv_display_get_vertical_resolution(disp));
  be_return(vm);
}

static int be_lvgl_color(bvm *vm)
{
  be_pushint(vm, be_lvgl_intarg(vm, 1, 0) & 0x00ffffff);
  be_return(vm);
}

static int be_lvgl_scr_act(bvm *vm)
{
  lv_obj_t *scr;

  be_lvgl_require_initialized(vm);
  scr = lv_screen_active();
  if (scr == NULL)
    {
      be_lvgl_raise(vm, "LVGL has no active screen");
    }

  be_lvgl_push_obj(vm, scr);
  be_return(vm);
}

static int be_lvgl_label(bvm *vm)
{
  lv_obj_t *parent;
  lv_obj_t *label;

  be_lvgl_require_initialized(vm);
  parent = be_lvgl_objarg(vm, 1, true);
  label = lv_label_create(parent);
  be_lvgl_push_obj(vm, label);
  be_return(vm);
}

static int be_lvgl_btn(bvm *vm)
{
  lv_obj_t *parent;
  lv_obj_t *btn;

  be_lvgl_require_initialized(vm);
  parent = be_lvgl_objarg(vm, 1, true);
  btn = lv_button_create(parent);
  be_lvgl_push_obj(vm, btn);
  be_return(vm);
}

static int be_lvgl_style(bvm *vm)
{
  struct be_lvgl_style_s *style;

  style = calloc(1, sizeof(*style));
  if (style == NULL)
    {
      be_lvgl_raise(vm, "out of memory allocating LVGL style");
    }

  lv_style_init(&style->style);
  style->initialized = true;
  style->flink = g_lvgl_styles;
  g_lvgl_styles = style;

  be_lvgl_push_style(vm, style);
  be_return(vm);
}

static int be_lvgl_obj_set_text(bvm *vm)
{
  lv_obj_t *obj = be_lvgl_self_obj(vm);

  if (be_top(vm) >= 2 && be_isstring(vm, 2))
    {
      lv_label_set_text(obj, be_tostring(vm, 2));
    }

  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_center(bvm *vm)
{
  lv_obj_center(be_lvgl_self_obj(vm));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_align(bvm *vm)
{
  lv_obj_align(be_lvgl_self_obj(vm),
               (lv_align_t)be_lvgl_intarg(vm, 2, LV_ALIGN_CENTER),
               be_lvgl_intarg(vm, 3, 0),
               be_lvgl_intarg(vm, 4, 0));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_set_pos(bvm *vm)
{
  lv_obj_set_pos(be_lvgl_self_obj(vm),
                 be_lvgl_intarg(vm, 2, 0),
                 be_lvgl_intarg(vm, 3, 0));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_set_size(bvm *vm)
{
  lv_obj_set_size(be_lvgl_self_obj(vm),
                  be_lvgl_intarg(vm, 2, 0),
                  be_lvgl_intarg(vm, 3, 0));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_set_width(bvm *vm)
{
  lv_obj_set_width(be_lvgl_self_obj(vm), be_lvgl_intarg(vm, 2, 0));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_set_height(bvm *vm)
{
  lv_obj_set_height(be_lvgl_self_obj(vm), be_lvgl_intarg(vm, 2, 0));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_add_style(bvm *vm)
{
  struct be_lvgl_style_s *style = be_lvgl_stylearg(vm, 2);

  if (style != NULL && style->initialized)
    {
      lv_obj_add_style(be_lvgl_self_obj(vm), &style->style,
                       (lv_style_selector_t)be_lvgl_intarg(vm, 3, 0));
      be_lvgl_root_value(vm, 2);
    }

  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_add_event_cb(bvm *vm)
{
  lv_obj_t *obj = be_lvgl_self_obj(vm);
  struct be_lvgl_event_s *event;
  lv_event_code_t code;

  if (be_top(vm) < 2 || !be_isfunction(vm, 2))
    {
      be_lvgl_raise(vm, "add_event_cb expects a Berry function");
    }

  event = calloc(1, sizeof(*event));
  if (event == NULL)
    {
      be_lvgl_raise(vm, "out of memory allocating LVGL event");
    }

  event->vm = vm;
  var_setval(&event->cb, be_indexof(vm, 2));
  var_setval(&event->obj, be_indexof(vm, 1));
  event->flink = g_lvgl_events;
  g_lvgl_events = event;

  code = (lv_event_code_t)be_lvgl_intarg(vm, 3, LV_EVENT_ALL);
  lv_obj_add_event_cb(obj, be_lvgl_event_trampoline, code, event);

  be_lvgl_root_value(vm, 1);
  be_lvgl_root_value(vm, 2);
  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_set_scroll_dir(bvm *vm)
{
  lv_obj_set_scroll_dir(be_lvgl_self_obj(vm),
                        (lv_dir_t)be_lvgl_intarg(vm, 2, LV_DIR_ALL));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_scroll_by(bvm *vm)
{
  lv_obj_scroll_by_bounded(be_lvgl_self_obj(vm),
                           be_lvgl_intarg(vm, 2, 0),
                           be_lvgl_intarg(vm, 3, 0),
                           LV_ANIM_OFF);
  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_remove_style_all(bvm *vm)
{
  lv_obj_remove_style_all(be_lvgl_self_obj(vm));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_add_flag(bvm *vm)
{
  lv_obj_add_flag(be_lvgl_self_obj(vm),
                  (lv_obj_flag_t)be_lvgl_intarg(vm, 2, 0));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_remove_flag(bvm *vm)
{
  lv_obj_remove_flag(be_lvgl_self_obj(vm),
                     (lv_obj_flag_t)be_lvgl_intarg(vm, 2, 0));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_set_style_bg_color(bvm *vm)
{
  lv_obj_set_style_bg_color(be_lvgl_self_obj(vm), be_lvgl_colorarg(vm, 2),
                            (lv_style_selector_t)be_lvgl_intarg(vm, 3, 0));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_set_style_bg_opa(bvm *vm)
{
  lv_obj_set_style_bg_opa(be_lvgl_self_obj(vm),
                          (lv_opa_t)be_lvgl_intarg(vm, 2, LV_OPA_COVER),
                          (lv_style_selector_t)be_lvgl_intarg(vm, 3, 0));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_set_style_text_color(bvm *vm)
{
  lv_obj_set_style_text_color(be_lvgl_self_obj(vm), be_lvgl_colorarg(vm, 2),
                              (lv_style_selector_t)be_lvgl_intarg(vm, 3, 0));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_set_style_border_color(bvm *vm)
{
  lv_obj_set_style_border_color(be_lvgl_self_obj(vm), be_lvgl_colorarg(vm, 2),
                                (lv_style_selector_t)be_lvgl_intarg(vm, 3, 0));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_set_style_border_width(bvm *vm)
{
  lv_obj_set_style_border_width(be_lvgl_self_obj(vm),
                                be_lvgl_intarg(vm, 2, 0),
                                (lv_style_selector_t)be_lvgl_intarg(vm, 3, 0));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_set_style_radius(bvm *vm)
{
  lv_obj_set_style_radius(be_lvgl_self_obj(vm), be_lvgl_intarg(vm, 2, 0),
                          (lv_style_selector_t)be_lvgl_intarg(vm, 3, 0));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_obj_set_style_pad_all(bvm *vm)
{
  lv_obj_set_style_pad_all(be_lvgl_self_obj(vm), be_lvgl_intarg(vm, 2, 0),
                           (lv_style_selector_t)be_lvgl_intarg(vm, 3, 0));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_style_set_bg_color(bvm *vm)
{
  lv_style_set_bg_color(&be_lvgl_self_style(vm)->style,
                        be_lvgl_colorarg(vm, 2));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_style_set_bg_opa(bvm *vm)
{
  lv_style_set_bg_opa(&be_lvgl_self_style(vm)->style,
                      (lv_opa_t)be_lvgl_intarg(vm, 2, LV_OPA_COVER));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_style_set_text_color(bvm *vm)
{
  lv_style_set_text_color(&be_lvgl_self_style(vm)->style,
                          be_lvgl_colorarg(vm, 2));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_style_set_border_color(bvm *vm)
{
  lv_style_set_border_color(&be_lvgl_self_style(vm)->style,
                            be_lvgl_colorarg(vm, 2));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_style_set_border_width(bvm *vm)
{
  lv_style_set_border_width(&be_lvgl_self_style(vm)->style,
                            be_lvgl_intarg(vm, 2, 0));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_style_set_radius(bvm *vm)
{
  lv_style_set_radius(&be_lvgl_self_style(vm)->style,
                      be_lvgl_intarg(vm, 2, 0));
  return be_lvgl_return_self(vm);
}

static int be_lvgl_style_set_pad_all(bvm *vm)
{
  lv_style_set_pad_all(&be_lvgl_self_style(vm)->style,
                       be_lvgl_intarg(vm, 2, 0));
  return be_lvgl_return_self(vm);
}

void be_lvgl_cleanup(bvm *vm)
{
  be_lvgl_stop_internal(vm);
}

static const bntvmodobj_t lv_attrs[] =
{
  { "start", BE_CFUNCTION, { .f = be_lvgl_start } },
  { "stop", BE_CFUNCTION, { .f = be_lvgl_stop } },
  { "task_handler", BE_CFUNCTION, { .f = be_lvgl_task_handler } },
  { "run", BE_CFUNCTION, { .f = be_lvgl_run } },
  { "millis", BE_CFUNCTION, { .f = be_lvgl_millis } },
  { "sleep_ms", BE_CFUNCTION, { .f = be_lvgl_sleep_ms } },
  { "get_hor_res", BE_CFUNCTION, { .f = be_lvgl_get_hor_res } },
  { "get_ver_res", BE_CFUNCTION, { .f = be_lvgl_get_ver_res } },
  { "color", BE_CFUNCTION, { .f = be_lvgl_color } },
  { "color_hex", BE_CFUNCTION, { .f = be_lvgl_color } },
  { "scr_act", BE_CFUNCTION, { .f = be_lvgl_scr_act } },
  { "screen_active", BE_CFUNCTION, { .f = be_lvgl_scr_act } },
  { "label", BE_CFUNCTION, { .f = be_lvgl_label } },
  { "btn", BE_CFUNCTION, { .f = be_lvgl_btn } },
  { "button", BE_CFUNCTION, { .f = be_lvgl_btn } },
  { "style", BE_CFUNCTION, { .f = be_lvgl_style } },
  { "ALIGN_CENTER", BE_CINT, { .i = LV_ALIGN_CENTER } },
  { "ALIGN_TOP_LEFT", BE_CINT, { .i = LV_ALIGN_TOP_LEFT } },
  { "ALIGN_TOP_MID", BE_CINT, { .i = LV_ALIGN_TOP_MID } },
  { "ALIGN_TOP_RIGHT", BE_CINT, { .i = LV_ALIGN_TOP_RIGHT } },
  { "ALIGN_BOTTOM_LEFT", BE_CINT, { .i = LV_ALIGN_BOTTOM_LEFT } },
  { "ALIGN_BOTTOM_MID", BE_CINT, { .i = LV_ALIGN_BOTTOM_MID } },
  { "ALIGN_BOTTOM_RIGHT", BE_CINT, { .i = LV_ALIGN_BOTTOM_RIGHT } },
  { "ALIGN_LEFT_MID", BE_CINT, { .i = LV_ALIGN_LEFT_MID } },
  { "ALIGN_RIGHT_MID", BE_CINT, { .i = LV_ALIGN_RIGHT_MID } },
  { "PART_MAIN", BE_CINT, { .i = LV_PART_MAIN } },
  { "PART_SCROLLBAR", BE_CINT, { .i = LV_PART_SCROLLBAR } },
  { "PART_INDICATOR", BE_CINT, { .i = LV_PART_INDICATOR } },
  { "PART_KNOB", BE_CINT, { .i = LV_PART_KNOB } },
  { "PART_ANY", BE_CINT, { .i = LV_PART_ANY } },
  { "STATE_DEFAULT", BE_CINT, { .i = LV_STATE_DEFAULT } },
  { "STATE_PRESSED", BE_CINT, { .i = LV_STATE_PRESSED } },
  { "STATE_FOCUSED", BE_CINT, { .i = LV_STATE_FOCUSED } },
  { "STATE_CHECKED", BE_CINT, { .i = LV_STATE_CHECKED } },
  { "STATE_ANY", BE_CINT, { .i = LV_STATE_ANY } },
  { "EVENT_ALL", BE_CINT, { .i = LV_EVENT_ALL } },
  { "EVENT_PRESSED", BE_CINT, { .i = LV_EVENT_PRESSED } },
  { "EVENT_CLICKED", BE_CINT, { .i = LV_EVENT_CLICKED } },
  { "EVENT_RELEASED", BE_CINT, { .i = LV_EVENT_RELEASED } },
  { "EVENT_VALUE_CHANGED", BE_CINT, { .i = LV_EVENT_VALUE_CHANGED } },
  { "EVENT_READY", BE_CINT, { .i = LV_EVENT_READY } },
  { "EVENT_CANCEL", BE_CINT, { .i = LV_EVENT_CANCEL } },
  { "OPA_TRANSP", BE_CINT, { .i = LV_OPA_TRANSP } },
  { "OPA_50", BE_CINT, { .i = LV_OPA_50 } },
  { "OPA_COVER", BE_CINT, { .i = LV_OPA_COVER } },
  { "DIR_NONE", BE_CINT, { .i = LV_DIR_NONE } },
  { "DIR_LEFT", BE_CINT, { .i = LV_DIR_LEFT } },
  { "DIR_RIGHT", BE_CINT, { .i = LV_DIR_RIGHT } },
  { "DIR_TOP", BE_CINT, { .i = LV_DIR_TOP } },
  { "DIR_BOTTOM", BE_CINT, { .i = LV_DIR_BOTTOM } },
  { "DIR_HOR", BE_CINT, { .i = LV_DIR_HOR } },
  { "DIR_VER", BE_CINT, { .i = LV_DIR_VER } },
  { "DIR_ALL", BE_CINT, { .i = LV_DIR_ALL } },
};

const bntvmodule_t be_native_module(lv) =
{
  "lv",
  lv_attrs,
  sizeof(lv_attrs) / sizeof(lv_attrs[0]),
  NULL
};
