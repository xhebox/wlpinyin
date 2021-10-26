#include <limits.h>
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
	bool running;
	bool ready;
	char *user_dir;
} rime_engine;

static void handle_notify(void *context_object,
													RimeSessionId session_id,
													const char *message_type,
													const char *message_value) {
	wlpinyin_dbg("context_obj: %p, sess: %ld, msgtype: %s, msg: %s",
							 context_object, session_id, message_type, message_value);
	rime_engine *engine = context_object;
	if (engine == NULL)
		return;

	if (strcmp(message_type, "deploy") == 0) {
		if (strcmp(message_value, "success") == 0) {
			engine->ready = true;
		}
	}
}

static void im_engine_update(rime_engine *engine) {
	RimeApi *api = engine->api;

	RIME_STRUCT_CLEAR(engine->status);
	api->free_status(&engine->status);
	api->get_status(engine->sess, &engine->status);

	RIME_STRUCT_CLEAR(engine->context);
	api->free_context(&engine->context);
	api->get_context(engine->sess, &engine->context);

	RIME_STRUCT_CLEAR(engine->commit);
	api->free_commit(&engine->commit);
	api->get_commit(engine->sess, &engine->commit);
}

rime_engine *im_engine_new() {
	rime_engine *engine = calloc(1, sizeof(rime_engine));
	if (engine == NULL) {
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

	engine->ready = false;
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
	if (engine == NULL)
		return;

	if (engine->user_dir != NULL)
		free(engine->user_dir);

	engine->api->free_commit(&engine->commit);
	engine->api->free_context(&engine->context);
	engine->api->free_status(&engine->status);
	engine->api->destroy_session(engine->sess);
	im_engine_deactivate(engine);
	engine->api->finalize(&engine->traits);
	free(engine);
}

void im_engine_activate(rime_engine *engine) {
	if (engine == NULL)
		return;

	if (!engine->running) {
		engine->running = true;
		engine->api->clear_composition(engine->sess);
	}
}

bool im_engine_activated(rime_engine *engine) {
	if (engine == NULL)
		return false;

	return engine->running;
}

void im_engine_deactivate(rime_engine *engine) {
	if (engine == NULL)
		return;

	if (engine->running) {
		engine->running = false;
	}
}

const char *im_engine_preedit_get(rime_engine *engine) {
	if (engine == NULL || !engine->running)
		return NULL;

	return engine->context.composition.preedit;
}

bool im_engine_key(rime_engine *engine,
									 xkb_keysym_t keycode,
									 xkb_mod_mask_t mods) {
	if (engine == NULL || !engine->running)
		return false;

	bool res = engine->api->process_key(engine->sess, keycode, mods);

	im_engine_update(engine);

	wlpinyin_dbg("processed: %d", res);

	return res;
}

bool im_engine_page(rime_engine *engine, bool next) {
	if (engine == NULL || !engine->running)
		return false;

	bool res = engine->api->process_key(
			engine->sess, next ? XKB_KEY_Page_Down : XKB_KEY_Page_Up, 0);
	im_engine_update(engine);

	wlpinyin_dbg("page[%s]: %d", next ? "next" : "prev", res);
	return res;
}

int im_engine_candidate_len(rime_engine *engine) {
	if (engine == NULL || !engine->running)
		return 0;

	return engine->context.menu.num_candidates;
}

const char *im_engine_candidate_get(rime_engine *engine, int index) {
	if (engine == NULL || !engine->running)
		return NULL;

	if (index >= engine->context.menu.num_candidates)
		return NULL;

	RimeCandidate *cand = &engine->context.menu.candidates[index];

	return cand->text;
}

const char *im_engine_commit_text(rime_engine *engine) {
	if (engine == NULL || !engine->running)
		return false;

	return engine->commit.text;
}

bool im_engine_candidate_choose(rime_engine *engine, int index) {
	if (engine == NULL || !engine->running)
		return false;

	if (index >= engine->context.menu.num_candidates)
		return false;

	bool res = engine->api->select_candidate_on_current_page(engine->sess, index);

	wlpinyin_dbg("select[%d]: %s", index, res ? "true" : "false");

	im_engine_update(engine);

	return res;
}

bool im_engine_cursor(rime_engine *engine, bool right) {
	if (engine == NULL || !engine->running)
		return false;

	bool res = engine->api->process_key(engine->sess,
																			right ? XKB_KEY_Right : XKB_KEY_Left, 0);
	im_engine_update(engine);

	wlpinyin_dbg("cursor[%s]: %d", right ? "right" : "left", res);

	return res;
}

bool im_engine_delete(rime_engine *engine, bool delete) {
	if (engine == NULL || !engine->running)
		return false;

	bool res = engine->api->process_key(
			engine->sess, delete ? XKB_KEY_Delete : XKB_KEY_BackSpace, 0);

	im_engine_update(engine);

	wlpinyin_dbg("delete[%s]: %d", delete ? "delete" : "backspace", res);

	return res;
}
