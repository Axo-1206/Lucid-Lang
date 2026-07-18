/**
 * @file ParserContext.hpp
 * @brief Shared parsing context across all files.
 * 
 * ParserContext holds state that is shared across all files being parsed:
 * - StringPool and ASTArena (shared memory)
 * - ModuleResolver (module coordination)
 * - Error reporting (variadic template functions that forward into the
 *   shared `diagnostic::` namespace — see Diagnostic.hpp)
 * - Context tracking (spawnDepth, inAsyncContext)
 * 
 * This is passed by reference to all parsing functions.
 *
 * @architectural_note Error state moved to `diagnostic::`
 *   ParserContext used to maintain its own `errors`/`hasErrors`/
 *   `consecutiveErrors`/`allDiagnostics` — the same bookkeeping
 *   `SemaContext` independently maintained too, and the lexer would have
 *   needed a third copy of. That's all gone now: `error()`/`errorAt()`/
 *   `warning()`/`warningAt()`/`note()` below are thin wrappers over
 *   `diagnostic::error()` etc., `canContinue()` forwards to
 *   `diagnostic::canContinue()`, and per-file isolation comes from
 *   `ScopedFileContext` holding a `diagnostic::ScopedSource` member instead
 *   of hand-saving/restoring three fields. ParserContext's OWN state is now
 *   limited to what's genuinely parser-specific: the syntactic context
 *   stack (`contextStack`), the current file path, and doc-comment
 *   harvesting — see Diagnostic.hpp's "source-scope stack" architecture
 *   note for the full rationale behind the split.
 */

#pragma once

#include "core/Tokens.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/memory/ASTArena.hpp"
#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"
#include "TokenStream.hpp"
#include "../ModuleResolver.hpp"

#include <vector>
#include <string>
#include <optional>
#include <sstream>
#include <type_traits>

namespace parser {

/**
 * @brief The kind of syntactic construct currently being parsed.
 *
 * Pushed/popped as the parser enters and leaves nested constructs
 * (attribute lists, generic parameter/argument lists, function bodies,
 * struct/enum/trait bodies, etc). This is grammar-level state — it has
 * no place in TokenStream, which only knows about tokens, not what
 * construct they belong to.
 *
 * Used by error recovery (`synchronizeUntil` and friends) to pick a
 * sensible follow-set without each call site having to hardcode one,
 * and to avoid skipping past a delimiter that belongs to an enclosing
 * construct rather than the one currently failing to parse.
 */
enum class SyntacticContext {
    TopLevel,       // File-level declarations
    Attribute,      // @[ ... ]
    GenericParams,  // < ... >  (declaration site: struct<T>, func<T>)
    GenericArgs,    // < ... >  (use site: map<int, string>)
    FuncParams,     // ( ... )  parameter list
    FuncBody,       // { ... }  function body, including nested/anonymous functions
    StructBody,     // struct { ... }
    EnumBody,       // enum { ... }
    TraitBody,      // trait { ... }
};

/// Human-readable name for a SyntacticContext, for diagnostics/logging.
inline const char* syntacticContextName(SyntacticContext kind) {
    switch (kind) {
        case SyntacticContext::TopLevel:      return "top level";
        case SyntacticContext::Attribute:     return "attribute list";
        case SyntacticContext::GenericParams: return "generic parameter list";
        case SyntacticContext::GenericArgs:   return "generic argument list";
        case SyntacticContext::FuncParams:    return "function parameter list";
        case SyntacticContext::FuncBody:      return "function body";
        case SyntacticContext::StructBody:    return "struct body";
        case SyntacticContext::EnumBody:      return "enum body";
        case SyntacticContext::TraitBody:     return "trait body";
    }
    return "unknown context";
}

/**
 * @brief One frame of the syntactic context stack.
 *
 * Records not just what kind of construct is open, but where it was
 * opened — so a diagnostic like "unclosed attribute list" can point
 * back at the '@[' rather than only at wherever the parser gave up.
 */
struct ContextFrame {
    SyntacticContext kind;
    SourceLocation openedAt;
};

/**
 * @brief Shared parsing context across all files.
 * 
 * ParserContext holds all state that is shared across files:
 * - Memory allocators (StringPool, ASTArena)
 * - Module resolver (imports, caching, circular detection)
 * - Error reporting with variadic templates
 * - Context tracking (spawn/async depth)
 * 
 * ## Error Reporting API
 * 
 * ```cpp
 * // Error at current token location with no extra args
 * ctx.error(stream, DiagCode::E1001);
 * 
 * // Error at current token location with format args
 * ctx.error(stream, DiagCode::E1002, "expected", "found");
 * 
 * // Error at specific location
 * ctx.errorAt(loc, DiagCode::E1002, "expected", "found");
 * 
 * // Warning with no extra args
 * ctx.warning(stream, DiagCode::W0001);
 * 
 * // Warning with format args
 * ctx.warning(stream, DiagCode::W0002, "variableName");
 * 
 * // Note (no diagnostic code)
 * ctx.note(stream, "Consider using '", alternative, "' instead");
 * ```
 * 
 * ## Usage
 * 
 * ```cpp
 * ParserContext ctx(pool, arena, resolver);
 * TokenStream stream(tokens, filePath);
 * 
 * // Parse a file - ctx is shared across all recursive parses
 * auto* ast = parse("main.lucid", source, ctx);
 * ```
 */
struct ParserContext {
    // ─────────────────────────────────────────────────────────────────────────
    // Shared Resources
    // ─────────────────────────────────────────────────────────────────────────

    /// String interner (shared across all files)
    StringPool& pool;
    
    /// AST allocator (shared across all files)
    ASTArena& arena;
    
    /// Module resolver (for imports, caching, circular detection)
    ModuleResolver* resolver = nullptr;

    /// File path of the current file being parsed. Set once by parse()
    /// before constructing this file's ScopedFileContext (which uses it to
    /// seed a diagnostic::ScopedSource — see ScopedFileContext's own doc
    /// comment). Also used to stamp ModuleAST::filePath.
    InternedString currentFilePath;

    // Error tracking (errors / hasErrors / consecutiveErrors /
    // allDiagnostics) used to live here. It's gone — see this file's
    // "Error state moved to `diagnostic::`" architecture note above for
    // where it went and why. `error()`/`errorAt()`/`warning()`/
    // `warningAt()`/`note()`/`canContinue()` below all forward into
    // `diagnostic::` directly.

    // ─────────────────────────────────────────────────────────────────────────
    // Syntactic Context Stack (attribute / generic / function / declaration nesting)
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Stack of currently-open syntactic constructs, innermost last.
     *
     * ## What this represents
     *
     * As recursive-descent parsing descends into nested constructs —
     * an attribute list, a generic argument list, a function body, a
     * struct/enum/trait body — each of those parse functions pushes a
     * ContextFrame recording *what* it's parsing and *where* it started
     * (see SyntacticContext / ContextFrame above). The stack is therefore
     * a live trace of the parser's current nesting, e.g. while parsing
     * `@[foo(1, <T>)]`, the stack would read (outermost to innermost):
     * `[Attribute]` while inside `foo(...)`'s own args, then briefly
     * `[Attribute, GenericArgs]` while inside `<T>`.
     *
     * It is a stack (not a flat flag or a single "current context" field)
     * specifically because these constructs nest: knowing only "we are
     * currently inside a GenericArgs" isn't enough on its own — recovery
     * may also need to know what encloses it (an Attribute, a FuncParams
     * list, TopLevel) if the innermost recovery attempt fails and control
     * needs to fall back to the next level out.
     *
     * ## Why this exists: bounded, context-aware error recovery
     *
     * This is what fixed the original bug this stack was introduced for:
     * a blind, untargeted `synchronize()` skipping past a malformed
     * attribute list's closing `]` because it had no idea it was inside
     * one. `synchronizeToContext()` consults `currentContext()` to pick
     * a follow-set bounded to whatever's actually open — `,`/`]` inside
     * an Attribute, `,`/`)` inside FuncParams, and so on — instead of one
     * fixed token set applied everywhere regardless of nesting.
     *
     * ## How it's kept correct: RAII, not manual push/pop
     *
     * Pushed and popped by ScopedContext, constructed once at the top of
     * whichever parse function *owns* a construct (e.g. inside
     * parseAttributes itself, not at each of its call sites). Its
     * destructor pops on every exit path of that function — normal
     * return, any of its early returns, or an exception unwinding through
     * it — so the stack can never desync from the actual call stack by a
     * forgotten pop. pushContext()/popContext() below exist for
     * ScopedContext to call; prefer ScopedContext over calling them by
     * hand for the same reason — see ScopedContext's own doc comment.
     *
     * ## Per-file, not whole-session
     *
     * ParserContext is shared across every file parse() recursively
     * visits, so this stack is saved and reset to empty at the start of
     * each file and restored on return by ScopedFileContext — a nested
     * `use` starts fresh at TopLevel rather than inheriting whatever the
     * importing file happened to have open (today this restore is
     * defensive: Lucid only allows `use` at file top level, so the stack
     * is always already empty at that point — but relying on that
     * invariant forever, instead of the entry point actually preserving
     * state, is exactly the kind of assumption that quietly breaks if the
     * grammar changes later).
     */
    std::vector<ContextFrame> contextStack;

    /**
     * @brief Push a new syntactic context frame.
     *
     * Prefer constructing a ScopedContext instead of calling this
     * directly — see contextStack's doc comment for why: a bare call
     * here needs a manually-paired popContext() on every exit path of
     * the caller, which is exactly the kind of easy-to-forget bookkeeping
     * RAII exists to make unnecessary.
     */
    void pushContext(SyntacticContext kind, const SourceLocation& loc) {
        contextStack.push_back({kind, loc});
    }

    /**
     * @brief Pop the innermost syntactic context frame.
     *
     * Prefer letting a ScopedContext's destructor call this over calling
     * it directly — same reasoning as pushContext().
     */
    void popContext() {
        if (!contextStack.empty()) {
            contextStack.pop_back();
        }
    }

    /**
     * @brief The innermost currently-open syntactic context.
     *
     * Returns SyntacticContext::TopLevel when the stack is empty — an
     * empty stack *is* TopLevel by convention, the same way spawnDepth
     * == 0 means "not in a spawn." No explicit TopLevel frame is ever
     * pushed; there's nothing to push for "nothing is open."
     *
     * This is what synchronizeToContext() reads to pick a follow-set —
     * see its own doc comment for how each SyntacticContext maps to a
     * bounded set of recovery tokens.
     */
    SyntacticContext currentContext() const {
        return contextStack.empty() ? SyntacticContext::TopLevel : contextStack.back().kind;
    }

    /**
     * @brief True if `kind` is open anywhere on the stack, not just innermost.
     *
     * Useful for checks that care about "am I nested inside a FuncBody at
     * all" (e.g. for detecting a nested/anonymous function) rather than
     * "is a FuncBody the *specific* thing directly enclosing me right
     * now" — the latter is what currentContext() answers instead.
     */
    bool isInsideContext(SyntacticContext kind) const {
        for (const auto& frame : contextStack) {
            if (frame.kind == kind) return true;
        }
        return false;
    }

    /// Current nesting depth, i.e. how many constructs are currently open.
    size_t contextDepth() const { return contextStack.size(); }

    /**
     * @brief Visual trace of contextStack over an actual nested parse.
     *
     * Two things worth seeing concretely, since they're easy to get
     * wrong by imagining the stack rather than tracing it:
     *
     * 1. The vector itself never contains TopLevel — an empty vector IS
     *    TopLevel, so there's nothing to push for it.
     * 2. Constructs that appear one-after-another in source (e.g. an
     *    attribute, then a generic param list, then a body) are usually
     *    SIBLINGS — each pops before the next pushes — not simultaneous
     *    nesting. Real simultaneous nesting requires one construct to
     *    start while an earlier one is still open, e.g. a nested/
     *    anonymous function, or an attribute on a local declaration.
     *
     * ```lucid
     * func outer() {
     *     @[inline]
     *     let helper = func(x int) int {
     *         return x * 2;
     *     };
     * }
     * ```
     *
     * ```
     * parsing outer()'s params '()'      push FuncParams   [FuncParams]
     * done with params                   pop                []
     * parsing outer()'s body '{'         push FuncBody      [FuncBody]
     * parsing '@[inline]'                push Attribute      [FuncBody, Attribute]
     * done with the attribute            pop                [FuncBody]
     * parsing helper's params '(x int)'  push FuncParams     [FuncBody, FuncParams]
     * done with those params             pop                [FuncBody]
     * parsing helper's body '{'          push FuncBody       [FuncBody, FuncBody]  ← genuine nesting
     * done with helper's body            pop                [FuncBody]
     * done with outer()'s body           pop                []   ← back to TopLevel
     * ```
     *
     * The `[FuncBody, FuncBody]` line is the one genuinely-nested moment
     * in this example — it's also exactly the case isInsideContext()
     * exists for: currentContext() only reports the innermost FuncBody,
     * but isInsideContext(FuncBody) is what would tell you "yes, we're
     * inside a function body at all," regardless of how many are
     * currently stacked.
     */

    // ─────────────────────────────────────────────────────────────────────────
    // Doc Comment Harvesting
    // ─────────────────────────────────────────────────────────────────────────
    
    /// Last harvested doc comment (stored between harvest and attachment)
    std::optional<DocComment> pendingDoc;
    
    // ─────────────────────────────────────────────────────────────────────────
    // Constructor
    // ─────────────────────────────────────────────────────────────────────────
    
    ParserContext(StringPool& p, ASTArena& a, ModuleResolver* r = nullptr)
        : pool(p)
        , arena(a)
        , resolver(r)
    {}
    
    // ─────────────────────────────────────────────────────────────────────────
    // String Conversion Helper
    // ─────────────────────────────────────────────────────────────────────────
    
    std::string toString(InternedString s) const {
        return std::string(pool.lookup(s));
    }
    
    // ─────────────────────────────────────────────────────────────────────────
    // Error Reporting - Variadic Template Functions
    // ─────────────────────────────────────────────────────────────────────────
    
private:
    /**
     * @brief Trait to detect if a type is InternedString.
     */
    template<typename T>
    struct is_interned_string : std::false_type {};
    
    template<>
    struct is_interned_string<InternedString> : std::true_type {};
    
    template<>
    struct is_interned_string<InternedString&> : std::true_type {};
    
    template<>
    struct is_interned_string<const InternedString&> : std::true_type {};
    
    /**
     * @brief Stream any value that is NOT InternedString.
     */
    template<typename T>
    typename std::enable_if<!is_interned_string<typename std::decay<T>::type>::value>::type
    streamTo(std::ostringstream& oss, T&& value) const {
        oss << std::forward<T>(value);
    }
    
    /**
     * @brief Stream InternedString using the StringPool.
     */
    void streamTo(std::ostringstream& oss, InternedString s) const {
        oss << pool.lookup(s);
    }
    
    template<typename T>
    void buildMessageImpl(std::ostringstream& oss, T&& value) const {
        streamTo(oss, std::forward<T>(value));
    }
    
    template<typename T, typename... Rest>
    void buildMessageImpl(std::ostringstream& oss, T&& first, Rest&&... rest) const {
        streamTo(oss, std::forward<T>(first));
        buildMessageImpl(oss, std::forward<Rest>(rest)...);
    }
    
    template<typename... Args>
    std::string buildMessage(Args&&... args) const {
        if constexpr (sizeof...(Args) == 0) {
            return "";
        } else {
            std::ostringstream oss;
            buildMessageImpl(oss, std::forward<Args>(args)...);
            return oss.str();
        }
    }
    
public:
    // ─────────────────────────────────────────────────────────────────────────
    // Public Error Reporting API
    // ─────────────────────────────────────────────────────────────────────────
    //
    // Every function below builds ONE composed message string from its
    // heterogeneous variadic args (via buildMessage() above — this is
    // unchanged from before), then forwards it as a single diagnostic to
    // `diagnostic::`. `file` is never passed explicitly: it comes from
    // whichever `diagnostic::ScopedSource` is currently open (pushed by
    // this file's `ScopedFileContext` — see its own doc comment), the same
    // way `currentFilePath` used to be read directly inside the old
    // `addDiagnostic()`. Category is always `DiagnosticCategory::Syntax`
    // here — this is the parser, it only ever reports syntax diagnostics.

    /**
     * @brief Report an error at the current token location with optional format args.
     * 
     * @tparam Args The types of the format arguments (can be empty)
     * @param stream The token stream to get the current location from
     * @param code The diagnostic code
     * @param args Optional format arguments for the error message
     * 
     * ## Usage Examples
     * 
     * ```cpp
     * // Error with no extra args
     * ctx.error(stream, DiagCode::E1001);
     * 
     * // Error with format args
     * ctx.error(stream, DiagCode::E1002, "expected", "found");
     * 
     * // Error with InternedString
     * ctx.error(stream, DiagCode::E2001, ctx.toString(varName));
     * ```
     */
    template<typename... Args>
    void error(TokenStream& stream, DiagCode code, Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        diagnostic::error(DiagnosticCategory::Syntax, stream.currentLoc(), code, {message});
    }
      
    /**
     * @brief Report an error at a specific location with optional format args.
     * 
     * @tparam Args The types of the format arguments (can be empty)
     * @param loc The source location
     * @param code The diagnostic code
     * @param args Optional format arguments for the error message
     */
    template<typename... Args>
    void errorAt(const SourceLocation& loc, DiagCode code, Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        diagnostic::error(DiagnosticCategory::Syntax, loc, code, {message});
    }
    
    /**
     * @brief Report a warning at the current token location with optional format args.
     * 
     * @tparam Args The types of the format arguments (can be empty)
     * @param stream The token stream to get the current location from
     * @param code The diagnostic code
     * @param args Optional format arguments for the warning message
     */
    template<typename... Args>
    void warning(TokenStream& stream, DiagCode code, Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        diagnostic::warning(DiagnosticCategory::Syntax, stream.currentLoc(), code, {message});
    }
    
    // ─── Warning at Specific Location ────────────────────────────────────
    
    /**
     * @brief Report a warning at a specific location with optional format args.
     * 
     * @tparam Args The types of the format arguments (can be empty)
     * @param loc The source location
     * @param code The diagnostic code
     * @param args Optional format arguments for the warning message
     */
    template<typename... Args>
    void warningAt(const SourceLocation& loc, DiagCode code, Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        diagnostic::warning(DiagnosticCategory::Syntax, loc, code, {message});
    }

    /**
     * @brief Report a free-text note at the current token location.
     *
     * @tparam Args The types of the format arguments (can be empty)
     * @param stream The token stream to get the current location from
     * @param args Pieces to stream together into the note's message —
     *        e.g. `ctx.note(stream, "Consider using '", alternative, "' instead")`
     */
    template<typename... Args>
    void note(TokenStream& stream, Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        diagnostic::note(stream.currentLoc(), message);
    }

    /// Report a free-text note at a specific location.
    template<typename... Args>
    void noteAt(const SourceLocation& loc, Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        diagnostic::note(loc, message);
    }
    
    // ─────────────────────────────────────────────────────────────────────────
    // Context Queries
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Check if we can safely continue parsing.
     *
     * Forwards to `diagnostic::canContinue()`, which reads the consecutive-
     * error count of whichever `diagnostic::ScopedSource` is currently open
     * for this file (pushed by this file's `ScopedFileContext`). See
     * Diagnostic.hpp's docs on `consecutiveErrorsInCurrentSource()` /
     * `resetStreak()` for exactly what "consecutive" means here and how to
     * get true back-to-back-only semantics if a caller wants them.
     */
    bool canContinue() const {
        return diagnostic::canContinue();
    }
};

/**
 * @brief RAII guard for syntactic context tracking.
 *
 * Pushes a SyntacticContext frame on construction and pops it on
 * destruction — automatically, on every exit path of the enclosing
 * function (normal return, early return, or an exception unwinding
 * through it). This is what makes the push/pop balanced without every
 * early-return site in a parse function having to remember to pop.
 *
 * Construct exactly one of these at the top of the parse function that
 * *owns* the construct (e.g. inside parseAttributes, not at each of its
 * call sites) — see Grammar.md discussion on error recovery for why
 * the context belongs to the callee, not the caller.
 *
 * ## Usage
 *
 * ```cpp
 * ArenaSpan<AttributePtr> parseAttributes(TokenStream& stream, ParserContext& ctx) {
 *     if (!stream.check(TokenType::AT_SIGN)) {
 *         return ctx.arena.makeBuilder<AttributePtr>().build();
 *     }
 *     stream.advance(); // consume '@'
 *     stream.advance(); // consume '['
 *     ScopedContext guard(ctx, SyntacticContext::Attribute, stream.currentLoc());
 *     // every return below — however many there are — pops correctly
 *     ...
 * }
 * ```
 *
 * Non-copyable, non-movable: a guard's identity is tied to the specific
 * stack frame that created it, so copying or moving it would make the
 * push/pop pairing ambiguous.
 */
struct ScopedContext {
    ScopedContext(ParserContext& ctx, SyntacticContext kind, const SourceLocation& loc)
        : ctx_(ctx)
    {
        ctx_.pushContext(kind, loc);
    }

    ~ScopedContext() {
        ctx_.popContext();
    }

    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;
    ScopedContext(ScopedContext&&) = delete;
    ScopedContext& operator=(ScopedContext&&) = delete;

private:
    ParserContext& ctx_;
};

/**
 * @brief RAII guard for entering a fresh file's parsing state.
 *
 * ParserContext is shared across the whole parse — including every
 * recursively-parsed `use`d file — so parse() needs each file to start
 * with a clean slate (empty contextStack, a fresh diagnostic scope) without
 * permanently discarding whatever the importing file had accumulated.
 * This guard saves the importer's contextStack on construction, resets it
 * so the new file starts clean, and restores the saved value on
 * destruction — on every exit path of parse(), including its early returns
 * (cyclic import, lexer failure, parseInternal failure).
 *
 * The diagnostic half of this used to be hand-rolled here too (save
 * `errors`/`hasErrors`/`consecutiveErrors`, reset them, drain into
 * `allDiagnostics` before restoring). That's gone: this guard now simply
 * owns a `diagnostic::ScopedSource` member for `ctx.currentFilePath`,
 * declared FIRST so it's constructed before `contextStack` is touched and
 * destroyed LAST (member destruction order is the reverse of declaration
 * order) — meaning the diagnostic scope for this file wraps the guard's
 * entire lifetime, start to finish. Nothing needs to be drained into
 * anywhere anymore: `diagnostic::`'s global list already IS the durable,
 * whole-compile record (see Diagnostic.hpp's "source-scope stack"
 * architecture note) — popping a `ScopedSource` doesn't delete anything,
 * it just closes the index range that counted as "this file."
 *
 * This guard owns the per-file reset entirely — callers of parse() no
 * longer need (and shouldn't make) any separate reset call; constructing
 * this guard is the only step required.
 *
 * Today the contextStack restore is defensive rather than load-bearing —
 * Lucid's grammar only allows `use` at file top level, so parse() is only
 * ever re-entered while the importing file's stack is already empty — but
 * relying on that invariant forever, rather than having the recursive
 * entry point actually preserve state, is exactly the kind of assumption
 * that silently breaks if the grammar changes. The diagnostic-scope half,
 * by contrast, is load-bearing today: without it, an error reported before
 * a `use` would get attributed to whichever file's frame happened to be on
 * top when it was reported, rather than staying correctly scoped to the
 * importer.
 *
 * ## Usage
 *
 * ```cpp
 * ModuleAST* parse(const std::string& path, const std::string& source, ParserContext& ctx) {
 *     ctx.currentFilePath = ctx.pool.intern(path);
 *     ScopedFileContext fileContext(ctx);  // resets contextStack + opens this
 *                                           // file's diagnostic::ScopedSource;
 *                                           // both restore/close on return
 *     ...
 *     mod->hasErrors = diagnostic::hasErrorsInCurrentSource();
 *     return mod;
 * }
 * ```
 *
 * Non-copyable, non-movable, for the same reason as ScopedContext: its
 * identity is tied to one specific parse() activation.
 */
struct ScopedFileContext {
    explicit ScopedFileContext(ParserContext& ctx)
        : ctx_(ctx)
        , sourceScope_(ctx.currentFilePath)
        , savedContextStack_(std::move(ctx.contextStack))
    {
        ctx_.contextStack.clear();
    }

    ~ScopedFileContext() {
        ctx_.contextStack = std::move(savedContextStack_);
        // sourceScope_ is destroyed after this (declared first ⇒ destroyed
        // last), closing this file's diagnostic scope only once everything
        // else about leaving this file is already done.
    }

    ScopedFileContext(const ScopedFileContext&) = delete;
    ScopedFileContext& operator=(const ScopedFileContext&) = delete;
    ScopedFileContext(ScopedFileContext&&) = delete;
    ScopedFileContext& operator=(ScopedFileContext&&) = delete;

private:
    ParserContext& ctx_;
    diagnostic::ScopedSource sourceScope_;
    std::vector<ContextFrame> savedContextStack_;
};

/**
 * @brief ScopedContext vs. ScopedFileContext — how they differ.
 *
 * Both are RAII save/reset/restore guards over ParserContext state, both
 * non-copyable/non-movable for the same reason (identity tied to one
 * specific activation), and both exist to eliminate the same class of
 * bug: state that must be manually balanced across multiple exit paths,
 * where one forgotten `pop`/restore silently corrupts everything parsed
 * afterward. Beyond that, they operate at different granularities and
 * solve different problems:
 *
 * **ScopedContext:**
 * 1. Guards `contextStack` only.
 * 2. Operation: *pushes* one frame, pops it on exit.
 * 3. Constructed by whichever parse function owns a construct
 *    (`parseAttributes`, a struct/enum/trait/func body parser).
 * 4. Frequency per file: many — one per attribute list, generic arg
 *    list, function body, nested body, etc.
 * 5. On exit: the *previous* frame becomes innermost again (stack
 *    shrinks by one).
 * 6. Answers: "What syntactic construct is the parser inside *right
 *    now*, within this one file?"
 * 7. Consumed by `synchronizeToContext()`, via `ctx.currentContext()`.
 *
 * **ScopedFileContext:**
 * 1. Guards `contextStack` directly, and (via an embedded
 *    `diagnostic::ScopedSource` member) this file's diagnostic scope —
 *    see Diagnostic.hpp's "source-scope stack" architecture note for what
 *    that actually tracks (an index range into the shared diagnostic list,
 *    plus this file's own error count/streak).
 * 2. Operation: *saves* `contextStack`, resets it to empty; *pushes* a new
 *    diagnostic scope (rather than saving/resetting a field, since there's
 *    no local field to save anymore — the diagnostic system owns that
 *    state itself now). Both undo symmetrically on exit: `contextStack` is
 *    restored, the diagnostic scope is popped.
 * 3. Constructed by `parse()` only — exactly once per file, including
 *    recursive calls for `use`.
 * 4. Frequency per file: exactly one — the whole file is one
 *    activation.
 * 5. On exit: the *importing* file's `contextStack` comes back exactly as
 *    it was; the importing file's diagnostic scope (if any) reappears
 *    underneath as the new innermost one, automatically, just by virtue of
 *    being a stack — see `diagnostic::pushSource()`'s own doc comment.
 * 6. Answers: "Whose file's state is currently live in `ctx` (and in
 *    `diagnostic::`), given both are shared across every file in the
 *    recursive parse?"
 * 7. Nothing reads its effect directly — it's what keeps
 *    ScopedContext's own bookkeeping, and diagnostics, from leaking
 *    between files.
 *
 * The relationship between them: ScopedContext frames are always
 * *relative to whichever file is currently being parsed*. ScopedFileContext
 * is what makes "currently being parsed" a well-defined, isolated notion
 * in the first place — without it, a `ScopedContext` pushed while parsing
 * an imported file could end up sitting on the same stack as frames from
 * the file that imported it, and a syntax error discovered before a
 * `use` statement could get attributed to the wrong file's diagnostic
 * scope the moment that `use` triggers a nested `parse()` call (see
 * ScopedFileContext's own doc comment for why the diagnostic-scope half is
 * load-bearing, not just defensive, unlike the contextStack half of the
 * same guard).
 *
 * A useful mental model: ScopedContext nests *within* a single
 * ScopedFileContext activation, potentially many levels deep and many
 * times over the course of one file. ScopedFileContext itself nests
 * across *recursive parse() calls*, at most as deep as the `use` import
 * chain — one activation per file, full stop.
 */

} // namespace parser