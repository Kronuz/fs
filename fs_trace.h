/*
 * Default (no-op) logging hooks for the standalone `fs` library.
 *
 * fs.cc instruments its operations through a small L_* family that is no-op by
 * default, so the library builds with zero dependency on any logging, error-string,
 * or repr header. The engine's behavior does not depend on them: they only emit
 * diagnostics a host may want to surface.
 *
 * The macros used by fs.cc:
 *
 *   - L_CALL(...)    — entry trace for each public function.
 *   - L_DEBUG(...)   — low-severity diagnostic.
 *   - L_ERR(...)     — a failed filesystem op, with errno name/description in its args.
 *   - L_INFO(...)    — an informational notice.
 *   - L_WARNING(...) — a recoverable problem.
 *   - L_FS(...)      — fs-specific verbose trace (defaults to L_NOTHING even in a host).
 *   - L_NOTHING(...) — explicit no-op (the default target for the others).
 *
 * Because error::name(errno) / error::description(errno) and repr(path) appear ONLY
 * inside these macros' arguments, a no-op expansion drops them unevaluated -- so the
 * standalone library needs neither an errno-names nor a repr dependency. A host that
 * wants real logging provides its own versions, two ways:
 *
 *   1. Point FS_TRACE_HEADER at a header that defines them (that header is then
 *      responsible for pulling in whatever error:: / repr it references), e.g.
 *        c++ -DFS_TRACE_HEADER='"my_fs_trace.h"' ...
 *      fs.cc includes that instead of this file.
 *
 *   2. Define the macros directly before fs.cc includes fs.hh.
 *
 * Each macro is `#ifndef`-guarded, so defining any subset is fine; the rest fall
 * back to the no-op defaults here.
 */

#pragma once

#ifndef L_NOTHING
#define L_NOTHING(...)
#endif

#ifndef L_CALL
#define L_CALL L_NOTHING
#endif

#ifndef L_DEBUG
#define L_DEBUG L_NOTHING
#endif

#ifndef L_ERR
#define L_ERR L_NOTHING
#endif

#ifndef L_INFO
#define L_INFO L_NOTHING
#endif

#ifndef L_WARNING
#define L_WARNING L_NOTHING
#endif
