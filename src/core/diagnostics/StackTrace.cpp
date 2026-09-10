/// @file StackTrace.cpp
/// @brief Platform-specific stack trace implementation.

#include "StackTrace.hpp"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32

// ─── Windows (MSVC / MinGW with DbgHelp) ──────────────────────────────────
#include <windows.h>
#include <dbghelp.h>
#include <mutex>

#pragma comment(lib, "dbghelp.lib")

namespace lucid::diag {

namespace {

/// @brief One-time DbgHelp initialization.
///
/// SymInitialize is documented as not thread-safe on first call. Guard it
/// with std::call_once so concurrent asserts don't race on the symbol table.
void ensureSymInitialized() {
    static std::once_flag once;
    std::call_once(once, [] {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_LOAD_LINES
                    | SYMOPT_DEFERRED_LOADS
                    | SYMOPT_UNDNAME
                    | SYMOPT_NO_PROMPTS);
        SymInitialize(process, nullptr, TRUE);
    });
}

} // anonymous namespace

void printStackTrace(int skipFrames) {
    ensureSymInitialized();

    constexpr int kMaxFrames = 62;
    void* frames[kMaxFrames];
    USHORT count = CaptureStackBackTrace(
        static_cast<DWORD>(skipFrames), kMaxFrames, frames, nullptr);

    if (count == 0) {
        std::fprintf(stderr, "  (no stack frames captured)\n");
        return;
    }

    HANDLE process = GetCurrentProcess();

    // SYMBOL_INFO is variable-length; the trailing array holds the name.
    char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen   = MAX_SYM_NAME;

    std::fprintf(stderr, "\n--- stack trace ---\n");
    for (USHORT i = 0; i < count; ++i) {
        DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        DWORD64 displacement = 0;

        if (SymFromAddr(process, addr, &displacement, symbol)) {
            DWORD lineDisplacement = 0;
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

            if (SymGetLineFromAddr64(process, addr,
                                     &lineDisplacement, &line)) {
                std::fprintf(stderr, "  [%2u] %s  (%s:%lu)\n",
                             i, symbol->Name, line.FileName,
                             static_cast<unsigned long>(line.LineNumber));
            } else {
                std::fprintf(stderr, "  [%2u] %s  +0x%llx\n",
                             i, symbol->Name,
                             static_cast<unsigned long long>(displacement));
            }
        } else {
            std::fprintf(stderr, "  [%2u] 0x%p  (no symbol)\n",
                         i, frames[i]);
        }
    }
    std::fprintf(stderr, "-------------------\n\n");
}

} // namespace lucid::diag

#else

// ─── POSIX (Linux, macOS, MinGW without DbgHelp) ──────────────────────────
#include <execinfo.h>

namespace lucid::diag {

void printStackTrace(int skipFrames) {
    constexpr int kMaxFrames = 62;
    void* frames[kMaxFrames];
    int count = backtrace(frames, kMaxFrames);

    if (count <= 0) {
        std::fprintf(stderr, "  (no stack frames captured)\n");
        return;
    }

    char** symbols = backtrace_symbols(frames, count);
    if (!symbols) {
        std::fprintf(stderr, "  (backtrace_symbols failed; raw addresses)\n");
        for (int i = skipFrames; i < count; ++i) {
            std::fprintf(stderr, "  [%2d] 0x%p\n", i - skipFrames, frames[i]);
        }
        return;
    }

    std::fprintf(stderr, "\n--- stack trace ---\n");
    for (int i = skipFrames; i < count; ++i) {
        std::fprintf(stderr, "  [%2d] %s\n", i - skipFrames, symbols[i]);
    }
    std::fprintf(stderr, "-------------------\n\n");

    std::free(symbols);
}

} // namespace lucid::diag

#endif