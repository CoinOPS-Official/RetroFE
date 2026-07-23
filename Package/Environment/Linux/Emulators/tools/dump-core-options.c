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

static const char *system_dir = "/tmp";

static void noop_log(int level, const char *fmt, ...)
{
  (void)level;
  (void)fmt;
}

static void dump_v2(const struct retro_core_options_v2 *opts) {
  if (!opts || !opts->definitions) return;
  for (const struct retro_core_option_v2_definition *d = opts->definitions; d->key; ++d) {
    printf("%s=%s\n", d->key, d->default_value ? d->default_value : "");
    fprintf(stderr, "OPTION %s default=%s\n", d->key, d->default_value ? d->default_value : "");
    fprintf(stderr, "  values:");
    for (int i=0; i<RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[i].value; ++i)
      fprintf(stderr, " %s", d->values[i].value);
    fprintf(stderr, "\n");
  }
}

static void dump_v1(const struct retro_core_option_definition *defs) {
  if (!defs) return;
  for (const struct retro_core_option_definition *d = defs; d->key; ++d) {
    printf("%s=%s\n", d->key, d->default_value ? d->default_value : "");
    fprintf(stderr, "OPTION %s default=%s\n", d->key, d->default_value ? d->default_value : "");
  }
}

static bool env_cb(unsigned cmd, void *data) {
  switch(cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
      ((struct retro_log_callback*)data)->log = noop_log;
      return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
      *(unsigned*)data = 2;
      return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
      *(unsigned*)data = 0;
      return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
      dump_v2((const struct retro_core_options_v2*)data);
      return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
      const struct retro_core_options_v2_intl *intl = (const struct retro_core_options_v2_intl*)data;
      dump_v2(intl ? intl->us : NULL);
      return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
      dump_v1((const struct retro_core_option_definition*)data);
      return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: {
      const struct retro_core_options_intl *intl = (const struct retro_core_options_intl*)data;
      dump_v1(intl ? intl->us : NULL);
      return true;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
      const struct retro_variable *v=(const struct retro_variable*)data;
      for (; v && v->key; ++v) fprintf(stderr,"LEGACY %s : %s\n", v->key, v->value?v->value:"");
      return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE:
      ((struct retro_variable*)data)->value = NULL;
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *(bool*)data = false;
      return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      *(const char**)data = system_dir;
      return true;
    default:
      return false;
  }
}

typedef void (*retro_set_environment_t)(bool (*cb)(unsigned, void*));
typedef void (*retro_init_t)(void);
typedef void (*retro_deinit_t)(void);

int main(int argc, char **argv) {
  bool call_init = false;
  const char *core_path = NULL;

  if (argc == 2) {
    core_path = argv[1];
  } else if (argc == 3 && strcmp(argv[1], "--init") == 0) {
    call_init = true;
    core_path = argv[2];
  } else {
    fprintf(stderr, "usage: %s [--init] core.so\n", argv[0]);
    return 2;
  }

  const char *configured_system_dir = getenv("COINOPS_CORE_OPTIONS_SYSTEM_DIR");
  if (configured_system_dir && *configured_system_dir)
    system_dir = configured_system_dir;

  void *h=dlopen(core_path, RTLD_NOW|RTLD_LOCAL);
  if(!h){ fprintf(stderr,"dlopen: %s\n",dlerror()); return 1; }

  retro_set_environment_t setenv=(retro_set_environment_t)dlsym(h,"retro_set_environment");
  if(!setenv){ fprintf(stderr,"missing retro_set_environment\n"); dlclose(h); return 1; }
  setenv(env_cb);

  if (call_init) {
    retro_init_t init=(retro_init_t)dlsym(h,"retro_init");
    retro_deinit_t deinit=(retro_deinit_t)dlsym(h,"retro_deinit");
    if(!init){ fprintf(stderr,"missing retro_init\n"); dlclose(h); return 1; }
    init();
    if (deinit)
      deinit();
  }

  dlclose(h);
  return 0;
}
