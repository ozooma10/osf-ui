#pragma once


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
