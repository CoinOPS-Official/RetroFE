#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <dlfcn.h>

#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT 10
#define RETRO_ENVIRONMENT_GET_VARIABLE 15
#define RETRO_ENVIRONMENT_SET_VARIABLES 16
#define RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE 17
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE 27
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY 9
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY 31
#define RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION 52
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS 53
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL 54
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY 55
#define RETRO_ENVIRONMENT_GET_LANGUAGE 39
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2 67
#define RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL 68
#define RETRO_NUM_CORE_OPTION_VALUES_MAX 128

struct retro_variable { const char *key; const char *value; };
struct retro_log_callback { void (*log)(int level, const char *fmt, ...); };
struct retro_core_option_value { const char *value; const char *label; };
struct retro_core_option_v2_category { const char *key; const char *desc; const char *info; };
struct retro_core_option_v2_definition {
  const char *key;
  const char *desc;
  const char *desc_categorized;
  const char *info;
  const char *info_categorized;
  const char *category_key;
  struct retro_core_option_value values[RETRO_NUM_CORE_OPTION_VALUES_MAX];
  const char *default_value;
};
struct retro_core_options_v2 {
  struct retro_core_option_v2_category *categories;
  struct retro_core_option_v2_definition *definitions;
};
struct retro_core_options_v2_intl {
  struct retro_core_options_v2 *us;
  struct retro_core_options_v2 *local;
};
struct retro_core_option_definition {
  const char *key;
  const char *desc;
  const char *info;
  struct retro_core_option_value values[RETRO_NUM_CORE_OPTION_VALUES_MAX];
  const char *default_value;
};
struct retro_core_options_intl {
  struct retro_core_option_definition *us;
  struct retro_core_option_definition *local;
};
struct retro_game_info {
  const char *path;
  const void *data;
  size_t size;
  const char *meta;
};

static const char *system_dir = "/tmp";
static bool exit_after_options = false;
static bool options_dumped = false;

static void noop_log(int level, const char *fmt, ...)
{
  (void)level;
  (void)fmt;
}

static void finish_dump(void)
{
  options_dumped = true;
  fflush(NULL);

  /* Some cores, including Dolphin, publish their options from
   * retro_load_game(). Stop immediately after capturing them so the dumper
   * never proceeds into audio/video/input initialization or content boot. */
  if (exit_after_options)
    _Exit(0);
}

static void dump_v2(const struct retro_core_options_v2 *opts)
{
  if (!opts || !opts->definitions)
    return;

  for (const struct retro_core_option_v2_definition *d = opts->definitions; d->key; ++d) {
    printf("%s=%s\n", d->key, d->default_value ? d->default_value : "");
    fprintf(stderr, "OPTION %s default=%s\n", d->key, d->default_value ? d->default_value : "");
    fprintf(stderr, "  values:");
    for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[i].value; ++i)
      fprintf(stderr, " %s", d->values[i].value);
    fprintf(stderr, "\n");
  }
}

static void dump_v1(const struct retro_core_option_definition *defs)
{
  if (!defs)
    return;

  for (const struct retro_core_option_definition *d = defs; d->key; ++d) {
    printf("%s=%s\n", d->key, d->default_value ? d->default_value : "");
    fprintf(stderr, "OPTION %s default=%s\n", d->key, d->default_value ? d->default_value : "");
  }
}

static void dump_legacy(const struct retro_variable *vars)
{
  for (const struct retro_variable *v = vars; v && v->key; ++v) {
    const char *raw = v->value ? v->value : "";
    const char *value = strstr(raw, "; ");
    value = value ? value + 2 : raw;

    const char *end = strchr(value, '|');
    size_t length = end ? (size_t)(end - value) : strlen(value);

    printf("%s=%.*s\n", v->key, (int)length, value);
    fprintf(stderr, "LEGACY %s : %s\n", v->key, raw);
  }
}

static bool env_cb(unsigned cmd, void *data)
{
  switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
      ((struct retro_log_callback *)data)->log = noop_log;
      return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
      *(unsigned *)data = 2;
      return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
      *(unsigned *)data = 0;
      return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
      dump_v2((const struct retro_core_options_v2 *)data);
      finish_dump();
      return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
      const struct retro_core_options_v2_intl *intl =
          (const struct retro_core_options_v2_intl *)data;
      dump_v2(intl ? intl->us : NULL);
      finish_dump();
      return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
      dump_v1((const struct retro_core_option_definition *)data);
      finish_dump();
      return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: {
      const struct retro_core_options_intl *intl =
          (const struct retro_core_options_intl *)data;
      dump_v1(intl ? intl->us : NULL);
      finish_dump();
      return true;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES:
      dump_legacy((const struct retro_variable *)data);
      finish_dump();
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
      ((struct retro_variable *)data)->value = NULL;
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *(bool *)data = false;
      return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      *(const char **)data = system_dir;
      return true;
    default:
      return false;
  }
}

typedef void (*retro_set_environment_t)(bool (*cb)(unsigned, void *));
typedef void (*retro_init_t)(void);
typedef void (*retro_deinit_t)(void);
typedef bool (*retro_load_game_t)(const struct retro_game_info *game);

int main(int argc, char **argv)
{
  bool call_init = false;
  bool call_load_game = false;
  const char *core_path = NULL;

  if (argc == 2) {
    core_path = argv[1];
  } else if (argc == 3 && strcmp(argv[1], "--init") == 0) {
    call_init = true;
    core_path = argv[2];
  } else if (argc == 3 && strcmp(argv[1], "--load-game") == 0) {
    call_init = true;
    call_load_game = true;
    exit_after_options = true;
    core_path = argv[2];
  } else {
    fprintf(stderr, "usage: %s [--init|--load-game] core.so\n", argv[0]);
    return 2;
  }

  const char *configured_system_dir = getenv("COINOPS_CORE_OPTIONS_SYSTEM_DIR");
  if (configured_system_dir && *configured_system_dir)
    system_dir = configured_system_dir;

  void *h = dlopen(core_path, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    fprintf(stderr, "dlopen: %s\n", dlerror());
    return 1;
  }

  retro_set_environment_t setenv =
      (retro_set_environment_t)dlsym(h, "retro_set_environment");
  if (!setenv) {
    fprintf(stderr, "missing retro_set_environment\n");
    dlclose(h);
    return 1;
  }
  setenv(env_cb);

  retro_init_t init = NULL;
  retro_deinit_t deinit = NULL;

  if (call_init) {
    init = (retro_init_t)dlsym(h, "retro_init");
    deinit = (retro_deinit_t)dlsym(h, "retro_deinit");
    if (!init) {
      fprintf(stderr, "missing retro_init\n");
      dlclose(h);
      return 1;
    }
    init();
  }

  if (call_load_game) {
    retro_load_game_t load_game =
        (retro_load_game_t)dlsym(h, "retro_load_game");
    if (!load_game) {
      fprintf(stderr, "missing retro_load_game\n");
      dlclose(h);
      return 1;
    }

    const struct retro_game_info dummy = {
      .path = "/nonexistent/coinops-core-options-dump.iso",
      .data = NULL,
      .size = 0,
      .meta = NULL,
    };

    (void)load_game(&dummy);

    fprintf(stderr,
            "ERROR: core returned from retro_load_game without registering options\n");
    if (deinit)
      deinit();
    dlclose(h);
    return 1;
  }

  if (call_init && deinit)
    deinit();

  dlclose(h);

  if (!options_dumped)
    fprintf(stderr, "WARNING: core did not register any options\n");

  return 0;
}
