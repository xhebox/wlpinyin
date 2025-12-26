#include <rime_api.h>
#include <stdlib.h>
#include <string.h>

#include "wlpinyin.h"

typedef struct engine {
	RimeApi *api;
	RimeTraits traits;
	RimeSessionId sess;
	char *user_dir;
} rime_engine;

static void handle_notify(void *context_object,
													RimeSessionId session_id,
													const char *message_type,
													const char *message_value) {
	wlpinyin_dbg("context_obj: %p, sess: %ld, msgtype: %s, msg: %s",
							 context_object, session_id, message_type, message_value);
	rime_engine *engine = context_object;
	if (!engine)
		return;
}

im_context_t *im_engine_fetch_context(rime_engine *engine) {
	im_context_t *ctx = calloc(1, sizeof(im_context_t));
	if (!ctx)
		return NULL;

	RimeApi *api = engine->api;

	// Get commit
	RimeCommit commit = {0};
	RIME_STRUCT_INIT(RimeCommit, commit);
	if (api->get_commit(engine->sess, &commit)) {
		ctx->commit_text = strdup(commit.text ? commit.text : "");
		wlpinyin_dbg("commit_text: %s",
								 ctx->commit_text ? ctx->commit_text : "(null)");
		api->free_commit(&commit);
	}

	// Get context
	RimeContext context = {0};
	RIME_STRUCT_INIT(RimeContext, context);
	if (!api->get_context(engine->sess, &context)) {
		return ctx;
	}
	wlpinyin_dbg(
			"composition.preedit: %s (len=%d), cursor=%d, sel=%d-%d",
			context.composition.preedit ? context.composition.preedit : "(null)",
			(int)strlen(context.composition.preedit ? context.composition.preedit
																							: ""),
			context.composition.cursor_pos, context.composition.sel_start,
			context.composition.sel_end);
	wlpinyin_dbg("menu: num_candidates=%d, page_no=%d, highlighted=%d",
							 context.menu.num_candidates, context.menu.page_no,
							 context.menu.highlighted_candidate_index);

	// Copy preedit
	ctx->preedit_text =
			strdup(context.composition.preedit ? context.composition.preedit : "");
	ctx->preedit_cursor = context.composition.cursor_pos;

	// Copy candidates
	ctx->num_candidates = context.menu.num_candidates;
	ctx->page_no = context.menu.page_no;
	ctx->highlighted_index = context.menu.highlighted_candidate_index;
	if (ctx->num_candidates > 0) {
		ctx->candidates = calloc(ctx->num_candidates, sizeof(char *));
		for (int i = 0; i < ctx->num_candidates; i++) {
			ctx->candidates[i] = strdup(context.menu.candidates[i].text);
		}
	}

	api->free_context(&context);

	return ctx;
}

void im_engine_free_context(im_context_t *ctx) {
	if (!ctx)
		return;

	free(ctx->preedit_text);
	free(ctx->commit_text);

	if (ctx->candidates) {
		for (int i = 0; i < ctx->num_candidates; i++) {
			free(ctx->candidates[i]);
		}
		free(ctx->candidates);
	}

	free(ctx);
}

rime_engine *im_engine_new() {
	rime_engine *engine = calloc(1, sizeof(rime_engine));
	if (!engine) {
		return NULL;
	}

	engine->api = rime_get_api();
	if (engine->api == NULL) {
		wlpinyin_err("failed to setup rime api");
		im_engine_free(engine);
		return NULL;
	}

	RimeApi *api = engine->api;

	RIME_STRUCT_INIT(RimeTraits, engine->traits);
	engine->traits.shared_data_dir = "/share/rime-data";

	char *home = getenv("HOME");
	if (home == NULL) {
		im_engine_free(engine);
		return false;
	}

	int size = snprintf(NULL, 0, "%s/.config/wlpinyin", home);
	engine->user_dir = malloc(size + 1);
	snprintf(engine->user_dir, size + 1, "%s/.config/wlpinyin", home);
	engine->traits.user_data_dir = engine->user_dir;

	engine->traits.distribution_name = "wlpinyin";
	engine->traits.distribution_code_name = "wlpinyin";
	engine->traits.distribution_version = "0.1";
	engine->traits.app_name = "rime.wlpinyin";
	api->setup(&engine->traits);

	api->set_notification_handler(handle_notify, engine);

	api->initialize(&engine->traits);

	api->start_maintenance(true);

	// wait for deploy
	// https://github.com/DogLooksGood/emacs-rime/blob/b296856c21d32e700005110328fb6a1d48dcbf8d/lib.c#L136
	api->join_maintenance_thread();

	engine->sess = api->create_session();
	if (engine->sess == 0) {
		wlpinyin_err("failed to setup rime session");
		im_engine_free(engine);
		return NULL;
	}

	RimeSchemaList schemas;
	api->get_schema_list(&schemas);
	for (size_t i = 0; i < schemas.size; i++) {
		wlpinyin_dbg("schema_name: %s, id: %s", schemas.list[i].name,
								 schemas.list[i].schema_id);
		if (api->select_schema(engine->sess, schemas.list[i].schema_id))
			break;
	}
	api->free_schema_list(&schemas);

	return engine;
}

void im_engine_free(rime_engine *engine) {
	if (!engine)
		return;

	if (engine->user_dir != NULL)
		free(engine->user_dir);

	engine->api->destroy_session(engine->sess);
	engine->api->finalize();
	free(engine);
}

bool im_engine_key(rime_engine *engine,
									 xkb_keysym_t keycode,
									 xkb_mod_mask_t mods) {
	if (!engine)
		return false;

	return engine->api->process_key(engine->sess, keycode, mods);
}

void im_engine_toggle(rime_engine *engine) {
	if (!engine)
		return;

	engine->api->set_option(engine->sess, "ascii_mode",
													!engine->api->get_option(engine->sess, "ascii_mode"));
	engine->api->commit_composition(engine->sess);
}

void im_engine_reset(rime_engine *engine) {
	if (!engine)
		return;

	engine->api->clear_composition(engine->sess);
}
