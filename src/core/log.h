#pragma once
#include <cstdio>

namespace logx {

void Open(const char* path);
void Close();
void Info(const char* fmt, ...);            // a plugin.log line (written by a background thread)
void Printf(FILE* f, const char* fmt, ...);  // text for another file, written by the same thread in order
void Flush();                                // writes everything queued now (before a deliberate exit, on a crash)

}  // namespace logx

#define LOG(...) ::logx::Info(__VA_ARGS__)
