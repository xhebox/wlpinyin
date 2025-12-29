#ifdef ENABLE_POPUP

#include <cairo.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <pango/pango-fontmap.h>
#include <pango/pangocairo.h>

#include "input-method-unstable-v2-client-protocol.h"
#include "wlpinyin.h"
#include "config.h"

#define ITEM_SPACING 8
#define ROW_SPACING 4
#define CORNER_RADIUS 4
#define MAX_BACK_ROWS 2
#define MAX_FWD_ROWS 5

static int DEFAULT_SHM_SIZE = 4096;

static void draw_rounded_rectangle(cairo_t *cr,
				   double x,
				   double y,
				   double width,
				   double height,
				   double radius) {
	cairo_move_to(cr, x + radius, y);
	cairo_line_to(cr, x + width - radius, y);
	cairo_curve_to(cr, x + width, y, x + width, y, x + width, y + radius);
	cairo_line_to(cr, x + width, y + height - radius);
	cairo_curve_to(cr, x + width, y + height, x + width, y + height,
		x + width - radius, y + height);
	cairo_line_to(cr, x + radius, y + height);
	cairo_curve_to(cr, x, y + height, x, y + height, x, y + height - radius);
	cairo_line_to(cr, x, y + radius);
	cairo_curve_to(cr, x, y, x, y, x + radius, y);
}

static void popup_handle_frame_done(void *data,
				    struct wl_callback *cb,
				    uint32_t serial) {
	UNUSED(serial);
	wl_callback_destroy(cb);

	struct wlpinyin_state *state = data;
	state->frame_callback_done = true;
	if (state->pending_render)
		im_panel_update(state);
}

int im_panel_update(struct wlpinyin_state *state) {
	char buf[256];
	int bufptr = 0;

	im_preedit_t preedit = im_engine_preedit(state->engine);
	zwp_input_method_v2_set_preedit_string(state->input_method, preedit.text,
					preedit.begin, preedit.end);

	im_context_t ctx = im_engine_context(state->engine);

	/* Empty, show nothing */
	if (ctx.page_size == 0) {
		wl_surface_attach(state->popup_surface, NULL, 0, 0);
		wl_surface_commit(state->popup_surface);
		state->pending_render = false;
		return 0;
	}

	/* If not ready, just return */
	if (!state->frame_callback_done) {
		state->pending_render = true;
		return 0;
	}

	/* Setup new frame callback */
	state->frame_callback_done = false;
	struct wl_callback *cb = wl_surface_frame(state->popup_surface);
	static const struct wl_callback_listener frame_listener = {
		.done = popup_handle_frame_done,
	};
	wl_callback_add_listener(cb, &frame_listener, state);

	int start_row, end_row;
	if (ctx.page_no == 0) {
		start_row = 0;
		end_row = 1;
	} else {
		start_row = MAX(0, ctx.page_no - MAX_BACK_ROWS);
		end_row = start_row + MAX_FWD_ROWS;
	}

	int start_idx = start_row * ctx.page_size;

	/* Measure column widths */
	int row_width[50] = {0};
	int row_height = 0;

	int real_end_row = 0;
	im_engine_cand_begin(state->engine, start_idx);
	int i;
	for (i = start_idx; im_engine_cand_next(state->engine); i++) {
		int row = i / ctx.page_size;
		int col = i % ctx.page_size;
		if (row >= end_row) {
			i--;
			break;
		}

		const char *text = im_engine_cand_get(state->engine);
		bufptr =
			snprintf(&buf[bufptr], sizeof(buf) - bufptr, "%d %s", col + 1, text);
		pango_layout_set_text(state->popup_pango_layout, buf, bufptr);
		PangoRectangle text_rect;
		pango_layout_get_pixel_extents(state->popup_pango_layout, NULL, &text_rect);

		int item_width = text_rect.width + ITEM_SPACING * 2;
		row_width[col] = MAX(row_width[col], item_width);
		row_height = MAX(row_height, text_rect.height + ROW_SPACING * 2);
	}
	int row = i / ctx.page_size;
	int col = i % ctx.page_size;
	if (col == 0 && !im_engine_cand_next(state->engine))
		real_end_row = row;
	else
		real_end_row = row + 1;
	im_engine_cand_end(state->engine);

	/* Calculate panel size */
	int panel_width = 0;
	for (int i = 0; i < ctx.page_size; i++)
		panel_width += row_width[i];
	int panel_height = row_height * MAX(1, real_end_row - start_row);

	/* Resize buffer if needed */
	int panel_stride =
		cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, panel_width);
	int buffer_newsz = panel_stride * panel_height;

	if (state->shm_size < buffer_newsz) {
		if (state->popup_data) {
			munmap(state->popup_data, state->shm_size);
			state->popup_data = NULL;
		}

		if (ftruncate(state->shm_pool_fd, buffer_newsz) < 0) {
			wlpinyin_err("fail to resize shm: %s", strerror(errno));
			return -1;
		}
		wl_shm_pool_resize(state->shm_pool, buffer_newsz);

		state->popup_data = mmap(NULL, buffer_newsz, PROT_READ | PROT_WRITE,
			   MAP_SHARED, state->shm_pool_fd, 0);
		if (state->popup_data == MAP_FAILED) {
			wlpinyin_err("mmap failed: %s", strerror(errno));
			state->popup_data = NULL;
			return -1;
		}
		state->shm_size = buffer_newsz;
	}

	/* Recreate buffer */
	if (state->shm_buffer)
		wl_buffer_destroy(state->shm_buffer);
	state->shm_buffer =
		wl_shm_pool_create_buffer(state->shm_pool, 0, panel_width, panel_height,
			    panel_stride, WL_SHM_FORMAT_ARGB8888);

	/* Create Cairo surface */
	cairo_surface_t *cairo_surface = cairo_image_surface_create_for_data(
		state->popup_data, CAIRO_FORMAT_ARGB32, panel_width, panel_height,
		panel_stride);
	cairo_t *cr = cairo_create(cairo_surface);

	/* Clear */
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);

	/* Draw background */
	draw_rounded_rectangle(cr, 0, 0, panel_width, panel_height, CORNER_RADIUS);
	cairo_set_source_rgba(cr, POPUP_BG_RGBA[0], POPUP_BG_RGBA[1], POPUP_BG_RGBA[2], POPUP_BG_RGBA[3]);
	cairo_fill(cr);

	/* Draw candidates in grid layout */
	im_engine_cand_begin(state->engine, start_idx);
	for (int i = start_idx; im_engine_cand_next(state->engine); i++) {
		const char *text = im_engine_cand_get(state->engine);
		int row = i / ctx.page_size;
		int col = i % ctx.page_size;
		if (row > real_end_row)
			break;

		int x = 0;
		for (int c = 0; c < col; c++)
			x += row_width[c];
		int y = (row - start_row) * row_height;

		bufptr = 0;
		bufptr = snprintf(&buf[bufptr], sizeof(buf) - bufptr, "%d ", col + 1);
		if (row != ctx.page_no)
			for (int i = 0; i < bufptr; i++)
				buf[i] = ' ';
		bufptr += snprintf(&buf[bufptr], sizeof(buf) - bufptr, "%s", text);
		pango_layout_set_text(state->popup_pango_layout, buf, bufptr);
		PangoRectangle text_rect;
		pango_layout_get_pixel_extents(state->popup_pango_layout, NULL, &text_rect);

		/* Draw highlight background */
		if (row == ctx.page_no && col == ctx.highlighted_index) {
			draw_rounded_rectangle(cr, x, y, row_width[col], row_height, 0);
			cairo_set_source_rgba(cr, POPUP_HL_RGBA[0], POPUP_HL_RGBA[1], POPUP_HL_RGBA[2], POPUP_HL_RGBA[3]);

			cairo_fill(cr);
		}

		cairo_set_source_rgba(cr, POPUP_TXT_RGBA[0], POPUP_TXT_RGBA[1], POPUP_TXT_RGBA[2], POPUP_TXT_RGBA[3]);
		cairo_move_to(cr, x + ITEM_SPACING, y + ROW_SPACING);
		pango_cairo_show_layout(cr, state->popup_pango_layout);
	}
	im_engine_cand_end(state->engine);

	cairo_destroy(cr);
	cairo_surface_destroy(cairo_surface);

	/* Commit to wayland */
	wl_surface_attach(state->popup_surface, state->shm_buffer, 0, 0);
	wl_surface_damage(state->popup_surface, 0, 0, panel_width, panel_height);
	wl_surface_commit(state->popup_surface);
	state->pending_render = false;

	return 0;
}

int im_panel_init(struct wlpinyin_state *state) {
	if (!state->wl_shm) {
		wlpinyin_err("wl_shm not available");
		return -1;
	}

	state->popup_surface = wl_compositor_create_surface(state->compositor);
	if (!state->popup_surface) {
		wlpinyin_err("failed to create popup surface");
		return -1;
	}

	state->popup_surface_v2 = zwp_input_method_v2_get_input_popup_surface(
		state->input_method, state->popup_surface);

	state->shm_pool_fd = memfd_create("wlpinyin", 0);
	if (state->shm_pool_fd < 0) {
		wlpinyin_err("fail to create shm: %s", strerror(errno));
		return -1;
	}

	if (ftruncate(state->shm_pool_fd, DEFAULT_SHM_SIZE) < 0) {
		wlpinyin_err("fail to init shm buffer: %s", strerror(errno));
		close(state->shm_pool_fd);
		return -1;
	}

	state->shm_pool =
		wl_shm_create_pool(state->wl_shm, state->shm_pool_fd, DEFAULT_SHM_SIZE);

	PangoFontMap *font_map = pango_cairo_font_map_new();
	if (font_map == NULL) {
		wlpinyin_err("failed to create cairo font map");
		return -1;
	}
	state->popup_pango_ctx = pango_font_map_create_context(font_map);
	g_object_unref(font_map);
	state->popup_pango_layout = pango_layout_new(state->popup_pango_ctx);
	PangoFontDescription *desc = pango_font_description_from_string(POPUP_FONT);
	pango_layout_set_font_description(state->popup_pango_layout, desc);
	pango_font_description_free(desc);

	state->frame_callback_done = true;
	state->pending_render = true;

	return 0;
}

void im_panel_destroy(struct wlpinyin_state *state) {
	if (state->shm_buffer) {
		wl_buffer_destroy(state->shm_buffer);
		state->shm_buffer = NULL;
	}
	if (state->popup_pango_layout) {
		g_object_unref(state->popup_pango_layout);
		state->popup_pango_layout = NULL;
	}
	if (state->popup_pango_ctx) {
		g_object_unref(state->popup_pango_ctx);
		state->popup_pango_ctx = NULL;
	}
	if (state->popup_data) {
		munmap(state->popup_data, state->shm_size);
		state->popup_data = NULL;
	}
	if (state->shm_pool) {
		wl_shm_pool_destroy(state->shm_pool);
		state->shm_pool = NULL;
	}
	if (state->shm_pool_fd >= 0) {
		close(state->shm_pool_fd);
		state->shm_pool_fd = -1;
	}
	if (state->popup_surface_v2) {
		zwp_input_popup_surface_v2_destroy(state->popup_surface_v2);
		state->popup_surface_v2 = NULL;
	}
	if (state->popup_surface) {
		wl_surface_destroy(state->popup_surface);
		state->popup_surface = NULL;
	}
}

#endif /* ENABLE_POPUP */
