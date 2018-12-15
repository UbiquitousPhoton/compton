// SPDX-License-Identifier: MIT
/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * See LICENSE-mit for more information.
 *
 */

#include <ctype.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xlibint.h>
#include <xcb/randr.h>
#include <xcb/present.h>
#include <xcb/damage.h>
#include <xcb/render.h>
#include <xcb/xcb_image.h>

#include <ev.h>

#include "compton.h"
#ifdef CONFIG_OPENGL
#include "opengl.h"
#endif
#include "win.h"
#include "x.h"
#include "config.h"
#include "diagnostic.h"
#include "string_utils.h"
#include "log.h"

#define auto __auto_type

/// Get session_t pointer from a pointer to a member of session_t
#define session_ptr(ptr, member) ({ \
  const __typeof__( ((session_t *)0)->member ) *__mptr = (ptr); \
  (session_t *)((char *)__mptr - offsetof(session_t, member)); \
})

static void
finish_destroy_win(session_t *ps, win **_w);

static void
configure_win(session_t *ps, xcb_configure_notify_event_t *ce);

static void
get_cfg(session_t *ps, int argc, char *const *argv, bool first_pass);

static void
update_refresh_rate(session_t *ps);

static bool
swopti_init(session_t *ps);

static void
cxinerama_upd_scrs(session_t *ps);

static void
session_destroy(session_t *ps);

#ifdef CONFIG_XINERAMA
static void
cxinerama_upd_scrs(session_t *ps);
#endif

static bool
vsync_drm_init(session_t *ps);

#ifdef CONFIG_VSYNC_DRM
static int
vsync_drm_wait(session_t *ps);
#endif

static bool
vsync_opengl_init(session_t *ps);

static bool
vsync_opengl_oml_init(session_t *ps);

static bool
vsync_opengl_swc_init(session_t *ps);

static bool
vsync_opengl_mswc_init(session_t *ps);

#ifdef CONFIG_OPENGL
static int
vsync_opengl_wait(session_t *ps);

static int
vsync_opengl_oml_wait(session_t *ps);

static void
vsync_opengl_swc_deinit(session_t *ps);
#endif

static void
vsync_wait(session_t *ps);

static void
redir_start(session_t *ps);

static void
redir_stop(session_t *ps);

static win *
recheck_focus(session_t *ps);

static bool
get_root_tile(session_t *ps);

static double
get_opacity_percent(win *w);

static void
restack_win(session_t *ps, win *w, Window new_above);

static void
update_ewmh_active_win(session_t *ps);

static void
draw_callback(EV_P_ ev_idle *w, int revents);

static void
render(session_t *ps, int x, int y, int dx, int dy, int wid, int hei,
    double opacity, bool argb, bool neg,
    xcb_render_picture_t pict, glx_texture_t *ptex,
    const region_t *reg_paint, const glx_prog_main_t *pprogram);

// === Global constants ===

/// Name strings for window types.
const char * const WINTYPES[NUM_WINTYPES] = {
  "unknown",
  "desktop",
  "dock",
  "toolbar",
  "menu",
  "utility",
  "splash",
  "dialog",
  "normal",
  "dropdown_menu",
  "popup_menu",
  "tooltip",
  "notify",
  "combo",
  "dnd",
};

/// Names of VSync modes.
const char * const VSYNC_STRS[NUM_VSYNC + 1] = {
  "none",             // VSYNC_NONE
  "drm",              // VSYNC_DRM
  "opengl",           // VSYNC_OPENGL
  "opengl-oml",       // VSYNC_OPENGL_OML
  "opengl-swc",       // VSYNC_OPENGL_SWC
  "opengl-mswc",      // VSYNC_OPENGL_MSWC
  NULL
};

/// Names of backends.
const char * const BACKEND_STRS[NUM_BKEND + 1] = {
  "xrender",      // BKEND_XRENDER
  "glx",          // BKEND_GLX
  "xr_glx_hybrid",// BKEND_XR_GLX_HYBRID
  NULL
};

/// Function pointers to init VSync modes.
static bool (* const (VSYNC_FUNCS_INIT[NUM_VSYNC]))(session_t *ps) = {
  [VSYNC_DRM          ] = vsync_drm_init,
  [VSYNC_OPENGL       ] = vsync_opengl_init,
  [VSYNC_OPENGL_OML   ] = vsync_opengl_oml_init,
  [VSYNC_OPENGL_SWC   ] = vsync_opengl_swc_init,
  [VSYNC_OPENGL_MSWC  ] = vsync_opengl_mswc_init,
};

/// Function pointers to wait for VSync.
static int (* const (VSYNC_FUNCS_WAIT[NUM_VSYNC]))(session_t *ps) = {
#ifdef CONFIG_VSYNC_DRM
  [VSYNC_DRM        ] = vsync_drm_wait,
#endif
#ifdef CONFIG_OPENGL
  [VSYNC_OPENGL     ] = vsync_opengl_wait,
  [VSYNC_OPENGL_OML ] = vsync_opengl_oml_wait,
#endif
};

/// Function pointers to deinitialize VSync.
static void (* const (VSYNC_FUNCS_DEINIT[NUM_VSYNC]))(session_t *ps) = {
#ifdef CONFIG_OPENGL
  [VSYNC_OPENGL_SWC   ] = vsync_opengl_swc_deinit,
  [VSYNC_OPENGL_MSWC  ] = vsync_opengl_swc_deinit,
#endif
};

/// Names of root window properties that could point to a pixmap of
/// background.
static const char *background_props_str[] = {
  "_XROOTPMAP_ID",
  "_XSETROOT_ID",
  0,
};

// === Global variables ===

/// Pointer to current session, as a global variable. Only used by
/// xerror(), which could not have a pointer to current session passed in.
/// XXX Limit what xerror can access by not having this pointer
session_t *ps_g = NULL;

/**
 * Free Xinerama screen info.
 *
 * XXX consider moving to x.c
 */
static inline void
free_xinerama_info(session_t *ps) {
#ifdef CONFIG_XINERAMA
  if (ps->xinerama_scr_regs) {
    for (int i = 0; i < ps->xinerama_nscrs; ++i)
      pixman_region32_fini(&ps->xinerama_scr_regs[i]);
    free(ps->xinerama_scr_regs);
  }
  cxfree(ps->xinerama_scrs);
  ps->xinerama_scrs = NULL;
  ps->xinerama_nscrs = 0;
#endif
}

/**
 * Destroy all resources in a <code>struct _win</code>.
 */
static inline void
free_win_res(session_t *ps, win *w) {
  free_win_res_glx(ps, w);
  free_paint(ps, &w->paint);
  free_fence(ps, &w->fence);
  pixman_region32_fini(&w->bounding_shape);
  free_paint(ps, &w->shadow_paint);
  // BadDamage may be thrown if the window is destroyed
  set_ignore_cookie(ps,
      xcb_damage_destroy(ps->c, w->damage));
  rc_region_unref(&w->reg_ignore);
  free(w->name);
  free(w->class_instance);
  free(w->class_general);
  free(w->role);
}

/**
 * Free root tile related things.
 */
static inline void
free_root_tile(session_t *ps) {
  free_picture(ps, &ps->root_tile_paint.pict);
  free_texture(ps, &ps->root_tile_paint.ptex);
  if (ps->root_tile_fill) {
    xcb_free_pixmap(ps->c, ps->root_tile_paint.pixmap);
    ps->root_tile_paint.pixmap = XCB_NONE;
  }
  ps->root_tile_paint.pixmap = None;
  ps->root_tile_fill = false;
}

/**
 * Get current system clock in milliseconds.
 */
static inline time_ms_t
get_time_ms(void) {
  struct timeval tv;

  gettimeofday(&tv, NULL);

  return tv.tv_sec % SEC_WRAP * 1000 + tv.tv_usec / 1000;
}

/**
 * Resize a region.
 */
static inline void
resize_region(session_t *ps, region_t *region, short mod) {
  if (!mod || !region) return;
  // Loop through all rectangles
  int nrects;
  int nnewrects = 0;
  pixman_box32_t *rects = pixman_region32_rectangles(region, &nrects);
  pixman_box32_t *newrects = calloc(nrects, sizeof *newrects);
  for (int i = 0; i < nrects; i++) {
    int x1 = max_i(rects[i].x1 - mod, 0);
    int y1 = max_i(rects[i].y1 - mod, 0);
    int x2 = min_i(rects[i].x2 + mod, ps->root_width);
    int y2 = min_i(rects[i].y2 + mod, ps->root_height);
    int wid = x2 - x1;
    int hei = y2 - y1;
    if (wid <= 0 || hei <= 0)
      continue;
    newrects[nnewrects] = (pixman_box32_t) {
      .x1 = x1, .x2 = x2, .y1 = y1, .y2 = y2
    };
    ++nnewrects;
  }

  pixman_region32_fini(region);
  pixman_region32_init_rects(region, newrects, nnewrects);

  free(newrects);
}

static inline void
__attribute__((nonnull(1, 2)))
set_tgt_clip(session_t *ps, region_t *reg) {
  switch (ps->o.backend) {
    case BKEND_XRENDER:
    case BKEND_XR_GLX_HYBRID:
      x_set_picture_clip_region(ps, ps->tgt_buffer.pict, 0, 0, reg);
      break;
#ifdef CONFIG_OPENGL
    case BKEND_GLX:
      glx_set_clip(ps, reg);
      break;
#endif
    default:
      assert(false);
  }
}

/**
 * Get the Xinerama screen a window is on.
 *
 * Return an index >= 0, or -1 if not found.
 *
 * XXX move to x.c
 */
static inline void
cxinerama_win_upd_scr(session_t *ps, win *w) {
#ifdef CONFIG_XINERAMA
  w->xinerama_scr = -1;

  if (!ps->xinerama_scrs)
    return;

  xcb_xinerama_screen_info_t *scrs = xcb_xinerama_query_screens_screen_info(ps->xinerama_scrs);
  int length = xcb_xinerama_query_screens_screen_info_length(ps->xinerama_scrs);
  for (int i = 0; i < length; i++) {
    xcb_xinerama_screen_info_t *s = &scrs[i];
    if (s->x_org <= w->g.x && s->y_org <= w->g.y
        && s->x_org + s->width >= w->g.x + w->widthb
        && s->y_org + s->height >= w->g.y + w->heightb) {
      w->xinerama_scr = i;
      return;
    }
  }
#endif
}

// XXX Move to x.c
static void
cxinerama_upd_scrs(session_t *ps) {
#ifdef CONFIG_XINERAMA
  // XXX Consider deprecating Xinerama, switch to RandR when necessary
  free_xinerama_info(ps);

  if (!ps->o.xinerama_shadow_crop || !ps->xinerama_exists) return;

  xcb_xinerama_is_active_reply_t *active =
    xcb_xinerama_is_active_reply(ps->c,
        xcb_xinerama_is_active(ps->c), NULL);
  if (!active || !active->state) {
    free(active);
    return;
  }
  free(active);

  ps->xinerama_scrs = xcb_xinerama_query_screens_reply(ps->c,
      xcb_xinerama_query_screens(ps->c), NULL);
  if (!ps->xinerama_scrs)
    return;

  xcb_xinerama_screen_info_t *scrs = xcb_xinerama_query_screens_screen_info(ps->xinerama_scrs);
  ps->xinerama_nscrs = xcb_xinerama_query_screens_screen_info_length(ps->xinerama_scrs);

  ps->xinerama_scr_regs = allocchk(malloc(sizeof(region_t)
        * ps->xinerama_nscrs));
  for (int i = 0; i < ps->xinerama_nscrs; ++i) {
    const xcb_xinerama_screen_info_t * const s = &scrs[i];
    pixman_region32_init_rect(&ps->xinerama_scr_regs[i], s->x_org, s->y_org, s->width, s->height);
  }
#endif
}

/**
 * Find matched window.
 *
 * XXX move to win.c
 */
static inline win *
find_win_all(session_t *ps, const Window wid) {
  if (!wid || PointerRoot == wid || wid == ps->root || wid == ps->overlay)
    return NULL;

  win *w = find_win(ps, wid);
  if (!w) w = find_toplevel(ps, wid);
  if (!w) w = find_toplevel2(ps, wid);
  return w;
}

void queue_redraw(session_t *ps) {
  // If --benchmark is used, redraw is always queued
  if (!ps->redraw_needed && !ps->o.benchmark)
    ev_idle_start(ps->loop, &ps->draw_idle);
  ps->redraw_needed = true;
}

static inline void
paint_region(session_t *ps, win *w, int x, int y, int wid, int hei,
    double opacity, const region_t *reg_paint, xcb_render_picture_t pict) {
  const int dx = (w ? w->g.x: 0) + x;
  const int dy = (w ? w->g.y: 0) + y;
  const bool argb = (w && (win_has_alpha(w) || ps->o.force_win_blend));
  const bool neg = (w && w->invert_color);

  render(ps, x, y, dx, dy, wid, hei, opacity, argb, neg,
      pict, (w ? w->paint.ptex: ps->root_tile_paint.ptex),
      reg_paint, (w ? &ps->o.glx_prog_win: NULL));
}

/**
 * Get a region of the screen size.
 */
static inline void
get_screen_region(session_t *ps, region_t *res) {
  pixman_box32_t b = {
    .x1 = 0, .y1 = 0,
    .x2 = ps->root_width,
    .y2 = ps->root_height
  };
  pixman_region32_fini(res);
  pixman_region32_init_rects(res, &b, 1);
}

/**
 * Check if current backend uses XRender for rendering.
 */
static inline bool
bkend_use_xrender(session_t *ps) {
  return BKEND_XRENDER == ps->o.backend
    || BKEND_XR_GLX_HYBRID == ps->o.backend;
}

/**
 * Check whether a paint_t contains enough data.
 */
static inline bool
paint_isvalid(session_t *ps, const paint_t *ppaint) {
  // Don't check for presence of Pixmap here, because older X Composite doesn't
  // provide it
  if (!ppaint)
    return false;

  if (bkend_use_xrender(ps) && !ppaint->pict)
    return false;

#ifdef CONFIG_OPENGL
  if (BKEND_GLX == ps->o.backend && !glx_tex_binded(ppaint->ptex, None))
    return false;
#endif

  return true;
}

void add_damage(session_t *ps, const region_t *damage) {
  // Ignore damage when screen isn't redirected
  if (!ps->redirected)
    return;

  if (!damage)
    return;
  pixman_region32_union(&ps->all_damage, &ps->all_damage, (region_t *)damage);
}

// === Fading ===

/**
 * Get the time left before next fading point.
 *
 * In milliseconds.
 */
static double
fade_timeout(session_t *ps) {
  int diff = ps->o.fade_delta - get_time_ms() + ps->fade_time;

  diff = normalize_i_range(diff, 0, ps->o.fade_delta * 2);

  return diff / 1000.0;
}

/**
 * Run fading on a window.
 *
 * @param steps steps of fading
 */
static void
run_fade(session_t *ps, win *w, unsigned steps) {
  // If we have reached target opacity, return
  if (w->opacity == w->opacity_tgt) {
    return;
  }

  if (!w->fade)
    w->opacity = w->opacity_tgt;
  else if (steps) {
    // Use double below because opacity_t will probably overflow during
    // calculations
    if (w->opacity < w->opacity_tgt)
      w->opacity = normalize_d_range(
          (double) w->opacity + (double) ps->o.fade_in_step * steps,
          0.0, w->opacity_tgt);
    else
      w->opacity = normalize_d_range(
          (double) w->opacity - (double) ps->o.fade_out_step * steps,
          w->opacity_tgt, OPAQUE);
  }

  if (w->opacity != w->opacity_tgt) {
    ps->fade_running = true;
  }
}

/**
 * Set fade callback of a window, and possibly execute the previous
 * callback.
 *
 * If a callback can cause rendering result to change, it should call
 * `queue_redraw`.
 *
 * @param exec_callback whether the previous callback is to be executed
 */
void set_fade_callback(session_t *ps, win **_w,
    void (*callback) (session_t *ps, win **w), bool exec_callback) {
  win *w = *_w;
  void (*old_callback) (session_t *ps, win **w) = w->fade_callback;

  w->fade_callback = callback;
  // Must be the last line as the callback could destroy w!
  if (exec_callback && old_callback)
    old_callback(ps, _w);
}

/**
 * Execute fade callback of a window if fading finished.
 * XXX should be in win.c
 */
static inline void
check_fade_fin(session_t *ps, win **_w) {
  win *w = *_w;
  if (w->fade_callback && w->opacity == w->opacity_tgt) {
    // Must be the last line as the callback could destroy w!
    set_fade_callback(ps, _w, NULL, true);
  }
}

// === Shadows ===

static double __attribute__((const))
gaussian(double r, double x, double y) {
  // Formula can be found here:
  // https://en.wikipedia.org/wiki/Gaussian_blur#Mathematics
  // Except a special case for r == 0 to produce sharp shadows
  if (r == 0)
    return 1;
  return exp(-0.5*(x*x+y*y)/(r*r))/(2*M_PI*r*r);
}

static conv *
make_gaussian_map(double r) {
  conv *c;
  int size = r*2+1;
  int center = size/2;
  double t;

  c = malloc(sizeof(conv)+size*size*sizeof(double));
  c->size = size;
  t = 0.0;

  /*printf_errf("(): %f", r);*/
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      double g = gaussian(r, x-center, y-center);
      t += g;
      c->data[y*size+x] = g;
      /*printf("%f ", c->data[y*size+x]);*/
    }
    /*printf("\n");*/
  }

  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      c->data[y*size+x] /= t;
      /*printf("%f ", c->data[y*size+x]);*/
    }
    /*printf("\n");*/
  }

  return c;
}

/*
 * A picture will help
 *
 *      -center   0                width  width+center
 *  -center +-----+-------------------+-----+
 *          |     |                   |     |
 *          |     |                   |     |
 *        0 +-----+-------------------+-----+
 *          |     |                   |     |
 *          |     |                   |     |
 *          |     |                   |     |
 *   height +-----+-------------------+-----+
 *          |     |                   |     |
 * height+  |     |                   |     |
 *  center  +-----+-------------------+-----+
 */

static unsigned char
sum_gaussian(conv *map, double opacity,
             int x, int y, int width, int height) {
  int fx, fy;
  double *g_data;
  double *g_line = map->data;
  int g_size = map->size;
  int center = g_size / 2;
  int fx_start, fx_end;
  int fy_start, fy_end;
  double v;

  /*
   * Compute set of filter values which are "in range",
   * that's the set with:
   *    0 <= x + (fx-center) && x + (fx-center) < width &&
   *  0 <= y + (fy-center) && y + (fy-center) < height
   *
   *  0 <= x + (fx - center)    x + fx - center < width
   *  center - x <= fx    fx < width + center - x
   */

  fx_start = center - x;
  if (fx_start < 0) fx_start = 0;
  fx_end = width + center - x;
  if (fx_end > g_size) fx_end = g_size;

  fy_start = center - y;
  if (fy_start < 0) fy_start = 0;
  fy_end = height + center - y;
  if (fy_end > g_size) fy_end = g_size;

  g_line = g_line + fy_start * g_size + fx_start;

  v = 0;

  for (fy = fy_start; fy < fy_end; fy++) {
    g_data = g_line;
    g_line += g_size;

    for (fx = fx_start; fx < fx_end; fx++) {
      v += *g_data++;
    }
  }

  if (v > 1) v = 1;

  return ((unsigned char) (v * opacity * 255.0));
}

/* precompute shadow corners and sides
   to save time for large windows */

static void
presum_gaussian(session_t *ps, conv *map) {
  int center = map->size / 2;
  int opacity, x, y;

  ps->cgsize = map->size;

  if (ps->shadow_corner)
    free(ps->shadow_corner);
  if (ps->shadow_top)
    free(ps->shadow_top);

  ps->shadow_corner = malloc((ps->cgsize + 1) * (ps->cgsize + 1) * 26);
  ps->shadow_top = malloc((ps->cgsize + 1) * 26);

  for (x = 0; x <= ps->cgsize; x++) {
    ps->shadow_top[25 * (ps->cgsize + 1) + x] =
      sum_gaussian(map, 1, x - center, center, ps->cgsize * 2, ps->cgsize * 2);

    for (opacity = 0; opacity < 25; opacity++) {
      ps->shadow_top[opacity * (ps->cgsize + 1) + x] =
        ps->shadow_top[25 * (ps->cgsize + 1) + x] * opacity / 25;
    }

    for (y = 0; y <= x; y++) {
      ps->shadow_corner[25 * (ps->cgsize + 1) * (ps->cgsize + 1) + y * (ps->cgsize + 1) + x]
        = sum_gaussian(map, 1, x - center, y - center, ps->cgsize * 2, ps->cgsize * 2);
      ps->shadow_corner[25 * (ps->cgsize + 1) * (ps->cgsize + 1) + x * (ps->cgsize + 1) + y]
        = ps->shadow_corner[25 * (ps->cgsize + 1) * (ps->cgsize + 1) + y * (ps->cgsize + 1) + x];

      for (opacity = 0; opacity < 25; opacity++) {
        ps->shadow_corner[opacity * (ps->cgsize + 1) * (ps->cgsize + 1)
                      + y * (ps->cgsize + 1) + x]
          = ps->shadow_corner[opacity * (ps->cgsize + 1) * (ps->cgsize + 1)
                          + x * (ps->cgsize + 1) + y]
          = ps->shadow_corner[25 * (ps->cgsize + 1) * (ps->cgsize + 1)
                          + y * (ps->cgsize + 1) + x] * opacity / 25;
      }
    }
  }
}

static xcb_image_t *
make_shadow(session_t *ps, double opacity,
            int width, int height) {
  xcb_image_t *ximage;
  int ylimit, xlimit;
  int swidth = width + ps->cgsize;
  int sheight = height + ps->cgsize;
  int center = ps->cgsize / 2;
  int x, y;
  unsigned char d;
  int x_diff;
  int opacity_int = (int)(opacity * 25);

  ximage = xcb_image_create_native(ps->c, swidth, sheight, XCB_IMAGE_FORMAT_Z_PIXMAP, 8,
    0, 0, NULL);

  if (!ximage) {
    printf_errf("(): failed to create an X image");
    return 0;
  }

  unsigned char *data = ximage->data;
  uint32_t sstride = ximage->stride;

  /*
   * Build the gaussian in sections
   */

  /*
   * center (fill the complete data array)
   */

  // XXX If the center part of the shadow would be entirely covered by
  // the body of the window, we shouldn't need to fill the center here.
  // XXX In general, we want to just fill the part that is not behind
  // the window, in order to reduce CPU load and make transparent window
  // look correct
  if (ps->cgsize > 0) {
    d = ps->shadow_top[opacity_int * (ps->cgsize + 1) + ps->cgsize];
  } else {
    d = sum_gaussian(ps->gaussian_map,
      opacity, center, center, width, height);
  }
  memset(data, d, sheight * swidth);

  /*
   * corners
   */

  ylimit = ps->cgsize;
  if (ylimit > sheight / 2) ylimit = (sheight + 1) / 2;

  xlimit = ps->cgsize;
  if (xlimit > swidth / 2) xlimit = (swidth + 1) / 2;

  for (y = 0; y < ylimit; y++) {
    for (x = 0; x < xlimit; x++) {
      if (xlimit == ps->cgsize && ylimit == ps->cgsize) {
        d = ps->shadow_corner[opacity_int * (ps->cgsize + 1) * (ps->cgsize + 1)
                          + y * (ps->cgsize + 1) + x];
      } else {
        d = sum_gaussian(ps->gaussian_map,
          opacity, x - center, y - center, width, height);
      }
      data[y * sstride + x] = d;
      data[(sheight - y - 1) * sstride + x] = d;
      data[(sheight - y - 1) * sstride + (swidth - x - 1)] = d;
      data[y * sstride + (swidth - x - 1)] = d;
    }
  }

  /*
   * top/bottom
   */

  x_diff = swidth - (ps->cgsize * 2);
  if (x_diff > 0 && ylimit > 0) {
    for (y = 0; y < ylimit; y++) {
      if (ylimit == ps->cgsize) {
        d = ps->shadow_top[opacity_int * (ps->cgsize + 1) + y];
      } else {
        d = sum_gaussian(ps->gaussian_map,
          opacity, center, y - center, width, height);
      }
      memset(&data[y * sstride + ps->cgsize], d, x_diff);
      memset(&data[(sheight - y - 1) * sstride + ps->cgsize], d, x_diff);
    }
  }

  /*
   * sides
   */

  for (x = 0; x < xlimit; x++) {
    if (xlimit == ps->cgsize) {
      d = ps->shadow_top[opacity_int * (ps->cgsize + 1) + x];
    } else {
      d = sum_gaussian(ps->gaussian_map,
        opacity, x - center, center, width, height);
    }
    for (y = ps->cgsize; y < sheight - ps->cgsize; y++) {
      data[y * sstride + x] = d;
      data[y * sstride + (swidth - x - 1)] = d;
    }
  }

  return ximage;
}

/**
 * Generate shadow <code>Picture</code> for a window.
 */
static bool
win_build_shadow(session_t *ps, win *w, double opacity) {
  const int width = w->widthb;
  const int height = w->heightb;
  //printf_errf("(): building shadow for %s %d %d", w->name, width, height);

  xcb_image_t *shadow_image = NULL;
  xcb_pixmap_t shadow_pixmap = None, shadow_pixmap_argb = None;
  xcb_render_picture_t shadow_picture = None, shadow_picture_argb = None;
  xcb_gcontext_t gc = None;

  shadow_image = make_shadow(ps, opacity, width, height);
  if (!shadow_image) {
    printf_errf("(): failed to make shadow");
    return None;
  }

  shadow_pixmap = x_create_pixmap(ps, 8, ps->root, shadow_image->width, shadow_image->height);
  shadow_pixmap_argb = x_create_pixmap(ps, 32, ps->root, shadow_image->width, shadow_image->height);

  if (!shadow_pixmap || !shadow_pixmap_argb) {
    printf_errf("(): failed to create shadow pixmaps");
    goto shadow_picture_err;
  }

  shadow_picture = x_create_picture_with_standard_and_pixmap(ps,
    XCB_PICT_STANDARD_A_8, shadow_pixmap, 0, NULL);
  shadow_picture_argb = x_create_picture_with_standard_and_pixmap(ps,
    XCB_PICT_STANDARD_ARGB_32, shadow_pixmap_argb, 0, NULL);
  if (!shadow_picture || !shadow_picture_argb)
    goto shadow_picture_err;

  gc = xcb_generate_id(ps->c);
  xcb_create_gc(ps->c, gc, shadow_pixmap, 0, NULL);

  xcb_image_put(ps->c, shadow_pixmap, gc, shadow_image, 0, 0, 0);
  xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC, ps->cshadow_picture, shadow_picture,
      shadow_picture_argb, 0, 0, 0, 0, 0, 0,
      shadow_image->width, shadow_image->height);

  assert(!w->shadow_paint.pixmap);
  w->shadow_paint.pixmap = shadow_pixmap_argb;
  assert(!w->shadow_paint.pict);
  w->shadow_paint.pict = shadow_picture_argb;

  // Sync it once and only once
  xr_sync(ps, w->shadow_paint.pixmap, NULL);

  xcb_free_gc(ps->c, gc);
  xcb_image_destroy(shadow_image);
  xcb_free_pixmap(ps->c, shadow_pixmap);
  xcb_render_free_picture(ps->c, shadow_picture);

  return true;

shadow_picture_err:
  if (shadow_image)
    xcb_image_destroy(shadow_image);
  if (shadow_pixmap)
    xcb_free_pixmap(ps->c, shadow_pixmap);
  if (shadow_pixmap_argb)
    xcb_free_pixmap(ps->c, shadow_pixmap_argb);
  if (shadow_picture)
    xcb_render_free_picture(ps->c, shadow_picture);
  if (shadow_picture_argb)
    xcb_render_free_picture(ps->c, shadow_picture_argb);
  if (gc)
    xcb_free_gc(ps->c, gc);

  return false;
}

/**
 * Generate a 1x1 <code>Picture</code> of a particular color.
 */
static xcb_render_picture_t
solid_picture(session_t *ps, bool argb, double a,
              double r, double g, double b) {
  xcb_pixmap_t pixmap;
  xcb_render_picture_t picture;
  xcb_render_create_picture_value_list_t pa;
  xcb_render_color_t col;
  xcb_rectangle_t rect;

  pixmap = x_create_pixmap(ps, argb ? 32 : 8, ps->root, 1, 1);
  if (!pixmap) return None;

  pa.repeat = True;
  picture = x_create_picture_with_standard_and_pixmap(ps,
    argb ? XCB_PICT_STANDARD_ARGB_32 : XCB_PICT_STANDARD_A_8, pixmap,
    XCB_RENDER_CP_REPEAT, &pa);

  if (!picture) {
    xcb_free_pixmap(ps->c, pixmap);
    return None;
  }

  col.alpha = a * 0xffff;
  col.red =   r * 0xffff;
  col.green = g * 0xffff;
  col.blue =  b * 0xffff;

  rect.x = 0;
  rect.y = 0;
  rect.width = 1;
  rect.height = 1;

  xcb_render_fill_rectangles(ps->c, XCB_RENDER_PICT_OP_SRC, picture, col, 1, &rect);
  xcb_free_pixmap(ps->c, pixmap);

  return picture;
}

// === Error handling ===

static void
discard_ignore(session_t *ps, unsigned long sequence) {
  while (ps->ignore_head) {
    if (sequence > ps->ignore_head->sequence) {
      ignore_t *next = ps->ignore_head->next;
      free(ps->ignore_head);
      ps->ignore_head = next;
      if (!ps->ignore_head) {
        ps->ignore_tail = &ps->ignore_head;
      }
    } else {
      break;
    }
  }
}

static int
should_ignore(session_t *ps, unsigned long sequence) {
  discard_ignore(ps, sequence);
  return ps->ignore_head && ps->ignore_head->sequence == sequence;
}

// === Windows ===

/**
 * Determine the event mask for a window.
 */
long determine_evmask(session_t *ps, Window wid, win_evmode_t mode) {
  long evmask = 0;
  win *w = NULL;

  // Check if it's a mapped frame window
  if (WIN_EVMODE_FRAME == mode
      || ((w = find_win(ps, wid)) && w->a.map_state == XCB_MAP_STATE_VIEWABLE)) {
    evmask |= XCB_EVENT_MASK_PROPERTY_CHANGE;
    if (ps->o.track_focus && !ps->o.use_ewmh_active_win)
      evmask |= XCB_EVENT_MASK_FOCUS_CHANGE;
  }

  // Check if it's a mapped client window
  if (WIN_EVMODE_CLIENT == mode
      || ((w = find_toplevel(ps, wid)) && w->a.map_state == XCB_MAP_STATE_VIEWABLE)) {
    if (ps->o.frame_opacity || ps->o.track_wdata || ps->track_atom_lst
        || ps->o.detect_client_opacity)
      evmask |= XCB_EVENT_MASK_PROPERTY_CHANGE;
  }

  return evmask;
}

/**
 * Find out the WM frame of a client window by querying X.
 *
 * @param ps current session
 * @param wid window ID
 * @return struct _win object of the found window, NULL if not found
 */
win *find_toplevel2(session_t *ps, Window wid) {
  // TODO this should probably be an "update tree", then find_toplevel.
  //      current approach is a bit more "racy"
  win *w = NULL;

  // We traverse through its ancestors to find out the frame
  while (wid && wid != ps->root && !(w = find_win(ps, wid))) {
    xcb_query_tree_reply_t *reply;

    // xcb_query_tree probably fails if you run compton when X is somehow
    // initializing (like add it in .xinitrc). In this case
    // just leave it alone.
    reply = xcb_query_tree_reply(ps->c, xcb_query_tree(ps->c, wid), NULL);
    if (reply == NULL) {
      break;
    }

    wid = reply->parent;

    free(reply);
  }

  return w;
}

/**
 * Recheck currently focused window and set its <code>w->focused</code>
 * to true.
 *
 * @param ps current session
 * @return struct _win of currently focused window, NULL if not found
 */
static win *
recheck_focus(session_t *ps) {
  // Use EWMH _NET_ACTIVE_WINDOW if enabled
  if (ps->o.use_ewmh_active_win) {
    update_ewmh_active_win(ps);
    return ps->active_win;
  }

  // Determine the currently focused window so we can apply appropriate
  // opacity on it
  xcb_window_t wid = XCB_NONE;
  xcb_get_input_focus_reply_t *reply =
    xcb_get_input_focus_reply(ps->c, xcb_get_input_focus(ps->c), NULL);

  if (reply) {
    wid = reply->focus;
    free(reply);
  }

  win *w = find_win_all(ps, wid);

#ifdef DEBUG_EVENTS
  print_timestamp(ps);
  printf_dbgf("(): %#010" PRIx32 " (%#010lx \"%s\") focused.\n", wid,
      (w ? w->id: None), (w ? w->name: NULL));
#endif

  // And we set the focus state here
  if (w) {
    win_set_focused(ps, w, true);
    return w;
  }

  return NULL;
}

static bool
get_root_tile(session_t *ps) {
  /*
  if (ps->o.paint_on_overlay) {
    return ps->root_picture;
  } */

  assert(!ps->root_tile_paint.pixmap);
  ps->root_tile_fill = false;

  bool fill = false;
  xcb_pixmap_t pixmap = None;

  // Get the values of background attributes
  for (int p = 0; background_props_str[p]; p++) {
    winprop_t prop = wid_get_prop(ps, ps->root,
        get_atom(ps, background_props_str[p]),
        1L, XCB_ATOM_PIXMAP, 32);
    if (prop.nitems) {
      pixmap = *prop.p32;
      fill = false;
      free_winprop(&prop);
      break;
    }
    free_winprop(&prop);
  }

  // Make sure the pixmap we got is valid
  if (pixmap && !validate_pixmap(ps, pixmap))
    pixmap = None;

  // Create a pixmap if there isn't any
  if (!pixmap) {
    pixmap = x_create_pixmap(ps, ps->depth, ps->root, 1, 1);
    if (pixmap == XCB_NONE) {
      fprintf(stderr, "Failed to create some pixmap\n");
      exit(1);
    }
    fill = true;
  }

  // Create Picture
  xcb_render_create_picture_value_list_t pa = {
    .repeat = True,
  };
  ps->root_tile_paint.pict = x_create_picture_with_visual_and_pixmap(
    ps, ps->vis, pixmap, XCB_RENDER_CP_REPEAT, &pa);

  // Fill pixmap if needed
  if (fill) {
    xcb_render_color_t col;
    xcb_rectangle_t rect;

    col.red = col.green = col.blue = 0x8080;
    col.alpha = 0xffff;

    rect.x = rect.y = 0;
    rect.width = rect.height = 1;

    xcb_render_fill_rectangles(ps->c, XCB_RENDER_PICT_OP_SRC, ps->root_tile_paint.pict, col, 1, &rect);
  }

  ps->root_tile_fill = fill;
  ps->root_tile_paint.pixmap = pixmap;
#ifdef CONFIG_OPENGL
  if (BKEND_GLX == ps->o.backend)
    return glx_bind_pixmap(ps, &ps->root_tile_paint.ptex, ps->root_tile_paint.pixmap, 0, 0, 0);
#endif

  return true;
}

/**
 * Paint root window content.
 */
static void
paint_root(session_t *ps, const region_t *reg_paint) {
  if (!ps->root_tile_paint.pixmap)
    get_root_tile(ps);

  paint_region(ps, NULL, 0, 0, ps->root_width, ps->root_height, 1.0, reg_paint,
    ps->root_tile_paint.pict);
}

/**
 * Look for the client window of a particular window.
 */
xcb_window_t
find_client_win(session_t *ps, xcb_window_t w) {
  if (wid_has_prop(ps, w, ps->atom_client)) {
    return w;
  }

  xcb_query_tree_reply_t *reply = xcb_query_tree_reply(ps->c,
      xcb_query_tree(ps->c, w), NULL);
  if (!reply)
    return 0;

  xcb_window_t *children = xcb_query_tree_children(reply);
  int nchildren = xcb_query_tree_children_length(reply);
  int i;
  xcb_window_t ret = 0;

  for (i = 0; i < nchildren; ++i) {
    if ((ret = find_client_win(ps, children[i])))
      break;
  }

  free(reply);

  return ret;
}

static win *
paint_preprocess(session_t *ps, win *list) {
  win *t = NULL, *next = NULL;

  // Fading step calculation
  time_ms_t steps = 0L;
  if (ps->fade_time)
    steps = (get_time_ms() - ps->fade_time +
             FADE_DELTA_TOLERANCE*ps->o.fade_delta) /
            ps->o.fade_delta;
  // Reset fade_time if unset, or there appears to be a time disorder
  if (!ps->fade_time || steps < 0L) {
    ps->fade_time = get_time_ms();
    steps = 0L;
  }
  ps->fade_time += steps * ps->o.fade_delta;

  // First, let's process fading
  for (win *w = list; w; w = next) {
    next = w->next;
    const winmode_t mode_old = w->mode;
    const bool was_painted = w->to_paint;
    const opacity_t opacity_old = w->opacity;
    // Restore flags from last paint if the window is being faded out
    if (w->a.map_state == XCB_MAP_STATE_UNMAPPED) {
      win_set_shadow(ps, w, w->shadow_last);
      w->fade = w->fade_last;
      win_set_invert_color(ps, w, w->invert_color_last);
      win_set_blur_background(ps, w, w->blur_background_last);
    }

    // Update window opacity target and dim state if asked
    if (WFLAG_OPCT_CHANGE & w->flags) {
      win_calc_opacity(ps, w);
      win_calc_dim(ps, w);
    }

    // Run fading
    run_fade(ps, w, steps);

    if (win_has_frame(w))
      w->frame_opacity = ps->o.frame_opacity;
    else
      w->frame_opacity = 1.0;

    // Update window mode
    win_determine_mode(ps, w);

    // Destroy all reg_ignore above when frame opaque state changes on
    // SOLID mode
    if (was_painted && w->mode != mode_old)
      w->reg_ignore_valid = false;

    // Add window to damaged area if its opacity changes
    // If was_painted == false, and to_paint is also false, we don't care
    // If was_painted == false, but to_paint is true, damage will be added in the loop below
    if (was_painted && w->opacity != opacity_old)
      add_damage_from_win(ps, w);
  }

  // Opacity will not change, from now on.
  rc_region_t *last_reg_ignore = rc_region_new();

  bool unredir_possible = false;
  // Trace whether it's the highest window to paint
  bool is_highest = true;
  bool reg_ignore_valid = true;
  for (win *w = list; w; w = next) {
    __label__ skip_window;
    bool to_paint = true;
    // w->to_paint remembers whether this window is painted last time
    const bool was_painted = w->to_paint;

    // In case calling the fade callback function destroys this window
    next = w->next;

    // Destroy reg_ignore if some window above us invalidated it
    if (!reg_ignore_valid)
      rc_region_unref(&w->reg_ignore);

    //printf_errf("(): %d %d %s", w->a.map_state, w->ever_damaged, w->name);

    // Give up if it's not damaged or invisible, or it's unmapped and its
    // pixmap is gone (for example due to a ConfigureNotify), or when it's
    // excluded
    if (!w->ever_damaged
        || w->g.x + w->g.width < 1 || w->g.y + w->g.height < 1
        || w->g.x >= ps->root_width || w->g.y >= ps->root_height
        || ((w->a.map_state == XCB_MAP_STATE_UNMAPPED || w->destroyed) && !w->paint.pixmap)
        || (double) w->opacity / OPAQUE * MAX_ALPHA < 1
        || w->paint_excluded)
      to_paint = false;
    //printf_errf("(): %s %d %d %d", w->name, to_paint, w->opacity, w->paint_excluded);

    // Add window to damaged area if its painting status changes
    // or opacity changes
    if (to_paint != was_painted) {
      w->reg_ignore_valid = false;
      add_damage_from_win(ps, w);
    }

    // to_paint will never change afterward
    if (!to_paint)
      goto skip_window;

    // Calculate shadow opacity
    w->shadow_opacity = ps->o.shadow_opacity * get_opacity_percent(w) * ps->o.frame_opacity;

    // Generate ignore region for painting to reduce GPU load
    if (!w->reg_ignore)
      w->reg_ignore = rc_region_ref(last_reg_ignore);

    // If the window is solid, we add the window region to the
    // ignored region
    // Otherwise last_reg_ignore shouldn't change
    if (w->mode == WMODE_SOLID && !ps->o.force_win_blend) {
      region_t *tmp = rc_region_new();
      if (w->frame_opacity == 1)
        *tmp = win_get_bounding_shape_global_by_val(w);
      else {
        win_get_region_noframe_local(w, tmp);
        pixman_region32_intersect(tmp, tmp, &w->bounding_shape);
        pixman_region32_translate(tmp, w->g.x, w->g.y);
      }

      pixman_region32_union(tmp, tmp, last_reg_ignore);
      rc_region_unref(&last_reg_ignore);
      last_reg_ignore = tmp;
    }

    // (Un)redirect screen
    // We could definitely unredirect the screen when there's no window to
    // paint, but this is typically unnecessary, may cause flickering when
    // fading is enabled, and could create inconsistency when the wallpaper
    // is not correctly set.
    if (ps->o.unredir_if_possible && is_highest) {
      if (win_is_solid(ps, w)
          && (w->frame_opacity == 1 || !win_has_frame(w))
          && win_is_fullscreen(ps, w)
          && !w->unredir_if_possible_excluded)
        unredir_possible = true;
    }

    // Reset flags
    w->flags = 0;
    w->prev_trans = t;
    t = w;

    // If the screen is not redirected and the window has redir_ignore set,
    // this window should not cause the screen to become redirected
    if (!(ps->o.wintype_option[w->window_type].redir_ignore && !ps->redirected)) {
      is_highest = false;
    }

  skip_window:
    reg_ignore_valid = reg_ignore_valid && w->reg_ignore_valid;
    w->reg_ignore_valid = true;

    assert(w->destroyed == (w->fade_callback == finish_destroy_win));
    check_fade_fin(ps, &w);

    // Avoid setting w->to_paint if w is freed
    if (w) {
      w->to_paint = to_paint;

      if (w->to_paint) {
        // Save flags
        w->shadow_last = w->shadow;
        w->fade_last = w->fade;
        w->invert_color_last = w->invert_color;
        w->blur_background_last = w->blur_background;
      }
    }
  }

  rc_region_unref(&last_reg_ignore);

  // If possible, unredirect all windows and stop painting
  if (UNSET != ps->o.redirected_force)
    unredir_possible = !ps->o.redirected_force;
  else if (ps->o.unredir_if_possible && is_highest && !ps->redirected)
    // If there's no window to paint, and the screen isn't redirected,
    // don't redirect it.
    unredir_possible = true;
  if (unredir_possible) {
    if (ps->redirected) {
      if (!ps->o.unredir_if_possible_delay || ps->tmout_unredir_hit)
        redir_stop(ps);
      else if (!ev_is_active(&ps->unredir_timer)) {
        ev_timer_set(&ps->unredir_timer,
          ps->o.unredir_if_possible_delay / 1000.0, 0);
        ev_timer_start(ps->loop, &ps->unredir_timer);
      }
    }
  } else {
    ev_timer_stop(ps->loop, &ps->unredir_timer);
    redir_start(ps);
  }

  return t;
}

/**
 * Paint the shadow of a window.
 */
static inline void
win_paint_shadow(session_t *ps, win *w, region_t *reg_paint) {
  // Bind shadow pixmap to GLX texture if needed
  paint_bind_tex(ps, &w->shadow_paint, 0, 0, 32, false);

  if (!paint_isvalid(ps, &w->shadow_paint)) {
    printf_errf("(%#010lx): Missing shadow data.", w->id);
    return;
  }

  render(ps, 0, 0, w->g.x + w->shadow_dx, w->g.y + w->shadow_dy,
      w->shadow_width, w->shadow_height, w->shadow_opacity, true, false,
      w->shadow_paint.pict, w->shadow_paint.ptex, reg_paint, NULL);
}

/**
 * @brief Blur an area on a buffer.
 *
 * @param ps current session
 * @param tgt_buffer a buffer as both source and destination
 * @param x x pos
 * @param y y pos
 * @param wid width
 * @param hei height
 * @param blur_kerns blur kernels, ending with a NULL, guaranteed to have at
 *                    least one kernel
 * @param reg_clip a clipping region to be applied on intermediate buffers
 *
 * @return true if successful, false otherwise
 */
static bool
xr_blur_dst(session_t *ps, xcb_render_picture_t tgt_buffer,
    int x, int y, int wid, int hei, xcb_render_fixed_t **blur_kerns,
    const region_t *reg_clip) {
  assert(blur_kerns[0]);

  // Directly copying from tgt_buffer to it does not work, so we create a
  // Picture in the middle.
  xcb_render_picture_t tmp_picture = x_create_picture(ps, wid, hei, NULL, 0, NULL);

  if (!tmp_picture) {
    printf_errf("(): Failed to build intermediate Picture.");
    return false;
  }

  if (reg_clip && tmp_picture)
    x_set_picture_clip_region(ps, tmp_picture, 0, 0, reg_clip);

  xcb_render_picture_t src_pict = tgt_buffer, dst_pict = tmp_picture;
  for (int i = 0; blur_kerns[i]; ++i) {
    assert(i < MAX_BLUR_PASS - 1);
    xcb_render_fixed_t *convolution_blur = blur_kerns[i];
    int kwid = XFIXED_TO_DOUBLE(convolution_blur[0]),
        khei = XFIXED_TO_DOUBLE(convolution_blur[1]);
    bool rd_from_tgt = (tgt_buffer == src_pict);

    // Copy from source picture to destination. The filter must
    // be applied on source picture, to get the nearby pixels outside the
    // window.
    xcb_render_set_picture_filter(ps->c, src_pict, strlen(XRFILTER_CONVOLUTION), XRFILTER_CONVOLUTION,
        kwid * khei + 2, convolution_blur);
    xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC, src_pict, None, dst_pict,
        (rd_from_tgt ? x: 0), (rd_from_tgt ? y: 0), 0, 0,
        (rd_from_tgt ? 0: x), (rd_from_tgt ? 0: y), wid, hei);
    xrfilter_reset(ps, src_pict);

    {
      xcb_render_picture_t tmp = src_pict;
      src_pict = dst_pict;
      dst_pict = tmp;
    }
  }

  if (src_pict != tgt_buffer)
    xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC, src_pict, None, tgt_buffer,
        0, 0, 0, 0, x, y, wid, hei);

  free_picture(ps, &tmp_picture);

  return true;
}

/*
 * WORK-IN-PROGRESS!
static void
xr_take_screenshot(session_t *ps) {
  XImage *img = XGetImage(ps->dpy, get_tgt_window(ps), 0, 0,
      ps->root_width, ps->root_height, AllPlanes, XYPixmap);
  if (!img) {
    printf_errf("(): Failed to get XImage.");
    return NULL;
  }
  assert(0 == img->xoffset);
}
*/

/**
 * Blur the background of a window.
 */
static inline void
win_blur_background(session_t *ps, win *w, xcb_render_picture_t tgt_buffer,
    const region_t *reg_paint) {
  const int x = w->g.x;
  const int y = w->g.y;
  const int wid = w->widthb;
  const int hei = w->heightb;

  double factor_center = 1.0;
  // Adjust blur strength according to window opacity, to make it appear
  // better during fading
  if (!ps->o.blur_background_fixed) {
    double pct = 1.0 - get_opacity_percent(w) * (1.0 - 1.0 / 9.0);
    factor_center = pct * 8.0 / (1.1 - pct);
  }

  switch (ps->o.backend) {
    case BKEND_XRENDER:
    case BKEND_XR_GLX_HYBRID:
      {
        // Normalize blur kernels
        for (int i = 0; i < MAX_BLUR_PASS; ++i) {
          xcb_render_fixed_t *kern_src = ps->o.blur_kerns[i];
          xcb_render_fixed_t *kern_dst = ps->blur_kerns_cache[i];
          assert(i < MAX_BLUR_PASS);
          if (!kern_src) {
            assert(!kern_dst);
            break;
          }

          assert(!kern_dst
              || (kern_src[0] == kern_dst[0] && kern_src[1] == kern_dst[1]));

          // Skip for fixed factor_center if the cache exists already
          if (ps->o.blur_background_fixed && kern_dst) continue;

          int kwid = XFIXED_TO_DOUBLE(kern_src[0]),
              khei = XFIXED_TO_DOUBLE(kern_src[1]);

          // Allocate cache space if needed
          if (!kern_dst) {
            kern_dst = malloc((kwid * khei + 2) * sizeof(xcb_render_fixed_t));
            if (!kern_dst) {
              printf_errf("(): Failed to allocate memory for blur kernel.");
              return;
            }
            ps->blur_kerns_cache[i] = kern_dst;
          }

          // Modify the factor of the center pixel
          kern_src[2 + (khei / 2) * kwid + kwid / 2] =
            DOUBLE_TO_XFIXED(factor_center);

          // Copy over
          memcpy(kern_dst, kern_src, (kwid * khei + 2) * sizeof(xcb_render_fixed_t));
          normalize_conv_kern(kwid, khei, kern_dst + 2);
        }

        // Minimize the region we try to blur, if the window itself is not
        // opaque, only the frame is.
        region_t reg_blur = win_get_bounding_shape_global_by_val(w);
        if (win_is_solid(ps, w)) {
          region_t reg_noframe;
          pixman_region32_init(&reg_noframe);
          win_get_region_noframe_local(w, &reg_noframe);
          pixman_region32_translate(&reg_noframe, w->g.x, w->g.y);
          pixman_region32_subtract(&reg_blur, &reg_blur, &reg_noframe);
          pixman_region32_fini(&reg_noframe);
        }
        // Translate global coordinates to local ones
        pixman_region32_translate(&reg_blur, -x, -y);
        xr_blur_dst(ps, tgt_buffer, x, y, wid, hei, ps->blur_kerns_cache,
            &reg_blur);
        pixman_region32_clear(&reg_blur);
      }
      break;
#ifdef CONFIG_OPENGL
    case BKEND_GLX:
      // TODO: Handle frame opacity
      glx_blur_dst(ps, x, y, wid, hei, ps->psglx->z - 0.5, factor_center,
          reg_paint, &w->glx_blur_cache);
      break;
#endif
    default:
      assert(0);
  }
}

static void
render(session_t *ps, int x, int y, int dx, int dy, int wid, int hei,
  double opacity, bool argb, bool neg,
  xcb_render_picture_t pict, glx_texture_t *ptex,
  const region_t *reg_paint, const glx_prog_main_t *pprogram)
{
  switch (ps->o.backend) {
    case BKEND_XRENDER:
    case BKEND_XR_GLX_HYBRID:
      {
        int alpha_step = opacity * MAX_ALPHA;
        xcb_render_picture_t alpha_pict = ps->alpha_picts[alpha_step];
        if (alpha_step != 0) {
          int op = ((!argb && !alpha_pict) ? XCB_RENDER_PICT_OP_SRC: XCB_RENDER_PICT_OP_OVER);
          xcb_render_composite(ps->c, op, pict, alpha_pict,
              ps->tgt_buffer.pict, x, y, 0, 0, dx, dy, wid, hei);
        }
        break;
      }
#ifdef CONFIG_OPENGL
    case BKEND_GLX:
      glx_render(ps, ptex, x, y, dx, dy, wid, hei,
          ps->psglx->z, opacity, argb, neg, reg_paint, pprogram);
      ps->psglx->z += 1;
      break;
#endif
    default:
      assert(0);
  }
}

/**
 * Paint a window itself and dim it if asked.
 */
static inline void
paint_one(session_t *ps, win *w, const region_t *reg_paint) {
  glx_mark(ps, w->id, true);

  // Fetch Pixmap
  if (!w->paint.pixmap && ps->has_name_pixmap) {
    w->paint.pixmap = xcb_generate_id(ps->c);
    set_ignore_cookie(ps,
        xcb_composite_name_window_pixmap(ps->c, w->id, w->paint.pixmap));
    if (w->paint.pixmap)
      free_fence(ps, &w->fence);
  }

  Drawable draw = w->paint.pixmap;
  if (!draw)
    draw = w->id;

  // XRender: Build picture
  if (bkend_use_xrender(ps) && !w->paint.pict) {
    xcb_render_create_picture_value_list_t pa = {
      .subwindowmode = IncludeInferiors,
    };

    w->paint.pict = x_create_picture_with_pictfmt_and_pixmap(ps, w->pictfmt,
        draw, XCB_RENDER_CP_SUBWINDOW_MODE, &pa);
  }

  if (IsViewable == w->a.map_state)
    xr_sync(ps, draw, &w->fence);

  // GLX: Build texture
  // Let glx_bind_pixmap() determine pixmap size, because if the user
  // is resizing windows, the width and height we get may not be up-to-date,
  // causing the jittering issue M4he reported in #7.
  if (!paint_bind_tex(ps, &w->paint, 0, 0, 0,
        (!ps->o.glx_no_rebind_pixmap && w->pixmap_damaged))) {
    printf_errf("(%#010lx): Failed to bind texture. Expect troubles.", w->id);
  }
  w->pixmap_damaged = false;

  if (!paint_isvalid(ps, &w->paint)) {
    printf_errf("(%#010lx): Missing painting data. This is a bad sign.", w->id);
    return;
  }

  const int x = w->g.x;
  const int y = w->g.y;
  const int wid = w->widthb;
  const int hei = w->heightb;

  xcb_render_picture_t pict = w->paint.pict;

  // Invert window color, if required
  if (bkend_use_xrender(ps) && w->invert_color) {
    xcb_render_picture_t newpict = x_create_picture(ps, wid, hei, w->pictfmt, 0, NULL);
    if (newpict) {
      // Apply clipping region to save some CPU
      if (reg_paint) {
        region_t reg;
        pixman_region32_init(&reg);
        pixman_region32_copy(&reg, (region_t *)reg_paint);
        pixman_region32_translate(&reg, -x, -y);
        // FIXME XFixesSetPictureClipRegion(ps->dpy, newpict, 0, 0, reg);
        pixman_region32_fini(&reg);
      }

      xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC, pict, None,
          newpict, 0, 0, 0, 0, 0, 0, wid, hei);
      xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_DIFFERENCE, ps->white_picture, None,
          newpict, 0, 0, 0, 0, 0, 0, wid, hei);
      // We use an extra PictOpInReverse operation to get correct pixel
      // alpha. There could be a better solution.
      if (win_has_alpha(w))
        xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_IN_REVERSE, pict, None,
            newpict, 0, 0, 0, 0, 0, 0, wid, hei);
      pict = newpict;
    }
  }

  const double dopacity = get_opacity_percent(w);

  if (w->frame_opacity == 1) {
    paint_region(ps, w, 0, 0, wid, hei, dopacity, reg_paint, pict);
  } else {
    // Painting parameters
    const margin_t extents = win_calc_frame_extents(w);
    const int t = extents.top;
    const int l = extents.left;
    const int b = extents.bottom;
    const int r = extents.right;

#define COMP_BDR(cx, cy, cwid, chei) \
paint_region(ps, w, (cx), (cy), (cwid), (chei), w->frame_opacity * dopacity, \
  reg_paint, pict)

    // Sanitize the margins, in case some broken WM makes
    // top_width + bottom_width > height in some cases.

    do {
      // top
      int body_height = hei;
      // ctop = checked top
      int ctop = min_i(body_height, t); // Make sure top margin is smaller than height
      if (ctop > 0)
        COMP_BDR(0, 0, wid, ctop);

      body_height -= ctop;
      if (body_height <= 0)
        break;

      // bottom
      // cbot = checked bottom
      int cbot = min_i(body_height, b); // Make sure bottom margin is not too large
      if (cbot > 0)
        COMP_BDR(0, hei-cbot, wid, cbot);

      body_height -= cbot; // Height of window exclude the margin
      if (body_height <= 0)
        break;

      // left
      int body_width = wid;
      int cleft = min_i(body_width, l);
      if (cleft > 0)
        COMP_BDR(0, ctop, cleft, body_height);

      body_width -= cleft;
      if (body_width <= 0)
        break;

      // right
      int cright = min_i(body_width, r);
      if (cright > 0)
        COMP_BDR(wid - cright, ctop, cright, body_height);

      body_width -= cright;
      if (body_width <= 0)
        break;

      // body
      paint_region(ps, w, cleft, ctop, body_width, body_height, dopacity, reg_paint, pict);
    } while (0);
  }

#undef COMP_BDR

  if (pict != w->paint.pict)
    free_picture(ps, &pict);

  // Dimming the window if needed
  if (w->dim) {
    double dim_opacity = ps->o.inactive_dim;
    if (!ps->o.inactive_dim_fixed)
      dim_opacity *= get_opacity_percent(w);

    switch (ps->o.backend) {
      case BKEND_XRENDER:
      case BKEND_XR_GLX_HYBRID:
        {
          unsigned short cval = 0xffff * dim_opacity;

          // Premultiply color
          xcb_render_color_t color = {
            .red = 0, .green = 0, .blue = 0, .alpha = cval,
          };

          xcb_rectangle_t rect = {
            .x = x,
            .y = y,
            .width = wid,
            .height = hei,
          };

          xcb_render_fill_rectangles(ps->c, XCB_RENDER_PICT_OP_OVER, ps->tgt_buffer.pict,
              color, 1, &rect);
        }
        break;
#ifdef CONFIG_OPENGL
      case BKEND_GLX:
        glx_dim_dst(ps, x, y, wid, hei, ps->psglx->z - 0.7, dim_opacity,
            reg_paint);
        break;
#endif
      default:
        assert(false);
    }
  }

  glx_mark(ps, w->id, false);
}

/**
 * Rebuild cached <code>screen_reg</code>.
 */
static void
rebuild_screen_reg(session_t *ps) {
  get_screen_region(ps, &ps->screen_reg);
}

/**
 * Rebuild <code>shadow_exclude_reg</code>.
 */
static void
rebuild_shadow_exclude_reg(session_t *ps) {
  bool ret = parse_geometry(ps, ps->o.shadow_exclude_reg_str,
      &ps->shadow_exclude_reg);
  if (!ret)
    exit(1);
}

/// paint all windows
/// region = ??
/// region_real = the damage region
static void
paint_all(session_t *ps, region_t *region, const region_t *region_real, win * const t) {
  if (!region_real)
    region_real = region;

#ifdef DEBUG_REPAINT
  static struct timespec last_paint = { 0 };
#endif

  if (!region)
    region_real = region = &ps->screen_reg;
  else
    // Remove the damaged area out of screen
    pixman_region32_intersect(region, region, &ps->screen_reg);

#ifdef CONFIG_OPENGL
  if (bkend_use_glx(ps))
    glx_paint_pre(ps, region);
#endif

  if (!paint_isvalid(ps, &ps->tgt_buffer)) {
    if (!ps->tgt_buffer.pixmap) {
      free_paint(ps, &ps->tgt_buffer);
      ps->tgt_buffer.pixmap = x_create_pixmap(ps, ps->depth, ps->root, ps->root_width, ps->root_height);
      if (ps->tgt_buffer.pixmap == XCB_NONE) {
        fprintf(stderr, "Failed to allocate a screen-sized pixmap\n");
        exit(1);
      }
    }

    if (BKEND_GLX != ps->o.backend)
      ps->tgt_buffer.pict = x_create_picture_with_visual_and_pixmap(
        ps, ps->vis, ps->tgt_buffer.pixmap, 0, 0);
  }

  if (BKEND_XRENDER == ps->o.backend) {
    x_set_picture_clip_region(ps, ps->tgt_picture, 0, 0, region_real);
  }

  region_t reg_tmp, *reg_paint;
  pixman_region32_init(&reg_tmp);
  if (t) {
    // Calculate the region upon which the root window is to be painted
    // based on the ignore region of the lowest window, if available
    pixman_region32_subtract(&reg_tmp, region, t->reg_ignore);
    reg_paint = &reg_tmp;
  } else {
    reg_paint = region;
  }

  set_tgt_clip(ps, reg_paint);
  paint_root(ps, reg_paint);

  // Windows are sorted from bottom to top
  // Each window has a reg_ignore, which is the region obscured by all the windows
  // on top of that window. This is used to reduce the number of pixels painted.
  //
  // Whether this is beneficial is to be determined XXX
  for (win *w = t; w; w = w->prev_trans) {
    region_t bshape = win_get_bounding_shape_global_by_val(w);
    // Painting shadow
    if (w->shadow) {
      // Lazy shadow building
      if (!w->shadow_paint.pixmap)
        if (!win_build_shadow(ps, w, 1))
          printf_errf("(): build shadow failed");

      // Shadow doesn't need to be painted underneath the body of the window
      // Because no one can see it
      pixman_region32_subtract(&reg_tmp, region, w->reg_ignore);

      // Mask out the region we don't want shadow on
      if (pixman_region32_not_empty(&ps->shadow_exclude_reg))
        pixman_region32_subtract(&reg_tmp, &reg_tmp, &ps->shadow_exclude_reg);

      // Might be worth while to crop the region to shadow border
      pixman_region32_intersect_rect(&reg_tmp, &reg_tmp,
        w->g.x + w->shadow_dx, w->g.y + w->shadow_dy,
        w->shadow_width, w->shadow_height);

      // Mask out the body of the window from the shadow if needed
      // Doing it here instead of in make_shadow() for saving GPU
      // power and handling shaped windows (XXX unconfirmed)
      if (!ps->o.wintype_option[w->window_type].full_shadow)
        pixman_region32_subtract(&reg_tmp, &reg_tmp, &bshape);

#ifdef CONFIG_XINERAMA
      if (ps->o.xinerama_shadow_crop && w->xinerama_scr >= 0 &&
          w->xinerama_scr < ps->xinerama_nscrs)
        // There can be a window where number of screens is updated,
        // but the screen number attached to the windows have not.
        //
        // Window screen number will be updated eventually, so here we
        // just check to make sure we don't access out of bounds.
        pixman_region32_intersect(&reg_tmp, &reg_tmp,
          &ps->xinerama_scr_regs[w->xinerama_scr]);
#endif

      // Detect if the region is empty before painting
      if (pixman_region32_not_empty(&reg_tmp)) {
        set_tgt_clip(ps, &reg_tmp);
        win_paint_shadow(ps, w, &reg_tmp);
      }
    }

    // Calculate the region based on the reg_ignore of the next (higher)
    // window and the bounding region
    // XXX XXX
    pixman_region32_subtract(&reg_tmp, region, w->reg_ignore);
    pixman_region32_intersect(&reg_tmp, &reg_tmp, &bshape);
    pixman_region32_fini(&bshape);

    if (pixman_region32_not_empty(&reg_tmp)) {
        set_tgt_clip(ps, &reg_tmp);
        // Blur window background
        if (w->blur_background && (!win_is_solid(ps, w)
            || (ps->o.blur_background_frame && w->frame_opacity != 1)))
          win_blur_background(ps, w, ps->tgt_buffer.pict, &reg_tmp);

        // Painting the window
        paint_one(ps, w, &reg_tmp);
    }
  }

  // Free up all temporary regions
  pixman_region32_fini(&reg_tmp);

  // Do this as early as possible
  set_tgt_clip(ps, &ps->screen_reg);

  if (ps->o.vsync) {
    // Make sure all previous requests are processed to achieve best
    // effect
    x_sync(ps->c);
#ifdef CONFIG_OPENGL
    if (glx_has_context(ps)) {
      if (ps->o.vsync_use_glfinish)
        glFinish();
      else
        glFlush();
      glXWaitX();
    }
#endif
  }

  // Wait for VBlank. We could do it aggressively (send the painting
  // request and XFlush() on VBlank) or conservatively (send the request
  // only on VBlank).
  if (!ps->o.vsync_aggressive)
    vsync_wait(ps);

  switch (ps->o.backend) {
    case BKEND_XRENDER:
      if (ps->o.monitor_repaint) {
        // Copy the screen content to a new picture, and highlight the paint
        // region. This is not very efficient, but since it's for debug only,
        // we don't really care

        // First, we clear tgt_buffer.pict's clip region, since we want to copy
        // everything
        x_set_picture_clip_region(ps, ps->tgt_buffer.pict, 0, 0, &ps->screen_reg);

        // Then we create a new picture, and copy content to it
        xcb_render_pictforminfo_t *pictfmt = x_get_pictform_for_visual(ps, ps->vis);
        xcb_render_picture_t new_pict = x_create_picture(
          ps, ps->root_width, ps->root_height, pictfmt, 0, NULL);
        xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC, ps->tgt_buffer.pict,
          None, new_pict, 0, 0, 0, 0, 0, 0, ps->root_width, ps->root_height);

        // Next, we set the region of paint and highlight it
        x_set_picture_clip_region(ps, new_pict, 0, 0, region_real);
        xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_OVER, ps->white_picture,
          ps->alpha_picts[MAX_ALPHA / 2],
          new_pict, 0, 0, 0, 0, 0, 0,
          ps->root_width, ps->root_height);

        // Finally, clear clip region and put the whole thing on screen
        x_set_picture_clip_region(ps, new_pict, 0, 0, &ps->screen_reg);
        xcb_render_composite(
          ps->c, XCB_RENDER_PICT_OP_SRC, new_pict, None,
          ps->tgt_picture, 0, 0, 0, 0,
          0, 0, ps->root_width, ps->root_height);
        xcb_render_free_picture(ps->c, new_pict);
      } else
        xcb_render_composite(
          ps->c, XCB_RENDER_PICT_OP_SRC, ps->tgt_buffer.pict, None,
          ps->tgt_picture, 0, 0, 0, 0,
          0, 0, ps->root_width, ps->root_height);
      break;
#ifdef CONFIG_OPENGL
    case BKEND_XR_GLX_HYBRID:
      x_sync(ps->c);
      if (ps->o.vsync_use_glfinish)
        glFinish();
      else
        glFlush();
      glXWaitX();
      assert(ps->tgt_buffer.pixmap);
      xr_sync(ps, ps->tgt_buffer.pixmap, &ps->tgt_buffer_fence);
      paint_bind_tex(ps, &ps->tgt_buffer,
          ps->root_width, ps->root_height, ps->depth,
          !ps->o.glx_no_rebind_pixmap);
      // See #163
      xr_sync(ps, ps->tgt_buffer.pixmap, &ps->tgt_buffer_fence);
      if (ps->o.vsync_use_glfinish)
        glFinish();
      else
        glFlush();
      glXWaitX();
      glx_render(ps, ps->tgt_buffer.ptex, 0, 0, 0, 0,
          ps->root_width, ps->root_height, 0, 1.0, false, false,
          region_real, NULL);
      // falls through
    case BKEND_GLX:
      glXSwapBuffers(ps->dpy, get_tgt_window(ps));
      break;
#endif
    default:
      assert(0);
  }
  glx_mark_frame(ps);

  if (ps->o.vsync_aggressive)
    vsync_wait(ps);

  XFlush(ps->dpy);

#ifdef CONFIG_OPENGL
  if (glx_has_context(ps)) {
    glFlush();
    glXWaitX();
  }
#endif

#ifdef DEBUG_REPAINT
  print_timestamp(ps);
  struct timespec now = get_time_timespec();
  struct timespec diff = { 0 };
  timespec_subtract(&diff, &now, &last_paint);
  printf("[ %5ld:%09ld ] ", diff.tv_sec, diff.tv_nsec);
  last_paint = now;
  printf("paint:");
  for (win *w = t; w; w = w->prev_trans)
    printf(" %#010lx", w->id);
  putchar('\n');
  fflush(stdout);
#endif

  // Check if fading is finished on all painted windows
  {
    win *pprev = NULL;
    for (win *w = t; w; w = pprev) {
      pprev = w->prev_trans;
      check_fade_fin(ps, &w);
    }
  }
}

static void
repair_win(session_t *ps, win *w) {
  if (w->a.map_state != XCB_MAP_STATE_VIEWABLE)
    return;

  region_t parts;
  pixman_region32_init(&parts);

  if (!w->ever_damaged) {
    win_extents(w, &parts);
    set_ignore_cookie(ps,
        xcb_damage_subtract(ps->c, w->damage, XCB_NONE, XCB_NONE));
  } else {
    xcb_xfixes_region_t tmp = xcb_generate_id(ps->c);
    xcb_xfixes_create_region(ps->c, tmp, 0, NULL);
    set_ignore_cookie(ps,
        xcb_damage_subtract(ps->c, w->damage, XCB_NONE, tmp));
    xcb_xfixes_translate_region(ps->c, tmp,
      w->g.x + w->g.border_width,
      w->g.y + w->g.border_width);
    x_fetch_region(ps, tmp, &parts);
    xcb_xfixes_destroy_region(ps->c, tmp);
  }

  w->ever_damaged = true;
  w->pixmap_damaged = true;

  // Why care about damage when screen is unredirected?
  // We will force full-screen repaint on redirection.
  if (!ps->redirected) {
    pixman_region32_fini(&parts);
    return;
  }

  // Remove the part in the damage area that could be ignored
  if (w->reg_ignore && win_is_region_ignore_valid(ps, w))
    pixman_region32_subtract(&parts, &parts, w->reg_ignore);

  add_damage(ps, &parts);
  pixman_region32_fini(&parts);
}

static void
finish_map_win(session_t *ps, win **_w) {
  win *w = *_w;
  w->in_openclose = false;
  if (ps->o.no_fading_openclose) {
    win_determine_fade(ps, w);
  }
}

void
map_win(session_t *ps, Window id) {
  // Unmap overlay window if it got mapped but we are currently not
  // in redirected state.
  if (ps->overlay && id == ps->overlay && !ps->redirected) {
    xcb_unmap_window(ps->c, ps->overlay);
    XFlush(ps->dpy);
  }

  win *w = find_win(ps, id);

#ifdef DEBUG_EVENTS
  printf_dbgf("(%#010lx \"%s\"): %p\n", id, (w ? w->name: NULL), w);
#endif

  // Don't care about window mapping if it's an InputOnly window
  // Try avoiding mapping a window twice
  if (!w || InputOnly == w->a._class
      || w->a.map_state == XCB_MAP_STATE_VIEWABLE)
    return;

  assert(!win_is_focused_real(ps, w));

  w->a.map_state = XCB_MAP_STATE_VIEWABLE;

  cxinerama_win_upd_scr(ps, w);

  // Set window event mask before reading properties so that no property
  // changes are lost
  xcb_change_window_attributes(ps->c, id, XCB_CW_EVENT_MASK,
      (const uint32_t[]) { determine_evmask(ps, id, WIN_EVMODE_FRAME) });

  // Notify compton when the shape of a window changes
  if (ps->shape_exists) {
    xcb_shape_select_input(ps->c, id, 1);
  }

  // Make sure the XSelectInput() requests are sent
  XFlush(ps->dpy);

  // Update window mode here to check for ARGB windows
  win_determine_mode(ps, w);

  // Detect client window here instead of in add_win() as the client
  // window should have been prepared at this point
  if (!w->client_win) {
    win_recheck_client(ps, w);
  }
  else {
    // Re-mark client window here
    win_mark_client(ps, w, w->client_win);
  }

  assert(w->client_win);

#ifdef DEBUG_WINTYPE
  printf_dbgf("(%#010lx): type %s\n", w->id, WINTYPES[w->window_type]);
#endif

  // FocusIn/Out may be ignored when the window is unmapped, so we must
  // recheck focus here
  if (ps->o.track_focus)
    recheck_focus(ps);

  // Update window focus state
  win_update_focused(ps, w);

  // Update opacity and dim state
  win_update_opacity_prop(ps, w);
  w->flags |= WFLAG_OPCT_CHANGE;

  // Check for _COMPTON_SHADOW
  if (ps->o.respect_prop_shadow)
    win_update_prop_shadow_raw(ps, w);

  // Many things above could affect shadow
  win_determine_shadow(ps, w);

  // Set fading state
  w->in_openclose = true;
  set_fade_callback(ps, &w, finish_map_win, true);
  win_determine_fade(ps, w);

  win_determine_blur_background(ps, w);

  w->ever_damaged = false;

  /* if any configure events happened while
     the window was unmapped, then configure
     the window to its correct place */
  if (w->need_configure)
    configure_win(ps, &w->queue_configure);

  // We stopped listening on ShapeNotify events
  // when the window is unmapped (XXX we shouldn't),
  // so the shape of the window might have changed,
  // update. (Issue #35)
  win_update_bounding_shape(ps, w);

#ifdef CONFIG_DBUS
  // Send D-Bus signal
  if (ps->o.dbus) {
    cdbus_ev_win_mapped(ps, w);
  }
#endif
}

static void
finish_unmap_win(session_t *ps, win **_w) {
  win *w = *_w;
  w->ever_damaged = false;
  w->in_openclose = false;
  w->reg_ignore_valid = false;

  /* damage region */
  add_damage_from_win(ps, w);

  free_paint(ps, &w->paint);
  free_paint(ps, &w->shadow_paint);
}

static void
unmap_win(session_t *ps, win **_w) {
  win *w = *_w;
  if (!w || w->a.map_state == XCB_MAP_STATE_UNMAPPED) return;

  if (w->destroyed) return;

  // One last synchronization
  if (w->paint.pixmap)
    xr_sync(ps, w->paint.pixmap, &w->fence);
  free_fence(ps, &w->fence);

  // Set focus out
  win_set_focused(ps, w, false);

  w->a.map_state = XCB_MAP_STATE_UNMAPPED;

  // Fading out
  w->flags |= WFLAG_OPCT_CHANGE;
  set_fade_callback(ps, _w, finish_unmap_win, false);
  w->in_openclose = true;
  win_determine_fade(ps, w);

  // Validate pixmap if we have to do fading
  if (w->fade)
    win_validate_pixmap(ps, w);

  // don't care about properties anymore
  win_ev_stop(ps, w);

#ifdef CONFIG_DBUS
  // Send D-Bus signal
  if (ps->o.dbus) {
    cdbus_ev_win_unmapped(ps, w);
  }
#endif
}

static void
restack_win(session_t *ps, win *w, Window new_above) {
  Window old_above;

  if (w->next) {
    old_above = w->next->id;
  } else {
    old_above = None;
  }

  if (old_above != new_above) {
    w->reg_ignore_valid = false;
    rc_region_unref(&w->reg_ignore);
    if (w->next) {
      w->next->reg_ignore_valid = false;
      rc_region_unref(&w->next->reg_ignore);
    }

    win **prev = NULL, **prev_old = NULL;

    // unhook
    for (prev = &ps->list; *prev; prev = &(*prev)->next) {
      if ((*prev) == w) break;
    }

    prev_old = prev;

    bool found = false;

    // rehook
    for (prev = &ps->list; *prev; prev = &(*prev)->next) {
      if ((*prev)->id == new_above && !(*prev)->destroyed) {
        found = true;
        break;
      }
    }

    if (new_above && !found) {
      printf_errf("(%#010lx, %#010lx): "
          "Failed to found new above window.", w->id, new_above);
      return;
    }

    *prev_old = w->next;

    w->next = *prev;
    *prev = w;

    // add damage for this window
    add_damage_from_win(ps, w);

#ifdef DEBUG_RESTACK
    {
      const char *desc;
      char *window_name = NULL;
      bool to_free;
      win* c = ps->list;

      printf_dbgf("(%#010lx, %#010lx): "
             "Window stack modified. Current stack:\n", w->id, new_above);

      for (; c; c = c->next) {
        window_name = "(Failed to get title)";

        to_free = ev_window_name(ps, c->id, &window_name);

        desc = "";
        if (c->destroyed) desc = "(D) ";
        printf("%#010lx \"%s\" %s", c->id, window_name, desc);
        if (c->next)
          printf("-> ");

        if (to_free) {
          cxfree(window_name);
          window_name = NULL;
        }
      }
      fputs("\n", stdout);
    }
#endif
  }
}

static bool
init_filters(session_t *ps);

static void
configure_win(session_t *ps, xcb_configure_notify_event_t *ce) {
  // On root window changes
  if (ce->window == ps->root) {
    free_paint(ps, &ps->tgt_buffer);

    ps->root_width = ce->width;
    ps->root_height = ce->height;

    rebuild_screen_reg(ps);
    rebuild_shadow_exclude_reg(ps);
    free_all_damage_last(ps);

    // Re-redirect screen if required
    if (ps->o.reredir_on_root_change && ps->redirected) {
      redir_stop(ps);
      redir_start(ps);
    }

    // Invalidate reg_ignore from the top
    rc_region_unref(&ps->list->reg_ignore);
    ps->list->reg_ignore_valid = false;

#ifdef CONFIG_OPENGL
    // Reinitialize GLX on root change
    if (ps->o.glx_reinit_on_root_change && ps->psglx) {
      if (!glx_reinit(ps, bkend_use_glx(ps)))
        printf_errf("(): Failed to reinitialize GLX, troubles ahead.");
      if (BKEND_GLX == ps->o.backend && !init_filters(ps))
        printf_errf("(): Failed to initialize filters.");
    }

    // GLX root change callback
    if (BKEND_GLX == ps->o.backend)
      glx_on_root_change(ps);
#endif

    force_repaint(ps);

    return;
  }

  // Other window changes
  win *w = find_win(ps, ce->window);
  region_t damage;
  pixman_region32_init(&damage);

  if (!w)
    return;

  if (w->a.map_state == XCB_MAP_STATE_UNMAPPED) {
    /* save the configure event for when the window maps */
    w->need_configure = true;
    w->queue_configure = *ce;
    restack_win(ps, w, ce->above_sibling);
  } else {
    if (!w->need_configure)
      restack_win(ps, w, ce->above_sibling);

    bool factor_change = false;

    w->need_configure = false;
    win_extents(w, &damage);

    // If window geometry change, free old extents
    if (w->g.x != ce->x || w->g.y != ce->y
        || w->g.width != ce->width || w->g.height != ce->height
        || w->g.border_width != ce->border_width)
      factor_change = true;

    w->g.x = ce->x;
    w->g.y = ce->y;

    if (w->g.width != ce->width || w->g.height != ce->height
        || w->g.border_width != ce->border_width) {
      w->g.width = ce->width;
      w->g.height = ce->height;
      w->g.border_width = ce->border_width;
      calc_win_size(ps, w);
      win_update_bounding_shape(ps, w);
    }

    region_t new_extents;
    pixman_region32_init(&new_extents);
    win_extents(w, &new_extents);
    pixman_region32_union(&damage, &damage, &new_extents);
    pixman_region32_fini(&new_extents);

    if (factor_change) {
      win_on_factor_change(ps, w);
      add_damage(ps, &damage);
      cxinerama_win_upd_scr(ps, w);
    }
  }

  pixman_region32_fini(&damage);

  // override_redirect flag cannot be changed after window creation, as far
  // as I know, so there's no point to re-match windows here.
  w->a.override_redirect = ce->override_redirect;
}

static void
circulate_win(session_t *ps, xcb_circulate_notify_event_t *ce) {
  win *w = find_win(ps, ce->window);
  Window new_above;

  if (!w) return;

  if (ce->place == PlaceOnTop) {
    new_above = ps->list->id;
  } else {
    new_above = None;
  }

  restack_win(ps, w, new_above);
}

static void
finish_destroy_win(session_t *ps, win **_w) {
  win *w = *_w;
  assert(w->destroyed);
  win **prev = NULL, *i = NULL;

#ifdef DEBUG_EVENTS
  printf_dbgf("(%#010lx): Starting...\n", w->id);
#endif

  for (prev = &ps->list; (i = *prev); prev = &i->next) {
    if (w == i) {
#ifdef DEBUG_EVENTS
      printf_dbgf("(%#010lx \"%s\"): %p\n", w->id, w->name, w);
#endif

      finish_unmap_win(ps, _w);
      *prev = w->next;

      // Clear active_win if it's pointing to the destroyed window
      if (w == ps->active_win)
        ps->active_win = NULL;

      free_win_res(ps, w);

      // Drop w from all prev_trans to avoid accessing freed memory in
      // repair_win()
      for (win *w2 = ps->list; w2; w2 = w2->next)
        if (w == w2->prev_trans)
          w2->prev_trans = NULL;

      free(w);
      *_w = NULL;
      break;
    }
  }
}

static void
destroy_win(session_t *ps, Window id) {
  win *w = find_win(ps, id);

#ifdef DEBUG_EVENTS
  printf_dbgf("(%#010lx \"%s\"): %p\n", id, (w ? w->name: NULL), w);
#endif

  if (w) {
    unmap_win(ps, &w);

    w->destroyed = true;

    if (ps->o.no_fading_destroyed_argb)
      win_determine_fade(ps, w);

    // Set fading callback
    set_fade_callback(ps, &w, finish_destroy_win, false);

#ifdef CONFIG_DBUS
    // Send D-Bus signal
    if (ps->o.dbus) {
      cdbus_ev_win_destroyed(ps, w);
    }
#endif
  }
}

static inline void
root_damaged(session_t *ps) {
  if (ps->root_tile_paint.pixmap) {
    xcb_clear_area(ps->c, true, ps->root, 0, 0, 0, 0);
    free_root_tile(ps);
  }

  // Mark screen damaged
  force_repaint(ps);
}

/**
 * Xlib error handler function.
 */
static int
xerror(Display __attribute__((unused)) *dpy, XErrorEvent *ev) {
  if (!should_ignore(ps_g, ev->serial))
    x_print_error(ev->serial, ev->request_code, ev->minor_code, ev->error_code);
  return 0;
}

/**
 * XCB error handler function.
 */
void
ev_xcb_error(session_t __attribute__((unused)) *ps, xcb_generic_error_t *err) {
  if (!should_ignore(ps, err->sequence))
    x_print_error(err->sequence, err->major_code, err->minor_code, err->error_code);
}

static void
expose_root(session_t *ps, const rect_t *rects, int nrects) {
  free_all_damage_last(ps);
  region_t region;
  pixman_region32_init_rects(&region, rects, nrects);
  add_damage(ps, &region);
}
/**
 * Force a full-screen repaint.
 */
void
force_repaint(session_t *ps) {
  assert(pixman_region32_not_empty(&ps->screen_reg));
  queue_redraw(ps);
  add_damage(ps, &ps->screen_reg);
}

#ifdef CONFIG_DBUS
/** @name DBus hooks
 */
///@{

/**
 * Set w->shadow_force of a window.
 */
void
win_set_shadow_force(session_t *ps, win *w, switch_t val) {
  if (val != w->shadow_force) {
    w->shadow_force = val;
    win_determine_shadow(ps, w);
    queue_redraw(ps);
  }
}

/**
 * Set w->fade_force of a window.
 */
void
win_set_fade_force(session_t *ps, win *w, switch_t val) {
  if (val != w->fade_force) {
    w->fade_force = val;
    win_determine_fade(ps, w);
    queue_redraw(ps);
  }
}

/**
 * Set w->focused_force of a window.
 */
void
win_set_focused_force(session_t *ps, win *w, switch_t val) {
  if (val != w->focused_force) {
    w->focused_force = val;
    win_update_focused(ps, w);
    queue_redraw(ps);
  }
}

/**
 * Set w->invert_color_force of a window.
 */
void
win_set_invert_color_force(session_t *ps, win *w, switch_t val) {
  if (val != w->invert_color_force) {
    w->invert_color_force = val;
    win_determine_invert_color(ps, w);
    queue_redraw(ps);
  }
}

/**
 * Enable focus tracking.
 */
void
opts_init_track_focus(session_t *ps) {
  // Already tracking focus
  if (ps->o.track_focus)
    return;

  ps->o.track_focus = true;

  if (!ps->o.use_ewmh_active_win) {
    // Start listening to FocusChange events
    for (win *w = ps->list; w; w = w->next)
      if (w->a.map_state == XCB_MAP_STATE_VIEWABLE)
        xcb_change_window_attributes(ps->c, w->id, XCB_CW_EVENT_MASK,
            (const uint32_t[]) { determine_evmask(ps, w->id, WIN_EVMODE_FRAME) });
  }

  // Recheck focus
  recheck_focus(ps);
}

/**
 * Set no_fading_openclose option.
 */
void
opts_set_no_fading_openclose(session_t *ps, bool newval) {
  if (newval != ps->o.no_fading_openclose) {
    ps->o.no_fading_openclose = newval;
    for (win *w = ps->list; w; w = w->next)
      win_determine_fade(ps, w);
    queue_redraw(ps);
  }
}

//!@}
#endif

static inline int __attribute__((unused))
ev_serial(xcb_generic_event_t *ev) {
  return ev->full_sequence;
}

static inline const char * __attribute__((unused))
ev_name(session_t *ps, xcb_generic_event_t *ev) {
  static char buf[128];
  switch (ev->response_type & 0x7f) {
    CASESTRRET(FocusIn);
    CASESTRRET(FocusOut);
    CASESTRRET(CreateNotify);
    CASESTRRET(ConfigureNotify);
    CASESTRRET(DestroyNotify);
    CASESTRRET(MapNotify);
    CASESTRRET(UnmapNotify);
    CASESTRRET(ReparentNotify);
    CASESTRRET(CirculateNotify);
    CASESTRRET(Expose);
    CASESTRRET(PropertyNotify);
    CASESTRRET(ClientMessage);
  }

  if (ps->damage_event + XCB_DAMAGE_NOTIFY == ev->response_type)
    return "Damage";

  if (ps->shape_exists && ev->response_type == ps->shape_event)
    return "ShapeNotify";

  if (ps->xsync_exists) {
    int o = ev->response_type - ps->xsync_event;
    switch (o) {
      CASESTRRET(XSyncCounterNotify);
      CASESTRRET(XSyncAlarmNotify);
    }
  }

  sprintf(buf, "Event %d", ev->response_type);

  return buf;
}

static inline Window __attribute__((unused))
ev_window(session_t *ps, xcb_generic_event_t *ev) {
  switch (ev->response_type) {
    case FocusIn:
    case FocusOut:
      return ((xcb_focus_in_event_t *)ev)->event;
    case CreateNotify:
      return ((xcb_create_notify_event_t *)ev)->window;
    case ConfigureNotify:
      return ((xcb_configure_notify_event_t *)ev)->window;
    case DestroyNotify:
      return ((xcb_destroy_notify_event_t *)ev)->window;
    case MapNotify:
      return ((xcb_map_notify_event_t *)ev)->window;
    case UnmapNotify:
      return ((xcb_unmap_notify_event_t *)ev)->window;
    case ReparentNotify:
      return ((xcb_reparent_notify_event_t *)ev)->window;
    case CirculateNotify:
      return ((xcb_circulate_notify_event_t *)ev)->window;
    case Expose:
      return ((xcb_expose_event_t *)ev)->window;
    case PropertyNotify:
      return ((xcb_property_notify_event_t *)ev)->window;
    case ClientMessage:
      return ((xcb_client_message_event_t *)ev)->window;
    default:
      if (ps->damage_event + XCB_DAMAGE_NOTIFY == ev->response_type) {
        return ((xcb_damage_notify_event_t *)ev)->drawable;
      }

      if (ps->shape_exists && ev->response_type == ps->shape_event) {
        return ((xcb_shape_notify_event_t *) ev)->affected_window;
      }

      return 0;
  }
}

static inline const char *
ev_focus_mode_name(xcb_focus_in_event_t* ev) {
  switch (ev->mode) {
    CASESTRRET(NotifyNormal);
    CASESTRRET(NotifyWhileGrabbed);
    CASESTRRET(NotifyGrab);
    CASESTRRET(NotifyUngrab);
  }

  return "Unknown";
}

static inline const char *
ev_focus_detail_name(xcb_focus_in_event_t* ev) {
  switch (ev->detail) {
    CASESTRRET(NotifyAncestor);
    CASESTRRET(NotifyVirtual);
    CASESTRRET(NotifyInferior);
    CASESTRRET(NotifyNonlinear);
    CASESTRRET(NotifyNonlinearVirtual);
    CASESTRRET(NotifyPointer);
    CASESTRRET(NotifyPointerRoot);
    CASESTRRET(NotifyDetailNone);
  }

  return "Unknown";
}

static inline void __attribute__((unused))
ev_focus_report(xcb_focus_in_event_t *ev) {
  printf("  { mode: %s, detail: %s }\n", ev_focus_mode_name(ev),
      ev_focus_detail_name(ev));
}

// === Events ===

/**
 * Determine whether we should respond to a <code>FocusIn/Out</code>
 * event.
 */
/*
inline static bool
ev_focus_accept(XFocusChangeEvent *ev) {
  return NotifyNormal == ev->mode || NotifyUngrab == ev->mode;
}
*/

static inline void
ev_focus_in(session_t *ps, xcb_focus_in_event_t *ev) {
#ifdef DEBUG_EVENTS
  ev_focus_report(ev);
#endif

  recheck_focus(ps);
}

inline static void
ev_focus_out(session_t *ps, xcb_focus_out_event_t *ev) {
#ifdef DEBUG_EVENTS
  ev_focus_report(ev);
#endif

  recheck_focus(ps);
}

inline static void
ev_create_notify(session_t *ps, xcb_create_notify_event_t *ev) {
  assert(ev->parent == ps->root);
  add_win(ps, ev->window, 0);
}

inline static void
ev_configure_notify(session_t *ps, xcb_configure_notify_event_t *ev) {
#ifdef DEBUG_EVENTS
  printf("  { send_event: %d, "
         " above: %#010x, "
         " override_redirect: %d }\n",
         ev->event, ev->above_sibling, ev->override_redirect);
#endif
  configure_win(ps, ev);
}

inline static void
ev_destroy_notify(session_t *ps, xcb_destroy_notify_event_t *ev) {
  destroy_win(ps, ev->window);
}

inline static void
ev_map_notify(session_t *ps, xcb_map_notify_event_t *ev) {
  map_win(ps, ev->window);
}

inline static void
ev_unmap_notify(session_t *ps, xcb_unmap_notify_event_t *ev) {
  win *w = find_win(ps, ev->window);

  if (w)
    unmap_win(ps, &w);
}

inline static void
ev_reparent_notify(session_t *ps, xcb_reparent_notify_event_t *ev) {
#ifdef DEBUG_EVENTS
  printf_dbg("  { new_parent: %#010x, override_redirect: %d }\n",
      ev->parent, ev->override_redirect);
#endif

  if (ev->parent == ps->root) {
    add_win(ps, ev->window, 0);
  } else {
    destroy_win(ps, ev->window);

    // Reset event mask in case something wrong happens
    xcb_change_window_attributes(ps->c, ev->window, XCB_CW_EVENT_MASK,
        (const uint32_t[]) { determine_evmask(ps, ev->window, WIN_EVMODE_UNKNOWN) });

    // Check if the window is an undetected client window
    // Firstly, check if it's a known client window
    if (!find_toplevel(ps, ev->window)) {
      // If not, look for its frame window
      win *w_top = find_toplevel2(ps, ev->parent);
      // If found, and the client window has not been determined, or its
      // frame may not have a correct client, continue
      if (w_top && (!w_top->client_win
            || w_top->client_win == w_top->id)) {
        // If it has WM_STATE, mark it the client window
        if (wid_has_prop(ps, ev->window, ps->atom_client)) {
          w_top->wmwin = false;
          win_unmark_client(ps, w_top);
          win_mark_client(ps, w_top, ev->window);
        }
        // Otherwise, watch for WM_STATE on it
        else {
          xcb_change_window_attributes(ps->c, ev->window, XCB_CW_EVENT_MASK, (const uint32_t[]) {
              determine_evmask(ps, ev->window, WIN_EVMODE_UNKNOWN) | XCB_EVENT_MASK_PROPERTY_CHANGE });
        }
      }
    }
  }
}

inline static void
ev_circulate_notify(session_t *ps, xcb_circulate_notify_event_t *ev) {
  circulate_win(ps, ev);
}

inline static void
ev_expose(session_t *ps, xcb_expose_event_t *ev) {
  if (ev->window == ps->root || (ps->overlay && ev->window == ps->overlay)) {
    int more = ev->count + 1;
    if (ps->n_expose == ps->size_expose) {
      if (ps->expose_rects) {
        ps->expose_rects = realloc(ps->expose_rects,
          (ps->size_expose + more) * sizeof(rect_t));
        ps->size_expose += more;
      } else {
        ps->expose_rects = malloc(more * sizeof(rect_t));
        ps->size_expose = more;
      }
    }

    ps->expose_rects[ps->n_expose].x1 = ev->x;
    ps->expose_rects[ps->n_expose].y1 = ev->y;
    ps->expose_rects[ps->n_expose].x2 = ev->x + ev->width;
    ps->expose_rects[ps->n_expose].y2 = ev->y + ev->height;
    ps->n_expose++;

    if (ev->count == 0) {
      expose_root(ps, ps->expose_rects, ps->n_expose);
      ps->n_expose = 0;
    }
  }
}

/**
 * Update current active window based on EWMH _NET_ACTIVE_WIN.
 *
 * Does not change anything if we fail to get the attribute or the window
 * returned could not be found.
 */
static void
update_ewmh_active_win(session_t *ps) {
  // Search for the window
  Window wid = wid_get_prop_window(ps, ps->root, ps->atom_ewmh_active_win);
  win *w = find_win_all(ps, wid);

  // Mark the window focused. No need to unfocus the previous one.
  if (w) win_set_focused(ps, w, true);
}

inline static void
ev_property_notify(session_t *ps, xcb_property_notify_event_t *ev) {
#ifdef DEBUG_EVENTS
  {
    // Print out changed atom
    xcb_get_atom_name_reply_t *reply =
      xcb_get_atom_name_reply(ps->c, xcb_get_atom_name(ps->c, ev->atom), NULL);
    const char *name = "?";
    int name_len = 1;
    if (reply) {
        name = xcb_get_atom_name_name(reply);
        name_len = xcb_get_atom_name_name_length(reply);
    }

    printf_dbg("  { atom = %.*s }\n", name_len, name);
    free(reply);
  }
#endif

  if (ps->root == ev->window) {
    if (ps->o.track_focus && ps->o.use_ewmh_active_win
        && ps->atom_ewmh_active_win == ev->atom) {
      update_ewmh_active_win(ps);
    }
    else {
      // Destroy the root "image" if the wallpaper probably changed
      for (int p = 0; background_props_str[p]; p++) {
        if (ev->atom == get_atom(ps, background_props_str[p])) {
          root_damaged(ps);
          break;
        }
      }
    }

    // Unconcerned about any other proprties on root window
    return;
  }

  // If WM_STATE changes
  if (ev->atom == ps->atom_client) {
    // Check whether it could be a client window
    if (!find_toplevel(ps, ev->window)) {
      // Reset event mask anyway
      xcb_change_window_attributes(ps->c, ev->window, XCB_CW_EVENT_MASK, (const uint32_t[]) {
          determine_evmask(ps, ev->window, WIN_EVMODE_UNKNOWN) });

      win *w_top = find_toplevel2(ps, ev->window);
      // Initialize client_win as early as possible
      if (w_top && (!w_top->client_win || w_top->client_win == w_top->id)
          && wid_has_prop(ps, ev->window, ps->atom_client)) {
        w_top->wmwin = false;
        win_unmark_client(ps, w_top);
        win_mark_client(ps, w_top, ev->window);
      }
    }
  }

  // If _NET_WM_WINDOW_TYPE changes... God knows why this would happen, but
  // there are always some stupid applications. (#144)
  if (ev->atom == ps->atom_win_type) {
    win *w = NULL;
    if ((w = find_toplevel(ps, ev->window)))
      win_upd_wintype(ps, w);
  }

  // If _NET_WM_OPACITY changes
  if (ev->atom == ps->atom_opacity) {
    win *w = find_win(ps, ev->window) ?: find_toplevel(ps, ev->window);
    if (w) {
      win_update_opacity_prop(ps, w);
      w->flags |= WFLAG_OPCT_CHANGE;
    }
  }

  // If frame extents property changes
  if (ps->o.frame_opacity && ev->atom == ps->atom_frame_extents) {
    win *w = find_toplevel(ps, ev->window);
    if (w) {
      win_update_frame_extents(ps, w, ev->window);
      // If frame extents change, the window needs repaint
      add_damage_from_win(ps, w);
    }
  }

  // If name changes
  if (ps->o.track_wdata
      && (ps->atom_name == ev->atom || ps->atom_name_ewmh == ev->atom)) {
    win *w = find_toplevel(ps, ev->window);
    if (w && 1 == win_get_name(ps, w)) {
      win_on_factor_change(ps, w);
    }
  }

  // If class changes
  if (ps->o.track_wdata && ps->atom_class == ev->atom) {
    win *w = find_toplevel(ps, ev->window);
    if (w) {
      win_get_class(ps, w);
      win_on_factor_change(ps, w);
    }
  }

  // If role changes
  if (ps->o.track_wdata && ps->atom_role == ev->atom) {
    win *w = find_toplevel(ps, ev->window);
    if (w && 1 == win_get_role(ps, w)) {
      win_on_factor_change(ps, w);
    }
  }

  // If _COMPTON_SHADOW changes
  if (ps->o.respect_prop_shadow && ps->atom_compton_shadow == ev->atom) {
    win *w = find_win(ps, ev->window);
    if (w)
      win_update_prop_shadow(ps, w);
  }

  // If a leader property changes
  if ((ps->o.detect_transient && ps->atom_transient == ev->atom)
      || (ps->o.detect_client_leader && ps->atom_client_leader == ev->atom)) {
    win *w = find_toplevel(ps, ev->window);
    if (w) {
      win_update_leader(ps, w);
    }
  }

  // Check for other atoms we are tracking
  for (latom_t *platom = ps->track_atom_lst; platom; platom = platom->next) {
    if (platom->atom == ev->atom) {
      win *w = find_win(ps, ev->window);
      if (!w)
        w = find_toplevel(ps, ev->window);
      if (w)
        win_on_factor_change(ps, w);
      break;
    }
  }
}

inline static void
ev_damage_notify(session_t *ps, xcb_damage_notify_event_t *de) {
  /*
  if (ps->root == de->drawable) {
    root_damaged();
    return;
  } */

  win *w = find_win(ps, de->drawable);

  if (!w) return;

  repair_win(ps, w);
}

inline static void
ev_shape_notify(session_t *ps, xcb_shape_notify_event_t *ev) {
  win *w = find_win(ps, ev->affected_window);
  if (!w || w->a.map_state == XCB_MAP_STATE_UNMAPPED) return;

  /*
   * Empty bounding_shape may indicated an
   * unmapped/destroyed window, in which case
   * seemingly BadRegion errors would be triggered
   * if we attempt to rebuild border_size
   */
  // Mark the old border_size as damaged
  region_t tmp = win_get_bounding_shape_global_by_val(w);
  add_damage(ps, &tmp);
  pixman_region32_fini(&tmp);

  win_update_bounding_shape(ps, w);

  // Mark the new border_size as damaged
  tmp = win_get_bounding_shape_global_by_val(w);
  add_damage(ps, &tmp);
  pixman_region32_fini(&tmp);

  w->reg_ignore_valid = false;
}

/**
 * Handle ScreenChangeNotify events from X RandR extension.
 */
static void
ev_screen_change_notify(session_t *ps,
    xcb_randr_screen_change_notify_event_t __attribute__((unused)) *ev) {
  if (ps->o.xinerama_shadow_crop)
    cxinerama_upd_scrs(ps);

  if (ps->o.sw_opti && !ps->o.refresh_rate) {
    update_refresh_rate(ps);
    if (!ps->refresh_rate) {
      fprintf(stderr, "ev_screen_change_notify(): Refresh rate detection failed."
        "swopti will be temporarily disabled");
    }
  }
}

inline static void
ev_selection_clear(session_t *ps,
    xcb_selection_clear_event_t __attribute__((unused)) *ev) {
  // The only selection we own is the _NET_WM_CM_Sn selection.
  // If we lose that one, we should exit.
  fprintf(stderr, "Another composite manager started and "
      "took the _NET_WM_CM_Sn selection.\n");
  exit(1);
}

/**
 * Get a window's name from window ID.
 */
static inline void __attribute__((unused))
ev_window_name(session_t *ps, Window wid, char **name) {
  *name = "";
  if (wid) {
    *name = "(Failed to get title)";
    if (ps->root == wid)
      *name = "(Root window)";
    else if (ps->overlay == wid)
      *name = "(Overlay)";
    else {
      win *w = find_win(ps, wid);
      if (!w)
        w = find_toplevel(ps, wid);

      if (w)
        win_get_name(ps, w);
      if (w && w->name)
        *name = w->name;
      else
        *name = "unknown";
    }
  }
}

static void
ev_handle(session_t *ps, xcb_generic_event_t *ev) {
  if ((ev->response_type & 0x7f) != KeymapNotify) {
    discard_ignore(ps, ev->full_sequence);
  }

#ifdef DEBUG_EVENTS
  if (ev->response_type != ps->damage_event + XCB_DAMAGE_NOTIFY) {
    Window wid = ev_window(ps, ev);
    char *window_name = NULL;
    ev_window_name(ps, wid, &window_name);

    print_timestamp(ps);
    printf_errf(" event %10.10s serial %#010x window %#010lx \"%s\"\n",
      ev_name(ps, ev), ev_serial(ev), wid, window_name);
  }
#endif

  // Check if a custom XEvent constructor was registered in xlib for this event
  // type, and call it discarding the constructed XEvent if any. XESetWireToEvent
  // might be used by libraries to intercept messages from the X server e.g. the
  // OpenGL lib waiting for DRI2 events.

  // XXX This exists to workaround compton issue #33, #34, #47
  // For even more details, see:
  // https://bugs.freedesktop.org/show_bug.cgi?id=35945
  // https://lists.freedesktop.org/archives/xcb/2011-November/007337.html
  auto proc = XESetWireToEvent(ps->dpy, ev->response_type, 0);
  if (proc) {
    XESetWireToEvent(ps->dpy, ev->response_type, proc);
    XEvent dummy;

    // Stop Xlib from complaining about lost sequence numbers.
    // proc might also just be Xlib internal event processing functions, and
    // because they probably won't see all X replies, they will complain about
    // missing sequence numbers.
    ev->sequence = LastKnownRequestProcessed(ps->dpy);
    proc(ps->dpy, &dummy, (xEvent *)ev);
  }

  // XXX redraw needs to be more fine grained
  queue_redraw(ps);

  switch (ev->response_type) {
    case FocusIn:
      ev_focus_in(ps, (xcb_focus_in_event_t *)ev);
      break;
    case FocusOut:
      ev_focus_out(ps, (xcb_focus_out_event_t *)ev);
      break;
    case CreateNotify:
      ev_create_notify(ps, (xcb_create_notify_event_t *)ev);
      break;
    case ConfigureNotify:
      ev_configure_notify(ps, (xcb_configure_notify_event_t *)ev);
      break;
    case DestroyNotify:
      ev_destroy_notify(ps, (xcb_destroy_notify_event_t *)ev);
      break;
    case MapNotify:
      ev_map_notify(ps, (xcb_map_notify_event_t *)ev);
      break;
    case UnmapNotify:
      ev_unmap_notify(ps, (xcb_unmap_notify_event_t *)ev);
      break;
    case ReparentNotify:
      ev_reparent_notify(ps, (xcb_reparent_notify_event_t *)ev);
      break;
    case CirculateNotify:
      ev_circulate_notify(ps, (xcb_circulate_notify_event_t *)ev);
      break;
    case Expose:
      ev_expose(ps, (xcb_expose_event_t *)ev);
      break;
    case PropertyNotify:
      ev_property_notify(ps, (xcb_property_notify_event_t *)ev);
      break;
    case SelectionClear:
      ev_selection_clear(ps, (xcb_selection_clear_event_t *)ev);
      break;
    case 0:
      ev_xcb_error(ps, (xcb_generic_error_t *)ev);
      break;
    default:
      if (ps->shape_exists && ev->response_type == ps->shape_event) {
        ev_shape_notify(ps, (xcb_shape_notify_event_t *) ev);
        break;
      }
      if (ps->randr_exists && ev->response_type == (ps->randr_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY)) {
        ev_screen_change_notify(ps, (xcb_randr_screen_change_notify_event_t *) ev);
        break;
      }
      if (ps->damage_event + XCB_DAMAGE_NOTIFY == ev->response_type) {
        ev_damage_notify(ps, (xcb_damage_notify_event_t *) ev);
        break;
      }
  }
}

// === Main ===

/**
 * Print usage text and exit.
 */
static void
usage(int ret) {
#define WARNING_DISABLED " (DISABLED AT COMPILE TIME)"
#define WARNING
  static const char *usage_text =
    "compton (" COMPTON_VERSION ")\n"
    "This is the maintenance fork of compton, please report\n"
    "bugs to https://github.com/yshui/compton\n\n"
    "usage: compton [options]\n"
    "Options:\n"
    "\n"
    "-d display\n"
    "  Which display should be managed.\n"
    "\n"
    "-r radius\n"
    "  The blur radius for shadows. (default 12)\n"
    "\n"
    "-o opacity\n"
    "  The translucency for shadows. (default .75)\n"
    "\n"
    "-l left-offset\n"
    "  The left offset for shadows. (default -15)\n"
    "\n"
    "-t top-offset\n"
    "  The top offset for shadows. (default -15)\n"
    "\n"
    "-I fade-in-step\n"
    "  Opacity change between steps while fading in. (default 0.028)\n"
    "\n"
    "-O fade-out-step\n"
    "  Opacity change between steps while fading out. (default 0.03)\n"
    "\n"
    "-D fade-delta-time\n"
    "  The time between steps in a fade in milliseconds. (default 10)\n"
    "\n"
    "-m opacity\n"
    "  The opacity for menus. (default 1.0)\n"
    "\n"
    "-c\n"
    "  Enabled client-side shadows on windows.\n"
    "\n"
    "-C\n"
    "  Avoid drawing shadows on dock/panel windows.\n"
    "\n"
    "-z\n"
    "  Zero the part of the shadow's mask behind the window.\n"
    "\n"
    "-f\n"
    "  Fade windows in/out when opening/closing and when opacity\n"
    "  changes, unless --no-fading-openclose is used.\n"
    "\n"
    "-F\n"
    "  Equals to -f. Deprecated.\n"
    "\n"
    "-i opacity\n"
    "  Opacity of inactive windows. (0.1 - 1.0)\n"
    "\n"
    "-e opacity\n"
    "  Opacity of window titlebars and borders. (0.1 - 1.0)\n"
    "\n"
    "-G\n"
    "  Don't draw shadows on DND windows\n"
    "\n"
    "-b\n"
    "  Daemonize process.\n"
    "\n"
    "-S\n"
    "  Enable synchronous operation (for debugging).\n"
    "\n"
    "--show-all-xerrors\n"
    "  Show all X errors (for debugging).\n"
    "\n"
#undef WARNING
#ifndef CONFIG_LIBCONFIG
#define WARNING WARNING_DISABLED
#else
#define WARNING
#endif
    "--config path\n"
    "  Look for configuration file at the path. Use /dev/null to avoid\n"
    "  loading configuration file." WARNING "\n"
    "\n"
    "--write-pid-path path\n"
    "  Write process ID to a file.\n"
    "\n"
    "--shadow-red value\n"
    "  Red color value of shadow (0.0 - 1.0, defaults to 0).\n"
    "\n"
    "--shadow-green value\n"
    "  Green color value of shadow (0.0 - 1.0, defaults to 0).\n"
    "\n"
    "--shadow-blue value\n"
    "  Blue color value of shadow (0.0 - 1.0, defaults to 0).\n"
    "\n"
    "--inactive-opacity-override\n"
    "  Inactive opacity set by -i overrides value of _NET_WM_OPACITY.\n"
    "\n"
    "--inactive-dim value\n"
    "  Dim inactive windows. (0.0 - 1.0, defaults to 0)\n"
    "\n"
    "--active-opacity opacity\n"
    "  Default opacity for active windows. (0.0 - 1.0)\n"
    "\n"
    "--mark-wmwin-focused\n"
    "  Try to detect WM windows and mark them as active.\n"
    "\n"
    "--shadow-exclude condition\n"
    "  Exclude conditions for shadows.\n"
    "\n"
    "--fade-exclude condition\n"
    "  Exclude conditions for fading.\n"
    "\n"
    "--mark-ovredir-focused\n"
    "  Mark windows that have no WM frame as active.\n"
    "\n"
    "--no-fading-openclose\n"
    "  Do not fade on window open/close.\n"
    "\n"
    "--no-fading-destroyed-argb\n"
    "  Do not fade destroyed ARGB windows with WM frame. Workaround of bugs\n"
    "  in Openbox, Fluxbox, etc.\n"
    "\n"
    "--shadow-ignore-shaped\n"
    "  Do not paint shadows on shaped windows. (Deprecated, use\n"
    "  --shadow-exclude \'bounding_shaped\' or\n"
    "  --shadow-exclude \'bounding_shaped && !rounded_corners\' instead.)\n"
    "\n"
    "--detect-rounded-corners\n"
    "  Try to detect windows with rounded corners and don't consider\n"
    "  them shaped windows. Affects --shadow-ignore-shaped,\n"
    "  --unredir-if-possible, and possibly others. You need to turn this\n"
    "  on manually if you want to match against rounded_corners in\n"
    "  conditions.\n"
    "\n"
    "--detect-client-opacity\n"
    "  Detect _NET_WM_OPACITY on client windows, useful for window\n"
    "  managers not passing _NET_WM_OPACITY of client windows to frame\n"
    "  windows.\n"
    "\n"
    "--refresh-rate val\n"
    "  Specify refresh rate of the screen. If not specified or 0, compton\n"
    "  will try detecting this with X RandR extension.\n"
    "\n"
    "--vsync vsync-method\n"
    "  Set VSync method. There are (up to) 5 VSync methods currently\n"
    "  available:\n"
    "    none = No VSync\n"
#undef WARNING
#ifndef CONFIG_VSYNC_DRM
#define WARNING WARNING_DISABLED
#else
#define WARNING
#endif
    "    drm = VSync with DRM_IOCTL_WAIT_VBLANK. May only work on some\n"
    "      (DRI-based) drivers." WARNING "\n"
#undef WARNING
#ifndef CONFIG_OPENGL
#define WARNING WARNING_DISABLED
#else
#define WARNING
#endif
    "    opengl = Try to VSync with SGI_video_sync OpenGL extension. Only\n"
    "      work on some drivers." WARNING"\n"
    "    opengl-oml = Try to VSync with OML_sync_control OpenGL extension.\n"
    "      Only work on some drivers." WARNING"\n"
    "    opengl-swc = Enable driver-level VSync. Works only with GLX backend." WARNING "\n"
    "    opengl-mswc = Deprecated, use opengl-swc instead." WARNING "\n"
    "\n"
    "--vsync-aggressive\n"
    "  Attempt to send painting request before VBlank and do XFlush()\n"
    "  during VBlank. This switch may be lifted out at any moment.\n"
    "\n"
    "--paint-on-overlay\n"
    "  Painting on X Composite overlay window.\n"
    "\n"
    "--sw-opti\n"
    "  Limit compton to repaint at most once every 1 / refresh_rate\n"
    "  second to boost performance.\n"
    "\n"
    "--use-ewmh-active-win\n"
    "  Use _NET_WM_ACTIVE_WINDOW on the root window to determine which\n"
    "  window is focused instead of using FocusIn/Out events.\n"
    "\n"
    "--respect-prop-shadow\n"
    "  Respect _COMPTON_SHADOW. This a prototype-level feature, which\n"
    "  you must not rely on.\n"
    "\n"
    "--unredir-if-possible\n"
    "  Unredirect all windows if a full-screen opaque window is\n"
    "  detected, to maximize performance for full-screen windows.\n"
    "\n"
    "--unredir-if-possible-delay ms\n"
    "  Delay before unredirecting the window, in milliseconds.\n"
    "  Defaults to 0.\n"
    "\n"
    "--unredir-if-possible-exclude condition\n"
    "  Conditions of windows that shouldn't be considered full-screen\n"
    "  for unredirecting screen.\n"
    "\n"
    "--focus-exclude condition\n"
    "  Specify a list of conditions of windows that should always be\n"
    "  considered focused.\n"
    "\n"
    "--inactive-dim-fixed\n"
    "  Use fixed inactive dim value.\n"
    "\n"
    "--detect-transient\n"
    "  Use WM_TRANSIENT_FOR to group windows, and consider windows in\n"
    "  the same group focused at the same time.\n"
    "\n"
    "--detect-client-leader\n"
    "  Use WM_CLIENT_LEADER to group windows, and consider windows in\n"
    "  the same group focused at the same time. WM_TRANSIENT_FOR has\n"
    "  higher priority if --detect-transient is enabled, too.\n"
    "\n"
    "--blur-background\n"
    "  Blur background of semi-transparent / ARGB windows. Bad in\n"
    "  performance. The switch name may change without prior\n"
    "  notifications.\n"
    "\n"
    "--blur-background-frame\n"
    "  Blur background of windows when the window frame is not opaque.\n"
    "  Implies --blur-background. Bad in performance. The switch name\n"
    "  may change.\n"
    "\n"
    "--blur-background-fixed\n"
    "  Use fixed blur strength instead of adjusting according to window\n"
    "  opacity.\n"
    "\n"
    "--blur-kern matrix\n"
    "  Specify the blur convolution kernel, with the following format:\n"
    "    WIDTH,HEIGHT,ELE1,ELE2,ELE3,ELE4,ELE5...\n"
    "  The element in the center must not be included, it will be forever\n"
    "  1.0 or changing based on opacity, depending on whether you have\n"
    "  --blur-background-fixed.\n"
    "  A 7x7 Gaussian blur kernel looks like:\n"
    "    --blur-kern '7,7,0.000003,0.000102,0.000849,0.001723,0.000849,0.000102,0.000003,0.000102,0.003494,0.029143,0.059106,0.029143,0.003494,0.000102,0.000849,0.029143,0.243117,0.493069,0.243117,0.029143,0.000849,0.001723,0.059106,0.493069,0.493069,0.059106,0.001723,0.000849,0.029143,0.243117,0.493069,0.243117,0.029143,0.000849,0.000102,0.003494,0.029143,0.059106,0.029143,0.003494,0.000102,0.000003,0.000102,0.000849,0.001723,0.000849,0.000102,0.000003'\n"
    "  Up to 4 blur kernels may be specified, separated with semicolon, for\n"
    "  multi-pass blur.\n"
    "  May also be one the predefined kernels: 3x3box (default), 5x5box,\n"
    "  7x7box, 3x3gaussian, 5x5gaussian, 7x7gaussian, 9x9gaussian,\n"
    "  11x11gaussian.\n"
    "\n"
    "--blur-background-exclude condition\n"
    "  Exclude conditions for background blur.\n"
    "\n"
    "--resize-damage integer\n"
    "  Resize damaged region by a specific number of pixels. A positive\n"
    "  value enlarges it while a negative one shrinks it. Useful for\n"
    "  fixing the line corruption issues of blur. May or may not\n"
    "  work with --glx-no-stencil. Shrinking doesn't function correctly.\n"
    "\n"
    "--invert-color-include condition\n"
    "  Specify a list of conditions of windows that should be painted with\n"
    "  inverted color. Resource-hogging, and is not well tested.\n"
    "\n"
    "--opacity-rule opacity:condition\n"
    "  Specify a list of opacity rules, in the format \"PERCENT:PATTERN\",\n"
    "  like \'50:name *= \"Firefox\"'. compton-trans is recommended over\n"
    "  this. Note we do not distinguish 100% and unset, and we don't make\n"
    "  any guarantee about possible conflicts with other programs that set\n"
    "  _NET_WM_WINDOW_OPACITY on frame or client windows.\n"
    "\n"
    "--shadow-exclude-reg geometry\n"
    "  Specify a X geometry that describes the region in which shadow\n"
    "  should not be painted in, such as a dock window region.\n"
    "  Use --shadow-exclude-reg \'x10+0-0\', for example, if the 10 pixels\n"
    "  on the bottom of the screen should not have shadows painted on.\n"
#undef WARNING
#ifndef CONFIG_XINERAMA
#define WARNING WARNING_DISABLED
#else
#define WARNING
#endif
    "\n"
    "--xinerama-shadow-crop\n"
    "  Crop shadow of a window fully on a particular Xinerama screen to the\n"
    "  screen." WARNING "\n"
    "\n"
#undef WARNING
#ifndef CONFIG_OPENGL
#define WARNING "(GLX BACKENDS DISABLED AT COMPILE TIME)"
#else
#define WARNING
#endif
    "--backend backend\n"
    "  Choose backend. Possible choices are xrender, glx, and\n"
    "  xr_glx_hybrid" WARNING ".\n"
    "\n"
    "--glx-no-stencil\n"
    "  GLX backend: Avoid using stencil buffer. Might cause issues\n"
    "  when rendering transparent content. My tests show a 15% performance\n"
    "  boost.\n"
    "\n"
    "--glx-no-rebind-pixmap\n"
    "  GLX backend: Avoid rebinding pixmap on window damage. Probably\n"
    "  could improve performance on rapid window content changes, but is\n"
    "  known to break things on some drivers (LLVMpipe, xf86-video-intel,\n"
    "  etc.).\n"
    "\n"
    "--glx-swap-method undefined/copy/exchange/3/4/5/6/buffer-age\n"
    "  GLX backend: GLX buffer swap method we assume. Could be\n"
    "  undefined (0), copy (1), exchange (2), 3-6, or buffer-age (-1).\n"
    "  \"undefined\" is the slowest and the safest, and the default value.\n"
    "  1 is fastest, but may fail on some drivers, 2-6 are gradually slower\n"
    "  but safer (6 is still faster than 0). -1 means auto-detect using\n"
    "  GLX_EXT_buffer_age, supported by some drivers. \n"
    "\n"
    "--glx-use-gpushader4\n"
    "  GLX backend: Use GL_EXT_gpu_shader4 for some optimization on blur\n"
    "  GLSL code. My tests on GTX 670 show no noticeable effect.\n"
    "\n"
    "--xrender-sync\n"
    "  Attempt to synchronize client applications' draw calls with XSync(),\n"
    "  used on GLX backend to ensure up-to-date window content is painted.\n"
#undef WARNING
#define WARNING
    "\n"
    "--xrender-sync-fence\n"
    "  Additionally use X Sync fence to sync clients' draw calls. Needed\n"
    "  on nvidia-drivers with GLX backend for some users." WARNING "\n"
    "\n"
    "--force-win-blend\n"
    "  Force all windows to be painted with blending. Useful if you have a\n"
    "  --glx-fshader-win that could turn opaque pixels transparent.\n"
    "\n"
#undef WARNING
#ifndef CONFIG_DBUS
#define WARNING WARNING_DISABLED
#else
#define WARNING
#endif
    "--dbus\n"
    "  Enable remote control via D-Bus. See the D-BUS API section in the\n"
    "  man page for more details." WARNING "\n"
    "\n"
    "--benchmark cycles\n"
    "  Benchmark mode. Repeatedly paint until reaching the specified cycles.\n"
    "\n"
    "--benchmark-wid window-id\n"
    "  Specify window ID to repaint in benchmark mode. If omitted or is 0,\n"
    "  the whole screen is repainted.\n"
    "--monitor-repaint\n"
    "  Highlight the updated area of the screen. For debugging the xrender\n"
    "  backend only.\n"
    ;
  FILE *f = (ret ? stderr: stdout);
  fputs(usage_text, f);
#undef WARNING
#undef WARNING_DISABLED

  exit(ret);
}

/**
 * Register a window as symbol, and initialize GLX context if wanted.
 */
static bool
register_cm(session_t *ps) {
  assert(!ps->reg_win);

  ps->reg_win = XCreateSimpleWindow(ps->dpy, ps->root, 0, 0, 1, 1, 0,
        None, None);

  if (!ps->reg_win) {
    printf_errf("(): Failed to create window.");
    return false;
  }

  // Unredirect the window if it's redirected, just in case
  if (ps->redirected)
    xcb_composite_unredirect_window(ps->c, ps->reg_win, XCB_COMPOSITE_REDIRECT_MANUAL);

  {
    XClassHint *h = XAllocClassHint();
    if (h) {
      h->res_name = "compton";
      h->res_class = "xcompmgr";
    }
    Xutf8SetWMProperties(ps->dpy, ps->reg_win, "xcompmgr", "xcompmgr",
        NULL, 0, NULL, NULL, h);
    cxfree(h);
  }

  // Set _NET_WM_PID
  {
    uint32_t pid = getpid();
    xcb_change_property(ps->c, XCB_PROP_MODE_REPLACE, ps->reg_win,
        get_atom(ps, "_NET_WM_PID"), XCB_ATOM_CARDINAL, 32, 1, &pid);
  }

  // Set COMPTON_VERSION
  if (!wid_set_text_prop(ps, ps->reg_win, get_atom(ps, "COMPTON_VERSION"), COMPTON_VERSION)) {
    printf_errf("(): Failed to set COMPTON_VERSION.");
  }

  // Acquire X Selection _NET_WM_CM_S?
  if (!ps->o.no_x_selection) {
    unsigned len = strlen(REGISTER_PROP) + 2;
    int s = ps->scr;
    Atom atom;

    while (s >= 10) {
      ++len;
      s /= 10;
    }

    char *buf = malloc(len);
    snprintf(buf, len, REGISTER_PROP "%d", ps->scr);
    buf[len - 1] = '\0';
    atom = get_atom(ps, buf);
    free(buf);

    xcb_get_selection_owner_reply_t *reply =
      xcb_get_selection_owner_reply(ps->c,
          xcb_get_selection_owner(ps->c, atom), NULL);

    if (reply && reply->owner != XCB_NONE) {
      free(reply);
      fprintf(stderr, "Another composite manager is already running\n");
      return false;
    }
    free(reply);
    xcb_set_selection_owner(ps->c, ps->reg_win, atom, 0);
  }

  return true;
}

/**
 * Reopen streams for logging.
 */
static bool
ostream_reopen(session_t *ps, const char *path) {
  if (!path)
    path = ps->o.logpath;
  if (!path)
    path = "/dev/null";

  bool success = freopen(path, "a", stdout);
  success = freopen(path, "a", stderr) && success;
  if (!success)
    printf_errfq(1, "(%s): freopen() failed.", path);

  return success;
}

/**
 * Fork program to background and disable all I/O streams.
 */
static inline bool
fork_after(session_t *ps) {
  if (getppid() == 1)
    return true;

#ifdef CONFIG_OPENGL
  // GLX context must be released and reattached on fork
  if (glx_has_context(ps) && !glXMakeCurrent(ps->dpy, None, NULL)) {
    printf_errf("(): Failed to detach GLx context.");
    return false;
  }
#endif

  int pid = fork();

  if (-1 == pid) {
    printf_errf("(): fork() failed.");
    return false;
  }

  if (pid > 0) _exit(0);

  setsid();

#ifdef CONFIG_OPENGL
  if (glx_has_context(ps)
      && !glXMakeCurrent(ps->dpy, get_tgt_window(ps), ps->psglx->context)) {
    printf_errf("(): Failed to make GLX context current.");
    return false;
  }
#endif

  // Mainly to suppress the _FORTIFY_SOURCE warning
  bool success = freopen("/dev/null", "r", stdin);
  if (!success) {
    printf_errf("(): freopen() failed.");
    return false;
  }

  return success;
}

/**
 * Write PID to a file.
 */
static inline bool
write_pid(session_t *ps) {
  if (!ps->o.write_pid_path)
    return true;

  FILE *f = fopen(ps->o.write_pid_path, "w");
  if (unlikely(!f)) {
    printf_errf("(): Failed to write PID to \"%s\".", ps->o.write_pid_path);
    return false;
  }

  fprintf(f, "%ld\n", (long) getpid());
  fclose(f);

  return true;
}

/**
 * Process arguments and configuration files.
 */
static void
get_cfg(session_t *ps, int argc, char *const *argv, bool first_pass) {
  static const char *shortopts = "D:I:O:d:r:o:m:l:t:i:e:hscnfFCaSzGb";
  static const struct option longopts[] = {
    { "help", no_argument, NULL, 'h' },
    { "config", required_argument, NULL, 256 },
    { "shadow-radius", required_argument, NULL, 'r' },
    { "shadow-opacity", required_argument, NULL, 'o' },
    { "shadow-offset-x", required_argument, NULL, 'l' },
    { "shadow-offset-y", required_argument, NULL, 't' },
    { "fade-in-step", required_argument, NULL, 'I' },
    { "fade-out-step", required_argument, NULL, 'O' },
    { "fade-delta", required_argument, NULL, 'D' },
    { "menu-opacity", required_argument, NULL, 'm' },
    { "shadow", no_argument, NULL, 'c' },
    { "no-dock-shadow", no_argument, NULL, 'C' },
    { "clear-shadow", no_argument, NULL, 'z' },
    { "fading", no_argument, NULL, 'f' },
    { "inactive-opacity", required_argument, NULL, 'i' },
    { "frame-opacity", required_argument, NULL, 'e' },
    { "daemon", no_argument, NULL, 'b' },
    { "no-dnd-shadow", no_argument, NULL, 'G' },
    { "shadow-red", required_argument, NULL, 257 },
    { "shadow-green", required_argument, NULL, 258 },
    { "shadow-blue", required_argument, NULL, 259 },
    { "inactive-opacity-override", no_argument, NULL, 260 },
    { "inactive-dim", required_argument, NULL, 261 },
    { "mark-wmwin-focused", no_argument, NULL, 262 },
    { "shadow-exclude", required_argument, NULL, 263 },
    { "mark-ovredir-focused", no_argument, NULL, 264 },
    { "no-fading-openclose", no_argument, NULL, 265 },
    { "shadow-ignore-shaped", no_argument, NULL, 266 },
    { "detect-rounded-corners", no_argument, NULL, 267 },
    { "detect-client-opacity", no_argument, NULL, 268 },
    { "refresh-rate", required_argument, NULL, 269 },
    { "vsync", required_argument, NULL, 270 },
    { "alpha-step", required_argument, NULL, 271 },
    { "dbe", no_argument, NULL, 272 },
    { "paint-on-overlay", no_argument, NULL, 273 },
    { "sw-opti", no_argument, NULL, 274 },
    { "vsync-aggressive", no_argument, NULL, 275 },
    { "use-ewmh-active-win", no_argument, NULL, 276 },
    { "respect-prop-shadow", no_argument, NULL, 277 },
    { "unredir-if-possible", no_argument, NULL, 278 },
    { "focus-exclude", required_argument, NULL, 279 },
    { "inactive-dim-fixed", no_argument, NULL, 280 },
    { "detect-transient", no_argument, NULL, 281 },
    { "detect-client-leader", no_argument, NULL, 282 },
    { "blur-background", no_argument, NULL, 283 },
    { "blur-background-frame", no_argument, NULL, 284 },
    { "blur-background-fixed", no_argument, NULL, 285 },
    { "dbus", no_argument, NULL, 286 },
    { "logpath", required_argument, NULL, 287 },
    { "invert-color-include", required_argument, NULL, 288 },
    { "opengl", no_argument, NULL, 289 },
    { "backend", required_argument, NULL, 290 },
    { "glx-no-stencil", no_argument, NULL, 291 },
    { "glx-copy-from-front", no_argument, NULL, 292 },
    { "benchmark", required_argument, NULL, 293 },
    { "benchmark-wid", required_argument, NULL, 294 },
    { "glx-use-copysubbuffermesa", no_argument, NULL, 295 },
    { "blur-background-exclude", required_argument, NULL, 296 },
    { "active-opacity", required_argument, NULL, 297 },
    { "glx-no-rebind-pixmap", no_argument, NULL, 298 },
    { "glx-swap-method", required_argument, NULL, 299 },
    { "fade-exclude", required_argument, NULL, 300 },
    { "blur-kern", required_argument, NULL, 301 },
    { "resize-damage", required_argument, NULL, 302 },
    { "glx-use-gpushader4", no_argument, NULL, 303 },
    { "opacity-rule", required_argument, NULL, 304 },
    { "shadow-exclude-reg", required_argument, NULL, 305 },
    { "paint-exclude", required_argument, NULL, 306 },
    { "xinerama-shadow-crop", no_argument, NULL, 307 },
    { "unredir-if-possible-exclude", required_argument, NULL, 308 },
    { "unredir-if-possible-delay", required_argument, NULL, 309 },
    { "write-pid-path", required_argument, NULL, 310 },
    { "vsync-use-glfinish", no_argument, NULL, 311 },
    { "xrender-sync", no_argument, NULL, 312 },
    { "xrender-sync-fence", no_argument, NULL, 313 },
    { "show-all-xerrors", no_argument, NULL, 314 },
    { "no-fading-destroyed-argb", no_argument, NULL, 315 },
    { "force-win-blend", no_argument, NULL, 316 },
    { "glx-fshader-win", required_argument, NULL, 317 },
    { "version", no_argument, NULL, 318 },
    { "no-x-selection", no_argument, NULL, 319 },
    { "no-name-pixmap", no_argument, NULL, 320 },
    { "reredir-on-root-change", no_argument, NULL, 731 },
    { "glx-reinit-on-root-change", no_argument, NULL, 732 },
    { "monitor-repaint", no_argument, NULL, 800 },
    { "diagnostics", no_argument, NULL, 801 },
    // Must terminate with a NULL entry
    { NULL, 0, NULL, 0 },
  };

  int o = 0, longopt_idx = -1;

  if (first_pass) {
    // Pre-parse the commandline arguments to check for --config and invalid
    // switches
    // Must reset optind to 0 here in case we reread the commandline
    // arguments
    optind = 1;
    while (-1 !=
        (o = getopt_long(argc, argv, shortopts, longopts, &longopt_idx))) {
      if (256 == o)
        ps->o.config_file = mstrcpy(optarg);
      else if ('d' == o)
        ps->o.display = mstrcpy(optarg);
      else if ('S' == o)
        ps->o.synchronize = true;
      else if (314 == o)
        ps->o.show_all_xerrors = true;
      else if (318 == o) {
        printf("%s\n", COMPTON_VERSION);
        exit(0);
      }
      else if (320 == o)
        ps->o.no_name_pixmap = true;
      else if ('?' == o || ':' == o)
        usage(1);
    }

    // Check for abundant positional arguments
    if (optind < argc)
      printf_errfq(1, "(): compton doesn't accept positional arguments.");

    return;
  }

  bool shadow_enable = false, fading_enable = false;
  char *lc_numeric_old = mstrcpy(setlocale(LC_NUMERIC, NULL));

  win_option_mask_t winopt_mask[NUM_WINTYPES] = {{0}};

  // Enforce LC_NUMERIC locale "C" here to make sure dots are recognized
  // instead of commas in atof().
  setlocale(LC_NUMERIC, "C");

  parse_config(ps, &shadow_enable, &fading_enable, winopt_mask);

  // Parse commandline arguments. Range checking will be done later.

  const char *deprecation_message = "has been removed. If you encounter problems "
    "without this feature, please feel free to open a bug report.";
  optind = 1;
  while (-1 !=
      (o = getopt_long(argc, argv, shortopts, longopts, &longopt_idx))) {
    long val = 0;
    switch (o) {
#define P_CASEBOOL(idx, option) case idx: ps->o.option = true; break
#define P_CASELONG(idx, option) \
      case idx: \
        if (!parse_long(optarg, &val)) exit(1); \
        ps->o.option = val; \
        break

      // Short options
      case 'h':
        usage(0);
        break;
      case 'd':
      case 'S':
      case 314:
      case 318:
      case 320:
        break;
      P_CASELONG('D', fade_delta);
      case 'I':
        ps->o.fade_in_step = normalize_d(atof(optarg)) * OPAQUE;
        break;
      case 'O':
        ps->o.fade_out_step = normalize_d(atof(optarg)) * OPAQUE;
        break;
      case 'c':
        shadow_enable = true;
        break;
      case 'C':
        winopt_mask[WINTYPE_DOCK].shadow = true;
        ps->o.wintype_option[WINTYPE_DOCK].shadow = true;
        break;
      case 'G':
        winopt_mask[WINTYPE_DND].shadow = true;
        ps->o.wintype_option[WINTYPE_DND].shadow = true;
        break;
      case 'm':;
        double tmp;
        tmp = normalize_d(atof(optarg));
        winopt_mask[WINTYPE_DROPDOWN_MENU].opacity = true;
        winopt_mask[WINTYPE_POPUP_MENU].opacity = true;
        ps->o.wintype_option[WINTYPE_POPUP_MENU].opacity = tmp;
        ps->o.wintype_option[WINTYPE_DROPDOWN_MENU].opacity = tmp;
        break;
      case 'f':
      case 'F':
        fading_enable = true;
        break;
      P_CASELONG('r', shadow_radius);
      case 'o':
        ps->o.shadow_opacity = atof(optarg);
        break;
      P_CASELONG('l', shadow_offset_x);
      P_CASELONG('t', shadow_offset_y);
      case 'i':
        ps->o.inactive_opacity = (normalize_d(atof(optarg)) * OPAQUE);
        break;
      case 'e':
        ps->o.frame_opacity = atof(optarg);
        break;
      case 'z':
        printf_errf("(): clear-shadow is removed, shadows are automatically cleared now.\n"
          "If you want to prevent shadow from been cleared under certain types of windows,\n"
          "you can use the \"full-shadow\" per window type option.");
        break;
      case 'n':
      case 'a':
      case 's':
        printf_errfq(1, "(): -n, -a, and -s have been removed.");
        break;
      P_CASEBOOL('b', fork_after_register);
      // Long options
      case 256:
        // --config
        break;
      case 257:
        // --shadow-red
        ps->o.shadow_red = atof(optarg);
        break;
      case 258:
        // --shadow-green
        ps->o.shadow_green = atof(optarg);
        break;
      case 259:
        // --shadow-blue
        ps->o.shadow_blue = atof(optarg);
        break;
      P_CASEBOOL(260, inactive_opacity_override);
      case 261:
        // --inactive-dim
        ps->o.inactive_dim = atof(optarg);
        break;
      P_CASEBOOL(262, mark_wmwin_focused);
      case 263:
        // --shadow-exclude
        condlst_add(ps, &ps->o.shadow_blacklist, optarg);
        break;
      P_CASEBOOL(264, mark_ovredir_focused);
      P_CASEBOOL(265, no_fading_openclose);
      P_CASEBOOL(266, shadow_ignore_shaped);
      P_CASEBOOL(267, detect_rounded_corners);
      P_CASEBOOL(268, detect_client_opacity);
      P_CASELONG(269, refresh_rate);
      case 270:
        // --vsync
        if (!parse_vsync(ps, optarg))
          exit(1);
        break;
      case 271:
        // --alpha-step
        printf_errf("(): --alpha-step has been removed, compton now tries to make use"
          " of all alpha values");
        break;
      case 272:
        printf_errf("(): use of --dbe is deprecated");
        break;
      case 273:
        printf_errf("(): --paint-on-overlay has been removed, and is enabled when "
          "possible");
        break;
      P_CASEBOOL(274, sw_opti);
      P_CASEBOOL(275, vsync_aggressive);
      P_CASEBOOL(276, use_ewmh_active_win);
      P_CASEBOOL(277, respect_prop_shadow);
      P_CASEBOOL(278, unredir_if_possible);
      case 279:
        // --focus-exclude
        condlst_add(ps, &ps->o.focus_blacklist, optarg);
        break;
      P_CASEBOOL(280, inactive_dim_fixed);
      P_CASEBOOL(281, detect_transient);
      P_CASEBOOL(282, detect_client_leader);
      P_CASEBOOL(283, blur_background);
      P_CASEBOOL(284, blur_background_frame);
      P_CASEBOOL(285, blur_background_fixed);
      P_CASEBOOL(286, dbus);
      case 287:
        // --logpath
        ps->o.logpath = mstrcpy(optarg);
        break;
      case 288:
        // --invert-color-include
        condlst_add(ps, &ps->o.invert_color_list, optarg);
        break;
      case 289:
        // --opengl
        ps->o.backend = BKEND_GLX;
        break;
      case 290:
        // --backend
        if (!parse_backend(ps, optarg))
          exit(1);
        break;
      P_CASEBOOL(291, glx_no_stencil);
      case 292:
        printf_errf("(): --glx-copy-from-front %s", deprecation_message);
        break;
      P_CASELONG(293, benchmark);
      case 294:
        // --benchmark-wid
        ps->o.benchmark_wid = strtol(optarg, NULL, 0);
        break;
      case 295:
        printf_errf("(): --glx-use-copysubbuffermesa %s", deprecation_message);
        break;
      case 296:
        // --blur-background-exclude
        condlst_add(ps, &ps->o.blur_background_blacklist, optarg);
        break;
      case 297:
        // --active-opacity
        ps->o.active_opacity = (normalize_d(atof(optarg)) * OPAQUE);
        break;
      P_CASEBOOL(298, glx_no_rebind_pixmap);
      case 299:
        // --glx-swap-method
        if (!parse_glx_swap_method(ps, optarg))
          exit(1);
        break;
      case 300:
        // --fade-exclude
        condlst_add(ps, &ps->o.fade_blacklist, optarg);
        break;
      case 301:
        // --blur-kern
        if (!parse_conv_kern_lst(ps, optarg, ps->o.blur_kerns, MAX_BLUR_PASS))
          exit(1);
        break;
      P_CASELONG(302, resize_damage);
      P_CASEBOOL(303, glx_use_gpushader4);
      case 304:
        // --opacity-rule
        if (!parse_rule_opacity(ps, optarg))
          exit(1);
        break;
      case 305:
        // --shadow-exclude-reg
        ps->o.shadow_exclude_reg_str = strdup(optarg);
        printf_err("--shadow-exclude-reg is deprecated.\n"
                   "You are likely better off using --shadow-exclude anyway");
        break;
      case 306:
        // --paint-exclude
        condlst_add(ps, &ps->o.paint_blacklist, optarg);
        break;
      P_CASEBOOL(307, xinerama_shadow_crop);
      case 308:
        // --unredir-if-possible-exclude
        condlst_add(ps, &ps->o.unredir_if_possible_blacklist, optarg);
        break;
      P_CASELONG(309, unredir_if_possible_delay);
      case 310:
        // --write-pid-path
        ps->o.write_pid_path = mstrcpy(optarg);
        break;
      P_CASEBOOL(311, vsync_use_glfinish);
      P_CASEBOOL(312, xrender_sync);
      P_CASEBOOL(313, xrender_sync_fence);
      P_CASEBOOL(315, no_fading_destroyed_argb);
      P_CASEBOOL(316, force_win_blend);
      case 317:
        ps->o.glx_fshader_win_str = mstrcpy(optarg);
        printf_errf("(): --glx-fshader-win is being deprecated, and might be\n"
          " removed in the future. If you really need this feature, please report\n"
          "an issue to let us know\n");
        break;
      P_CASEBOOL(319, no_x_selection);
      P_CASEBOOL(731, reredir_on_root_change);
      P_CASEBOOL(732, glx_reinit_on_root_change);
      P_CASEBOOL(800, monitor_repaint);
      case 801:
        ps->o.print_diagnostics = true;
        break;
      default:
        usage(1);
        break;
#undef P_CASEBOOL
    }
  }

  // Restore LC_NUMERIC
  setlocale(LC_NUMERIC, lc_numeric_old);
  free(lc_numeric_old);

  if (ps->o.monitor_repaint && ps->o.backend != BKEND_XRENDER)
    printf_errf("(): --monitor-repaint has no effect when backend is not xrender");

  // Range checking and option assignments
  ps->o.fade_delta = max_i(ps->o.fade_delta, 1);
  ps->o.shadow_radius = max_i(ps->o.shadow_radius, 0);
  ps->o.shadow_red = normalize_d(ps->o.shadow_red);
  ps->o.shadow_green = normalize_d(ps->o.shadow_green);
  ps->o.shadow_blue = normalize_d(ps->o.shadow_blue);
  ps->o.inactive_dim = normalize_d(ps->o.inactive_dim);
  ps->o.frame_opacity = normalize_d(ps->o.frame_opacity);
  ps->o.shadow_opacity = normalize_d(ps->o.shadow_opacity);
  ps->o.refresh_rate = normalize_i_range(ps->o.refresh_rate, 0, 300);

  // Apply default wintype options that are dependent on global options
  for (int i = 0; i < NUM_WINTYPES; i++) {
    auto wo = &ps->o.wintype_option[i];
    auto mask = &winopt_mask[i];
    if (!mask->shadow) {
      wo->shadow = shadow_enable;
      mask->shadow = true;
    }
    if (!mask->fade) {
      wo->fade = fading_enable;
      mask->fade = true;
    }
  }

  // --blur-background-frame implies --blur-background
  if (ps->o.blur_background_frame)
    ps->o.blur_background = true;

  if (ps->o.xrender_sync_fence)
    ps->o.xrender_sync = true;

  // Other variables determined by options

  // Determine whether we need to track focus changes
  if (ps->o.inactive_opacity != ps->o.active_opacity ||
      ps->o.inactive_dim) {
    ps->o.track_focus = true;
  }

  // Determine whether we track window grouping
  if (ps->o.detect_transient || ps->o.detect_client_leader) {
    ps->o.track_leader = true;
  }

  // Fill default blur kernel
  if (ps->o.blur_background && !ps->o.blur_kerns[0]) {
    // Convolution filter parameter (box blur)
    // gaussian or binomial filters are definitely superior, yet looks
    // like they aren't supported as of xorg-server-1.13.0
    static const xcb_render_fixed_t convolution_blur[] = {
      // Must convert to XFixed with DOUBLE_TO_XFIXED()
      // Matrix size
      DOUBLE_TO_XFIXED(3), DOUBLE_TO_XFIXED(3),
      // Matrix
      DOUBLE_TO_XFIXED(1), DOUBLE_TO_XFIXED(1), DOUBLE_TO_XFIXED(1),
      DOUBLE_TO_XFIXED(1), DOUBLE_TO_XFIXED(1), DOUBLE_TO_XFIXED(1),
      DOUBLE_TO_XFIXED(1), DOUBLE_TO_XFIXED(1), DOUBLE_TO_XFIXED(1),
    };
    ps->o.blur_kerns[0] = malloc(sizeof(convolution_blur));
    if (!ps->o.blur_kerns[0]) {
      printf_errf("(): Failed to allocate memory for convolution kernel.");
      exit(1);
    }
    memcpy(ps->o.blur_kerns[0], &convolution_blur, sizeof(convolution_blur));
  }

  rebuild_shadow_exclude_reg(ps);

  if (ps->o.resize_damage < 0)
    printf_errf("(): Negative --resize-damage does not work correctly.");
}

/**
 * Fetch all required atoms and save them to a session.
 */
static void
init_atoms(session_t *ps) {
  ps->atom_opacity = get_atom(ps, "_NET_WM_WINDOW_OPACITY");
  ps->atom_frame_extents = get_atom(ps, "_NET_FRAME_EXTENTS");
  ps->atom_client = get_atom(ps, "WM_STATE");
  ps->atom_name = XCB_ATOM_WM_NAME;
  ps->atom_name_ewmh = get_atom(ps, "_NET_WM_NAME");
  ps->atom_class = XCB_ATOM_WM_CLASS;
  ps->atom_role = get_atom(ps, "WM_WINDOW_ROLE");
  ps->atom_transient = XCB_ATOM_WM_TRANSIENT_FOR;
  ps->atom_client_leader = get_atom(ps, "WM_CLIENT_LEADER");
  ps->atom_ewmh_active_win = get_atom(ps, "_NET_ACTIVE_WINDOW");
  ps->atom_compton_shadow = get_atom(ps, "_COMPTON_SHADOW");

  ps->atom_win_type = get_atom(ps, "_NET_WM_WINDOW_TYPE");
  ps->atoms_wintypes[WINTYPE_UNKNOWN] = 0;
  ps->atoms_wintypes[WINTYPE_DESKTOP] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_DESKTOP");
  ps->atoms_wintypes[WINTYPE_DOCK] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_DOCK");
  ps->atoms_wintypes[WINTYPE_TOOLBAR] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_TOOLBAR");
  ps->atoms_wintypes[WINTYPE_MENU] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_MENU");
  ps->atoms_wintypes[WINTYPE_UTILITY] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_UTILITY");
  ps->atoms_wintypes[WINTYPE_SPLASH] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_SPLASH");
  ps->atoms_wintypes[WINTYPE_DIALOG] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_DIALOG");
  ps->atoms_wintypes[WINTYPE_NORMAL] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_NORMAL");
  ps->atoms_wintypes[WINTYPE_DROPDOWN_MENU] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU");
  ps->atoms_wintypes[WINTYPE_POPUP_MENU] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_POPUP_MENU");
  ps->atoms_wintypes[WINTYPE_TOOLTIP] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_TOOLTIP");
  ps->atoms_wintypes[WINTYPE_NOTIFY] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_NOTIFICATION");
  ps->atoms_wintypes[WINTYPE_COMBO] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_COMBO");
  ps->atoms_wintypes[WINTYPE_DND] = get_atom(ps,
      "_NET_WM_WINDOW_TYPE_DND");
}

/**
 * Update refresh rate info with X Randr extension.
 */
static void
update_refresh_rate(session_t *ps) {
  xcb_randr_get_screen_info_reply_t *randr_info =
    xcb_randr_get_screen_info_reply(ps->c,
        xcb_randr_get_screen_info(ps->c, ps->root), NULL);

  if (!randr_info)
    return;
  ps->refresh_rate = randr_info->rate;
  free(randr_info);

  if (ps->refresh_rate)
    ps->refresh_intv = US_PER_SEC / ps->refresh_rate;
  else
    ps->refresh_intv = 0;
}

/**
 * Initialize refresh-rated based software optimization.
 *
 * @return true for success, false otherwise
 */
static bool
swopti_init(session_t *ps) {
  // Prepare refresh rate
  // Check if user provides one
  ps->refresh_rate = ps->o.refresh_rate;
  if (ps->refresh_rate)
    ps->refresh_intv = US_PER_SEC / ps->refresh_rate;

  // Auto-detect refresh rate otherwise
  if (!ps->refresh_rate && ps->randr_exists) {
    update_refresh_rate(ps);
  }

  // Turn off vsync_sw if we can't get the refresh rate
  if (!ps->refresh_rate)
    return false;

  return true;
}

/**
 * Modify a struct timeval timeout value to render at a fixed pace.
 *
 * @param ps current session
 * @param[in,out] ptv pointer to the timeout
 */
static double
swopti_handle_timeout(session_t *ps) {
  if (!ps->refresh_intv)
    return 0;

  // Get the microsecond offset of the time when the we reach the timeout
  // I don't think a 32-bit long could overflow here.
  long offset = (get_time_timeval().tv_usec - ps->paint_tm_offset) % ps->refresh_intv;
  if (offset < 0)
    offset += ps->refresh_intv;

  // If the target time is sufficiently close to a refresh time, don't add
  // an offset, to avoid certain blocking conditions.
  if (offset < SWOPTI_TOLERANCE
      || offset > ps->refresh_intv - SWOPTI_TOLERANCE)
    return 0;

  // Add an offset so we wait until the next refresh after timeout
  return (ps->refresh_intv - offset) / 1e6;
}

/**
 * Initialize DRM VSync.
 *
 * @return true for success, false otherwise
 */
static bool
vsync_drm_init(session_t *ps) {
#ifdef CONFIG_VSYNC_DRM
  // Should we always open card0?
  if (ps->drm_fd < 0 && (ps->drm_fd = open("/dev/dri/card0", O_RDWR)) < 0) {
    printf_errf("(): Failed to open device.");
    return false;
  }

  if (vsync_drm_wait(ps))
    return false;

  return true;
#else
  printf_errf("(): Program not compiled with DRM VSync support.");
  return false;
#endif
}

#ifdef CONFIG_VSYNC_DRM
/**
 * Wait for next VSync, DRM method.
 *
 * Stolen from: https://github.com/MythTV/mythtv/blob/master/mythtv/libs/libmythtv/vsync.cpp
 */
static int
vsync_drm_wait(session_t *ps) {
  int ret = -1;
  drm_wait_vblank_t vbl;

  vbl.request.type = _DRM_VBLANK_RELATIVE,
  vbl.request.sequence = 1;

  do {
     ret = ioctl(ps->drm_fd, DRM_IOCTL_WAIT_VBLANK, &vbl);
     vbl.request.type &= ~_DRM_VBLANK_RELATIVE;
  } while (ret && errno == EINTR);

  if (ret)
    fprintf(stderr, "vsync_drm_wait(): VBlank ioctl did not work, "
        "unimplemented in this drmver?\n");

  return ret;

}
#endif

/**
 * Initialize OpenGL VSync.
 *
 * Stolen from: http://git.tuxfamily.org/?p=ccm/cairocompmgr.git;a=commitdiff;h=efa4ceb97da501e8630ca7f12c99b1dce853c73e
 * Possible original source: http://www.inb.uni-luebeck.de/~boehme/xvideo_sync.html
 *
 * @return true for success, false otherwise
 */
static bool
vsync_opengl_init(session_t *ps) {
#ifdef CONFIG_OPENGL
  if (!ensure_glx_context(ps))
    return false;

  if (!glx_hasglxext(ps, "GLX_SGI_video_sync")) {
    printf_errf("(): Your driver doesn't support SGI_video_sync, giving up.");
    return false;
  }

  // Get video sync functions
  if (!ps->psglx->glXGetVideoSyncSGI)
    ps->psglx->glXGetVideoSyncSGI = (f_GetVideoSync)
      glXGetProcAddress((const GLubyte *) "glXGetVideoSyncSGI");
  if (!ps->psglx->glXWaitVideoSyncSGI)
    ps->psglx->glXWaitVideoSyncSGI = (f_WaitVideoSync)
      glXGetProcAddress((const GLubyte *) "glXWaitVideoSyncSGI");
  if (!ps->psglx->glXWaitVideoSyncSGI || !ps->psglx->glXGetVideoSyncSGI) {
    printf_errf("(): Failed to get glXWait/GetVideoSyncSGI function.");
    return false;
  }

  return true;
#else
  printf_errf("(): Program not compiled with OpenGL VSync support.");
  return false;
#endif
}

static bool
vsync_opengl_oml_init(session_t *ps) {
#ifdef CONFIG_OPENGL
  if (!ensure_glx_context(ps))
    return false;

  if (!glx_hasglxext(ps, "GLX_OML_sync_control")) {
    printf_errf("(): Your driver doesn't support OML_sync_control, giving up.");
    return false;
  }

  // Get video sync functions
  if (!ps->psglx->glXGetSyncValuesOML)
    ps->psglx->glXGetSyncValuesOML = (f_GetSyncValuesOML)
      glXGetProcAddress ((const GLubyte *) "glXGetSyncValuesOML");
  if (!ps->psglx->glXWaitForMscOML)
    ps->psglx->glXWaitForMscOML = (f_WaitForMscOML)
      glXGetProcAddress ((const GLubyte *) "glXWaitForMscOML");
  if (!ps->psglx->glXGetSyncValuesOML || !ps->psglx->glXWaitForMscOML) {
    printf_errf("(): Failed to get OML_sync_control functions.");
    return false;
  }

  return true;
#else
  printf_errf("(): Program not compiled with OpenGL VSync support.");
  return false;
#endif
}

static bool
vsync_opengl_swc_swap_interval(session_t *ps, unsigned int interval) {
  if (!ensure_glx_context(ps))
    return false;

  if (!ps->psglx->glXSwapIntervalProc && !ps->psglx->glXSwapIntervalMESAProc) {
    if (glx_hasglxext(ps, "GLX_MESA_swap_control")) {
      ps->psglx->glXSwapIntervalMESAProc = (f_SwapIntervalMESA)
        glXGetProcAddress ((const GLubyte *) "glXSwapIntervalMESA");
    } else if (glx_hasglxext(ps, "GLX_SGI_swap_control")) {
      ps->psglx->glXSwapIntervalProc = (f_SwapIntervalSGI)
        glXGetProcAddress ((const GLubyte *) "glXSwapIntervalSGI");
    } else {
      printf_errf("(): Your driver doesn't support SGI_swap_control nor MESA_swap_control, giving up.");
      return false;
    }
  }

  if (ps->psglx->glXSwapIntervalMESAProc)
    ps->psglx->glXSwapIntervalMESAProc(interval);
  else if (ps->psglx->glXSwapIntervalProc)
    ps->psglx->glXSwapIntervalProc(interval);
  else
    return false;

  return true;
}

static bool
vsync_opengl_swc_init(session_t *ps) {
#ifdef CONFIG_OPENGL
  if (!bkend_use_glx(ps)) {
    printf_errf("(): OpenGL swap control requires the GLX backend.");
    return false;
  }

  if (!vsync_opengl_swc_swap_interval(ps, 1)) {
    printf_errf("(): Failed to load a swap control extension.");
    return false;
  }

  return true;
#else
  printf_errf("(): Program not compiled with OpenGL VSync support.");
  return false;
#endif
}

static bool
vsync_opengl_mswc_init(session_t *ps) {
  printf_errf("(): opengl-mswc is deprecated, please use opengl-swc instead.");
  return vsync_opengl_swc_init(ps);
}

#ifdef CONFIG_OPENGL
/**
 * Wait for next VSync, OpenGL method.
 */
static int
vsync_opengl_wait(session_t *ps) {
  unsigned vblank_count = 0;

  ps->psglx->glXGetVideoSyncSGI(&vblank_count);
  ps->psglx->glXWaitVideoSyncSGI(2, (vblank_count + 1) % 2, &vblank_count);
  // I see some code calling glXSwapIntervalSGI(1) afterwards, is it required?

  return 0;
}

/**
 * Wait for next VSync, OpenGL OML method.
 *
 * https://mail.gnome.org/archives/clutter-list/2012-November/msg00031.html
 */
static int
vsync_opengl_oml_wait(session_t *ps) {
  int64_t ust = 0, msc = 0, sbc = 0;

  ps->psglx->glXGetSyncValuesOML(ps->dpy, ps->reg_win, &ust, &msc, &sbc);
  ps->psglx->glXWaitForMscOML(ps->dpy, ps->reg_win, 0, 2, (msc + 1) % 2,
      &ust, &msc, &sbc);

  return 0;
}

static void
vsync_opengl_swc_deinit(session_t *ps) {
  vsync_opengl_swc_swap_interval(ps, 0);
}
#endif

/**
 * Initialize current VSync method.
 */
bool
vsync_init(session_t *ps) {
  // Mesa turns on swap control by default, undo that
  if (bkend_use_glx(ps))
    vsync_opengl_swc_swap_interval(ps, 0);

  if (ps->o.vsync && VSYNC_FUNCS_INIT[ps->o.vsync]
      && !VSYNC_FUNCS_INIT[ps->o.vsync](ps)) {
    ps->o.vsync = VSYNC_NONE;
    return false;
  }
  else
    return true;
}

/**
 * Wait for next VSync.
 */
static void
vsync_wait(session_t *ps) {
  if (!ps->o.vsync)
    return;

  if (VSYNC_FUNCS_WAIT[ps->o.vsync])
    VSYNC_FUNCS_WAIT[ps->o.vsync](ps);
}

/**
 * Deinitialize current VSync method.
 */
void
vsync_deinit(session_t *ps) {
  if (ps->o.vsync && VSYNC_FUNCS_DEINIT[ps->o.vsync])
    VSYNC_FUNCS_DEINIT[ps->o.vsync](ps);
}

/**
 * Pregenerate alpha pictures.
 */
static void
init_alpha_picts(session_t *ps) {
  ps->alpha_picts = malloc(sizeof(xcb_render_picture_t) * (MAX_ALPHA+1));

  for (int i = 0; i <= MAX_ALPHA; ++i) {
    double o = (double) i / MAX_ALPHA;
    ps->alpha_picts[i] = solid_picture(ps, false, o, 0, 0, 0);
    assert(ps->alpha_picts[i] != None);
  }
}

/**
 * Initialize X composite overlay window.
 */
static bool
init_overlay(session_t *ps) {
  xcb_composite_get_overlay_window_reply_t *reply =
    xcb_composite_get_overlay_window_reply(ps->c,
        xcb_composite_get_overlay_window(ps->c, ps->root), NULL);
  if (reply) {
    ps->overlay = reply->overlay_win;
    free(reply);
  } else {
    ps->overlay = XCB_NONE;
  }
  if (ps->overlay) {
    // Set window region of the overlay window, code stolen from
    // compiz-0.8.8
    xcb_generic_error_t *e;
    e = XCB_SYNCED_VOID(xcb_shape_mask, ps->c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING,
      ps->overlay, 0, 0, 0);
    if (e) {
      printf_errf("(): failed to set the bounding shape of overlay, giving up.");
      exit(1);
    }
    e = XCB_SYNCED_VOID(xcb_shape_rectangles, ps->c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT,
      XCB_CLIP_ORDERING_UNSORTED, ps->overlay, 0, 0, 0, NULL);
    if (e) {
      printf_errf("(): failed to set the input shape of overlay, giving up.");
      exit(1);
    }

    // Listen to Expose events on the overlay
    xcb_change_window_attributes(ps->c, ps->overlay, XCB_CW_EVENT_MASK,
        (const uint32_t[]) { XCB_EVENT_MASK_EXPOSURE });

    // Retrieve DamageNotify on root window if we are painting on an
    // overlay
    // root_damage = XDamageCreate(ps->dpy, root, XDamageReportNonEmpty);

    // Unmap overlay, firstly. But this typically does not work because
    // the window isn't created yet.
    // xcb_unmap_window(c, ps->overlay);
    // XFlush(ps->dpy);
  }
  else
    fprintf(stderr, "Cannot get X Composite overlay window. Falling "
        "back to painting on root window.\n");
#ifdef DEBUG_REDIR
  printf_dbgf("(): overlay = %#010lx\n", ps->overlay);
#endif

  return ps->overlay;
}

/**
 * Query needed X Render / OpenGL filters to check for their existence.
 */
static bool
init_filters(session_t *ps) {
  // Blur filter
  if (ps->o.blur_background || ps->o.blur_background_frame) {
    switch (ps->o.backend) {
      case BKEND_XRENDER:
      case BKEND_XR_GLX_HYBRID:
        {
          // Query filters
          xcb_render_query_filters_reply_t *pf = xcb_render_query_filters_reply(ps->c,
              xcb_render_query_filters(ps->c, get_tgt_window(ps)), NULL);
          if (pf) {
            xcb_str_iterator_t iter = xcb_render_query_filters_filters_iterator(pf);
            for (; iter.rem; xcb_str_next(&iter)) {
              // Convolution filter
              if (strlen(XRFILTER_CONVOLUTION) == xcb_str_name_length(iter.data)
                  && !memcmp(XRFILTER_CONVOLUTION, xcb_str_name(iter.data), strlen(XRFILTER_CONVOLUTION)))
                ps->xrfilter_convolution_exists = true;
            }
            free(pf);
          }

          // Turn features off if any required filter is not present
          if (!ps->xrfilter_convolution_exists) {
            printf_errf("(): X Render convolution filter unsupported by your X server. Background blur is not possible.");
            return false;
          }
          break;
        }
#ifdef CONFIG_OPENGL
      case BKEND_GLX:
        return glx_init_blur(ps);
#endif
      default:
        assert(false);
    }
  }

  return true;
}

/**
 * Redirect all windows.
 */
static void
redir_start(session_t *ps) {
  if (!ps->redirected) {
#ifdef DEBUG_REDIR
    print_timestamp(ps);
    printf_dbgf("(): Screen redirected.\n");
#endif

    // Map overlay window. Done firstly according to this:
    // https://bugzilla.gnome.org/show_bug.cgi?id=597014
    if (ps->overlay)
      xcb_map_window(ps->c, ps->overlay);

    xcb_composite_redirect_subwindows(ps->c, ps->root, XCB_COMPOSITE_REDIRECT_MANUAL);

    /*
    // Unredirect GL context window as this may have an effect on VSync:
    // < http://dri.freedesktop.org/wiki/CompositeSwap >
    xcb_composite_unredirect_window(c, ps->reg_win, XCB_COMPOSITE_REDIRECT_MANUAL);
    if (ps->o.paint_on_overlay && ps->overlay) {
      xcb_composite_unredirect_window(c, ps->overlay,
          XCB_COMPOSITE_REDIRECT_MANUAL);
    } */

    // Must call XSync() here
    x_sync(ps->c);

    ps->redirected = true;

    // Repaint the whole screen
    force_repaint(ps);
  }
}

/**
 * Unredirect all windows.
 */
static void
redir_stop(session_t *ps) {
  if (ps->redirected) {
#ifdef DEBUG_REDIR
    print_timestamp(ps);
    printf_dbgf("(): Screen unredirected.\n");
#endif
    // Destroy all Pictures as they expire once windows are unredirected
    // If we don't destroy them here, looks like the resources are just
    // kept inaccessible somehow
    for (win *w = ps->list; w; w = w->next) {
      free_paint(ps, &w->paint);
      free_fence(ps, &w->fence);
    }

    xcb_composite_unredirect_subwindows(ps->c, ps->root, XCB_COMPOSITE_REDIRECT_MANUAL);
    // Unmap overlay window
    if (ps->overlay)
      xcb_unmap_window(ps->c, ps->overlay);

    // Must call XSync() here
    x_sync(ps->c);

    ps->redirected = false;
  }
}

// Handle queued events before we go to sleep
static void
handle_queued_x_events(EV_P_ ev_prepare *w, int revents) {
  session_t *ps = session_ptr(w, event_check);
  xcb_generic_event_t *ev;
  while ((ev = xcb_poll_for_queued_event(ps->c))) {
    ev_handle(ps, ev);
    free(ev);
  };
  XFlush(ps->dpy);
  xcb_flush(ps->c);

  int err = xcb_connection_has_error(ps->c);
  if (err) {
    printf_errfq(1, "(): X11 server connection broke (error %d)", err);
  }
}

/**
 * Unredirection timeout callback.
 */
static void
tmout_unredir_callback(EV_P_ ev_timer *w, int revents) {
  session_t *ps = session_ptr(w, unredir_timer);
  ps->tmout_unredir_hit = true;
  queue_redraw(ps);
}

static void
fade_timer_callback(EV_P_ ev_timer *w, int revents) {
  session_t *ps = session_ptr(w, fade_timer);
  queue_redraw(ps);
}

static void
_draw_callback(EV_P_ session_t *ps, int revents) {
  if (ps->o.benchmark) {
    if (ps->o.benchmark_wid) {
      win *wi = find_win(ps, ps->o.benchmark_wid);
      if (!wi) {
        printf_errf("(): Couldn't find specified benchmark window.");
        exit(1);
      }
      add_damage_from_win(ps, wi);
    }
    else {
      force_repaint(ps);
    }
  }

  ps->fade_running = false;
  win *t = paint_preprocess(ps, ps->list);
  ps->tmout_unredir_hit = false;

  // Start/stop fade timer depends on whether window are fading
  if (!ps->fade_running && ev_is_active(&ps->fade_timer))
    ev_timer_stop(ps->loop, &ps->fade_timer);
  else if (ps->fade_running && !ev_is_active(&ps->fade_timer)) {
    ev_timer_set(&ps->fade_timer, fade_timeout(ps), 0);
    ev_timer_start(ps->loop, &ps->fade_timer);
  }

  // If the screen is unredirected, free all_damage to stop painting
  if (!ps->redirected || ps->o.stoppaint_force == ON)
    pixman_region32_clear(&ps->all_damage);

  if (pixman_region32_not_empty(&ps->all_damage)) {
    region_t all_damage_orig, *region_real = NULL;
    pixman_region32_init(&all_damage_orig);

    // keep a copy of non-resized all_damage for region_real
    if (ps->o.resize_damage > 0) {
      copy_region(&all_damage_orig, &ps->all_damage);
      resize_region(ps, &ps->all_damage, ps->o.resize_damage);
      region_real = &all_damage_orig;
    }

    static int paint = 0;
    paint_all(ps, &ps->all_damage, region_real, t);

    pixman_region32_clear(&ps->all_damage);
    pixman_region32_fini(&all_damage_orig);

    paint++;
    if (ps->o.benchmark && paint >= ps->o.benchmark)
      exit(0);
  }

  if (!ps->fade_running)
    ps->fade_time = 0L;

  ps->redraw_needed = false;
}

static void
draw_callback(EV_P_ ev_idle *w, int revents) {
  // This function is not used if we are using --swopti
  session_t *ps = session_ptr(w, draw_idle);

  _draw_callback(EV_A_ ps, revents);

  // Don't do painting non-stop unless we are in benchmark mode
  if (!ps->o.benchmark)
    ev_idle_stop(ps->loop, &ps->draw_idle);
}

static void
delayed_draw_timer_callback(EV_P_ ev_timer *w, int revents) {
  session_t *ps = session_ptr(w, delayed_draw_timer);
  _draw_callback(EV_A_ ps, revents);

  // We might have stopped the ev_idle in delayed_draw_callback,
  // so we restart it if we are in benchmark mode
  if (ps->o.benchmark)
    ev_idle_start(EV_A_ &ps->draw_idle);
}

static void
delayed_draw_callback(EV_P_ ev_idle *w, int revents) {
  // This function is only used if we are using --swopti
  session_t *ps = session_ptr(w, draw_idle);
  if (ev_is_active(&ps->delayed_draw_timer))
    return;

  double delay = swopti_handle_timeout(ps);
  if (delay < 1e-6)
    return _draw_callback(EV_A_ ps, revents);

  // This is a little bit hacky. When we get to this point in code, we need
  // to update the screen , but we will only be updating after a delay, So
  // we want to stop the ev_idle, so this callback doesn't get call repeatedly
  // during the delay, we also want queue_redraw to not restart the ev_idle.
  // So we stop ev_idle and leave ps->redraw_needed to be true. (effectively,
  // ps->redraw_needed means if redraw is needed or if draw is in progress).
  //
  // We do this anyway even if we are in benchmark mode. That means we will
  // have to restart draw_idle after the draw actually happened when we are in
  // benchmark mode.
  ev_idle_stop(ps->loop, &ps->draw_idle);

  ev_timer_set(&ps->delayed_draw_timer, delay, 0);
  ev_timer_start(ps->loop, &ps->delayed_draw_timer);
}

static void
x_event_callback(EV_P_ ev_io *w, int revents) {
  session_t *ps = (session_t *)w;
  xcb_generic_event_t *ev = xcb_poll_for_event(ps->c);
  if (ev) {
    ev_handle(ps, ev);
    free(ev);
  }
}

/**
 * Turn on the program reset flag.
 *
 * This will result in compton resetting itself after next paint.
 */
static void
reset_enable(EV_P_ ev_signal *w, int revents) {
  session_t *ps = session_ptr(w, usr1_signal);
  ev_break(ps->loop, EVBREAK_ALL);
}

/**
 * Initialize a session.
 *
 * @param ps_old old session, from which the function will take the X
 *    connection, then free it
 * @param argc number of commandline arguments
 * @param argv commandline arguments
 */
static session_t *
session_init(session_t *ps_old, int argc, char **argv) {
  static const session_t s_def = {
    .dpy = NULL,
    .scr = 0,
    .c = NULL,
    .vis = 0,
    .depth = 0,
    .root = None,
    .root_height = 0,
    .root_width = 0,
    // .root_damage = None,
    .overlay = None,
    .root_tile_fill = false,
    .root_tile_paint = PAINT_INIT,
    .tgt_picture = None,
    .tgt_buffer = PAINT_INIT,
    .reg_win = None,
    .o = {
      .config_file = NULL,
      .display = NULL,
      .backend = BKEND_XRENDER,
      .glx_no_stencil = false,
#ifdef CONFIG_OPENGL
      .glx_prog_win = GLX_PROG_MAIN_INIT,
#endif
      .mark_wmwin_focused = false,
      .mark_ovredir_focused = false,
      .fork_after_register = false,
      .synchronize = false,
      .detect_rounded_corners = false,
      .resize_damage = 0,
      .unredir_if_possible = false,
      .unredir_if_possible_blacklist = NULL,
      .unredir_if_possible_delay = 0,
      .redirected_force = UNSET,
      .stoppaint_force = UNSET,
      .dbus = false,
      .benchmark = 0,
      .benchmark_wid = None,
      .logpath = NULL,

      .refresh_rate = 0,
      .sw_opti = false,
      .vsync = VSYNC_NONE,
      .vsync_aggressive = false,

      .shadow_red = 0.0,
      .shadow_green = 0.0,
      .shadow_blue = 0.0,
      .shadow_radius = 12,
      .shadow_offset_x = -15,
      .shadow_offset_y = -15,
      .shadow_opacity = .75,
      .shadow_blacklist = NULL,
      .shadow_ignore_shaped = false,
      .respect_prop_shadow = false,
      .xinerama_shadow_crop = false,

      .fade_in_step = 0.028 * OPAQUE,
      .fade_out_step = 0.03 * OPAQUE,
      .fade_delta = 10,
      .no_fading_openclose = false,
      .no_fading_destroyed_argb = false,
      .fade_blacklist = NULL,

      .inactive_opacity = OPAQUE,
      .inactive_opacity_override = false,
      .active_opacity = OPAQUE,
      .frame_opacity = 1.0,
      .detect_client_opacity = false,

      .blur_background = false,
      .blur_background_frame = false,
      .blur_background_fixed = false,
      .blur_background_blacklist = NULL,
      .blur_kerns = { NULL },
      .inactive_dim = 0.0,
      .inactive_dim_fixed = false,
      .invert_color_list = NULL,
      .opacity_rules = NULL,

      .use_ewmh_active_win = false,
      .focus_blacklist = NULL,
      .detect_transient = false,
      .detect_client_leader = false,

      .track_focus = false,
      .track_wdata = false,
      .track_leader = false,
    },

    .time_start = { 0, 0 },
    .redirected = false,
    .alpha_picts = NULL,
    .fade_running = false,
    .fade_time = 0L,
    .ignore_head = NULL,
    .ignore_tail = NULL,
    .quit = false,

    .expose_rects = NULL,
    .size_expose = 0,
    .n_expose = 0,

    .list = NULL,
    .active_win = NULL,
    .active_leader = None,

    .black_picture = None,
    .cshadow_picture = None,
    .white_picture = None,
    .gaussian_map = NULL,
    .cgsize = 0,
    .shadow_corner = NULL,
    .shadow_top = NULL,

    .refresh_rate = 0,
    .refresh_intv = 0UL,
    .paint_tm_offset = 0L,

#ifdef CONFIG_VSYNC_DRM
    .drm_fd = -1,
#endif

    .xfixes_event = 0,
    .xfixes_error = 0,
    .damage_event = 0,
    .damage_error = 0,
    .render_event = 0,
    .render_error = 0,
    .composite_event = 0,
    .composite_error = 0,
    .composite_opcode = 0,
    .has_name_pixmap = false,
    .shape_exists = false,
    .shape_event = 0,
    .shape_error = 0,
    .randr_exists = 0,
    .randr_event = 0,
    .randr_error = 0,
#ifdef CONFIG_OPENGL
    .glx_exists = false,
    .glx_event = 0,
    .glx_error = 0,
#endif
    .xrfilter_convolution_exists = false,

    .atom_opacity = None,
    .atom_frame_extents = None,
    .atom_client = None,
    .atom_name = None,
    .atom_name_ewmh = None,
    .atom_class = None,
    .atom_role = None,
    .atom_transient = None,
    .atom_ewmh_active_win = None,
    .atom_compton_shadow = None,
    .atom_win_type = None,
    .atoms_wintypes = { 0 },
    .track_atom_lst = NULL,

#ifdef CONFIG_DBUS
    .dbus_conn = NULL,
    .dbus_service = NULL,
#endif
  };

  // Allocate a session and copy default values into it
  session_t *ps = malloc(sizeof(session_t));
  *ps = s_def;
  ps->loop = EV_DEFAULT;
  pixman_region32_init(&ps->screen_reg);
  pixman_region32_init(&ps->all_damage);
  for (int i = 0; i < CGLX_MAX_BUFFER_AGE; i ++)
    pixman_region32_init(&ps->all_damage_last[i]);

  ps_g = ps;
  ps->ignore_tail = &ps->ignore_head;
  gettimeofday(&ps->time_start, NULL);

  // First pass
  get_cfg(ps, argc, argv, true);

  // Inherit old Display if possible, primarily for resource leak checking
  if (ps_old && ps_old->dpy)
    ps->dpy = ps_old->dpy;

  // Open Display
  if (!ps->dpy) {
    ps->dpy = XOpenDisplay(ps->o.display);
    if (!ps->dpy) {
      printf_errfq(1, "(): Can't open display.");
    }
    XSetEventQueueOwner(ps->dpy, XCBOwnsEventQueue);
  }
  ps->c = XGetXCBConnection(ps->dpy);
  const xcb_query_extension_reply_t *ext_info;

  XSetErrorHandler(xerror);
  if (ps->o.synchronize) {
    XSynchronize(ps->dpy, 1);
  }

  ps->scr = DefaultScreen(ps->dpy);
  ps->root = RootWindow(ps->dpy, ps->scr);

  ps->vis = XVisualIDFromVisual(DefaultVisual(ps->dpy, ps->scr));
  ps->depth = DefaultDepth(ps->dpy, ps->scr);

  // Start listening to events on root earlier to catch all possible
  // root geometry changes
  xcb_change_window_attributes(ps->c, ps->root, XCB_CW_EVENT_MASK, (const uint32_t[]) {
      XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
      | XCB_EVENT_MASK_EXPOSURE
      | XCB_EVENT_MASK_STRUCTURE_NOTIFY
      | XCB_EVENT_MASK_PROPERTY_CHANGE });
  XFlush(ps->dpy);

  ps->root_width = DisplayWidth(ps->dpy, ps->scr);
  ps->root_height = DisplayHeight(ps->dpy, ps->scr);

  xcb_prefetch_extension_data(ps->c, &xcb_render_id);
  xcb_prefetch_extension_data(ps->c, &xcb_composite_id);
  xcb_prefetch_extension_data(ps->c, &xcb_damage_id);
  xcb_prefetch_extension_data(ps->c, &xcb_shape_id);
  xcb_prefetch_extension_data(ps->c, &xcb_xfixes_id);
  xcb_prefetch_extension_data(ps->c, &xcb_randr_id);
  xcb_prefetch_extension_data(ps->c, &xcb_xinerama_id);
  xcb_prefetch_extension_data(ps->c, &xcb_present_id);

  ext_info = xcb_get_extension_data(ps->c, &xcb_render_id);
  if (!ext_info || !ext_info->present) {
    fprintf(stderr, "No render extension\n");
    exit(1);
  }
  ps->render_event = ext_info->first_event;
  ps->render_error = ext_info->first_error;

  ext_info = xcb_get_extension_data(ps->c, &xcb_composite_id);
  if (!ext_info || !ext_info->present) {
    fprintf(stderr, "No composite extension\n");
    exit(1);
  }
  ps->composite_opcode = ext_info->major_opcode;
  ps->composite_event = ext_info->first_event;
  ps->composite_error = ext_info->first_error;

  {
    xcb_composite_query_version_reply_t *reply =
      xcb_composite_query_version_reply(ps->c,
          xcb_composite_query_version(ps->c, XCB_COMPOSITE_MAJOR_VERSION, XCB_COMPOSITE_MINOR_VERSION),
          NULL);

    if (!ps->o.no_name_pixmap
        && reply && (reply->major_version > 0 || reply->minor_version >= 2)) {
      ps->has_name_pixmap = true;
    }
    free(reply);
  }

  ext_info = xcb_get_extension_data(ps->c, &xcb_damage_id);
  if (!ext_info || !ext_info->present) {
    fprintf(stderr, "No damage extension\n");
    exit(1);
  }
  ps->damage_event = ext_info->first_event;
  ps->damage_error = ext_info->first_error;
  xcb_discard_reply(ps->c,
      xcb_damage_query_version(ps->c, XCB_DAMAGE_MAJOR_VERSION, XCB_DAMAGE_MINOR_VERSION).sequence);

  ext_info = xcb_get_extension_data(ps->c, &xcb_xfixes_id);
  if (!ext_info || !ext_info->present) {
    fprintf(stderr, "No XFixes extension\n");
    exit(1);
  }
  ps->xfixes_event = ext_info->first_event;
  ps->xfixes_error = ext_info->first_error;
  xcb_discard_reply(ps->c,
      xcb_xfixes_query_version(ps->c, XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION).sequence);

  // Build a safe representation of display name
  {
    char *display_repr = DisplayString(ps->dpy);
    if (!display_repr)
      display_repr = "unknown";
    display_repr = mstrcpy(display_repr);

    // Convert all special characters in display_repr name to underscore
    {
      char *pdisp = display_repr;

      while (*pdisp) {
        if (!isalnum(*pdisp))
          *pdisp = '_';
        ++pdisp;
      }
    }

    ps->o.display_repr = display_repr;
  }

  // Second pass
  get_cfg(ps, argc, argv, false);

  // Query X Shape
  ext_info = xcb_get_extension_data(ps->c, &xcb_shape_id);
  if (ext_info && ext_info->present) {
    ps->shape_event = ext_info->first_event;
    ps->shape_error = ext_info->first_error;
    ps->shape_exists = true;
  }

  ext_info = xcb_get_extension_data(ps->c, &xcb_randr_id);
  if (ext_info && ext_info->present) {
    ps->randr_exists = true;
    ps->randr_event = ext_info->first_event;
    ps->randr_error = ext_info->first_error;
  }

  ext_info = xcb_get_extension_data(ps->c, &xcb_present_id);
  if (ext_info && ext_info->present) {
    ps->present_exists = true;
  }

  // Query X Sync
  if (XSyncQueryExtension(ps->dpy, &ps->xsync_event, &ps->xsync_error)) {
    // TODO: Fencing may require version >= 3.0?
    int major_version_return = 0, minor_version_return = 0;
    if (XSyncInitialize(ps->dpy, &major_version_return, &minor_version_return))
      ps->xsync_exists = true;
  }

  if (!ps->xsync_exists && ps->o.xrender_sync_fence) {
    printf_errf("(): X Sync extension not found. No X Sync fence sync is "
        "possible.");
    exit(1);
  }

  // Query X RandR
  if ((ps->o.sw_opti && !ps->o.refresh_rate) || ps->o.xinerama_shadow_crop) {
    if (!ps->randr_exists) {
      printf_errf("(): No XRandR extension, automatic screen change "
          "detection impossible.");
    }
  }

  // Query X Xinerama extension
  if (ps->o.xinerama_shadow_crop) {
#ifdef CONFIG_XINERAMA
    ext_info = xcb_get_extension_data(ps->c, &xcb_xinerama_id);
    ps->xinerama_exists = ext_info && ext_info->present;
#else
    printf_errf("(): Xinerama support not compiled in.");
#endif
  }

  rebuild_screen_reg(ps);

  // Overlay must be initialized before double buffer, and before creation
  // of OpenGL context.
  init_overlay(ps);

  if (ps->o.print_diagnostics) {
    print_diagnostics(ps);
    exit(0);
  }

  // Initialize OpenGL as early as possible
  if (bkend_use_glx(ps)) {
#ifdef CONFIG_OPENGL
    if (!glx_init(ps, true))
      exit(1);
#else
    printf_errfq(1, "(): GLX backend support not compiled in.");
#endif
  }

  // Initialize window GL shader
  if (BKEND_GLX == ps->o.backend && ps->o.glx_fshader_win_str) {
#ifdef CONFIG_OPENGL
    if (!glx_load_prog_main(ps, NULL, ps->o.glx_fshader_win_str, &ps->o.glx_prog_win))
      exit(1);
#else
    printf_errf("(): GLSL supported not compiled in, can't load shader.");
    exit(1);
#endif
  }

  // Initialize software optimization
  if (ps->o.sw_opti)
    ps->o.sw_opti = swopti_init(ps);

  // Monitor screen changes if vsync_sw is enabled and we are using
  // an auto-detected refresh rate, or when Xinerama features are enabled
  if (ps->randr_exists && ((ps->o.sw_opti && !ps->o.refresh_rate)
        || ps->o.xinerama_shadow_crop))
    xcb_randr_select_input(ps->c, ps->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);

  // Initialize VSync
  if (!vsync_init(ps))
    exit(1);

  cxinerama_upd_scrs(ps);

  // Create registration window
  if (!ps->reg_win && !register_cm(ps))
    exit(1);

  init_atoms(ps);
  init_alpha_picts(ps);

  ps->gaussian_map = make_gaussian_map(ps->o.shadow_radius);
  presum_gaussian(ps, ps->gaussian_map);

  {
    xcb_render_create_picture_value_list_t pa = {
      .subwindowmode = IncludeInferiors,
    };

    ps->root_picture = x_create_picture_with_visual_and_pixmap(ps,
      ps->vis, ps->root, XCB_RENDER_CP_SUBWINDOW_MODE, &pa);
    if (ps->overlay != XCB_NONE) {
      ps->tgt_picture = x_create_picture_with_visual_and_pixmap(ps,
        ps->vis, ps->overlay, XCB_RENDER_CP_SUBWINDOW_MODE, &pa);
    } else
      ps->tgt_picture = ps->root_picture;
  }

  // Initialize filters, must be preceded by OpenGL context creation
  if (!init_filters(ps))
    exit(1);

  ps->black_picture = solid_picture(ps, true, 1, 0, 0, 0);
  ps->white_picture = solid_picture(ps, true, 1, 1, 1, 1);

  // Generates another Picture for shadows if the color is modified by
  // user
  if (!ps->o.shadow_red && !ps->o.shadow_green && !ps->o.shadow_blue) {
    ps->cshadow_picture = ps->black_picture;
  } else {
    ps->cshadow_picture = solid_picture(ps, true, 1,
        ps->o.shadow_red, ps->o.shadow_green, ps->o.shadow_blue);
  }

  ev_io_init(&ps->xiow, x_event_callback, ConnectionNumber(ps->dpy), EV_READ);
  ev_io_start(ps->loop, &ps->xiow);
  ev_init(&ps->unredir_timer, tmout_unredir_callback);
  if (ps->o.sw_opti)
    ev_idle_init(&ps->draw_idle, delayed_draw_callback);
  else
    ev_idle_init(&ps->draw_idle, draw_callback);

  ev_init(&ps->fade_timer, fade_timer_callback);
  ev_init(&ps->delayed_draw_timer, delayed_draw_timer_callback);

  // Set up SIGUSR1 signal handler to reset program
  ev_signal_init(&ps->usr1_signal, reset_enable, SIGUSR1);
  ev_signal_start(ps->loop, &ps->usr1_signal);

  // xcb can read multiple events from the socket when a request with reply is
  // made.
  //
  // Use an ev_prepare to make sure we cannot accidentally forget to handle them
  // before we go to sleep.
  //
  // If we don't drain the queue before goes to sleep (i.e. blocking on socket
  // input), we will be sleeping with events available in queue. Which might
  // cause us to block indefinitely because arrival of new events could be
  // dependent on processing of existing events (e.g. if we don't process damage
  // event and do damage subtract, new damage event won't be generated).
  //
  // So we make use of a ev_prepare handle, which is called right before libev
  // goes into sleep, to handle all the queued X events.
  ev_prepare_init(&ps->event_check, handle_queued_x_events);
  // Make sure nothing can cause xcb to read from the X socket after events are
  // handled and before we going to sleep.
  ev_set_priority(&ps->event_check, EV_MINPRI);
  ev_prepare_start(ps->loop, &ps->event_check);

  xcb_grab_server(ps->c);

  {
    xcb_window_t *children;
    int nchildren;

    xcb_query_tree_reply_t *reply = xcb_query_tree_reply(ps->c,
        xcb_query_tree(ps->c, ps->root), NULL);

    if (reply) {
      children = xcb_query_tree_children(reply);
      nchildren = xcb_query_tree_children_length(reply);
    } else {
      children = NULL;
      nchildren = 0;
    }

    for (int i = 0; i < nchildren; i++) {
      add_win(ps, children[i], i ? children[i-1] : XCB_NONE);
    }

    free(reply);
  }

  if (ps->o.track_focus) {
    recheck_focus(ps);
  }

  xcb_ungrab_server(ps->c);
  // ALWAYS flush after xcb_ungrab_server()!
  XFlush(ps->dpy);

  // Initialize DBus
  if (ps->o.dbus) {
#ifdef CONFIG_DBUS
    cdbus_init(ps);
    if (!ps->dbus_conn) {
      cdbus_destroy(ps);
      ps->o.dbus = false;
    }
#else
    printf_errfq(1, "(): DBus support not compiled in!");
#endif
  }

  // Fork to background, if asked
  if (ps->o.fork_after_register) {
    if (!fork_after(ps)) {
      session_destroy(ps);
      return NULL;
    }
  }

  // Redirect output stream
  if (ps->o.fork_after_register || ps->o.logpath)
    ostream_reopen(ps, NULL);

  write_pid(ps);

  // Free the old session
  if (ps_old)
    free(ps_old);

  return ps;
}

/**
 * Destroy a session.
 *
 * Does not close the X connection or free the <code>session_t</code>
 * structure, though.
 *
 * @param ps session to destroy
 */
static void
session_destroy(session_t *ps) {
  redir_stop(ps);

  // Stop listening to events on root window
  xcb_change_window_attributes(ps->c, ps->root, XCB_CW_EVENT_MASK,
      (const uint32_t[]) { 0 });

#ifdef CONFIG_DBUS
  // Kill DBus connection
  if (ps->o.dbus)
    cdbus_destroy(ps);

  free(ps->dbus_service);
#endif

  // Free window linked list
  {
    win *next = NULL;
    for (win *w = ps->list; w; w = next) {
      // Must be put here to avoid segfault
      next = w->next;

      if (w->a.map_state == XCB_MAP_STATE_VIEWABLE && !w->destroyed)
        win_ev_stop(ps, w);

      free_win_res(ps, w);
      free(w);
    }

    ps->list = NULL;
  }

  // Free alpha_picts
  {
    for (int i = 0; i <= MAX_ALPHA; ++i)
      free_picture(ps, &ps->alpha_picts[i]);
    free(ps->alpha_picts);
    ps->alpha_picts = NULL;
  }

  // Free blacklists
  free_wincondlst(&ps->o.shadow_blacklist);
  free_wincondlst(&ps->o.fade_blacklist);
  free_wincondlst(&ps->o.focus_blacklist);
  free_wincondlst(&ps->o.invert_color_list);
  free_wincondlst(&ps->o.blur_background_blacklist);
  free_wincondlst(&ps->o.opacity_rules);
  free_wincondlst(&ps->o.paint_blacklist);
  free_wincondlst(&ps->o.unredir_if_possible_blacklist);

  // Free tracked atom list
  {
    latom_t *next = NULL;
    for (latom_t *this = ps->track_atom_lst; this; this = next) {
      next = this->next;
      free(this);
    }

    ps->track_atom_lst = NULL;
  }

  // Free ignore linked list
  {
    ignore_t *next = NULL;
    for (ignore_t *ign = ps->ignore_head; ign; ign = next) {
      next = ign->next;

      free(ign);
    }

    // Reset head and tail
    ps->ignore_head = NULL;
    ps->ignore_tail = &ps->ignore_head;
  }

  // Free cshadow_picture and black_picture
  if (ps->cshadow_picture == ps->black_picture)
    ps->cshadow_picture = None;
  else
    free_picture(ps, &ps->cshadow_picture);

  free_picture(ps, &ps->black_picture);
  free_picture(ps, &ps->white_picture);

  // Free tgt_{buffer,picture} and root_picture
  if (ps->tgt_buffer.pict == ps->tgt_picture)
    ps->tgt_buffer.pict = None;

  if (ps->tgt_picture == ps->root_picture)
    ps->tgt_picture = None;
  else
    free_picture(ps, &ps->tgt_picture);
  free_fence(ps, &ps->tgt_buffer_fence);

  free_picture(ps, &ps->root_picture);
  free_paint(ps, &ps->tgt_buffer);

  // Free other X resources
  free_root_tile(ps);
  pixman_region32_fini(&ps->screen_reg);
  pixman_region32_fini(&ps->all_damage);
  for (int i = 0; i < CGLX_MAX_BUFFER_AGE; ++i)
    pixman_region32_fini(&ps->all_damage_last[i]);
  free(ps->expose_rects);
  free(ps->shadow_corner);
  free(ps->shadow_top);
  free(ps->gaussian_map);

  free(ps->o.config_file);
  free(ps->o.write_pid_path);
  free(ps->o.display);
  free(ps->o.display_repr);
  free(ps->o.logpath);
  for (int i = 0; i < MAX_BLUR_PASS; ++i) {
    free(ps->o.blur_kerns[i]);
    free(ps->blur_kerns_cache[i]);
  }
  free(ps->o.glx_fshader_win_str);
  free_xinerama_info(ps);
  free(ps->pictfmts);

#ifdef CONFIG_OPENGL
  glx_destroy(ps);
#endif

#ifdef CONFIG_VSYNC_DRM
  // Close file opened for DRM VSync
  if (ps->drm_fd >= 0) {
    close(ps->drm_fd);
    ps->drm_fd = -1;
  }
#endif

  // Release overlay window
  if (ps->overlay) {
    xcb_composite_release_overlay_window(ps->c, ps->overlay);
    ps->overlay = None;
  }

  // Free reg_win
  if (ps->reg_win) {
    xcb_destroy_window(ps->c, ps->reg_win);
    ps->reg_win = None;
  }

  // Flush all events
  x_sync(ps->c);
  ev_io_stop(ps->loop, &ps->xiow);

#ifdef DEBUG_XRC
  // Report about resource leakage
  xrc_report_xid();
#endif

  // Stop libev event handlers
  ev_timer_stop(ps->loop, &ps->unredir_timer);
  ev_timer_stop(ps->loop, &ps->fade_timer);
  ev_idle_stop(ps->loop, &ps->draw_idle);
  ev_prepare_stop(ps->loop, &ps->event_check);
  ev_signal_stop(ps->loop, &ps->usr1_signal);

  if (ps == ps_g)
    ps_g = NULL;
}

/*
static inline void
dump_img(session_t *ps) {
  int len = 0;
  unsigned char *d = glx_take_screenshot(ps, &len);
  write_binary_data("/tmp/dump.raw", d, len);
  free(d);
}
*/

/**
 * Do the actual work.
 *
 * @param ps current session
 */
static void
session_run(session_t *ps) {
  win *t;

  if (ps->o.sw_opti)
    ps->paint_tm_offset = get_time_timeval().tv_usec;

  t = paint_preprocess(ps, ps->list);

  if (ps->redirected)
    paint_all(ps, NULL, NULL, t);

  // In benchmark mode, we want draw_idle handler to always be active
  if (ps->o.benchmark)
    ev_idle_start(ps->loop, &ps->draw_idle);

  ev_run(ps->loop, 0);
}

static void
sigint_handler(int __attribute__((unused)) signum) {
  exit(0);
}

/**
 * The function that everybody knows.
 */
int
main(int argc, char **argv) {
  // Set locale so window names with special characters are interpreted
  // correctly
  setlocale(LC_ALL, "");

  sigset_t sigmask;
  sigemptyset(&sigmask);
  const struct sigaction int_action = {
    .sa_handler = sigint_handler,
    .sa_mask = sigmask,
    .sa_flags = 0
  };
  sigaction(SIGINT, &int_action, NULL);

  // Main loop
  session_t *ps_old = ps_g;
  bool quit = false;
  while (!quit) {
    ps_g = session_init(ps_old, argc, argv);
    if (!ps_g) {
      printf_errf("(): Failed to create new session.");
      return 1;
    }
    session_run(ps_g);
    ps_old = ps_g;
    quit = ps_g->quit;
    session_destroy(ps_g);
  }

  free(ps_g);

  return 0;
}

// vim: set et sw=2 :
