#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <pandagl.h>
#include <yutil.h>

#include "ptk.h"
#include "waylandapp.h"
#include "xdg-shell-client-protocol.h"

#ifdef PTK_LINUX

#define WAYLAND_DEFAULT_SCREEN_WIDTH 1280
#define WAYLAND_DEFAULT_SCREEN_HEIGHT 720

typedef struct ptk_wayland_app {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	list_t windows;
	int exit_code;
	int screen_width;
	int screen_height;
	bool running;
} ptk_wayland_app_t;

struct ptk_window {
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	struct wl_buffer *buffer;
	void *buffer_data;
	size_t buffer_size;
	int width;
	int height;
	bool configured;
	pd_context_t *paint_ctx;
	list_node_t node;
};

static ptk_wayland_app_t app;

static void ptk_waylandwindow_destroy_buffer(ptk_window_t *wnd)
{
	if (wnd->buffer) {
		wl_buffer_destroy(wnd->buffer);
		wnd->buffer = NULL;
	}
	if (wnd->buffer_data) {
		munmap(wnd->buffer_data, wnd->buffer_size);
		wnd->buffer_data = NULL;
	}
	wnd->buffer_size = 0;
}

static void ptk_waylandwindow_post_size_event(ptk_window_t *wnd)
{
	ptk_event_t e = { 0 };

	e.type = PTK_EVENT_SIZE;
	e.window = wnd;
	e.size.width = wnd->width;
	e.size.height = wnd->height;
	ptk_post_event(&e);
}

static void ptk_waylandapp_on_registry_global(void *data,
					      struct wl_registry *registry,
					      uint32_t name,
					      const char *interface,
					      uint32_t version)
{
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		app.compositor = wl_registry_bind(registry, name,
						  &wl_compositor_interface, 1);
		return;
	}
	if (strcmp(interface, wl_shm_interface.name) == 0) {
		app.shm =
		    wl_registry_bind(registry, name, &wl_shm_interface, 1);
		return;
	}
	if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		uint32_t bind_version = version < 1 ? version : 1;
		app.wm_base = wl_registry_bind(registry, name,
					       &xdg_wm_base_interface,
					       bind_version);
	}
}

static void ptk_waylandapp_on_registry_global_remove(void *data,
						      struct wl_registry *registry,
						      uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	ptk_waylandapp_on_registry_global,
	ptk_waylandapp_on_registry_global_remove
};

static void ptk_waylandapp_on_wm_base_ping(void *data,
					   struct xdg_wm_base *wm_base,
					   uint32_t serial)
{
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	ptk_waylandapp_on_wm_base_ping
};

static void ptk_waylandwindow_on_xdg_surface_configure(
    void *data, struct xdg_surface *surface, uint32_t serial)
{
	ptk_window_t *wnd = data;

	xdg_surface_ack_configure(surface, serial);
	wnd->configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	ptk_waylandwindow_on_xdg_surface_configure
};

static void ptk_waylandwindow_on_toplevel_configure(
    void *data, struct xdg_toplevel *xdg_toplevel, int32_t width,
    int32_t height, struct wl_array *states)
{
	ptk_window_t *wnd = data;

	if (width <= 0 || height <= 0) {
		return;
	}
	if (wnd->width == width && wnd->height == height) {
		return;
	}
	wnd->width = width;
	wnd->height = height;
	ptk_waylandwindow_destroy_buffer(wnd);
	ptk_waylandwindow_post_size_event(wnd);
}

static void ptk_waylandwindow_on_toplevel_close(void *data,
						struct xdg_toplevel *xdg_toplevel)
{
	ptk_window_t *wnd = data;
	ptk_event_t e = { 0 };

	e.type = PTK_EVENT_CLOSE;
	e.window = wnd;
	ptk_post_event(&e);
}

static void ptk_waylandwindow_on_toplevel_configure_bounds(
    void *data, struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height)
{
}

static void ptk_waylandwindow_on_toplevel_wm_capabilities(
    void *data, struct xdg_toplevel *xdg_toplevel, struct wl_array *capabilities)
{
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	ptk_waylandwindow_on_toplevel_configure,
	ptk_waylandwindow_on_toplevel_close,
	ptk_waylandwindow_on_toplevel_configure_bounds,
	ptk_waylandwindow_on_toplevel_wm_capabilities
};

static int ptk_waylandapp_poll_events(int timeout_ms, bool dispatch_all)
{
	struct pollfd pfd;
	int result;

	result = wl_display_dispatch_pending(app.display);
	if (result < 0) {
		return -1;
	}
	if (dispatch_all && result > 0) {
		do {
			result = wl_display_dispatch_pending(app.display);
		} while (result > 0);
		if (result < 0) {
			return -1;
		}
	}
	if (wl_display_flush(app.display) < 0 && errno != EAGAIN) {
		return -1;
	}

	pfd.fd = wl_display_get_fd(app.display);
	pfd.events = POLLIN;
	pfd.revents = 0;
	result = poll(&pfd, 1, timeout_ms);
	if (result <= 0) {
		return result;
	}
	if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
		return -1;
	}
	if (!(pfd.revents & POLLIN)) {
		return 0;
	}
	result = wl_display_dispatch(app.display);
	if (result < 0) {
		return -1;
	}
	if (!dispatch_all) {
		return result;
	}
	while ((result = wl_display_dispatch_pending(app.display)) > 0) {
	}
	return result < 0 ? -1 : 1;
}

static int ptk_waylandapp_init(const wchar_t *name)
{
	memset(&app, 0, sizeof(app));
	app.display = wl_display_connect(NULL);
	if (!app.display) {
		logger_error("[wayland] wl_display_connect() failed\n");
		return -1;
	}
	app.screen_width = WAYLAND_DEFAULT_SCREEN_WIDTH;
	app.screen_height = WAYLAND_DEFAULT_SCREEN_HEIGHT;
	app.registry = wl_display_get_registry(app.display);
	if (!app.registry) {
		logger_error("[wayland] wl_display_get_registry() failed\n");
		wl_display_disconnect(app.display);
		app.display = NULL;
		return -1;
	}
	list_create(&app.windows);
	wl_registry_add_listener(app.registry, &registry_listener, NULL);
	if (wl_display_roundtrip(app.display) < 0) {
		logger_error("[wayland] wl_display_roundtrip() failed during registry init\n");
		return -1;
	}
	if (!app.compositor || !app.shm || !app.wm_base) {
		logger_error("[wayland] missing globals: compositor=%p shm=%p wm_base=%p\n",
			     app.compositor, app.shm, app.wm_base);
		return -1;
	}
	xdg_wm_base_add_listener(app.wm_base, &wm_base_listener, NULL);
	app.running = true;
	app.exit_code = 0;
	return 0;
}

static int ptk_waylandapp_destroy(void)
{
	list_node_t *node, *next;
	ptk_window_t *wnd;

	for (node = app.windows.head.next; node; node = next) {
		next = node->next;
		wnd = node->data;
		if (wnd) {
			if (wnd->xdg_toplevel) {
				xdg_toplevel_destroy(wnd->xdg_toplevel);
			}
			if (wnd->xdg_surface) {
				xdg_surface_destroy(wnd->xdg_surface);
			}
			if (wnd->surface) {
				wl_surface_destroy(wnd->surface);
			}
			ptk_waylandwindow_destroy_buffer(wnd);
			free(wnd->paint_ctx);
			free(wnd);
		}
	}
	list_create(&app.windows);
	if (app.wm_base) {
		xdg_wm_base_destroy(app.wm_base);
	}
	if (app.shm) {
		wl_shm_destroy(app.shm);
	}
	if (app.compositor) {
		wl_compositor_destroy(app.compositor);
	}
	if (app.registry) {
		wl_registry_destroy(app.registry);
	}
	if (app.display) {
		wl_display_disconnect(app.display);
	}
	memset(&app, 0, sizeof(app));
	return 0;
}

static int ptk_waylandapp_process_events(ptk_process_events_option_t option)
{
	int result = 0;

	app.exit_code = 0;
	if (option == PTK_PROCESS_EVENTS_ONE_IF_PRESENT ||
	    option == PTK_PROCESS_EVENTS_ALL_IF_PRESENT) {
		ptk_tick();
		ptk_process_events();
		result = ptk_waylandapp_poll_events(
		    0, option == PTK_PROCESS_EVENTS_ALL_IF_PRESENT);
		ptk_process_events();
		return result < 0 ? -1 : app.exit_code;
	}
	while (app.running) {
		ptk_tick();
		ptk_process_events();
		result = ptk_waylandapp_poll_events(1, true);
		if (result < 0) {
			break;
		}
		ptk_process_events();
	}
	return app.exit_code;
}

static ptk_window_t *ptk_waylandwindow_create(const wchar_t *title, int x, int y,
					      int width, int height,
					      ptk_window_t *parent)
{
	ptk_window_t *wnd;

	if (!app.compositor || !app.wm_base || !app.shm) {
		return NULL;
	}
	wnd = calloc(1, sizeof(*wnd));
	if (!wnd) {
		return NULL;
	}
	wnd->width = width > 0 ? width : PTK_WINDOW_DEFAULT_WIDTH;
	wnd->height = height > 0 ? height : PTK_WINDOW_DEFAULT_HEIGHT;
	wnd->surface = wl_compositor_create_surface(app.compositor);
	if (!wnd->surface) {
		free(wnd);
		return NULL;
	}
	wnd->xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, wnd->surface);
	wnd->xdg_toplevel = xdg_surface_get_toplevel(wnd->xdg_surface);
	if (!wnd->xdg_surface || !wnd->xdg_toplevel) {
		if (wnd->xdg_toplevel) {
			xdg_toplevel_destroy(wnd->xdg_toplevel);
		}
		if (wnd->xdg_surface) {
			xdg_surface_destroy(wnd->xdg_surface);
		}
		wl_surface_destroy(wnd->surface);
		free(wnd);
		return NULL;
	}
	wnd->node.data = wnd;
	xdg_surface_add_listener(wnd->xdg_surface, &xdg_surface_listener, wnd);
	xdg_toplevel_add_listener(wnd->xdg_toplevel, &xdg_toplevel_listener, wnd);
	if (title) {
		size_t len = encode_utf8(NULL, title, 0) + 1;
		char *utf8_title = malloc(sizeof(char) * len);

		if (utf8_title) {
			encode_utf8(utf8_title, title, len);
			xdg_toplevel_set_title(wnd->xdg_toplevel, utf8_title);
			free(utf8_title);
		}
	}
	wl_surface_commit(wnd->surface);
	wl_display_roundtrip(app.display);
	list_append_node(&app.windows, &wnd->node);
	return wnd;
}

static ptk_window_t *ptk_waylandapp_get_window(void *handle)
{
	list_node_t *node;

	for (list_each(node, &app.windows)) {
		ptk_window_t *wnd = node->data;

		if (wnd && wnd->surface == handle) {
			return wnd;
		}
	}
	return NULL;
}

static void ptk_waylandapp_present(void)
{
	wl_display_flush(app.display);
}

static void ptk_waylandapp_exit(int exit_code)
{
	app.running = false;
	app.exit_code = exit_code;
}

static int ptk_waylandapp_on_event(int type,
				   ptk_native_event_handler_t handler,
				   void *data)
{
	return -1;
}

static int ptk_waylandapp_off_event(int type,
				    ptk_native_event_handler_t handler)
{
	return -1;
}

static int ptk_waylandapp_get_screen_width(void)
{
	return app.screen_width;
}

static int ptk_waylandapp_get_screen_height(void)
{
	return app.screen_height;
}

static void ptk_waylandwindow_show(ptk_window_t *wnd)
{
	wl_surface_commit(wnd->surface);
}

static void ptk_waylandwindow_activate(ptk_window_t *wnd)
{
	wl_surface_commit(wnd->surface);
}

static void ptk_waylandwindow_close(ptk_window_t *wnd)
{
	ptk_event_t e = { 0 };

	e.type = PTK_EVENT_CLOSE;
	e.window = wnd;
	ptk_post_event(&e);
}

static void ptk_waylandwindow_destroy(ptk_window_t *wnd)
{
	if (!wnd) {
		return;
	}
	list_unlink(&app.windows, &wnd->node);
	if (wnd->xdg_toplevel) {
		xdg_toplevel_destroy(wnd->xdg_toplevel);
	}
	if (wnd->xdg_surface) {
		xdg_surface_destroy(wnd->xdg_surface);
	}
	if (wnd->surface) {
		wl_surface_destroy(wnd->surface);
	}
	ptk_waylandwindow_destroy_buffer(wnd);
	free(wnd->paint_ctx);
	free(wnd);
}

static void ptk_waylandwindow_set_title(ptk_window_t *wnd, const wchar_t *title)
{
	size_t len;
	char *utf8_title;

	if (!title) {
		return;
	}
	len = encode_utf8(NULL, title, 0) + 1;
	utf8_title = malloc(sizeof(char) * len);
	if (!utf8_title) {
		return;
	}
	encode_utf8(utf8_title, title, len);
	xdg_toplevel_set_title(wnd->xdg_toplevel, utf8_title);
	free(utf8_title);
}

static void ptk_waylandwindow_set_size(ptk_window_t *wnd, int width, int height)
{
	if (width <= 0 || height <= 0) {
		return;
	}
	if (wnd->width == width && wnd->height == height) {
		return;
	}
	wnd->width = width;
	wnd->height = height;
	ptk_waylandwindow_destroy_buffer(wnd);
	ptk_waylandwindow_post_size_event(wnd);
}

static void ptk_waylandwindow_set_position(ptk_window_t *wnd, int x, int y)
{
}

static void *ptk_waylandwindow_get_handle(ptk_window_t *wnd)
{
	return wnd->surface;
}

static int ptk_waylandwindow_get_width(ptk_window_t *wnd)
{
	return wnd->width;
}

static int ptk_waylandwindow_get_height(ptk_window_t *wnd)
{
	return wnd->height;
}

static void ptk_waylandwindow_set_min_width(ptk_window_t *wnd, int min_width)
{
	xdg_toplevel_set_min_size(wnd->xdg_toplevel, min_width, 0);
}

static void ptk_waylandwindow_set_min_height(ptk_window_t *wnd, int min_height)
{
	xdg_toplevel_set_min_size(wnd->xdg_toplevel, 0, min_height);
}

static void ptk_waylandwindow_set_max_width(ptk_window_t *wnd, int max_width)
{
	xdg_toplevel_set_max_size(wnd->xdg_toplevel, max_width, 0);
}

static void ptk_waylandwindow_set_max_height(ptk_window_t *wnd, int max_height)
{
	xdg_toplevel_set_max_size(wnd->xdg_toplevel, 0, max_height);
}

static ptk_window_paint_t *ptk_waylandwindow_begin_paint(ptk_window_t *wnd,
							 pd_rect_t *rect)
{
	size_t size;
	int fd;
	struct wl_shm_pool *pool;

	if (!wnd->configured || !rect) {
		return NULL;
	}
	if (!wnd->buffer) {
		size = (size_t)wnd->width * (size_t)wnd->height * 4;
		fd = memfd_create("lcui-wayland-buffer", MFD_CLOEXEC);
		if (fd < 0) {
			return NULL;
		}
		if (ftruncate(fd, (off_t)size) < 0) {
			close(fd);
			return NULL;
		}
		wnd->buffer_data =
		    mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (wnd->buffer_data == MAP_FAILED) {
			wnd->buffer_data = NULL;
			close(fd);
			return NULL;
		}
		pool = wl_shm_create_pool(app.shm, fd, (int)size);
		wnd->buffer = wl_shm_pool_create_buffer(
		    pool, 0, wnd->width, wnd->height, wnd->width * 4,
		    WL_SHM_FORMAT_XRGB8888);
		wl_shm_pool_destroy(pool);
		close(fd);
		wnd->buffer_size = size;
	}
	if (!wnd->paint_ctx) {
		wnd->paint_ctx = calloc(1, sizeof(*wnd->paint_ctx));
		if (!wnd->paint_ctx) {
			return NULL;
		}
	}
	pd_canvas_init(&wnd->paint_ctx->canvas);
	wnd->paint_ctx->canvas.width = wnd->width;
	wnd->paint_ctx->canvas.height = wnd->height;
	wnd->paint_ctx->canvas.color_type = PD_COLOR_TYPE_ARGB;
	wnd->paint_ctx->canvas.bytes = wnd->buffer_data;
	wnd->paint_ctx->canvas.bytes_per_pixel = 4;
	wnd->paint_ctx->canvas.bytes_per_row = wnd->width * 4;
	wnd->paint_ctx->rect = *rect;
	wnd->paint_ctx->with_alpha = false;
	pd_rect_correct(&wnd->paint_ctx->rect, wnd->width, wnd->height);
	if (wnd->paint_ctx->rect.width > 0 && wnd->paint_ctx->rect.height > 0) {
		pd_canvas_t area;

		pd_canvas_init(&area);
		pd_canvas_quote(&area, &wnd->paint_ctx->canvas,
				&wnd->paint_ctx->rect);
		pd_canvas_fill(&area, pd_rgb(255, 255, 255));
	}
	return wnd->paint_ctx;
}

static void ptk_waylandwindow_end_paint(ptk_window_t *wnd,
					ptk_window_paint_t *paint)
{
	if (!wnd || !paint || !wnd->buffer) {
		return;
	}
	wl_surface_attach(wnd->surface, wnd->buffer, 0, 0);
	wl_surface_damage(wnd->surface, paint->rect.x, paint->rect.y,
			  paint->rect.width, paint->rect.height);
	wl_surface_commit(wnd->surface);
	wl_display_flush(app.display);
}

static void ptk_waylandwindow_present(ptk_window_t *wnd)
{
	wl_surface_commit(wnd->surface);
	wl_display_flush(app.display);
}

void ptk_waylandapp_driver_init(ptk_app_driver_t *driver)
{
	memset(driver, 0, sizeof(*driver));
	driver->init = ptk_waylandapp_init;
	driver->destroy = ptk_waylandapp_destroy;
	driver->process_events = ptk_waylandapp_process_events;
	driver->on_event = ptk_waylandapp_on_event;
	driver->off_event = ptk_waylandapp_off_event;
	driver->get_screen_width = ptk_waylandapp_get_screen_width;
	driver->get_screen_height = ptk_waylandapp_get_screen_height;
	driver->create_window = ptk_waylandwindow_create;
	driver->get_window = ptk_waylandapp_get_window;
	driver->present = ptk_waylandapp_present;
	driver->exit = ptk_waylandapp_exit;
}

void ptk_waylandwindow_driver_init(ptk_window_driver_t *driver)
{
	memset(driver, 0, sizeof(*driver));
	driver->show = ptk_waylandwindow_show;
	driver->activate = ptk_waylandwindow_activate;
	driver->close = ptk_waylandwindow_close;
	driver->destroy = ptk_waylandwindow_destroy;
	driver->set_title = ptk_waylandwindow_set_title;
	driver->set_size = ptk_waylandwindow_set_size;
	driver->set_position = ptk_waylandwindow_set_position;
	driver->get_handle = ptk_waylandwindow_get_handle;
	driver->get_width = ptk_waylandwindow_get_width;
	driver->get_height = ptk_waylandwindow_get_height;
	driver->set_min_width = ptk_waylandwindow_set_min_width;
	driver->set_min_height = ptk_waylandwindow_set_min_height;
	driver->set_max_width = ptk_waylandwindow_set_max_width;
	driver->set_max_height = ptk_waylandwindow_set_max_height;
	driver->begin_paint = ptk_waylandwindow_begin_paint;
	driver->end_paint = ptk_waylandwindow_end_paint;
	driver->present = ptk_waylandwindow_present;
}

#endif
