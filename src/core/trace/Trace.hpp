/// @file core/trace/Trace.hpp
/// @brief Runtime tracing system for user-facing compilation progress.

#pragma once

#include <string>
#include <iostream>

/**
 * @brief Runtime tracing system for compilation progress.
 * 
 * This is a user-facing feature, controlled by --verbose and --trace flags.
 * Output goes to stderr, so stdout remains clean for structured output (JSON).
 * 
 * This is a core infrastructure component (like Diagnostic), not CLI-specific.
 * 
 * Usage:
 *   Trace::info("Parsing file: ", filePath);
 *   Trace::detail("Resolved ", imports.size(), " imports");
 *   Trace::error("Failed to open file: ", path);
 * 
 * @note This is header-only to avoid a separate .cpp file.
 */
class Trace {
public:
    /// @brief Set the trace level (0=none, 1=info, 2=detail)
    static void setLevel(int level) { s_level = level; }

    /// @brief Get the current trace level
    static int level() { return s_level; }

    /// @brief Log an info-level message (level 1 or higher)
    template<typename... Args>
    static void info(Args&&... args) {
        if (s_level >= 1) {
            log("[Info] ", std::forward<Args>(args)...);
        }
    }

    /// @brief Log a detail-level message (level 2 or higher)
    template<typename... Args>
    static void detail(Args&&... args) {
        if (s_level >= 2) {
            log("[Detail] ", std::forward<Args>(args)...);
        }
    }

    /// @brief Log an error message (always shown, regardless of level)
    template<typename... Args>
    static void error(Args&&... args) {
        log("[Error] ", std::forward<Args>(args)...);
    }

private:
    static int s_level;

    template<typename... Args>
    static void log(const char* prefix, Args&&... args) {
        std::cerr << prefix;
        buildMessage(std::cerr, std::forward<Args>(args)...);
        std::cerr << std::endl;
    }

    // Helper to stream arguments to ostream
    template<typename T, typename... Rest>
    static void buildMessage(std::ostream& os, T&& first, Rest&&... rest) {
        os << std::forward<T>(first);
        buildMessage(os, std::forward<Rest>(rest)...);
    }

    static void buildMessage(std::ostream&) {}
};
