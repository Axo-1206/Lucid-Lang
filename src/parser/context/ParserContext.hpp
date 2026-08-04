/**
 * @file ParserContext.hpp
 * @brief Shared parsing context across all files.
 * 
 * ParserContext holds state that is shared across all files being parsed:
 * - StringPool and ASTArena (shared memory)
 * - ModuleResolver (module coordination)
 * - DiagnosticEngine (error reporting)
 * - Context tracking (syntactic context stack)
 * 
 * @design_decision Diagnostics use DiagnosticEngine directly
 *   ParserContext holds a reference to DiagnosticEngine. Error reporting
 *   goes through ctx.diagnostics.error() directly, just like SemaContext.
 *   No convenience wrappers - this maintains consistency across the codebase.
 * 
 * @design_decision TokenStream is separate from ParserContext
 *   TokenStream is per-file (the "tape"), ParserContext is cross-file
 *   (shared state). They are composed in the parser, not merged.
 */

#pragma once

#include "core/Tokens.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/memory/ASTArena.hpp"
#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "TokenStream.hpp"
#include "../ModuleResolver.hpp"

#include <vector>
#include <string>
#include <optional>

namespace parser {

/**
 * @brief The kind of syntactic construct currently being parsed.
 *
 * Pushed/popped as the parser enters and leaves nested constructs.
 * Used by error recovery to pick a sensible follow-set.
 */
enum class SyntacticContext {
    TopLevel,       // File-level declarations
    Attribute,      // @[ ... ]
    GenericParams,  // < ... >  (declaration site: struct<T>, func<T>)
    GenericArgs,    // < ... >  (use site: map<int, string>)
    FuncParams,     // ( ... )  parameter list
    FuncBody,       // { ... }  function body
    FieldBody,      // similar to function body but don't have the 
                    // feature like generic function, this rely on its struct generic 
    StructBody,     // struct { ... }
    EnumBody,       // enum { ... }
    TraitBody,      // trait { ... }
};

inline const char* syntacticContextName(SyntacticContext kind) {
    switch (kind) {
        case SyntacticContext::TopLevel:      return "top level";
        case SyntacticContext::Attribute:     return "attribute list";
        case SyntacticContext::GenericParams: return "generic parameter list";
        case SyntacticContext::GenericArgs:   return "generic argument list";
        case SyntacticContext::FuncParams:    return "function parameter list";
        case SyntacticContext::FuncBody:      return "function body";
        case SyntacticContext::FieldBody:     return "field body";
        case SyntacticContext::StructBody:    return "struct body";
        case SyntacticContext::EnumBody:      return "enum body";
        case SyntacticContext::TraitBody:     return "trait body";
    }
    return "unknown context";
}

struct ContextFrame {
    SyntacticContext kind;
    SourceLocation openedAt;
};

/**
 * @brief Shared parsing context across all files.
 * 
 * ## Usage
 * 
 * ```cpp
 * ParserContext ctx(pool, arena, resolver, diagnostics);
 * TokenStream stream(tokens);
 * 
 * // Parse a file
 * auto* ast = parse(stream, ctx);
 * ```
 * 
 * ## Error Reporting
 * 
 * Use ctx.diagnostics directly (consistent with SemaContext):
 * ```cpp
 * ctx.diagnostics.error(DiagCode::Syntax_ExpectedToken, node, "expected ';'");
 * ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc, "expected ';'");
 * ```
 */
struct ParserContext {
    // ─────────────────────────────────────────────────────────────────────────
    // Shared Resources
    // ─────────────────────────────────────────────────────────────────────────

    StringPool& pool;
    ASTArena& arena;
    DiagnosticEngine& diagnostics;
    ModuleResolver* resolver = nullptr;

    // ─────────────────────────────────────────────────────────────────────────
    // Syntactic Context Stack
    // ─────────────────────────────────────────────────────────────────────────

    std::vector<ContextFrame> contextStack;

    void pushContext(SyntacticContext kind, const SourceLocation& loc) {
        contextStack.push_back({kind, loc});
    }

    void popContext() {
        if (!contextStack.empty()) {
            contextStack.pop_back();
        }
    }

    SyntacticContext currentContext() const {
        return contextStack.empty() ? SyntacticContext::TopLevel : contextStack.back().kind;
    }

    bool isInsideContext(SyntacticContext kind) const {
        for (const auto& frame : contextStack) {
            if (frame.kind == kind) return true;
        }
        return false;
    }

    size_t contextDepth() const { return contextStack.size(); }

    // ─────────────────────────────────────────────────────────────────────────
    // Doc Comment Harvesting
    // ─────────────────────────────────────────────────────────────────────────
    
    std::optional<DocComment> pendingDoc;
    
    // ─────────────────────────────────────────────────────────────────────────
    // Constructor
    // ─────────────────────────────────────────────────────────────────────────
    
    ParserContext(StringPool& p, ASTArena& a, DiagnosticEngine& d, ModuleResolver* r = nullptr)
        : pool(p)
        , arena(a)
        , diagnostics(d)
        , resolver(r)
    {}
    
    // ─── Query Helpers ─────────────────────────────────────────────────
    
    bool canContinue(int maxErrors = 100) const {
        return diagnostics.canContinue(maxErrors);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// RAII Guards
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief RAII guard for syntactic context tracking.
 * 
 * Pushes a SyntacticContext frame on construction and pops it on destruction.
 * 
 * ## Usage
 * 
 * ```cpp
 * ArenaSpan<AttributePtr> parseAttributes(TokenStream& stream, ParserContext& ctx) {
 *     if (!stream.check(TokenType::AT_SIGN)) return {};
 *     stream.consume(); // consume '@'
 *     stream.consume(); // consume '['
 *     ScopedContext guard(ctx, SyntacticContext::Attribute, stream.currentLoc());
 *     // ... parse ...
 * }
 * ```
 */
struct ScopedContext {
    ScopedContext(ParserContext& ctx, SyntacticContext kind, const SourceLocation& loc)
        : ctx_(ctx) {
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
 * Saves and restores the context stack for recursive parsing of imported files.
 * 
 * ## Usage
 * 
 * ```cpp
 * ModuleAST* parse(TokenStream& stream, ParserContext& ctx) {
 *     ScopedFileContext fileContext(ctx);
 *     // ... parse ...
 *     return module;
 * }
 * ```
 */
struct ScopedFileContext {
    explicit ScopedFileContext(ParserContext& ctx)
        : ctx_(ctx)
        , savedContextStack_(std::move(ctx.contextStack))
    {
        ctx_.contextStack.clear();
    }

    ~ScopedFileContext() {
        ctx_.contextStack = std::move(savedContextStack_);
    }

    ScopedFileContext(const ScopedFileContext&) = delete;
    ScopedFileContext& operator=(const ScopedFileContext&) = delete;
    ScopedFileContext(ScopedFileContext&&) = delete;
    ScopedFileContext& operator=(ScopedFileContext&&) = delete;

private:
    ParserContext& ctx_;
    std::vector<ContextFrame> savedContextStack_;
};

} // namespace parser