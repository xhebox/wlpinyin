#include <rime_api.h>
#include <stdlib.h>
#include <string.h>

#include "wlpinyin.h"

typedef struct engine {
	RimeApi *api;
	RimeTraits traits;
	RimeSessionId sess;
	RimeStatus status;
	RimeContext context;
	RimeCommit commit;
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

static void im_engine_update(rime_engine *engine) {
	RimeApi *api = engine->api;

	api->free_status(&engine->status);
	api->get_status(engine->sess, &engine->status);

	api->free_context(&engine->context);
	api->get_context(engine->sess, &engine->context);

	api->free_commit(&engine->commit);
	api->get_commit(engine->sess, &engine->commit);
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
	for (int i = 0; i < schemas.size; i++) {
		wlpinyin_dbg("schema_name: %s, id: %s", schemas.list[i].name,
								 schemas.list[i].schema_id);
		if (api->select_schema(engine->sess, schemas.list[i].schema_id))
			break;
	}
	api->free_schema_list(&schemas);

	RIME_STRUCT_INIT(RimeStatus, engine->status);
	RIME_STRUCT_INIT(RimeContext, engine->context);
	RIME_STRUCT_INIT(RimeCommit, engine->commit);

	return engine;
}

void im_engine_free(rime_engine *engine) {
	if (!engine)
		return;

	if (engine->user_dir != NULL)
		free(engine->user_dir);

	engine->api->free_commit(&engine->commit);
	engine->api->free_context(&engine->context);
	engine->api->free_status(&engine->status);
	engine->api->destroy_session(engine->sess);
	engine->api->finalize();
	free(engine);
}

preedit_t im_engine_preedit(rime_engine *engine) {
	preedit_t res = {};

	if (!engine)
		return res;

	res.start = engine->context.composition.sel_start;
	res.end = engine->context.composition.sel_end;
	res.cursor = engine->context.composition.cursor_pos;
	res.text = engine->context.composition.preedit;

	return res;
}

bool im_engine_key(rime_engine *engine,
									 xkb_keysym_t keycode,
									 xkb_mod_mask_t mods) {
	if (!engine)
		return false;

	bool res = engine->api->process_key(engine->sess, keycode, mods);

	im_engine_update(engine);
	return res;
}

candidate_t im_engine_candidate(rime_engine *engine) {
	candidate_t res = {};

	if (!engine)
		return res;

	res.highlighted_candidate_index =
			engine->context.menu.highlighted_candidate_index;
	res.page_no = engine->context.menu.page_no;
	res.num_candidates = engine->context.menu.num_candidates;
	return res;
}

const char *im_engine_candidate_get(rime_engine *engine, int index) {
	if (!engine)
		return NULL;

	if (index >= engine->context.menu.num_candidates)
		return NULL;

	RimeCandidate *cand = &engine->context.menu.candidates[index];
	return cand->text;
}

const char *im_engine_commit_text(rime_engine *engine) {
	if (!engine)
		return "";

	const char *ret = engine->commit.text;
	im_engine_update(engine);
	return ret ? ret : "";
}

bool im_engine_composing(rime_engine *engine) {
	if (!engine)
		return false;

	return engine->status.is_composing;
}

bool im_engine_activated(rime_engine *engine) {
	if (!engine)
		return false;

	return !engine->status.is_ascii_mode;
}

void im_engine_toggle(rime_engine *engine) {
	if (!engine)
		return;

	engine->api->set_option(engine->sess, "ascii_mode",
													!engine->api->get_option(engine->sess, "ascii_mode"));
	engine->api->commit_composition(engine->sess);
	im_engine_update(engine);
}

void im_engine_reset(rime_engine *engine) {
	if (!engine)
		return;

	engine->api->clear_composition(engine->sess);
	engine->api->commit_composition(engine->sess);
	im_engine_update(engine);
}
