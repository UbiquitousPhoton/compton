// SPDX-License-Identifier: MIT
/*
 * Compton - a compositor for X11
 *
 * Based on `xcompmgr` - Copyright (c) 2003, Keith Packard
 *
 * Copyright (c) 2011-2013, Christopher Jeffrey
 * Copyright (c) 2019 Yuxuan Shui <yshuiv7@gmail.com>
 * See LICENSE-mit for more information.
 *
 */

#include <X11/Xlib-xcb.h>
#include <assert.h>
#include <limits.h>
#include <pixman.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/composite.h>
#include <xcb/xcb.h>

#include "backend/backend.h"
#include "backend/backend_common.h"
#include "backend/gl/gl_common.h"
#include "backend/gl/glx.h"
#include "common.h"
#include "compiler.h"
#include "config.h"
#include "log.h"
#include "region.h"
#include "utils.h"
#include "win.h"
#include "x.h"

struct _glx_image_data {
	gl_texture_t texture;
	GLXPixmap glpixmap;
	xcb_pixmap_t pixmap;
};

struct _glx_data {
	struct gl_data gl;
	Display *display;
	int screen;
	int target_win;
	int glx_event;
	int glx_error;
	GLXContext ctx;
};

struct glx_fbconfig_info *glx_find_fbconfig(Display *dpy, int screen, struct xvisual_info m) {
	log_debug("Looking for FBConfig for RGBA%d%d%d%d, depth %d", m.red_size,
	          m.blue_size, m.green_size, m.alpha_size, m.visual_depth);

	int ncfg;
	// clang-format off
	GLXFBConfig *cfg =
	    glXChooseFBConfig(dpy, screen, (int[]){
	                          GLX_RENDER_TYPE, GLX_RGBA_BIT,
	                          GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
	                          GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
	                          GLX_X_RENDERABLE, true,
	                          GLX_FRAMEBUFFER_SRGB_CAPABLE_EXT, GLX_DONT_CARE,
	                          GLX_BUFFER_SIZE, m.red_size + m.green_size +
	                                           m.blue_size + m.alpha_size,
	                          GLX_RED_SIZE, m.red_size,
	                          GLX_BLUE_SIZE, m.blue_size,
	                          GLX_GREEN_SIZE, m.green_size,
	                          GLX_ALPHA_SIZE, m.alpha_size,
	                          GLX_STENCIL_SIZE, 0,
	                          GLX_DEPTH_SIZE, 0,
	                          0
	                      }, &ncfg);
	// clang-format on

#define glXGetFBConfigAttribChecked(a, b, attr, c)                                       \
	do {                                                                             \
		if (glXGetFBConfigAttrib(a, b, attr, c)) {                               \
			log_info("Cannot get FBConfig attribute " #attr);                \
			continue;                                                        \
		}                                                                        \
	} while (0)
	int texture_tgts, y_inverted, texture_fmt;
	bool found = false;
	int min_cost = INT_MAX;
	GLXFBConfig ret;
	for (int i = 0; i < ncfg; i++) {
		int depthbuf, stencil, doublebuf, bufsize;
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_BUFFER_SIZE, &bufsize);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_DEPTH_SIZE, &depthbuf);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_STENCIL_SIZE, &stencil);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_DOUBLEBUFFER, &doublebuf);
		if (depthbuf + stencil + bufsize * (doublebuf + 1) >= min_cost) {
			continue;
		}
		int red, green, blue;
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_RED_SIZE, &red);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_BLUE_SIZE, &blue);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_GREEN_SIZE, &green);
		if (red != m.red_size || green != m.green_size || blue != m.blue_size) {
			// Color size doesn't match, this cannot work
			continue;
		}

		int rgb, rgba;
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_BIND_TO_TEXTURE_RGB_EXT, &rgb);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_BIND_TO_TEXTURE_RGBA_EXT, &rgba);
		if (!rgb && !rgba) {
			log_info("FBConfig is neither RGBA nor RGB, compton cannot "
			         "handle this setup.");
			continue;
		}

		int visual;
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_VISUAL_ID, &visual);
		if (m.visual_depth != -1 &&
		    x_get_visual_depth(XGetXCBConnection(dpy), visual) != m.visual_depth) {
			// Some driver might attach fbconfig to a GLX visual with a
			// different depth.
			//
			// (That makes total sense. - NVIDIA developers)
			continue;
		}

		// All check passed, we are using this one.
		found = true;
		ret = cfg[i];
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_BIND_TO_TEXTURE_TARGETS_EXT,
		                            &texture_tgts);
		glXGetFBConfigAttribChecked(dpy, cfg[i], GLX_Y_INVERTED_EXT, &y_inverted);

		// Prefer the texture format with matching alpha, with the other one as
		// fallback
		if (m.alpha_size) {
			texture_fmt = rgba ? GLX_TEXTURE_FORMAT_RGBA_EXT
			                   : GLX_TEXTURE_FORMAT_RGB_EXT;
		} else {
			texture_fmt =
			    rgb ? GLX_TEXTURE_FORMAT_RGB_EXT : GLX_TEXTURE_FORMAT_RGBA_EXT;
		}
		min_cost = depthbuf + stencil + bufsize * (doublebuf + 1);
	}
#undef glXGetFBConfigAttribChecked
	free(cfg);
	if (!found) {
		return NULL;
	}

	auto info = cmalloc(struct glx_fbconfig_info);
	info->cfg = ret;
	info->texture_tgts = texture_tgts;
	info->texture_fmt = texture_fmt;
	info->y_inverted = y_inverted;
	return info;
}

/**
 * Free a glx_texture_t.
 */
void glx_release_image(backend_t *base, void *image_data) {
	struct _glx_image_data *wd = image_data;
	struct _glx_data *gd = (void *)base;
	// Release binding
	if (wd->glpixmap && wd->texture.texture) {
		glBindTexture(wd->texture.target, wd->texture.texture);
		glXReleaseTexImageEXT(gd->display, wd->glpixmap, GLX_FRONT_LEFT_EXT);
		glBindTexture(wd->texture.target, 0);
	}

	// Free GLX Pixmap
	if (wd->glpixmap) {
		glXDestroyPixmap(gd->display, wd->glpixmap);
		wd->glpixmap = 0;
	}

	gl_delete_texture(wd->texture.texture);

	// Free structure itself
	free(wd);

	gl_check_err();
}

/**
 * Destroy GLX related resources.
 */
void glx_deinit(backend_t *base) {
	struct _glx_data *gd = (void *)base;

	gl_deinit(&gd->gl);

	// Destroy GLX context
	if (gd->ctx) {
		glXDestroyContext(gd->display, gd->ctx);
		gd->ctx = 0;
	}

	free(gd);
}

/**
 * Initialize OpenGL.
 */
static backend_t *glx_init(session_t *ps) {
	bool success = false;
	glxext_init(ps->dpy, ps->scr);
	auto gd = ccalloc(1, struct _glx_data);
	gd->gl.base.c = ps->c;
	gd->gl.base.root = ps->root;
	gd->display = ps->dpy;
	gd->screen = ps->scr;
	gd->target_win = ps->overlay != XCB_NONE ? ps->overlay : ps->root;

	XVisualInfo *pvis = NULL;

	// Check for GLX extension
	if (!glXQueryExtension(ps->dpy, &gd->glx_event, &gd->glx_error)) {
		log_error("No GLX extension.");
		goto end;
	}

	// Get XVisualInfo
	int nitems = 0;
	XVisualInfo vreq = {.visualid = ps->vis};
	pvis = XGetVisualInfo(ps->dpy, VisualIDMask, &vreq, &nitems);
	if (!pvis) {
		log_error("Failed to acquire XVisualInfo for current visual.");
		goto end;
	}

	// Ensure the visual is double-buffered
	int value = 0;
	if (glXGetConfig(ps->dpy, pvis, GLX_USE_GL, &value) || !value) {
		log_error("Root visual is not a GL visual.");
		goto end;
	}

	if (glXGetConfig(ps->dpy, pvis, GLX_DOUBLEBUFFER, &value) || !value) {
		log_error("Root visual is not a double buffered GL visual.");
		goto end;
	}

	// Ensure GLX_EXT_texture_from_pixmap exists
	if (!glxext.has_GLX_EXT_texture_from_pixmap) {
		log_error("GLX_EXT_texture_from_pixmap is not supported by your driver");
		goto end;
	}

	// Get GLX context
	gd->ctx = glXCreateContext(ps->dpy, pvis, NULL, GL_TRUE);

	if (!gd->ctx) {
		log_error("Failed to get GLX context.");
		goto end;
	}

	// Attach GLX context
	GLXDrawable tgt = ps->overlay;
	if (!tgt) {
		tgt = ps->root;
	}
	if (!glXMakeCurrent(ps->dpy, tgt, gd->ctx)) {
		log_error("Failed to attach GLX context.");
		goto end;
	}

#ifdef DEBUG_GLX_DEBUG_CONTEXT
	f_DebugMessageCallback p_DebugMessageCallback =
	    (f_DebugMessageCallback)glXGetProcAddress((const GLubyte *)"glDebugMessageCal"
	                                                               "lback");
	if (!p_DebugMessageCallback) {
		log_error("Failed to get glDebugMessageCallback(0.");
		goto glx_init_end;
	}
	p_DebugMessageCallback(glx_debug_msg_callback, ps);
#endif

	if (!gl_init(&gd->gl, ps)) {
		log_error("Failed to setup OpenGL");
		goto end;
	}

	success = true;

end:
	if (pvis) {
		XFree(pvis);
	}

	if (!success) {
		glx_deinit(&gd->gl.base);
		return NULL;
	}

	return &gd->gl.base;
}

static void *glx_bind_pixmap(backend_t *base, xcb_pixmap_t pixmap,
                             struct xvisual_info fmt, bool owned) {
	struct _glx_data *gd = (void *)base;
	// Retrieve pixmap parameters, if they aren't provided
	if (fmt.visual_depth > OPENGL_MAX_DEPTH) {
		log_error("Requested depth %d higher than max possible depth %d.",
		          fmt.visual_depth, OPENGL_MAX_DEPTH);
		return false;
	}

	auto r = xcb_get_geometry_reply(base->c, xcb_get_geometry(base->c, pixmap), NULL);
	if (!r) {
		log_error("Invalid pixmap %#010x", pixmap);
		return NULL;
	}

	auto wd = ccalloc(1, struct _glx_image_data);
	wd->pixmap = pixmap;
	wd->texture.width = r->width;
	wd->texture.height = r->height;
	free(r);

	auto fbcfg = glx_find_fbconfig(gd->display, gd->screen, fmt);
	if (!fbcfg) {
		log_error("Couldn't find FBConfig with requested visual %x", fmt.visual);
		goto err;
	}

	// Choose a suitable texture target for our pixmap.
	// Refer to GLX_EXT_texture_om_pixmap spec to see what are the mean
	// of the bits in texture_tgts
	GLenum tex_tgt = 0;
	if (GLX_TEXTURE_2D_BIT_EXT & fbcfg->texture_tgts && gd->gl.non_power_of_two_texture)
		tex_tgt = GLX_TEXTURE_2D_EXT;
	else if (GLX_TEXTURE_RECTANGLE_BIT_EXT & fbcfg->texture_tgts)
		tex_tgt = GLX_TEXTURE_RECTANGLE_EXT;
	else if (!(GLX_TEXTURE_2D_BIT_EXT & fbcfg->texture_tgts))
		tex_tgt = GLX_TEXTURE_RECTANGLE_EXT;
	else
		tex_tgt = GLX_TEXTURE_2D_EXT;

	log_debug("depth %d, tgt %#x, rgba %d\n", fmt.visual_depth, tex_tgt,
	          (GLX_TEXTURE_FORMAT_RGBA_EXT == fbcfg->texture_fmt));

	GLint attrs[] = {
	    GLX_TEXTURE_FORMAT_EXT,
	    fbcfg->texture_fmt,
	    GLX_TEXTURE_TARGET_EXT,
	    tex_tgt,
	    0,
	};

	wd->texture.target =
	    (GLX_TEXTURE_2D_EXT == tex_tgt ? GL_TEXTURE_2D : GL_TEXTURE_RECTANGLE);
	wd->texture.y_inverted = fbcfg->y_inverted;

	wd->glpixmap = glXCreatePixmap(gd->display, fbcfg->cfg, wd->pixmap, attrs);
	free(fbcfg);

	if (!wd->glpixmap) {
		log_error("Failed to create glpixmap for pixmap %#010x", pixmap);
		goto err;
	}

	// Create texture
	wd->texture.texture = gl_new_texture(wd->texture.target);
	wd->texture.opacity = 1;
	wd->texture.depth = fmt.visual_depth;
	wd->texture.color_inverted = false;
	wd->texture.has_alpha = fmt.alpha_size != 0;
	glBindTexture(wd->texture.target, wd->texture.texture);
	glXBindTexImageEXT(gd->display, wd->glpixmap, GLX_FRONT_LEFT_EXT, NULL);
	glBindTexture(wd->texture.target, 0);

	gl_check_err();
	return wd;
err:
	if (wd->glpixmap) {
		glXDestroyPixmap(gd->display, wd->glpixmap);
	}
	if (owned) {
		xcb_free_pixmap(base->c, pixmap);
	}
	free(wd);
	return NULL;
}

static void glx_present(backend_t *base) {
	struct _glx_data *gd = (void *)base;
	glXSwapBuffers(gd->display, gd->target_win);
}

static int glx_buffer_age(backend_t *base) {
	if (!glxext.has_GLX_EXT_buffer_age) {
		return -1;
	}

	struct _glx_data *gd = (void *)base;
	unsigned int val;
	glXQueryDrawable(gd->display, gd->target_win, GLX_BACK_BUFFER_AGE_EXT, &val);
	return (int)val ?: -1;
}

struct backend_operations glx_ops = {
    .init = glx_init,
    .deinit = glx_deinit,
    .bind_pixmap = glx_bind_pixmap,
    .compose = gl_compose,
    .image_op = gl_image_op,
    .copy = gl_copy,
    .blur = gl_blur,
    .is_image_transparent = gl_is_image_transparent,
    .present = glx_present,
    .buffer_age = glx_buffer_age,
    .render_shadow = default_backend_render_shadow,
    .max_buffer_age = 5, // Why?
};

/**
 * Check if a GLX extension exists.
 */
static inline bool glx_has_extension(Display *dpy, int screen, const char *ext) {
	const char *glx_exts = glXQueryExtensionsString(dpy, screen);
	if (!glx_exts) {
		log_error("Failed get GLX extension list.");
		return false;
	}

	long inlen = strlen(ext);
	const char *curr = glx_exts;
	bool match = false;
	while (curr && !match) {
		const char *end = strchr(curr, ' ');
		if (!end) {
			// Last extension string
			match = strcmp(ext, curr) == 0;
		} else if (end - curr == inlen) {
			// Length match, do match string
			match = strncmp(ext, curr, end - curr) == 0;
		}
		curr = end ? end + 1 : NULL;
	}

	if (!match) {
		log_info("Missing GLX extension %s.", ext);
	} else {
		log_info("Found GLX extension %s.", ext);
	}

	return match;
}

struct glxext_info glxext = {0};
PFNGLXGETVIDEOSYNCSGIPROC glXGetVideoSyncSGI;
PFNGLXWAITVIDEOSYNCSGIPROC glXWaitVideoSyncSGI;
PFNGLXGETSYNCVALUESOMLPROC glXGetSyncValuesOML;
PFNGLXWAITFORMSCOMLPROC glXWaitForMscOML;
PFNGLXSWAPINTERVALEXTPROC glXSwapIntervalEXT;
PFNGLXSWAPINTERVALSGIPROC glXSwapIntervalSGI;
PFNGLXSWAPINTERVALMESAPROC glXSwapIntervalMESA;
PFNGLXBINDTEXIMAGEEXTPROC glXBindTexImageEXT;
PFNGLXRELEASETEXIMAGEEXTPROC glXReleaseTexImageEXT;

void glxext_init(Display *dpy, int screen) {
	if (glxext.initialized) {
		return;
	}
	glxext.initialized = true;
#define check_ext(name) glxext.has_##name = glx_has_extension(dpy, screen, #name)
	check_ext(GLX_SGI_video_sync);
	check_ext(GLX_SGI_swap_control);
	check_ext(GLX_OML_sync_control);
	check_ext(GLX_MESA_swap_control);
	check_ext(GLX_EXT_swap_control);
	check_ext(GLX_EXT_texture_from_pixmap);
	check_ext(GLX_EXT_buffer_age);
#undef check_ext

#define lookup(name) (name = (__typeof__(name))glXGetProcAddress((GLubyte *)#name))
	// Checking if the returned function pointer is NULL is not really necessary,
	// or maybe not even useful, since glXGetProcAddress might always return
	// something. We are doing it just for completeness' sake.
	if (!lookup(glXGetVideoSyncSGI) || !lookup(glXWaitVideoSyncSGI)) {
		glxext.has_GLX_SGI_video_sync = false;
	}
	if (!lookup(glXSwapIntervalEXT)) {
		glxext.has_GLX_EXT_swap_control = false;
	}
	if (!lookup(glXSwapIntervalMESA)) {
		glxext.has_GLX_MESA_swap_control = false;
	}
	if (!lookup(glXSwapIntervalSGI)) {
		glxext.has_GLX_SGI_swap_control = false;
	}
	if (!lookup(glXWaitForMscOML) || !lookup(glXGetSyncValuesOML)) {
		glxext.has_GLX_OML_sync_control = false;
	}
	if (!lookup(glXBindTexImageEXT) || !lookup(glXReleaseTexImageEXT)) {
		glxext.has_GLX_EXT_texture_from_pixmap = false;
	}
#undef lookup
}
