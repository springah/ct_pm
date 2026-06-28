/* crash.c -- Linux/PortMaster crash + termination signal handling.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * Split out of main.c. On a fault, the handler prints the faulting PC/LR +
 * accessed address and resolves each back to libchrono.so / libc++_shared.so +
 * offset (so `nm` on the .so maps it to a function); a SIGTERM/SIGINT handler
 * requests a clean exit the main loop honours within a frame. This unit lives
 * in portmaster/ (outside the Switch source set), so it is Linux-only and needs
 * no #ifdef __SWITCH__ guard; the Switch build uses libnx's own facilities.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <execinfo.h>
#include <ucontext.h>

#include "so_util.h"
#include "crash.h"

// The two guest modules (defined in main.c) so a fault address can be resolved
// to a module-relative offset.
extern so_module cpp_mod;   // libc++_shared.so
extern so_module game_mod;  // libchrono.so

// Output is unbuffered (see ct_install_crash_handler) so nothing is lost on the
// crash.
static void ct_report_addr(const char *what, uintptr_t a) {
  if (game_mod.load_virtbase && a >= (uintptr_t)game_mod.load_virtbase &&
      a < (uintptr_t)game_mod.load_virtbase + game_mod.load_size)
    fprintf(stderr, "  %s = libchrono.so + 0x%lx\n", what,
            (unsigned long)(a - (uintptr_t)game_mod.load_virtbase));
  else if (cpp_mod.load_virtbase && a >= (uintptr_t)cpp_mod.load_virtbase &&
           a < (uintptr_t)cpp_mod.load_virtbase + cpp_mod.load_size)
    fprintf(stderr, "  %s = libc++_shared.so + 0x%lx\n", what,
            (unsigned long)(a - (uintptr_t)cpp_mod.load_virtbase));
  else
    fprintf(stderr, "  %s = %p (outside guest modules)\n", what, (void *)a);
}

static void ct_crash_handler(int sig, siginfo_t *info, void *uctx) {
  fprintf(stderr, "\n*** ct CRASH: signal %d, fault addr %p ***\n",
          sig, info ? info->si_addr : NULL);
#if defined(__aarch64__)
  if (uctx) {
    uintptr_t pc = (uintptr_t)((ucontext_t *)uctx)->uc_mcontext.pc;
    uintptr_t lr = (uintptr_t)((ucontext_t *)uctx)->uc_mcontext.regs[30];
    ct_report_addr("PC", pc);
    ct_report_addr("LR", lr);
  }
#endif
  if (info) ct_report_addr("accessed", (uintptr_t)info->si_addr);
  void *bt[24];
  int n = backtrace(bt, 24);
  fprintf(stderr, "  --- host backtrace (%d) ---\n", n);
  backtrace_symbols_fd(bt, n, 2);
  fflush(stderr);
  _exit(139);
}

// External-kill (framework / system quit hotkey) -> request a clean exit that
// the main loop honours within a frame, so returning to the frontend is
// reliable + fast.
static volatile sig_atomic_t g_term_requested = 0;
static void ct_term_handler(int sig) { (void)sig; g_term_requested = 1; }
int ct_term_requested(void) { return g_term_requested; }

void ct_install_crash_handler(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = ct_crash_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);

  struct sigaction st;
  memset(&st, 0, sizeof(st));
  st.sa_handler = ct_term_handler;
  sigemptyset(&st.sa_mask);
  sigaction(SIGTERM, &st, NULL);
  sigaction(SIGINT, &st, NULL);
}
