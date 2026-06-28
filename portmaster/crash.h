/* crash.h -- Linux/PortMaster crash + termination signal handling.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */
#ifndef CT_CRASH_H
#define CT_CRASH_H

// Install fault handlers (SIGSEGV/ABRT/BUS/ILL/FPE -> host backtrace + guest-
// module offset) and clean-exit handlers (SIGTERM/SIGINT). Linux/PortMaster
// only; the Switch build uses libnx's own facilities.
void ct_install_crash_handler(void);

// Non-zero once an external kill (SIGTERM/SIGINT) has requested a clean exit.
// The main loop polls this and returns to the frontend within a frame.
int ct_term_requested(void);

#endif // CT_CRASH_H
