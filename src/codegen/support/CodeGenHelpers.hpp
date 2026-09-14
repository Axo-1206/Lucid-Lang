#pragma once

#include "../context/CodeGenContext.hpp"
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/BasicBlock.h>
#include <string>

namespace codegen {

/// @brief Get the length of an array at runtime.
/// @param target The array value (pointer for dynamic/fixed, or struct for slice).
/// @param arrayType The Lucid array type.
/// @param ctx The code generation context.
/// @return The length as an LLVM i64 value.
llvm::Value* getArrayLength(
    llvm::Value* target,
    ArrayTypeAST* arrayType,
    CodeGenContext& ctx
);

/// @brief Does this expression's value carry a *fresh* temporary claim that
///        a receiving position can take over, or is it a *load* from an
///        existing binding that still owns its claim?
///
/// This is the single predicate that decides retain-vs-transfer at every
/// store site. See the ownership model in CodeGenOwnership.hpp: Rule 1
/// (transfer from a fresh temporary) vs Rule 2 (copy from an existing
/// binding). The rule is:
///
///   Fresh   → transfer, no retain
///   Load    → copy, retain
///
/// Fresh kinds are the ones whose lowering produces a value with exactly
/// one temporary claim that no named binding owns:
///
///   AnonFuncExprAST     — lowerClosure: alloc_env sets refcount 1.
///   CallExprAST         — callee's Rule 3 retain on return gives the
///                         caller a claim; the caller takes it over.
///   ComposeExprAST      — createCompositionWrapper's result.
///   PipelineExprAST     — the last step's result.
///   IfExprAST           — a PHI of fresh values from both branches, or
///                         one branch is a load. Conservative: treated as
///                         fresh here because a closure-producing `if`
///                         with a fresh branch is the common case, and
///                         misclassifying a load as fresh under-retains.
///                         Callers that know both branches are loads
///                         should not rely on this.
///   NullCoalesceExprAST — same reasoning as IfExprAST.
///
/// Load kinds are the ones that read an existing binding without taking
/// its claim:
///
///   IdentifierExprAST   — load from a named binding.
///   FieldAccessExprAST  — extract from a struct or load through a field.
///   IndexExprAST        — load from an array element.
///
/// Anything else defaults to load (retain), which over-retains rather than
/// under-retains. Over-retaining leaks a refcount; under-retaining is a
/// use-after-free. Prefer the leak.
static bool isFreshExpression(ExprAST* expr) {
    if (!expr) return false;

    switch (expr->kind) {
        case ASTKind::AnonFuncExpr:
        case ASTKind::CallExpr:
        case ASTKind::ComposeExpr:
        case ASTKind::PipelineExpr:
            return true;

        // IfExpr and NullCoalesce can produce either a fresh value or a
        // load depending on which arm the PHI selects. Conservative:
        // treat as a load so the retain fires. A double-retain leaks; a
        // missed retain use-after-frees. Prefer the leak.
        case ASTKind::IfExpr:
        case ASTKind::NullCoalesceExpr:
        case ASTKind::IdentifierExpr:
        case ASTKind::FieldAccessExpr:
        case ASTKind::IndexExpr:
        default:
            return false;
    }
}

}