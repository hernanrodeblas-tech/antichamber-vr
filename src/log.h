#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstddef>

void LogInit();
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void LogHex(const char* title, const void* p, size_t n);

// Safe memory probing (no SEH available on i686 mingw).
bool Readable(const void* p, size_t n);
