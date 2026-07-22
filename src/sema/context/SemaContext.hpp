/**
 * @file SemaContext.hpp
 * @brief Unified semantic context - composes all sub-contexts.
 *
 * This is the main context passed to all semantic analysis functions.
 * It composes the smaller, focused contexts into a single interface.
 *
 * @architectural_note Composition over inheritance
 *   Each sub-context has a single responsibility. SemaContext composes
 *   them and provides a unified interface for the semantic phase.
 *
 * @architectural_note No toString() needed
 *   StringPool::lookup() now returns std::string directly, so callers
 *   can use pool.lookup(name) without an intermediate conversion step.
 *
 * @architectural_note Diagnostic integration
 *   Uses the consolidated diagnostic:: API directly. No DiagnosticCategory
 *   is needed - the category is derived from the error code range.
 */
#pragma once

#include "SemanticResources.hpp"
#include "SymbolStorage.hpp"
#include "SemanticContextStack.hpp"
#include "DefiningTypeStack.hpp"

#include "core/diagnostics/Diagnostic.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"

#include <unordered_map>
#include <vector>
#include <sstream>
#include <type_traits>

namespace sema {

/**
 * @brief Unified semantic context for all analysis passes.
 *
 * Composes:
 *   - SemanticResources (shared, immutable)
 *   - SymbolStorage (two-tier symbol tables)
 *   - SemanticContextStack (semantic nesting)
 *   - DefiningTypeStack (self-reference support)
 *
 * Also provides:
 *   - Module management (modules list, path lookup)
 *   - Diagnostic forwarding
 */
struct SemaContext {
    // ─── Sub-Contexts ──────────────────────────────────────────────────

    /// Shared, immutable resources
    SemanticResources resources;

    /// Two-tier symbol storage
    SymbolStorage symbols;

    /// Semantic context stack
    SemanticContextStack contexts;

    /// Self-reference support
    DefiningTypeStack definingTypes;

    // ─── Modules ────────────────────────────────────────────────────────

    /// Every module being analyzed, in the order provided
    std::vector<ModuleAST*> modules;

    /// Fast path: module path → module AST
    std::unordered_map<InternedString, ModuleAST*> modulesByPath;

    // ─── Constructor ────────────────────────────────────────────────────

    /**
     * @brief Construct the semantic context for a whole compilation.
     *
     * @param p   Shared string interner (same one used by the parser).
     * @param a   Shared AST allocator (same one used by the parser).
     * @param mods Every module produced by the parse phase, in dependency order.
     */
    SemaContext(StringPool& p, ASTArena& a, std::vector<ModuleAST*> mods)
        : resources(p, a)
        , modules(std::move(mods))
    {
        for (ModuleAST* m : modules) {
            if (m) modulesByPath[m->filePath] = m;
        }
    }

    // Non-copyable (contains references)
    SemaContext(const SemaContext&) = delete;
    SemaContext& operator=(const SemaContext&) = delete;

    // Move is not allowed (contains references)
    SemaContext(SemaContext&&) = delete;
    SemaContext& operator=(SemaContext&&) = delete;

    // ─── Convenience Accessors ──────────────────────────────────────────

    StringPool& pool() { return resources.pool; }
    const StringPool& pool() const { return resources.pool; }

    ASTArena& arena() { return resources.arena; }
    const ASTArena& arena() const { return resources.arena; }

    // ─── Module Lookup ──────────────────────────────────────────────────

    /**
     * @brief Resolve a module by its interned file/package path.
     *
     * Used when processing an `import` statement: the path string must be
     * turned into a ModuleAST before an alias can be registered.
     */
    ModuleAST* findModuleByPath(InternedString path) const {
        auto it = modulesByPath.find(path);
        return it != modulesByPath.end() ? it->second : nullptr;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Diagnostic Forwarding
    // ─────────────────────────────────────────────────────────────────────

private:
    // ─── Internal Streaming Helpers ─────────────────────────────────────

    template<typename T>
    struct is_interned_string : std::false_type {};

    template<>
    struct is_interned_string<InternedString> : std::true_type {};

    template<>
    struct is_interned_string<InternedString&> : std::true_type {};

    template<>
    struct is_interned_string<const InternedString&> : std::true_type {};

    template<typename T>
    typename std::enable_if<!is_interned_string<typename std::decay<T>::type>::value>::type
    streamTo(std::ostringstream& oss, T&& value) const {
        oss << std::forward<T>(value);
    }

    /**
     * @brief Stream an InternedString by converting it to std::string.
     * 
     * Since StringPool::lookup() now returns std::string, we can stream
     * InternedString directly by looking it up in the pool.
     */
    void streamTo(std::ostringstream& oss, InternedString s) const {
        oss << resources.pool.lookup(s);
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
    // ─── Public Error Reporting API ────────────────────────────────────

    /**
     * @brief Report an error at an AST node's location.
     *
     * Example usage with InternedString:
     * ```cpp
     * ctx.error(useSite, DiagCode::E2001, "undefined type '", ctx.pool().lookup(name), "'");
     * ```
     * 
     * Or, since InternedString is streamable through the diagnostic system:
     * ```cpp
     * ctx.error(useSite, DiagCode::E2001, "undefined type '", name, "'");
     * ```
     * 
     * @note The diagnostic category is derived from the error code range.
     *       No DiagnosticCategory parameter is needed.
     */
    template<typename... Args>
    void error(const BaseAST* node, DiagCode code, Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        diagnostic::error(node ? node->loc : SourceLocation{}, code, {message});
    }

    /// Report an error at a specific location.
    template<typename... Args>
    void errorAt(const SourceLocation& loc, DiagCode code, Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        diagnostic::error(loc, code, {message});
    }

    /// Report a warning at an AST node's location.
    template<typename... Args>
    void warning(const BaseAST* node, DiagCode code, Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        diagnostic::warning(node ? node->loc : SourceLocation{}, code, {message});
    }

    /// Report a warning at a specific location.
    template<typename... Args>
    void warningAt(const SourceLocation& loc, DiagCode code, Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        diagnostic::warning(loc, code, {message});
    }

    /// Report a free-text note at an AST node's location.
    template<typename... Args>
    void note(const BaseAST* node, Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        diagnostic::note(node ? node->loc : SourceLocation{}, message);
    }

    /// Report a free-text note at a specific location.
    template<typename... Args>
    void noteAt(const SourceLocation& loc, Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        diagnostic::note(loc, message);
    }

    /// Report a free-text hint at an AST node's location.
    template<typename... Args>
    void hint(const BaseAST* node, Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        diagnostic::hint(node ? node->loc : SourceLocation{}, message);
    }

    /// Report a free-text hint at a specific location.
    template<typename... Args>
    void hintAt(const SourceLocation& loc, Args&&... args) {
        std::string message = buildMessage(std::forward<Args>(args)...);
        diagnostic::hint(loc, message);
    }

    // ─── Context Queries ────────────────────────────────────────────────

    /**
     * @brief True if analysis can safely continue.
     *
     * Forwards to `diagnostic::canContinue()`, which checks the
     * consecutive-error threshold for the current diagnostic scope.
     */
    bool canContinue() const {
        return diagnostic::canContinue();
    }
};

/**
 * @brief RAII guard for semantic context tracking.
 *
 * Pushes a SemanticContext frame on construction and pops it on
 * destruction — automatically, on every exit path.
 *
 * ```cpp
 * void visitFunction(FuncDeclAST* func, SemaContext& ctx) {
 *     ScopedSemanticContext guard(ctx, SemanticContext::FuncBody,
 *                                   func, func->loc);
 *     // ctx.contexts.current() now returns FuncBody
 *     // return is legal inside the body
 * }
 * ```
 *
 * Non-copyable, non-movable: identity is tied to one specific activation.
 */
struct ScopedSemanticContext {
    explicit ScopedSemanticContext(SemaContext& ctx, SemanticContext kind,
                                    BaseAST* node, const SourceLocation& loc)
        : ctx_(ctx) {
        ctx_.contexts.push(kind, node, loc);
    }

    ~ScopedSemanticContext() {
        ctx_.contexts.pop();
    }

    ScopedSemanticContext(const ScopedSemanticContext&) = delete;
    ScopedSemanticContext& operator=(const ScopedSemanticContext&) = delete;
    ScopedSemanticContext(ScopedSemanticContext&&) = delete;
    ScopedSemanticContext& operator=(ScopedSemanticContext&&) = delete;

private:
    SemaContext& ctx_;
};

/**
 * @brief RAII guard marking a TypeDeclAST as "currently being defined".
 *
 * ```cpp
 * void visitStruct(StructDeclAST* s, SemaContext& ctx) {
 *     ctx.symbols.insertType(s->name, s);
 *     ScopedTypeDefinition defining(ctx, s);
 *     // ctx.definingTypes.isDefining(s) returns true
 *     for (auto* f : s->fields) checkRecursiveFieldType(f, ctx);
 * }
 * ```
 *
 * Non-copyable, non-movable: identity is tied to one specific activation.
 */
struct ScopedTypeDefinition {
    explicit ScopedTypeDefinition(SemaContext& ctx, TypeDeclAST* decl)
        : ctx_(ctx) {
        ctx_.definingTypes.beginDefining(decl);
    }

    ~ScopedTypeDefinition() {
        ctx_.definingTypes.endDefining();
    }

    ScopedTypeDefinition(const ScopedTypeDefinition&) = delete;
    ScopedTypeDefinition& operator=(const ScopedTypeDefinition&) = delete;
    ScopedTypeDefinition(ScopedTypeDefinition&&) = delete;
    ScopedTypeDefinition& operator=(ScopedTypeDefinition&&) = delete;

private:
    SemaContext& ctx_;
};

} // namespace sema