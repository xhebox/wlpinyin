#include <rime_api.h>
#include <stdlib.h>
#include <string.h>

#include "wlpinyin.h"

typedef struct engine {
	RimeApi *api;
	RimeTraits traits;
	RimeSessionId sess;
	char *user_dir;
	im_context_t ctx;
	im_preedit_t preedit;
	char *commit_text;
	RimeCandidateListIterator iter;
} rime_engine;

static void im_engine_update_context(rime_engine *engine);

static void handle_notify(void *context_object,
													RimeSessionId session_id,
													const char *message_type,
													const char *message_value) {
	wlpinyin_dbg("context_obj: %p, sess: %ld, msgtype: %s, msg: %s",
							 context_object, session_id, message_type, message_value);
}

static void im_engine_update_context(rime_engine *engine) {
	RimeApi *api = engine->api;

	// Get commit
	RimeCommit commit = {0};
	RIME_STRUCT_INIT(RimeCommit, commit);
	if (engine->commit_text) {
		free(engine->commit_text);
		engine->commit_text = NULL;
	}
	if (api->get_commit(engine->sess, &commit)) {
		engine->commit_text = strdup(commit.text ? commit.text : "");
		wlpinyin_dbg("commit_text: %s", engine->commit_text);
		api->free_commit(&commit);
	}

	// Get context
	RimeContext context = {0};
	RIME_STRUCT_INIT(RimeContext, context);
	if (api->get_context(engine->sess, &context)) {
		wlpinyin_dbg(
				"composition.preedit: %s, cursor=%d, sel=%d-%d",
				context.composition.preedit ? context.composition.preedit : "(null)",
				context.composition.cursor_pos, context.composition.sel_start,
				context.composition.sel_end);
		wlpinyin_dbg("menu: num_candidates=%d, page_no=%d, highlighted=%d",
								 context.menu.num_candidates, context.menu.page_no,
								 context.menu.highlighted_candidate_index);

		// Copy preedit
		if (engine->preedit.text) {
			free(engine->preedit.text);
			engine->preedit.text = NULL;
		}
		engine->preedit.text =
				strdup(context.composition.preedit ? context.composition.preedit : "");
		engine->preedit.begin = context.composition.sel_start;
		engine->preedit.end = context.composition.sel_end;

		// Copy candidates
		engine->ctx.page_size = context.menu.page_size;
		engine->ctx.page_no = context.menu.page_no;
		engine->ctx.highlighted_index = context.menu.highlighted_candidate_index;

		api->free_context(&context);
	}
}

im_context_t im_engine_context(rime_engine *engine) {
	return engine->ctx;
}

im_preedit_t im_engine_preedit(rime_engine *engine) {
	return engine->preedit;
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
	if (engine->preedit.text)
		free(engine->preedit.text);
	if (engine->commit_text)
		free(engine->commit_text);
	if (engine->user_dir != NULL)
		free(engine->user_dir);
	engine->api->destroy_session(engine->sess);
	engine->api->finalize();
	free(engine);
}

void im_engine_cand_begin(struct engine *engine, int off) {
	engine->api->candidate_list_from_index(engine->sess, &engine->iter, off);
}

const char *im_engine_cand_get(struct engine *engine) {
	return engine->iter.candidate.text ? engine->iter.candidate.text : "";
}

bool im_engine_cand_next(struct engine *engine) {
	return engine->api->candidate_list_next(&engine->iter);
}

void im_engine_cand_end(struct engine *engine) {
	engine->api->candidate_list_end(&engine->iter);
}

const char *im_engine_commit(struct engine *engine) {
	return engine->commit_text ? engine->commit_text : "";
}

bool im_engine_key(rime_engine *engine,
									 xkb_keysym_t keycode,
									 xkb_mod_mask_t mods) {
	bool handled = engine->api->process_key(engine->sess, keycode, mods);
	if (handled)
		im_engine_update_context(engine);
	return handled;
}

void im_engine_toggle(rime_engine *engine) {
	engine->api->set_option(engine->sess, "ascii_mode",
													!engine->api->get_option(engine->sess, "ascii_mode"));
	engine->api->commit_composition(engine->sess);
	im_engine_update_context(engine);
}

void im_engine_reset(rime_engine *engine) {
	engine->api->clear_composition(engine->sess);
	im_engine_update_context(engine);
}
