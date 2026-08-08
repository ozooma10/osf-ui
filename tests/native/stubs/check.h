#pragma once

// Shared assertion scaffolding for the native suites. Sixteen suites used to
// carry their own copy of this macro and its counters with identical
// semantics; they now share this one. `stubs/` is on every suite's include
// path (run.sh and the wv2-pipe-tests xmake target), so a suite just includes
// "check.h", uses CHECK(expr), keeps its own summary line, and returns
// g_failures from main() — the failure count is what run.sh sums into the
// overall exit code.
//
// CHECK never aborts: a failing suite keeps running so one run reports every
// failing check.

#include <cstdio>

inline int g_failures = 0;
inline int g_checks = 0;

#define CHECK(expr)                                                              \
	do {                                                                         \
		++g_checks;                                                              \
		if (!(expr)) {                                                           \
			++g_failures;                                                        \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
		}                                                                        \
	} while (false)
