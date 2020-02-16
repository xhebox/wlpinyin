#include <pinyin.h>

#include "wlpinyin.h"

#define PINYIN_DATA_SYSTEM "/lib/libpinyin/data"
#define PINYIN_DATA_USER "/home/xhe/.config/pinyin"

typedef struct pinyin_engine {
	pinyin_context_t *context;
	pinyin_instance_t *instance;
	bool running;
} pinyin_engine;

void *im_engine_new() {
	pinyin_engine *engine = calloc(1, sizeof(pinyin_engine));
	if (engine == NULL) {
		return NULL;
	}

	engine->context = pinyin_init(PINYIN_DATA_SYSTEM, PINYIN_DATA_USER);
	if (engine->context == NULL) {
		wlpinyin_err("failed to setup pinyin context");
		im_engine_free(engine);
		return NULL;
	}

	pinyin_option_t options = PINYIN_INCOMPLETE | PINYIN_CORRECT_ALL |
														USE_DIVIDED_TABLE | USE_RESPLIT_TABLE |
														DYNAMIC_ADJUST;
	pinyin_set_options(engine->context, options);

	engine->instance = pinyin_alloc_instance(engine->context);
	if (engine->instance == NULL) {
		wlpinyin_err("failed to setup pinyin instance");
		im_engine_free(engine);
		return NULL;
	}

	engine->running = false;

	pinyin_set_double_pinyin_scheme(engine->context, DOUBLE_PINYIN_XHE);

	return engine;
}

void im_engine_free(void *_engine) {
	pinyin_engine *engine = _engine;
	if (engine == NULL)
		return;

	if (engine->instance != NULL)
		pinyin_free_instance(engine->instance);

	if (engine->context != NULL) {
		pinyin_mask_out(engine->context, 0, 0);
		pinyin_save(engine->context);
		pinyin_fini(engine->context);
	}

	free(engine);
}

void im_engine_activate(void *_engine) {
	pinyin_engine *engine = _engine;
	if (engine == NULL)
		return;

	if (!engine->running) {
		engine->running = true;
		pinyin_reset(engine->instance);
	}
}

bool im_engine_activated(void *_engine) {
	pinyin_engine *engine = _engine;
	if (engine == NULL)
		return false;

	return engine->running;
}

void im_engine_deactivate(void *_engine) {
	pinyin_engine *engine = _engine;
	if (engine == NULL)
		return;

	if (engine->running) {
		engine->running = false;
		pinyin_train(engine->instance, 0);
		pinyin_save(engine->context);
	}
}

const char *im_engine_aux_get(void *_engine, int cursor) {
	pinyin_engine *engine = _engine;
	if (engine == NULL || !engine->running)
		return NULL;

	gchar *aux_text = NULL;
	pinyin_get_double_pinyin_auxiliary_text(engine->instance, cursor, &aux_text);
	return aux_text;
}

void im_engine_aux_free(void *_engine, const char *aux) {
	pinyin_engine *engine = _engine;
	if (engine == NULL)
		return;

	g_free((void *)aux);
}

void im_engine_parse(void *_engine, const char *text, const char *prefix) {
	pinyin_engine *engine = _engine;
	if (engine == NULL || !engine->running)
		return;

	pinyin_parse_more_double_pinyins(engine->instance, text);

	pinyin_guess_sentence_with_prefix(engine->instance, prefix);
	pinyin_guess_predicted_candidates(engine->instance, prefix);
	pinyin_guess_candidates(
			engine->instance, 0,
			SORT_BY_PHRASE_LENGTH_AND_PINYIN_LENGTH_AND_FREQUENCY);
}

const char *im_engine_candidate_get(void *_engine, int index) {
	pinyin_engine *engine = _engine;
	if (engine == NULL || !engine->running)
		return NULL;

	guint uind = index;
	guint num = 0;
	pinyin_get_n_candidate(engine->instance, &num);
	if (uind > num)
		return NULL;

	lookup_candidate_t *candidate = NULL;
	pinyin_get_candidate(engine->instance, uind, &candidate);
	if (candidate == NULL)
		return NULL;

	const char *ptr = NULL;
	pinyin_get_candidate_string(engine->instance, candidate, &ptr);
	return ptr;
}

void im_engine_candidate_free(void *_engine, const char *text) {
	pinyin_engine *engine = _engine;
	if (engine == NULL)
		return;

	// g_free((void *)text);
}

size_t im_engine_candidate_choose(void *_engine, int index) {
	pinyin_engine *engine = _engine;
	if (engine == NULL || !engine->running)
		return 0;

	lookup_candidate_t *candidate = NULL;
	pinyin_get_candidate(engine->instance, index, &candidate);
	if (candidate == NULL)
		return 0;

	return pinyin_choose_candidate(engine->instance, 0, candidate);
}

void im_engine_remember(void *_engine, const char *text) {
	pinyin_engine *engine = _engine;
	if (engine == NULL)
		return;

	pinyin_remember_user_input(engine->instance, text, -1);
}
