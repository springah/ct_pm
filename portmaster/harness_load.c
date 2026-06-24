// harness_load.c — milestone #1 for the PortMaster port: prove the loader maps
// libc++_shared.so + libchrono.so, applies aarch64 relocations, and reports the
// unresolved-import surface. Does NOT run guest code (no init_array) — that needs
// the full shim layer. Build + run inside the PortMaster builder:
//
//   ~/Desktop/ct-pm-shell.sh
//   gcc -O2 -g -march=armv8-a -Iportmaster -Isource $(sdl2-config --cflags) \
//       portmaster/harness_load.c source/so_util.c portmaster/os_linux.c \
//       $(sdl2-config --libs) -o /tmp/harness && /tmp/harness <dir-with-.so>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>

#include "so_util.h"
#include "config.h"   // SO_NAME, SOCPP_NAME
#include "imports.h"  // dynlib_functions[], dynlib_numfunctions

// Loader deps the Switch build gets from util.c/error.c — minimal Linux versions.
int debugPrintf(char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int n = vprintf(fmt, ap);
  va_end(ap); return n;
}
void fatal_error(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "FATAL: "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
  va_end(ap); exit(1);
}

static so_module cpp_mod, game_mod;

int main(int argc, char **argv) {
  const char *dir = (argc > 1) ? argv[1] : ".";
  char cpp[1024], chrono[1024];
  snprintf(cpp, sizeof cpp, "%s/%s", dir, SOCPP_NAME);
  snprintf(chrono, sizeof chrono, "%s/%s", dir, SO_NAME);

  printf("== load %s ==\n", cpp);
  if (so_load(&cpp_mod, cpp, NULL, (size_t)-1) < 0) { fprintf(stderr, "load %s failed\n", cpp); return 1; }
  printf("   mapped %zu KiB @ %p  syms=%d\n", cpp_mod.load_size / 1024, cpp_mod.load_virtbase, cpp_mod.num_syms);
  so_relocate(&cpp_mod);
  printf("   relocated OK\n");

  printf("== load %s ==\n", chrono);
  if (so_load(&game_mod, chrono, NULL, (size_t)-1) < 0) { fprintf(stderr, "load %s failed\n", chrono); return 1; }
  printf("   mapped %zu KiB @ %p  syms=%d\n", game_mod.load_size / 1024, game_mod.load_virtbase, game_mod.num_syms);
  so_relocate(&game_mod);
  printf("   relocated OK\n");

  // Resolve against the REAL Linux import table (imports.c) + the other loaded
  // module. Anything still printed as "unresolved import" is genuinely missing.
  printf("== resolve libc++_shared (table=%zu entries) ==\n", dynlib_numfunctions);
  so_resolve(&cpp_mod, dynlib_functions, (int)dynlib_numfunctions, 0);
  printf("== resolve libchrono ==\n");
  so_resolve(&game_mod, dynlib_functions, (int)dynlib_numfunctions, 0);

  printf("== finalize (mprotect RX) ==\n");
  so_finalize(&cpp_mod);
  so_finalize(&game_mod);
  so_flush_caches(&cpp_mod);
  so_flush_caches(&game_mod);

  printf("\n== LOADS CLEAN: both modules mapped, relocated, resolved, finalized ==\n");
  return 0;
}
