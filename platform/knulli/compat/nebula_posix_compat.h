/* nebula_posix_compat.h — MSVC CRT names used by Nebula's runtime, mapped to POSIX.
 * Force-include (-include) when building the portable runtime for Linux/aarch64 (KNULLI). */
#pragma once
#if !defined(_WIN32)
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef __cplusplus
#include <cstddef>
#include <cstdlib>
#include <cstring>
extern "C++" {
inline int getenv_s(std::size_t* required, char* buffer, std::size_t size, const char* name) {
    const char* v = std::getenv(name);
    if (!v) { if (required) *required = 0; if (buffer && size) buffer[0] = 0; return 0; }
    const std::size_t n = std::strlen(v) + 1;
    if (required) *required = n;
    if (!buffer) return 0;
    if (size < n) { if (size) buffer[0] = 0; return ERANGE; }
    std::memcpy(buffer, v, n);
    return 0;
}
inline int _dupenv_s(char** out, std::size_t* len, const char* name) {
    const char* v = std::getenv(name);
    if (!v) { *out = nullptr; if (len) *len = 0; return 0; }
    const std::size_t n = std::strlen(v) + 1;
    *out = static_cast<char*>(std::malloc(n));
    if (!*out) return ENOMEM;
    std::memcpy(*out, v, n);
    if (len) *len = n;
    return 0;
}
}
#endif
#include <stdio.h>
#include <limits.h>
#ifndef MAX_PATH
#define MAX_PATH PATH_MAX
#endif
#ifdef __cplusplus
extern "C++" inline int fopen_s(FILE** f, const char* path, const char* mode) { *f = fopen(path, mode); return *f ? 0 : errno; }
#endif
#ifdef __cplusplus
extern "C++" inline int _putenv_s(const char* n, const char* v) { return (v && *v) ? setenv(n, v, 1) : unsetenv(n); }
#endif
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
typedef uint32_t UINT32;
typedef uint64_t UINT64;
#endif
