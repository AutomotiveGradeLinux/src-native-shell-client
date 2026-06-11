/* SPDX-License-Identifier: MIT
 * Copyright © 2022 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/input.h>

#include <wayland-client.h>
#include "shared/os-compatibility.h"

#include "agl-shell-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define DEFAULT_WIDTH_SIZE 150
#define DEFAULT_HEIGHT_SIZE 150

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

struct display;

struct data_output {
	struct display *display;
	struct wl_output *output;
	uint32_t output_id;
	int width, height;
	int offset_x, offset_y;
	struct wl_list link; /** display::outputs */
};

enum window_type {
	WINDOW_NORMAL,
	WINDOW_BG,
	WINDOW_TOP,
	WINDOW_BOTTOM
};

const char *names[] = { "normal", "bg", "top", "bottom" };

struct display {
	struct wl_display *display;
	struct wl_registry *registry;

	struct wl_compositor *compositor;
	struct xdg_wm_base *wm_base;

	struct agl_shell *agl_shell;
	struct wl_list outputs;
	struct wl_list windows;

	struct wl_shm *shm;
	bool has_xrgb;

	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_touch *touch;
        struct wl_keyboard *keyboard;
};

struct buffer {
	struct wl_buffer *buffer;
	void *shm_data;
	int busy;
};

struct window {
	struct display *display;
	int width, height;

	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;

	struct buffer buffers[2];
	struct buffer *prev_buffer;
	struct wl_callback *callback;
	bool wait_for_configure;

	int fullscreen, maximized;
	enum window_type type;
	struct wl_list link; /* display::windows */
};

static int running = 1;

static void
redraw(void *data, struct wl_callback *callback, uint32_t _data);

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
	struct buffer *mybuf = data;
	mybuf->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

static int
create_shm_buffer(struct display *display, struct buffer *buffer,
		int width, int height, uint32_t format)
{
	struct wl_shm_pool *pool;
	int fd, size, stride;
	void *data;

	stride = width * 4;
	size = stride * height;

	fd = os_create_anonymous_file(size);
	if (fd < 0) {
		fprintf(stderr, "creating a buffer file for %d B failed: %s\n",
				size, strerror(errno));
		return -1;
	}

	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	pool = wl_shm_create_pool(display->shm, fd, size);
	buffer->buffer = wl_shm_pool_create_buffer(pool, 0,
			width, height,
			stride, format);
	wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
	wl_shm_pool_destroy(pool);
	close(fd);

	buffer->shm_data = data;
	return 0;
}

static void
handle_xdg_surface_configure(void *data, struct xdg_surface *surface,
		uint32_t serial)
{
	struct window *window = data;

	xdg_surface_ack_configure(surface, serial);

	if (window->wait_for_configure) {
		fprintf(stderr, "got a configure event, doing a redraw\n");
		redraw(window, NULL, 0);
		window->wait_for_configure = false;
	}
}

static const struct xdg_surface_listener xdg_surface_listener = {
	handle_xdg_surface_configure,
};

static void
handle_xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
			      int32_t width, int32_t height, struct wl_array *states)
{
	struct window *window = data;
	uint32_t *p;

	window->fullscreen = 0;
	window->maximized = 0;

	wl_array_for_each(p, states) {
		uint32_t state = *p; 
		switch (state) {
		case XDG_TOPLEVEL_STATE_FULLSCREEN:
			window->fullscreen = 1;
			break;
		case XDG_TOPLEVEL_STATE_MAXIMIZED:
			window->maximized = 1;
			break;
		}
	}

	fprintf(stdout, "Got handle_xdg_toplevel_configure() "
			"width %d, height %d, full %d, max %d, type %d\n", width, height,
			window->fullscreen, window->maximized, window->type);

	if (width > 0 && height > 0) {
		if (!window->fullscreen && !window->maximized) {
			window->width = width;
			window->height = height;
		}
		window->width = width;
		window->height = height;
	} else if (!window->fullscreen && !window->maximized) {
		/* FIXME: should actually use the window geometry here, first
		 * assigned to derive the the width/height in case we can a
		 * configure event for panels */
		if (width == 0)
			window->width = DEFAULT_WIDTH_SIZE;
		else
			window->width = width;

		if (height == 0)
			window->height = DEFAULT_HEIGHT_SIZE;
		else
			window->height = height;
	}

	fprintf(stdout, "settting  width %d, height %d\n",
			window->width, window->height);
}

static void
handle_xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
	running = 0;
}


static void handle_xdg_toplevel_configure_caps(void *data, struct
		xdg_toplevel *xdg_toplevel, struct wl_array *caps)
{

}

static void handle_xdg_toplevel_configure_bounds(void *data, struct
		xdg_toplevel *xdg_toplevel, int32_t width, int32_t height)
{
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	handle_xdg_toplevel_configure,
	handle_xdg_toplevel_close,
	handle_xdg_toplevel_configure_bounds,
	handle_xdg_toplevel_configure_caps,
};

static struct window *
create_window(struct display *display, int width, int height, enum window_type type)
{
	struct window *window;
	const char *name = NULL;

	window = calloc(1, sizeof *window);
	if (!window)
		return NULL;

	window->callback = NULL;
	window->display = display;
	window->width = width;
	window->height = height;
	window->type = type;
	window->surface = wl_compositor_create_surface(display->compositor);

	if (display->wm_base) {
		window->xdg_surface =
			xdg_wm_base_get_xdg_surface(display->wm_base, window->surface);
		assert(window->xdg_surface);

		xdg_surface_add_listener(window->xdg_surface,
					 &xdg_surface_listener, window);
		window->xdg_toplevel = xdg_surface_get_toplevel(window->xdg_surface);
		assert(window->xdg_toplevel);

		xdg_toplevel_add_listener(window->xdg_toplevel,
					  &xdg_toplevel_listener, window);

		name = names[window->type];
		xdg_toplevel_set_title(window->xdg_toplevel, name);
		xdg_toplevel_set_app_id(window->xdg_toplevel, name);
		wl_surface_commit(window->surface);

		window->wait_for_configure = true;
	} else {
		assert(0);
	}

	return window;
}

static void
destroy_window(struct window *window)
{
	if (window->callback)
		wl_callback_destroy(window->callback);

	if (window->buffers[0].buffer)
		wl_buffer_destroy(window->buffers[0].buffer);
	if (window->buffers[1].buffer)
		wl_buffer_destroy(window->buffers[1].buffer);

	if (window->xdg_toplevel)
		xdg_toplevel_destroy(window->xdg_toplevel);

	if (window->xdg_surface)
		xdg_surface_destroy(window->xdg_surface);

	wl_surface_destroy(window->surface);
	wl_list_remove(&window->link);
	free(window);
}

static struct buffer *
window_next_buffer(struct window *window)
{
	struct buffer *buffer;
	int ret = 0;

	if (!window->buffers[0].busy)
		buffer = &window->buffers[0];
	else if (!window->buffers[1].busy)
		buffer = &window->buffers[1];
	else
		return NULL;

	if (!buffer->buffer) {
		ret = create_shm_buffer(window->display, buffer,
				window->width, window->height,
				WL_SHM_FORMAT_XRGB8888);

		if (ret < 0)
			return NULL;

		/* paint the padding */
		memset(buffer->shm_data, 0xff, window->width * window->height * 4);
	}

	return buffer;
}

static void
paint_pixels_top(void *image, int padding, int width, int height)
{
	memset(image, 0xa0, width * height * 4);
}

static void
paint_pixels_bottom(void *image, int padding, int width, int height)
{
	memset(image, 0x00, width * height * 4);
}

static void
paint_pixels_bg(void *image, int padding, int width, int height)
{
	memset(image, 0x00, width * height * 4);
}

static const struct wl_callback_listener frame_listener;

static void
redraw(void *data, struct wl_callback *callback, uint32_t _data)
{
	struct window *window = data;
	struct buffer *buffer;
	(void) _data;
	static int repaints = 2;

	buffer = window_next_buffer(window);
	if (!buffer) {
		fprintf(stderr,
				!callback ? "Failed to create the first buffer.\n" :
				"Both buffers busy at redraw(). Server bug?\n");
		abort();
	}

	switch (window->type) {
	case WINDOW_NORMAL:
	default:
		assert(!"Ooops\n");
	case WINDOW_BOTTOM:
		paint_pixels_bottom(buffer->shm_data, 0, window->width, window->height);
		break;
	case WINDOW_TOP:
		paint_pixels_top(buffer->shm_data, 0, window->width, window->height);
		break;
	case WINDOW_BG:
		paint_pixels_bg(buffer->shm_data, 0, window->width, window->height);
		break;
	}

	wl_surface_attach(window->surface, buffer->buffer, 0, 0);
	wl_surface_damage(window->surface, 0, 0, window->width, window->height);

	if (callback) {
		wl_callback_destroy(callback);
		repaints--;
	}

	if (repaints == 0)
		return;

	window->callback = wl_surface_frame(window->surface);
	wl_callback_add_listener(window->callback, &frame_listener, window);
	wl_surface_commit(window->surface);

	buffer->busy = 1;
}

static const struct wl_callback_listener frame_listener = {
	redraw
};

static void
shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
	struct display *d = data;

	if (format == WL_SHM_FORMAT_XRGB8888)
		d->has_xrgb = true;
}

struct wl_shm_listener shm_listener = {
	shm_format
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *shell, uint32_t serial)
{
        xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
        xdg_wm_base_ping,
};

static void
display_handle_geometry(void *data, struct wl_output *wl_output,
			int x, int y, int physical_width, int physical_height,
			int subpixel, const char *make, const char *model, int transform)
{

}

static void
display_handle_mode(void *data, struct wl_output *wl_output, uint32_t flags,
		    int width, int height, int refresh)
{
	struct data_output *d = data;

	d->width = width;
	d->height = height;
}

static void
display_handle_done(void *data, struct wl_output *output)
{

}

static void
display_handle_scale(void *data, struct wl_output *output, int32_t factor)
{
}

static void
display_handle_name(void *data, struct wl_output *wl_output, const char *name)
{
}

static void
display_handle_desc(void *data, struct wl_output *wl_output, const char *desc)
{
}

static const struct wl_output_listener output_listener = {
	display_handle_geometry,
	display_handle_mode,
	display_handle_done,
	display_handle_scale,
	display_handle_name,
	display_handle_desc,
};

static void
display_add_output(struct display *d, uint32_t id)
{
	struct data_output *output;

	output = calloc(1, sizeof *output);
	output->display = d;
	output->output =
		wl_registry_bind(d->registry, id, &wl_output_interface, 4);

	output->output_id = id;
	wl_list_insert(d->outputs.prev, &output->link);

	wl_output_add_listener(output->output, &output_listener, output);
}

static void
display_remove_output(struct data_output *output)
{
	wl_list_remove(&output->link);
	free(output);
}

static void
pointer_handle_enter(void *data, struct wl_pointer *pointer,
                     uint32_t serial, struct wl_surface *surface,
                     wl_fixed_t sx, wl_fixed_t sy)
{
}


static void
pointer_handle_leave(void *data, struct wl_pointer *pointer,
                     uint32_t serial, struct wl_surface *surface)
{
}

static void
pointer_handle_motion(void *data, struct wl_pointer *pointer,
                      uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
}

static void
pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
                      uint32_t serial, uint32_t time, uint32_t button,
                      uint32_t state)
{
}

static void
pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
                    uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static void
pointer_handle_frame(void *data, struct wl_pointer *pointer)
{
}

static void
pointer_handle_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source)
{
}

static void
pointer_handle_axis_stop(void *data, struct wl_pointer *wl_pointer,
			 uint32_t time, uint32_t axis)
{
}

static void
pointer_handle_axis_discrete(void *data, struct wl_pointer *pointer,
			     uint32_t axis, int32_t discrete)
{
}

static void
pointer_handle_axis_value120(void *data, struct wl_pointer *wl_pointer, uint32_t axis, int32_t value120)
{
}

static const struct wl_pointer_listener pointer_listener = {
	pointer_handle_enter,
	pointer_handle_leave,
	pointer_handle_motion,
	pointer_handle_button,
	pointer_handle_axis,
	pointer_handle_frame,
	pointer_handle_axis_source,
	pointer_handle_axis_stop,
	pointer_handle_axis_discrete,
	pointer_handle_axis_value120,

};


static void
touch_handle_down(void *data, struct wl_touch *wl_touch,
		uint32_t serial, uint32_t time, struct wl_surface *surface,
		int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
}

static void
touch_handle_up(void *data, struct wl_touch *wl_touch,
		uint32_t serial, uint32_t time, int32_t id)
{
}

static void
touch_handle_motion(void *data, struct wl_touch *wl_touch,
		uint32_t time, int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
}

static void
touch_handle_frame(void *data, struct wl_touch *wl_touch)
{
}

static void
touch_handle_cancel(void *data, struct wl_touch *wl_touch)
{
}

static void
touch_handle_shape(void *data, struct wl_touch *touch, int32_t id, wl_fixed_t major, wl_fixed_t minor)
{

}

static void
touch_handle_orientation(void *data, struct wl_touch *wl_touch, int32_t id, wl_fixed_t orientation)
{
}

static const struct wl_touch_listener touch_listener = {
	touch_handle_down,
	touch_handle_up,
	touch_handle_motion,
	touch_handle_frame,
	touch_handle_cancel,
	touch_handle_shape,
	touch_handle_orientation,
};


static void
keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
		uint32_t format, int fd, uint32_t size)
{
	/* Just so we don’t leak the keymap fd */
	close(fd);
}

static void
keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, struct wl_surface *surface,
		struct wl_array *keys)
{
}

static void
keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, struct wl_surface *surface)
{
}

static void
keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, uint32_t time, uint32_t key,
		uint32_t state)
{
	struct display *d = data;

	if (!d->wm_base)
		return;

}

static void
keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, uint32_t mods_depressed,
		uint32_t mods_latched, uint32_t mods_locked,
		uint32_t group)
{
}


static void
keyboard_handle_repeat_info(void *data, struct wl_keyboard *wl_keyboard, int32_t rate, int32_t delay)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
	keyboard_handle_keymap,
	keyboard_handle_enter,
	keyboard_handle_leave,
	keyboard_handle_key,
	keyboard_handle_modifiers,
	keyboard_handle_repeat_info,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *seat, enum wl_seat_capability caps)
{
	struct display *d = data;

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d->pointer) {
		d->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(d->pointer, &pointer_listener, d);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && d->pointer) {
		wl_pointer_destroy(d->pointer);
		d->pointer = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->keyboard) {
		d->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(d->keyboard, &keyboard_listener, d);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && d->keyboard) {
		wl_keyboard_destroy(d->keyboard);
		d->keyboard = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !d->touch) {
		d->touch = wl_seat_get_touch(seat);
		wl_touch_set_user_data(d->touch, d);
		wl_touch_add_listener(d->touch, &touch_listener, d);
	} else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && d->touch) {
		wl_touch_destroy(d->touch);
		d->touch = NULL;
	}
}

static void
seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{

}

static const struct wl_seat_listener seat_listener = {
	seat_handle_capabilities,
	seat_handle_name,
};


static void
registry_handle_global(void *data, struct wl_registry *registry,
		uint32_t id, const char *interface, uint32_t version)
{
	struct display *d = data;

	if (strcmp(interface, "wl_compositor") == 0) {
		d->compositor = wl_registry_bind(registry, id,
						 &wl_compositor_interface, 1);
	} else if (strcmp(interface, "xdg_wm_base") == 0) {
		d->wm_base = wl_registry_bind(registry,
				id, &xdg_wm_base_interface, MAX(version, 3));
		xdg_wm_base_add_listener(d->wm_base, &xdg_wm_base_listener, d);
	} else if (strcmp(interface, "wl_shm") == 0) {
		d->shm = wl_registry_bind(registry,
				id, &wl_shm_interface, 1);
		wl_shm_add_listener(d->shm, &shm_listener, d);
        } else if (strcmp(interface, "wl_seat") == 0) {
                d->seat = wl_registry_bind(registry, id,
                                           &wl_seat_interface, 1);
                wl_seat_add_listener(d->seat, &seat_listener, d);
	} else if (strcmp(interface, "agl_shell") == 0) {
		d->agl_shell = wl_registry_bind(registry, id,
				&agl_shell_interface, MAX(version, 1));
	} else if (strcmp(interface, "wl_output") == 0) {
		display_add_output(d, id);
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name)
{

}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

static struct display *
create_display(void)
{
	struct display *display;

	display = calloc(1, sizeof *display);
	if (display == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	display->display = wl_display_connect(NULL);
	assert(display->display);

	wl_list_init(&display->outputs);
	wl_list_init(&display->windows);

	display->has_xrgb = false;
	display->registry = wl_display_get_registry(display->display);

	wl_registry_add_listener(display->registry, &registry_listener, display);
	wl_display_roundtrip(display->display);

	if (display->shm == NULL) {
		fprintf(stderr, "No wl_shm global\n");
		exit(1);
	}

	if (display->agl_shell == NULL) {
		fprintf(stderr, "No agl_shell extension present\n");
	}

	wl_display_roundtrip(display->display);

	if (!display->has_xrgb) {
		fprintf(stderr, "WL_SHM_FORMAT_XRGB32 not available\n");
		exit(1);
	}

	return display;
}

static void
destroy_display(struct display *display)
{
	struct data_output *output;
	struct window *window;

	if (display->shm)
		wl_shm_destroy(display->shm);

	if (display->wm_base)
		xdg_wm_base_destroy(display->wm_base);

	if (display->compositor)
		wl_compositor_destroy(display->compositor);

	wl_list_for_each(output, &display->outputs, link) {
		display_remove_output(output);
	}

	wl_list_for_each(window, &display->windows, link) {
		destroy_window(window);
	}

	wl_registry_destroy(display->registry);
	wl_display_flush(display->display);
	wl_display_disconnect(display->display);
	free(display);
}

static void
signal_int(int signum)
{
	running = 0;
}

static void
agl_shell_do_background(struct display *d)
{
	struct data_output *output;

	if (!d->agl_shell)
		return;

	wl_list_for_each(output, &d->outputs, link) {
		struct window *window;

		window = create_window(d, 250, 250, WINDOW_BG);

		if (!window)
			return;

		/* Initialise damage to full surface, so the padding gets painted */
		wl_surface_damage(window->surface, 0, 0,
				window->width, window->height);

		if (!window->wait_for_configure)
			redraw(window, NULL, 0);

		agl_shell_set_background(d->agl_shell, window->surface, output->output);

		wl_list_insert(&d->windows, &window->link);
	}
}

void
agl_shell_do_panel(struct display *d, struct wl_surface *sufrace,
		  enum agl_shell_edge mode)
{
	struct data_output *output;

	if (!d->agl_shell)
		return;

	/* handle panel set-up here */
}

static void
agl_shell_do_ready(struct display *d)
{
	if (d->agl_shell) {
		agl_shell_ready(d->agl_shell);
	}
}

int
main(int argc, char *argv[])
{
	struct sigaction sigint;
        struct display *display;

	int ret = 0;
	int set_ready = 0;

	sigint.sa_handler = signal_int;
	sigemptyset(&sigint.sa_mask);
	sigint.sa_flags = SA_RESETHAND | SA_SIGINFO;
	sigaction(SIGINT, &sigint, NULL);

	display = create_display();
	if (!display) {
		fprintf(stderr, "failed to create display!\n");
		exit(EXIT_FAILURE);
	}


	agl_shell_do_background(display);

	while (running && ret != -1) {
		ret = wl_display_dispatch(display->display);

		if (set_ready == 0) {
			agl_shell_do_ready(display);
			set_ready = 1;
		}
	}

	destroy_display(display);

	return 0;
}
