/**
 * @file Diagnostic.hpp
 * @brief The diagnostic engine: collection, query, and formatting.
 *
 * ─── Design: engine is session-scoped, not a singleton ────────────────────
 * A DiagnosticEngine is created by whoever runs a compilation — the CLI, the
 * LSP server, a test — and passed by reference to every stage that can
 * report. It is not global state. Two compilations running in the same
 * process (the LSP server handles many) get two engines and their
 * diagnostics do not mix.
 *
 * ─── Design: messages are built at the call site ──────────────────────────
 * There is no code-to-message table. A diagnostic's text is assembled from
 * the variadic arguments passed to `error`/`warning`/`note`/`hint`. This
 * keeps the message next to the condition that produced it, and it lets the
 * caller include whatever context it has (a name, a type, a count) without
 * a formatting language in between.
 *
 * ─── Design: no AST dependency in this header ─────────────────────────────
 * The AST-aware overloads (`error(code, BaseAST*, args...)`) are templates
 * that forward to a `.cpp` helper. This header does not include BaseAST.hpp.
 * Every translation unit in the frontend includes Diagnostic.hpp, and the
 * AST header tree is large; keeping it out of this header is the difference
 * between a fast incremental build and a slow one.
 *
 * ─── Design: free-text notes and hints ────────────────────────────────────
 * A note or hint has no diagnostic identity of its own — it is context for
 * an error or warning. It is reported with `note`/`hint`, which take no
 * DiagCode, and is stored with code 0. The formatting code emits no code
 * prefix for code 0.
 *
 * ─── Severity, DiagCode, and the code space ───────────────────────────────
 * Those live in DiagCode.hpp, which is independent of this header. A
 * subsystem that only needs the code space — a runtime panic that carries a
 * DiagCode, a serialized error that names its code — includes DiagCode.hpp
 * and nothing else.
 */

#pragma once

#include "core/SourceLocation.hpp"
#include "core/diagnostics/DiagCode.hpp"
#include "core/memory/InternedString.hpp"

#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Forward declaration: the AST-aware overloads need the pointer type, not
// the definition. The actual `node->loc` dereference lives in Diagnostic.cpp.
struct BaseAST;

class StringPool;   // forward declaration; DiagnosticEngine holds a pointer

namespace lucid::diag {

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostic
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One collected diagnostic.
///
/// A diagnostic is a value: severity, code, location, message, and the file
/// it belongs to. It is not a live object — once added to the engine, it is
/// immutable and can be copied, stored, or serialized.
struct Diagnostic {
    Severity        severity;
    DiagCode        code;          // code 0 for free-text notes and hints
    SourceLocation  location;
    std::string     message;
    InternedString  file;          // may be invalid (see note below)

    /// The category of this diagnostic, derived from its code.
    /// Returns `DiagCategory::Internal` for code 0.
    DiagCategory category() const noexcept {
        return categoryFromCode(code);
    }

    /// True if this is a free-text note or hint with no diagnostic identity.
    bool isFreeText() const noexcept {
        return raw(code) == 0;
    }

    /// True if this diagnostic is an error or fatal.
    bool isError() const noexcept {
        return severity == Severity::Error || severity == Severity::Fatal;
    }

    /// True if this diagnostic is a warning.
    bool isWarning() const noexcept {
        return severity == Severity::Warning;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// DiagnosticEngine
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Collects, queries, and formats diagnostics.
///
/// The engine owns the diagnostics it has collected. It is not thread-safe:
/// the compiler pipeline is sequential, and the LSP runs one engine per
/// in-flight analysis. If a future stage wants to report from a worker
/// thread, it queues the message and reports on the main thread.
///
/// ─── String pool ──────────────────────────────────────────────────────────
/// The engine holds an optional pointer to the session's StringPool, used
/// only by `toString(InternedString)`. If the pointer is null, an interned
/// string formats as `<interned:N>` rather than crashing — a diagnostic
/// raised before the pool is set up (a driver-level error) still formats.
///
/// ─── Current file ─────────────────────────────────────────────────────────
/// The engine tracks the "current file" as a convenience for callers that
/// report against a `SourceLocation` and do not want to pass the file
/// explicitly. `parse()` sets it on entry to a file and restores the
/// previous value on exit. A diagnostic added while no file is set has an
/// invalid `file` field; callers should render that as "unknown file".
class DiagnosticEngine {
public:
    DiagnosticEngine() = default;

    explicit DiagnosticEngine(StringPool* pool)
        : m_pool(pool) {}

    // ─── String pool ────────────────────────────────────────────────────

    StringPool* stringPool() const noexcept { return m_pool; }
    void setStringPool(StringPool* pool) noexcept { m_pool = pool; }

    // ─── Current file ───────────────────────────────────────────────────

    InternedString currentFile() const noexcept { return m_currentFile; }
    void setCurrentFile(InternedString f) noexcept { m_currentFile = f; }

    // ─── Report: AST-anchored ───────────────────────────────────────────
    //
    // These overloads take a `BaseAST*` and use its location. The location
    // lookup is a non-template helper defined in Diagnostic.cpp, so this
    // header does not include BaseAST.hpp.

    template <typename... Args>
    void error(DiagCode code, BaseAST* node, Args&&... args) {
        add(severityFromCode(code), code,
            node ? locationOf(node) : SourceLocation{},
            buildMessage(std::forward<Args>(args)...));
    }

    template <typename... Args>
    void warning(DiagCode code, BaseAST* node, Args&&... args) {
        add(Severity::Warning, code,
            node ? locationOf(node) : SourceLocation{},
            buildMessage(std::forward<Args>(args)...));
    }

    template <typename... Args>
    void note(BaseAST* node, Args&&... args) {
        add(Severity::Note, DiagCode(0),
            node ? locationOf(node) : SourceLocation{},
            buildMessage(std::forward<Args>(args)...));
    }

    template <typename... Args>
    void hint(BaseAST* node, Args&&... args) {
        add(Severity::Hint, DiagCode(0),
            node ? locationOf(node) : SourceLocation{},
            buildMessage(std::forward<Args>(args)...));
    }

    // ─── Report: location-anchored ──────────────────────────────────────
    //
    // These take an explicit SourceLocation, for callers that do not have
    // an AST node (the lexer, driver-level errors, the LSP).

    template <typename... Args>
    void errorAt(DiagCode code, const SourceLocation& loc, Args&&... args) {
        add(severityFromCode(code), code, loc,
            buildMessage(std::forward<Args>(args)...));
    }

    template <typename... Args>
    void warningAt(DiagCode code, const SourceLocation& loc, Args&&... args) {
        add(Severity::Warning, code, loc,
            buildMessage(std::forward<Args>(args)...));
    }

    template <typename... Args>
    void noteAt(const SourceLocation& loc, Args&&... args) {
        add(Severity::Note, DiagCode(0), loc,
            buildMessage(std::forward<Args>(args)...));
    }

    template <typename... Args>
    void hintAt(const SourceLocation& loc, Args&&... args) {
        add(Severity::Hint, DiagCode(0), loc,
            buildMessage(std::forward<Args>(args)...));
    }

    // ─── Report: fatal ──────────────────────────────────────────────────
    //
    // A fatal diagnostic is an error the compiler cannot recover from at
    // the current stage. The engine records it like any error; the caller
    // is responsible for stopping the pipeline. The severity is stored
    // explicitly because the code cannot distinguish error from fatal.

    template <typename... Args>
    void fatalAt(DiagCode code, const SourceLocation& loc, Args&&... args) {
        add(Severity::Fatal, code, loc,
            buildMessage(std::forward<Args>(args)...));
    }

    // ─── Query ──────────────────────────────────────────────────────────

    /// True if any diagnostic is an error or fatal.
    bool hasErrors() const noexcept;

    /// True if any diagnostic is a warning.
    bool hasWarnings() const noexcept;

    /// Number of error and fatal diagnostics.
    int errorCount() const noexcept;

    /// Number of warnings.
    int warningCount() const noexcept;

    /// Number of all diagnostics.
    int totalCount() const noexcept {
        return static_cast<int>(m_diagnostics.size());
    }

    /// True if the error count is below the pipeline's abort threshold.
    bool canContinue(int maxErrors = 100) const noexcept {
        return errorCount() < maxErrors;
    }

    /// All collected diagnostics, in the order they were reported.
    const std::vector<Diagnostic>& all() const noexcept {
        return m_diagnostics;
    }

    /// True if no diagnostics have been reported.
    bool empty() const noexcept { return m_diagnostics.empty(); }

    /// Remove all collected diagnostics. The current file and string pool
    /// are left unchanged.
    void clear() noexcept { m_diagnostics.clear(); }

    // ─── Formatting ─────────────────────────────────────────────────────

    /// Write every diagnostic to `os`, one per line, followed by a summary
    /// line if any were reported. Not colored.
    void dump(std::ostream& os) const;

    /// Write every diagnostic to `os` with ANSI color. The summary line is
    /// colored red if there are errors, yellow otherwise.
    void dumpWithColor(std::ostream& os) const;

    /// Format one diagnostic as a single line without color.
    ///
    ///   [ERROR]   E4001: type mismatch at 12:5
    ///   [WARNING] W8002: unused variable 'x' at 3:9
    ///   [NOTE]    consider using '_' at 3:9
    std::string formatOneLine(const Diagnostic& d) const;

    /// Format one diagnostic as a single line with ANSI color.
    std::string formatOneLineWithColor(const Diagnostic& d) const;

private:
    std::vector<Diagnostic> m_diagnostics;
    InternedString          m_currentFile;
    StringPool*             m_pool = nullptr;

    void add(Severity sev, DiagCode code,
             const SourceLocation& loc, std::string msg) {
        m_diagnostics.push_back(
            Diagnostic{sev, code, loc, std::move(msg), m_currentFile});
    }

    // ─── Message building ───────────────────────────────────────────────

    // Every argument is stringified through `toString`, concatenated, and
    // returned. Overloads below cover the types the compiler actually
    // reports: interned strings, C strings, std::string, string_view,
    // booleans, the integer widths in use, floating point, and pointers.
    //
    // A `toString` template that uses `operator<<` handles anything else
    // (a custom type a caller wants to embed by reference).

    std::string toString(InternedString s) const;
    std::string toString(const char* s) const { return s ? std::string(s) : "null"; }
    std::string toString(std::string_view s) const { return std::string(s); }
    std::string toString(const std::string& s) const { return s; }
    std::string toString(bool b) const { return b ? "true" : "false"; }
    std::string toString(char c) const { return std::string(1, c); }

    std::string toString(int32_t v) const { return std::to_string(v); }
    std::string toString(uint32_t v) const { return std::to_string(v); }
    std::string toString(int64_t v) const { return std::to_string(v); }
    std::string toString(uint64_t v) const { return std::to_string(v); }
    std::string toString(double v) const { return std::to_string(v); }

    template <typename T>
    std::string toString(const T* ptr) const {
        if (!ptr) return "null";
        std::ostringstream oss;
        oss << ptr;
        return oss.str();
    }

    template <typename T>
    std::string toString(const T& value) const {
        std::ostringstream oss;
        oss << value;
        return oss.str();
    }

    template <typename First, typename... Rest>
    std::string buildMessage(First&& first, Rest&&... rest) const {
        return toString(std::forward<First>(first))
             + buildMessage(std::forward<Rest>(rest)...);
    }

    std::string buildMessage() const { return {}; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Location helper (declared here, defined in Diagnostic.cpp)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The source location of an AST node.
///
/// Defined in Diagnostic.cpp so this header does not include BaseAST.hpp.
/// `node` must be non-null; callers use `node ? locationOf(node) :
/// SourceLocation{}`.
SourceLocation locationOf(const BaseAST* node) noexcept;

} // namespace lucid::diag