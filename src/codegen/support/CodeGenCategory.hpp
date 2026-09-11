/// @file support/CodeGenCategory.hpp
/// @brief Classifies ValueDeclAST nodes by the function-category taxonomy.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────
/// Every function in Lucid sits on a grid defined by three independent
/// properties:
///
///   - Naming     : Named (func_decl) vs Anonymous (func_literal)
///   - Capture    : Non-capturing vs Capturing (hasClosure == true)
///   - Mutability : const vs let (named only — anon funcs have no keyword)
///
/// Six concrete categories result, plus generic variants:
///
///   ┌──────────────────────────────────────────────────────────────────┐
///   │  NamedNonCapturing   const add (a int) -> int = { return a; };   │
///   │  NamedCapturing      const add (x int) -> int = { return x+base; };│
///   │  AnonNonCapturing    (a int) -> int { return a + 1 }             │
///   │  AnonCapturing       (a int) -> int { return a + base }          │
///   │  NotAFunction         let x int = 5; / struct S { ... }          │
///   └──────────────────────────────────────────────────────────────────┘
///
/// (The `const`/`let` distinction is orthogonal — it's a property of a
/// FuncDeclAST's keyword, available via `decl->as<FuncDeclAST>()->keyword`,
/// not a separate category. Reassignment support depends on the keyword,
/// not on the category, so it doesn't belong in this enum.)
///
/// "Closure" means NamedCapturing or AnonCapturing — `hasClosure == true`.
///
/// ─── Why This Exists ─────────────────────────────────────────────────────
/// Before this file, the question "is this a closure?" was answered ad-hoc
/// at each call site:
///
///     if (decl->isa<FuncDeclAST>() && decl->as<FuncDeclAST>()->hasClosure)
///
/// That two-condition check appeared in ownsResource, emitRelease,
/// emitRetain, lowerIdentifierExpr, lowerNormalFunctionDecl, and (after
/// Phase 3) lowerAssignExpr. Six places reading the same two fields, each
/// spelling the check slightly differently.
///
/// categorizeFunction collapses it to one call. If the taxonomy changes
/// (a new category, a rename), one file updates.

#pragma once

#include "core/ast/DeclAST.hpp"

namespace codegen {

/// @brief The function-category taxonomy.
///
/// See file header for the full grid. "Closure" is a derived predicate:
/// `isClosureCategory(cat)` is true for NamedCapturing and AnonCapturing.
enum class FunctionCategory {
    /// Not a function at all (VarDeclAST, StructDeclAST, EnumVariantAST, ...).
    /// Callers should take the non-function code path.
    NotAFunction,

    /// Named function with no captures. Produces a bare llvm::Function.
    /// Represents categories 1 and 2 from the taxonomy (const and let
    /// are distinguished by the keyword field, not by this enum).
    NamedNonCapturing,

    /// Named function that captures outer-scope variables. Produces a
    /// { func, env } fat pointer. Represents categories 3 and 4.
    NamedCapturing,

    /// Anonymous function with no captures. Produces a { func, nullptr }
    /// fat pointer (uniform shape, null env). Represents category 5.
    AnonNonCapturing,

    /// Anonymous function that captures outer-scope variables. Produces
    /// a { func, env } fat pointer. Represents category 6.
    AnonCapturing,
};

/// @brief Classify a ValueDeclAST by function category.
///
/// Dispatches on:
///   - The node kind (FuncDeclAST vs AnonFuncExprAST vs anything else)
///   - The hasClosure flag (for function nodes)
///
/// @param decl The declaration to classify. May be null.
/// @return The category. Null returns NotAFunction.
FunctionCategory categorizeFunction(ValueDeclAST* decl);

/// @brief Convenience: does this category represent a closure?
///
/// True for NamedCapturing and AnonCapturing. False for NamedNonCapturing,
/// AnonNonCapturing, and NotAFunction.
///
/// Use this instead of writing `cat == NamedCapturing || cat == AnonCapturing`
/// at call sites — it stays correct if the enum's closure set ever changes.
bool isClosureCategory(FunctionCategory cat);

/// @brief Convenience: is this a named function of any kind?
///
/// True for NamedNonCapturing and NamedCapturing. Useful for call sites
/// that need to look up the FuncDeclAST for keyword, mangledName, etc.
bool isNamedFunctionCategory(FunctionCategory cat);

/// @brief Convenience: is this a capturing function of any kind?
///
/// Alias for isClosureCategory, but named from the "capture" angle for
/// call sites where the capture property is what matters, not the closure
/// value's shape. Kept as a separate name for readability.
inline bool isCapturingCategory(FunctionCategory cat) {
    return isClosureCategory(cat);
}

} // namespace codegen