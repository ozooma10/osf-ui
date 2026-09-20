#pragma once
// Portable fixture validation never enters string conversion. Fail loudly if that changes.
// Windows CI uses the real SDK; this header is only on the portable script's include path.
#include <cstdlib>
inline constexpr unsigned CP_UTF8 = 65001;
inline constexpr unsigned MB_ERR_INVALID_CHARS = 8;
inline int MultiByteToWideChar(unsigned, unsigned, const char*, int, wchar_t*, int) { std::abort(); }
