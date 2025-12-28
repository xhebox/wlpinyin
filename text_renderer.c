#ifndef ENABLE_POPUP

#include <stdio.h>
#include <string.h>

#include "wlpinyin.h"

int im_panel_init(struct wlpinyin_state *state) {
	UNUSED(state);
	return 0;
}

void im_panel_destroy(struct wlpinyin_state *state) {
	UNUSED(state);
}

int im_panel_update(struct wlpinyin_state *state) {
	im_preedit_t preedit = im_engine_preedit(state->engine);
	im_context_t ctx = im_engine_context(state->engine);

	char buf[2048] = {0};
	int bufptr = 0;

	if (!preedit.text || strlen(preedit.text) == 0) {
		zwp_input_method_v2_set_preedit_string(state->input_method, "", 0, 0);
		return 0;
	}

	bufptr += snprintf(&buf[bufptr], sizeof buf - bufptr, "[%d] ", ctx.page_no);
	int preedit_begin = bufptr + preedit.begin;
	int preedit_end = bufptr + preedit.end;
	bufptr += snprintf(&buf[bufptr], sizeof buf - bufptr, "%s", preedit.text);

	im_engine_cand_begin(state->engine, 0);
	for (int i = 0; i < ctx.page_size && im_engine_cand_next(state->engine);
			 i++) {
		bool highlighted = i == ctx.highlighted_index;
		const char *text = im_engine_cand_get(state->engine);
		bufptr += snprintf(&buf[bufptr], sizeof buf - bufptr, " %s%d %s%s",
											 highlighted ? "[" : "", i + 1, text ? text : "",
											 highlighted ? "]" : "");
	}
	im_engine_cand_end(state->engine);
	zwp_input_method_v2_set_preedit_string(state->input_method, buf,
																				 preedit_begin, preedit_end);
	return 0;
}

#endif
