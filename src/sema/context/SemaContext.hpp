/// @file SemaContext.hpp
/// @brief Unified semantic context - monolithic design.
///
/// # Quick Reference
///
/// | Feature                   | Method                   |
/// | ------------------------- | ------------------------ |
/// | Self-reference check      | `isDefiningType(decl)`   |
/// | Push type being defined   | `pushDefiningType(decl)` |
/// | Get current defining type | `currentDefiningType()`  |
///
/// # Self-Reference Example
///
/// ```lucid
/// struct Node<T> {           ← pushDefiningType(Node) called
///     value T,
///     next *Node<T>?         ← isDefiningType(Node) → true
///                            ← Self-reference allowed via ptr
/// }
///                            ← popDefiningType() called
/// ```
///
/// # Flow for Self-Reference Resolution
///
/// ```
/// 1. registerStructName()
///    └── ctx.symbols.insertType(decl)  ← Name registered
///
/// 2. resolveStructDecl()
///    └── ScopedTypeDefinition guard(ctx, decl)  ← pushDefiningType()
///        └── resolveStructFields()
///            └── resolveType(field->type)
///                └── resolveNamedType()
///                    └── if isDefiningType(type->name) {
///                          // Self-reference detected!
///                          // Allowed if wrapped in ptr/ref/nullable and was not declared with `const` keyword
///                        }
///
/// 3. ~ScopedTypeDefinition()  ← popDefiningType() called
/// ```
///
/// @see ScopedTypeDefinition RAII guard below
/// @see resolveNamedType() in Resolution.cpp

#pragma once

#include "ContextStack.hpp"
#include "core/memory/ASTArena.hpp"
#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"

#include <vector>
#include <unordered_map>
#include <sstream>

namespace sema {

/// @brief Unified semantic context - all in one struct.
/// 
/// This is the main context passed to all semantic analysis functions.
/// It's intentionally monolithic - all state is directly accessible.
struct SemaContext {
    // ─── Resources ──────────────────────────────────────────────────────
    
    StringPool& pool;
    ASTArena& arena;
    ContextStack contexts;
    
    // ─── Modules ────────────────────────────────────────────────────────
    
    std::vector<ModuleAST*> modules;
    std::unordered_map<InternedString, ModuleAST*> modulesByPath;
    
    // ─── Self-Reference Tracking ──────────────────────────────────────
    
    /// Stack of types currently being defined.
    /// 
    /// When resolving a struct's fields, we push the struct onto this stack.
    /// This allows `resolveNamedType()` to detect self-references.
    /// 
    /// @example
    ///   struct Node<T> {           // push Node
    ///       value T,
    ///       next *Node<T>?     // isDefiningType(Node) → true
    ///   }                          // pop Node
    std::vector<const TypeDeclAST*> definingTypes;
    
    // ─── Constructor ────────────────────────────────────────────────────
    
    SemaContext(StringPool& p, ASTArena& a, std::vector<ModuleAST*> mods)
        : pool(p), arena(a), modules(std::move(mods)) {
        for (ModuleAST* m : modules) {
            if (m) modulesByPath[m->filePath] = m;
        }
    }
    
    SemaContext(const SemaContext&) = delete;
    SemaContext& operator=(const SemaContext&) = delete;
    
    // ─── Module Lookup ──────────────────────────────────────────────────
    
    ModuleAST* findModuleByPath(InternedString path) const {
        auto it = modulesByPath.find(path);
        return it != modulesByPath.end() ? it->second : nullptr;
    }
    
    // ─── Self-Reference Helpers ──────────────────────────────────────
    
    /// @brief Push a type that's currently being defined.
    /// 
    /// Called when we start resolving a struct/enum/trait's internals.
    /// @see ScopedTypeDefinition RAII guard
    void pushDefiningType(const TypeDeclAST* decl) {
        definingTypes.push_back(decl);
    }
    
    /// @brief Pop the current defining type.
    void popDefiningType() {
        if (!definingTypes.empty()) {
            definingTypes.pop_back();
        }
    }
    
    /// @brief Check if a type is currently being defined.
    /// 
    /// Used by `resolveNamedType()` to detect self-references.
    /// 
    /// @param decl The type declaration to check.
    /// @return true if the type is on the defining stack.
    /// 
    /// @example
    ///   // In resolveNamedType for Node<T>:
    ///   if (ctx.isDefiningType(decl)) {
    ///       // This is a self-reference!
    ///       // Check if it's wrapped in ptr/ref/nullable
    ///   }
    bool isDefiningType(const TypeDeclAST* decl) const {
        for (const TypeDeclAST* d : definingTypes) {
            if (d == decl) return true;
        }
        return false;
    }
    
    /// @brief Get the innermost type currently being defined.
    /// @return The innermost TypeDeclAST, or nullptr if none.
    const TypeDeclAST* currentDefiningType() const {
        return definingTypes.empty() ? nullptr : definingTypes.back();
    }
    
    // ─── Error Reporting ─────────────────────────────────────────────────
    
    template<typename... Args>
    void error(const BaseAST* node, DiagCode code, Args&&... args) {
        std::string msg = buildMessage(std::forward<Args>(args)...);
        diagnostic::error(node ? node->loc : SourceLocation{}, code, {msg});
    }
    
    template<typename... Args>
    void warning(const BaseAST* node, DiagCode code, Args&&... args) {
        std::string msg = buildMessage(std::forward<Args>(args)...);
        diagnostic::warning(node ? node->loc : SourceLocation{}, code, {msg});
    }
    
    template<typename... Args>
    void note(const BaseAST* node, Args&&... args) {
        std::string msg = buildMessage(std::forward<Args>(args)...);
        diagnostic::note(node ? node->loc : SourceLocation{}, msg);
    }
    
    bool canContinue() const {
        return diagnostic::canContinue();
    }
    
private:
    template<typename... Args>
    std::string buildMessage(Args&&... args) const {
        std::ostringstream oss;
        (oss << ... << args);
        return oss.str();
    }
};

// ─── RAII Guards ─────────────────────────────────────────────────────────

/// @brief RAII guard for semantic context tracking.
/// 
/// Pushes a ContextKind frame on construction and pops it on destruction.
/// 
/// @example
/// ```cpp
/// void resolveFuncDecl(const FuncDeclAST* decl, SemaContext& ctx) {
///     ScopedSemanticContext guard(ctx, ContextKind::FuncBody, decl, decl->loc);
///     // ctx.contexts.current() now returns FuncBody
///     // return is legal inside the body
/// }
/// ```
struct ScopedSemanticContext {
    ScopedSemanticContext(SemaContext& ctx, ContextKind kind,
                          const BaseAST* node, const SourceLocation& loc)
        : ctx_(ctx) {
        ctx_.contexts.push(kind, const_cast<BaseAST*>(node), loc);
    }
    ~ScopedSemanticContext() { ctx_.contexts.pop(); }
    
    ScopedSemanticContext(const ScopedSemanticContext&) = delete;
    ScopedSemanticContext& operator=(const ScopedSemanticContext&) = delete;

private:
    SemaContext& ctx_;
};

/// @brief RAII guard for if condition context.
/// 
/// Enables type narrowing detection during condition analysis.
/// 
/// @example
/// ```cpp
/// void resolveIfStmt(const IfStmtAST* stmt, SemaContext& ctx) {
///     ScopedIfCondition guard(ctx, stmt->elseBranch != nullptr);
///     // During checkExpr, ctx.contexts.isIfConditionCtx() returns true
///     // checkBinaryExpr can detect patterns like x != nil
///     resolveExpr(stmt->condition, ctx);
/// }
/// ```
struct ScopedIfCondition {
    ScopedIfCondition(SemaContext& ctx, bool hasElse)
        : ctx_(ctx) {
        ctx_.contexts.setIfConditionCtx(true);
        ctx_.contexts.setHasElse(hasElse);
        ctx_.contexts.clearPendingNarrowing();
    }
    ~ScopedIfCondition() {
        ctx_.contexts.setIfConditionCtx(false);
    }
    
    ScopedIfCondition(const ScopedIfCondition&) = delete;
    ScopedIfCondition& operator=(const ScopedIfCondition&) = delete;

private:
    SemaContext& ctx_;
};

/// @brief RAII guard for type narrowing in a branch.
/// 
/// Pushes a narrowing level for the then/else branch of an if statement.
/// 
/// @example
/// ```cpp
/// // Then branch: direct narrowing
/// ScopedNarrowing guard(ctx, varName, narrowedType, false);
/// analyzeBlock(thenBranch, ctx);
/// 
/// // Else branch: inverse narrowing
/// ScopedNarrowing guard(ctx, varName, narrowedType, true);
/// analyzeBlock(elseBranch, ctx);
/// ```
struct ScopedNarrowing {
    ScopedNarrowing(SemaContext& ctx, InternedString varName, 
                    const TypeAST* narrowedType, bool isInverse = false)
        : ctx_(ctx) {
        ctx_.contexts.pushNarrowingLevel(isInverse);
        ctx_.contexts.narrowVariable(varName, narrowedType);
    }
    ~ScopedNarrowing() {
        ctx_.contexts.popNarrowingLevel();
    }
    
    ScopedNarrowing(const ScopedNarrowing&) = delete;
    ScopedNarrowing& operator=(const ScopedNarrowing&) = delete;

private:
    SemaContext& ctx_;
};

/// @brief RAII guard for self-reference detection.
/// 
/// Marks a type as "currently being defined" so that self-references
/// can be detected and validated.
/// 
/// @example
/// ```cpp
/// void resolveStructDecl(const StructDeclAST* decl, SemaContext& ctx) {
///     ScopedTypeDefinition guard(ctx, decl);
///     // ctx.isDefiningType(decl) returns true
///     // ctx.currentDefiningType() returns decl
///     resolveStructFields(decl, ctx);
/// }
/// ```
struct ScopedTypeDefinition {
    ScopedTypeDefinition(SemaContext& ctx, const TypeDeclAST* decl)
        : ctx_(ctx) {
        ctx_.pushDefiningType(decl);
    }
    ~ScopedTypeDefinition() {
        ctx_.popDefiningType();
    }
    
    ScopedTypeDefinition(const ScopedTypeDefinition&) = delete;
    ScopedTypeDefinition& operator=(const ScopedTypeDefinition&) = delete;

private:
    SemaContext& ctx_;
};

} // namespace sema