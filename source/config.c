/* config.c -- simple configuration parser
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "config.h"

#define CONFIG_VARS \
  CONFIG_VAR_INT(screen_width); \
  CONFIG_VAR_INT(screen_height); \
  CONFIG_VAR_STR(language); \
  CONFIG_VAR_FLOAT(render_scale); \
  CONFIG_VAR_FLOAT(min_aspect); \
  CONFIG_VAR_INT(gl_threaded); \
  CONFIG_VAR_INT(gl_no_error); \
  CONFIG_VAR_INT(cursor_fix); \
  CONFIG_VAR_INT(remove_mobile_ui); \
  CONFIG_VAR_INT(controller_glyphs); \
  CONFIG_VAR_INT(fix_diagonal_movement); \
  CONFIG_VAR_STR(key_zl); \
  CONFIG_VAR_STR(key_zr); \
  CONFIG_VAR_STR(key_start); \
  CONFIG_VAR_STR(key_select); \
  CONFIG_VAR_INT(right_stick_mirror);

Config config;
static int config_needs_rewrite = 0;

// actual screen size that is in use right now
int screen_width = 1280;
int screen_height = 720;

static inline void parse_var(const char *name, const char *value) {
  #define CONFIG_VAR_INT(var) if (!strcmp(name, #var)) { config.var = atoi(value); return; }
  #define CONFIG_VAR_FLOAT(var) if (!strcmp(name, #var)) { config.var = atof(value); return; }
  #define CONFIG_VAR_STR(var) if (!strcmp(name, #var)) { strlcpy(config.var, value, sizeof(config.var)); return; }
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_FLOAT
  #undef CONFIG_VAR_STR
}

int read_config(const char *file) {
  char line[1024] = { 0 };

  memset(&config, 0, sizeof(Config));
  config_needs_rewrite = 0;
  config.screen_width = -1; // auto
  config.screen_height = -1;
  strlcpy(config.language, LANG_DEFAULT, sizeof(config.language));
  config.render_scale = 0.75f; // GPU-bound handhelds: render 3/4-size, upscale
  config.min_aspect = 1.1f;    // letterbox square-ish panels (e.g. RG CubeXX 1:1) to ~1.1
  config.gl_threaded = 1;      // offload GL submission to a worker core (mesa_glthread)
  config.gl_no_error = 1;      // skip mesa's per-call GL validation (MESA_NO_ERROR)
  config.cursor_fix = 1;            // libchrono patch groups (patches.h); on by default (v2.1.5-verified on-device)
  config.remove_mobile_ui = 1;
  config.controller_glyphs = 1;
  config.fix_diagonal_movement = 1;
  // Input remap: default each extra button to its own stock action (a no-op
  // remap) so config.txt shows a clear, editable value rather than a blank.
  strlcpy(config.key_zl, "zl", sizeof(config.key_zl));
  strlcpy(config.key_zr, "zr", sizeof(config.key_zr));
  strlcpy(config.key_start, "start", sizeof(config.key_start));
  strlcpy(config.key_select, "select", sizeof(config.key_select));
  config.right_stick_mirror = 1; // right stick mirrors movement (current behaviour)

  FILE *f = fopen(file, "r");
  if (f == NULL)
    return -1;

  // parse lines of the forms
  // <spaces> # <whatever> \n
  // <spaces> NAME <spaces> VALUE <spaces> \n
  do {
    char *name = NULL, *value = NULL, *tmp = NULL;
    if (fgets(line, sizeof(line), f) != NULL) {
      name = line;
      // trim name
      while (*name && isspace((int)*name)) ++name;
      if (name[0] == '#') continue; // skip comments
      for (tmp = name; *tmp && !isspace((int)*tmp); ++tmp);
      // if tmp points to the end of the string, there's no value to parse
      if (*tmp != 0) {
        *tmp = 0;
        // value is next; trim value
        for (value = tmp + 1; *value && isspace((int)*value); ++value);
        for (tmp = value + strlen(value) - 1; tmp >= value && isspace((int)*tmp); --tmp) *tmp = 0;
        // got key value pair
        parse_var(name, value);
      }
    }
  } while (!feof(f));

  fclose(f);

  // A malformed/blank "language" line would leave the field empty; restore the
  // default so lang_index() gets a valid value (empty would silently mean en).
  if (config.language[0] == 0)
    strlcpy(config.language, LANG_DEFAULT, sizeof(config.language));

  return config_needs_rewrite ? 1 : 0;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (f == NULL)
    return -1;

  #define CONFIG_VAR_INT(var) fprintf(f, "%s %d\n", #var, config.var)
  #define CONFIG_VAR_FLOAT(var) fprintf(f, "%s %g\n", #var, config.var)
  #define CONFIG_VAR_STR(var) fprintf(f, "%s %s\n", #var, config.var[0] ? config.var : LANG_DEFAULT)
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_FLOAT
  #undef CONFIG_VAR_STR

  fclose(f);

  return 0;
}
