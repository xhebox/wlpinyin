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

static void popup_handle_buffer_release(void *data, struct wl_buffer *buffer);

/* Returns false if not drawable yet (slot busy or error); retry after release.
 */
static bool popup_buffer_prepare(struct wlpinyin_state *state,
																 struct wlpinyin_popup_buffer *slot,
																 int width,
																 int height,
																 int stride) {
	size_t required_size = (size_t)stride * height;

	/* Reuse the existing buffer if nothing changed. */
	if (slot->buffer && slot->width == width && slot->height == height &&
			slot->stride == stride)
		return true;

	if (required_size > (size_t)state->slot_capacity) {
		/* Growing re-layouts both slots; only safe when none is busy. */
		for (int i = 0; i < WLPINYIN_POPUP_BUFFER_COUNT; i++) {
			if (state->popup_buffers[i].busy) {
				/* Force the compositor to release the busy buffer;
				 * the release event triggers the deferred render. */
				wl_surface_attach(state->popup_surface, NULL, 0, 0);
				wl_surface_commit(state->popup_surface);
				return false;
			}
		}
		size_t capacity = MAX((size_t)DEFAULT_SHM_SIZE,
													MAX((size_t)state->slot_capacity * 2, required_size));
		size_t pool_size = capacity * WLPINYIN_POPUP_BUFFER_COUNT;

		/* Grow the pool (only grows, never shrinks). */
		if (ftruncate(state->shm_pool_fd, pool_size) < 0) {
			wlpinyin_err("fail to resize shm: %s", strerror(errno));
			return false;
		}
		/* Map the new size first; keep the old mapping on failure. */
		void *data = mmap(NULL, pool_size, PROT_READ | PROT_WRITE, MAP_SHARED,
											state->shm_pool_fd, 0);
		if (data == MAP_FAILED) {
			wlpinyin_err("mmap failed: %s", strerror(errno));
			return false;
		}
		if (state->popup_data)
			munmap(state->popup_data, state->shm_size);
		state->popup_data = data;
		wl_shm_pool_resize(state->shm_pool, pool_size);

		state->shm_size = pool_size;
		state->slot_capacity = capacity;
		/* Region offsets changed; recreate all wl_buffers lazily. */
		for (int i = 0; i < WLPINYIN_POPUP_BUFFER_COUNT; i++) {
			if (state->popup_buffers[i].buffer) {
				wl_buffer_destroy(state->popup_buffers[i].buffer);
				state->popup_buffers[i].buffer = NULL;
			}
		}
	}

	if (slot->buffer) {
		wl_buffer_destroy(slot->buffer);
		slot->buffer = NULL;
	}
	slot->offset = (slot - state->popup_buffers) * state->slot_capacity;
	slot->buffer =
			wl_shm_pool_create_buffer(state->shm_pool, slot->offset, width, height,
																stride, WL_SHM_FORMAT_ARGB8888);
	if (slot->buffer == NULL)
		return false;

	static const struct wl_buffer_listener listener = {
			.release = popup_handle_buffer_release,
	};
	if (wl_buffer_add_listener(slot->buffer, &listener, state) != 0) {
		wl_buffer_destroy(slot->buffer);
		slot->buffer = NULL;
		return false;
	}

	slot->width = width;
	slot->height = height;
	slot->stride = stride;
	return true;
}

static void popup_handle_buffer_release(void *data, struct wl_buffer *buffer) {
	struct wlpinyin_state *state = data;
	for (int i = 0; i < WLPINYIN_POPUP_BUFFER_COUNT; i++) {
		struct wlpinyin_popup_buffer *slot = &state->popup_buffers[i];
		if (slot->buffer == buffer) {
			slot->busy = false;
			break;
		}
	}
	if (state->pending_render)
		im_panel_update(state);
}

int im_panel_update(struct wlpinyin_state *state) {
	im_preedit_t preedit = im_engine_preedit(state->engine);
	zwp_input_method_v2_set_preedit_string(state->input_method, preedit.text,
																				 preedit.begin, preedit.end);

	im_context_t ctx = im_engine_context(state->engine);

	/* Empty, show nothing */
	if (ctx.page_size == 0) {
		wl_surface_attach(state->popup_surface, NULL, 0, 0);
		wl_surface_commit(state->popup_surface);
		state->pending_render = false;
		im_panel_retry_later(state, 0); /* disarm retry timer */
		return 0;
	}

	/* Find a free buffer; defer to wl_buffer.release if both are busy. */
	struct wlpinyin_popup_buffer *slot = NULL;
	for (int i = 0; i < WLPINYIN_POPUP_BUFFER_COUNT; i++) {
		if (!state->popup_buffers[i].busy) {
			slot = &state->popup_buffers[i];
			break;
		}
	}
	if (slot == NULL) {
		state->pending_render = true;
		return 0;
	}

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
		char prefix[16];
		int prefix_len = snprintf(prefix, sizeof(prefix), "%d ", col + 1);
		if (prefix_len < 0 || (size_t)prefix_len >= sizeof(prefix))
			continue;

		pango_layout_set_text(state->popup_pango_layout, prefix, prefix_len);
		PangoRectangle prefix_rect;
		pango_layout_get_pixel_extents(state->popup_pango_layout, NULL,
																	 &prefix_rect);

		pango_layout_set_text(state->popup_pango_layout, text, -1);
		PangoRectangle text_rect;
		pango_layout_get_pixel_extents(state->popup_pango_layout, NULL, &text_rect);

		row_width[col] = MAX(
				row_width[col], prefix_rect.width + text_rect.width + ITEM_SPACING * 2);
		row_height = MAX(row_height, MAX(prefix_rect.height, text_rect.height) +
																		 ROW_SPACING * 2);
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
	int panel_stride =
			cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, panel_width);

	/* Resize buffer if needed */
	if (!popup_buffer_prepare(state, slot, panel_width, panel_height,
														panel_stride)) {
		/* Deferred (slot busy) or failed; retry on release or timer. */
		state->pending_render = true;
		im_panel_retry_later(state, 100);
		return 0;
	}

	/* Create Cairo surface */
	cairo_surface_t *cairo_surface = cairo_image_surface_create_for_data(
			(unsigned char *)state->popup_data + slot->offset, CAIRO_FORMAT_ARGB32,
			panel_width, panel_height, panel_stride);
	cairo_t *cr = cairo_create(cairo_surface);

	/* Clear */
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);

	/* Draw background */
	draw_rounded_rectangle(cr, 0, 0, panel_width, panel_height, CORNER_RADIUS);
	cairo_set_source_rgba(cr, 0.25, 0.25, 0.27, 0.95);
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

		char prefix[16];
		int prefix_len = snprintf(prefix, sizeof(prefix), "%d ", col + 1);
		if (prefix_len < 0 || (size_t)prefix_len >= sizeof(prefix))
			continue;
		if (row != ctx.page_no)
			for (int i = 0; i < prefix_len; i++)
				prefix[i] = ' ';

		/* Draw highlight background */
		if (row == ctx.page_no && col == ctx.highlighted_index) {
			draw_rounded_rectangle(cr, x, y, row_width[col], row_height, 0);
			cairo_set_source_rgba(cr, 0.3, 0.5, 0.8, 1.0);
			cairo_fill(cr);
		}

		cairo_set_source_rgba(cr, 0.95, 0.95, 0.95, 1.0);
		pango_layout_set_text(state->popup_pango_layout, prefix, prefix_len);
		PangoRectangle prefix_rect;
		pango_layout_get_pixel_extents(state->popup_pango_layout, NULL,
																	 &prefix_rect);
		cairo_move_to(cr, x + ITEM_SPACING, y + ROW_SPACING);
		pango_cairo_show_layout(cr, state->popup_pango_layout);

		pango_layout_set_text(state->popup_pango_layout, text, -1);
		cairo_move_to(cr, x + ITEM_SPACING + prefix_rect.width, y + ROW_SPACING);
		pango_cairo_show_layout(cr, state->popup_pango_layout);
	}
	im_engine_cand_end(state->engine);

	cairo_destroy(cr);
	cairo_surface_destroy(cairo_surface);

	/* Commit to wayland; only wl_buffer.release makes this slot reusable. */
	slot->busy = true;
	wl_surface_attach(state->popup_surface, slot->buffer, 0, 0);
	wl_surface_damage(state->popup_surface, 0, 0, panel_width, panel_height);
	wl_surface_commit(state->popup_surface);
	state->pending_render = false;
	im_panel_retry_later(state, 0); /* disarm retry timer */
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

	state->popup_pango_ctx =
			pango_font_map_create_context(pango_cairo_font_map_get_default());
	state->popup_pango_layout = pango_layout_new(state->popup_pango_ctx);
	state->pending_render = true;
	return 0;
}

void im_panel_destroy(struct wlpinyin_state *state) {
	if (state->popup_pango_layout) {
		g_object_unref(state->popup_pango_layout);
		state->popup_pango_layout = NULL;
	}
	if (state->popup_pango_ctx) {
		g_object_unref(state->popup_pango_ctx);
		state->popup_pango_ctx = NULL;
	}

	for (int i = 0; i < WLPINYIN_POPUP_BUFFER_COUNT; i++) {
		if (state->popup_buffers[i].buffer) {
			wl_buffer_destroy(state->popup_buffers[i].buffer);
			state->popup_buffers[i].buffer = NULL;
		}
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
