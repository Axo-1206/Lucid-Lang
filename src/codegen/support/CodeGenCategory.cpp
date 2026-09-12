/// @file support/CodeGenCategory.cpp
/// @brief Implementation of the function-category classifier.

#include "CodeGenCategory.hpp"
#include "core/ast/ExprAST.hpp"

namespace codegen {

FunctionCategory categorizeFunction(ValueDeclAST* decl) {
    if (!decl) return FunctionCategory::NotAFunction;

    // ─── Named functions (FuncDeclAST) ───────────────────────────────────
    if (decl->isa<FuncDeclAST>()) {
        FuncDeclAST* func = decl->as<FuncDeclAST>();
        bool capturing = func->init && func->init->isa<AnonFuncExprAST>() &&
            func->init->as<AnonFuncExprAST>()->hasClosure;
        return capturing ? FunctionCategory::NamedCapturing
                         : FunctionCategory::NamedNonCapturing;
    }

    // ─── Anonymous functions (AnonFuncExprAST) ───────────────────────────
    // AnonFuncExprAST is not a ValueDeclAST in the AST hierarchy — it's an
    // ExprAST. But if a caller passes one (e.g., in code that walks
    // expression nodes with a ValueDeclAST* widened pointer), classify it
    // correctly rather than falling through to NotAFunction.
    //
    // Note: in practice, call sites that classify functions have already
    // resolved the expression's declaration, so they pass a FuncDeclAST,
    // not an AnonFuncExprAST. This branch is defensive.
    if (decl->kind == ASTKind::AnonFuncExpr) {
        // AnonFuncExprAST is ExprAST-derived, not ValueDeclAST-derived,
        // so this is technically unreachable via the normal API. Left
        // here as documentation of the intent, guarded so it doesn't
        // misbehave if the hierarchy ever changes.
        //
        // If you're reading this because the branch did fire: the caller
        // is passing a non-ValueDeclAST into categorizeFunction, which
        // the signature says not to do. Use a different entry point.
        return FunctionCategory::NotAFunction;
    }

    // ─── Everything else ─────────────────────────────────────────────────
    // VarDeclAST, ParamAST, FieldDeclAST, EnumVariantAST, TypeDeclASTs.
    return FunctionCategory::NotAFunction;
}

bool isClosureCategory(FunctionCategory cat) {
    return cat == FunctionCategory::NamedCapturing
        || cat == FunctionCategory::AnonCapturing;
}

bool isNamedFunctionCategory(FunctionCategory cat) {
    return cat == FunctionCategory::NamedNonCapturing
        || cat == FunctionCategory::NamedCapturing;
}

} // namespace codegen