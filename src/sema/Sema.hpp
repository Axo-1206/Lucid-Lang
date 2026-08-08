/// @file Sema.hpp
/// @brief Lucid semantic analyzer – validates and annotates parsed ASTs.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/ASTArena.hpp"
#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "debug/DebugUtils.hpp"
#include "context/SemaContext.hpp"
#include "support/TypeNarrowHelpers.hpp"
#include "support/SwitchHelpers.hpp"
#include "types/SemaType.hpp"

#include <vector>
#include <optional>

namespace sema {

// =============================================================================
// Module-Level Analysis
// =============================================================================

/// @brief Analyze all modules in the program.
/// The ONLY entry point for semantic analysis.
void analyze(std::vector<ModuleAST*>& modules, SemaContext& ctx);

// =============================================================================
// NAME REGISTRATION (Phase 1)
// =============================================================================

/// @brief Register all names in a module (no type resolution).
void registerTopLevelNames(ModuleAST* module, SemaContext& ctx);

/// @brief Register a declaration's name only (no type resolution).
void registerDeclName(const DeclAST* decl, SemaContext& ctx);

// ─── Specific Name Registration Functions ──────────────────────────────

void registerImportName(const ImportDeclAST* decl, SemaContext& ctx);
void registerVarName(const VarDeclAST* decl, SemaContext& ctx);
void registerFuncName(const FuncDeclAST* decl, SemaContext& ctx);
void registerStructName(const StructDeclAST* decl, SemaContext& ctx);
void registerEnumName(const EnumDeclAST* decl, SemaContext& ctx);
void registerTraitName(const TraitDeclAST* decl, SemaContext& ctx);

// ─── Struct Field Registration (Phase 1 of struct two-pass) ────────────

/// @brief Register all field names in a struct (no type resolution).
void registerStructFieldNames(const StructDeclAST* decl, SemaContext& ctx);

// =============================================================================
// TYPE RESOLUTION (Phase 2)
// =============================================================================

/// @brief Resolve all types in a module (after all names are registered).
void resolveModuleDecls(ModuleAST* module, SemaContext& ctx);

/// @brief Resolve a declaration's type and check its body.
void resolveDecl(const DeclAST* decl, SemaContext& ctx);

// ─── Specific Declaration Resolvers ─────────────────────────────────────

void resolveImportDecl(const ImportDeclAST* decl, SemaContext& ctx);
void resolveVarDecl(const VarDeclAST* decl, SemaContext& ctx);
void resolveFuncDecl(const FuncDeclAST* decl, SemaContext& ctx);
void resolveParam(const ParamAST* param, SemaContext& ctx);
void resolveGenericParam(const GenericParamDeclAST* param, SemaContext& ctx);
void resolveStructDecl(const StructDeclAST* decl, SemaContext& ctx);
void resolveEnumDecl(const EnumDeclAST* decl, SemaContext& ctx);
void resolveTraitDecl(const TraitDeclAST* decl, SemaContext& ctx);

// ─── Struct Field Resolution (Phase 2 of struct two-pass) ──────────────

/// @brief Resolve all field types in a struct (after all fields are registered).
void resolveStructFields(const StructDeclAST* decl, SemaContext& ctx);

// ─── Statement Resolution ──────────────────────────────────────────────

/// @brief Resolve types in a statement (after all names are registered).
bool resolveStmt(const StmtAST* stmt, SemaContext& ctx);

// =============================================================================
// STATEMENTS - Control flow analysis (Phase 2)
// =============================================================================

bool resolveBlock(const BlockStmtAST* block, SemaContext& ctx);
bool resolveIfStmt(const IfStmtAST* stmt, SemaContext& ctx);
bool resolveSwitchStmt(const SwitchStmtAST* stmt, SemaContext& ctx);
bool resolveForStmt(const ForStmtAST* stmt, SemaContext& ctx);
bool resolveWhileStmt(const WhileStmtAST* stmt, SemaContext& ctx);
bool resolveDoWhileStmt(const DoWhileStmtAST* stmt, SemaContext& ctx);
bool resolveReturnStmt(const ReturnStmtAST* stmt, SemaContext& ctx);
bool resolveBreakStmt(const BreakStmtAST* stmt, SemaContext& ctx);
bool resolveContinueStmt(const ContinueStmtAST* stmt, SemaContext& ctx);
bool resolveExprStmt(const ExprStmtAST* stmt, SemaContext& ctx);
bool resolveDeclStmt(const DeclStmtAST* stmt, SemaContext& ctx);

// ─── Concurrency ─────────────────────────────────────────────────────────

bool resolveAsyncStmt(const AsyncStmtAST* stmt, SemaContext& ctx);
bool resolveAwaitStmt(const AwaitStmtAST* stmt, SemaContext& ctx);
bool resolveSpawnStmt(const SpawnStmtAST* stmt, SemaContext& ctx);
bool resolveJoinStmt(const JoinStmtAST* stmt, SemaContext& ctx);

// =============================================================================
// EXPRESSIONS - Type Resolution (New Design)
// =============================================================================

/// @brief Resolve the type of an expression with an optional target type.
/// 
/// This is the new main entry point for expression resolution.
/// It resolves the expression's type, validates against targetType if provided,
/// and stores the result directly on the expression node (resolvedType, valueState).
/// 
/// @param expr The expression to resolve.
/// @param targetType The expected type (nullptr if no constraint).
/// @param ctx The semantic context.
/// @return The resolved type, or UnknownTypeAST on failure.
/// 
/// @note On success, expr->resolvedType is set to the resolved type.
///       On failure, expr->resolvedType is set to UnknownTypeAST.
TypeAST* resolveExprWithTarget(ExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve the type of an expression (legacy wrapper).
/// 
/// This is a wrapper around resolveExprWithTarget() that provides
/// backward compatibility for existing code.
/// 
/// @param expr The expression to resolve.
/// @param ctx The semantic context.
/// @return The resolved type, or UnknownTypeAST on failure.
TypeAST* resolveExpr(ExprAST* expr, SemaContext& ctx);

// ─── Specific Expression Resolvers (New Design) ──────────────────────

/// @brief Resolve a literal expression.
/// @param expr The expression to resolve.
/// @param targetType The expected type (nullptr if no constraint).
/// @param ctx The semantic context.
/// @return The resolved type, or UnknownTypeAST on failure.
TypeAST* resolveLiteralExpr(LiteralExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve an identifier expression.
TypeAST* resolveIdentifierExpr(IdentifierExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve an array literal expression.
TypeAST* resolveArrayLiteralExpr(ArrayLiteralExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve a struct literal expression.
TypeAST* resolveStructLiteralExpr(StructLiteralExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve a binary expression.
TypeAST* resolveBinaryExpr(BinaryExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve a unary expression.
TypeAST* resolveUnaryExpr(UnaryExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve a call expression.
TypeAST* resolveCallExpr(CallExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve an intrinsic call expression.
TypeAST* resolveIntrinsicCallExpr(IntrinsicCallExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve an index expression.
TypeAST* resolveIndexExpr(IndexExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve a slice expression.
TypeAST* resolveSliceExpr(SliceExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve a field access expression.
TypeAST* resolveFieldAccessExpr(FieldAccessExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve a module access expression.
TypeAST* resolveModuleAccessExpr(ModuleAccessExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve a null coalesce expression.
TypeAST* resolveNullCoalesceExpr(NullCoalesceExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve an assignment expression.
TypeAST* resolveAssignExpr(AssignExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve a pipeline expression.
TypeAST* resolvePipelineExpr(PipelineExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve a pipeline step.
TypeAST* resolvePipelineStep(PipelineStepAST* step, const TypeAST* inputType, SemaContext& ctx);

/// @brief Resolve a composition expression.
TypeAST* resolveComposeExpr(ComposeExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve a composition operand.
TypeAST* resolveComposeOperand(ComposeOperandAST* operand, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve an anonymous function expression.
TypeAST* resolveAnonFuncExpr(AnonFuncExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve an if expression.
TypeAST* resolveIfExpr(IfExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

/// @brief Resolve a range expression.
TypeAST* resolveRangeExpr(RangeExprAST* expr, const TypeAST* targetType, SemaContext& ctx);


// =============================================================================
// Helper: Check if an expression is a function value used by 
// resolveStructLiteralExpr and resolveStructFields
// =============================================================================

/// @brief Check if an expression is a function value.
/// 
/// A function value can be:
///   - A named function reference (IdentifierExprAST)
///   - A module function reference (ModuleAccessExprAST)
///   - A call that returns a function (CallExprAST)
///   - A field access that returns a function (FieldAccessExprAST)
/// 
/// @param expr The expression to check.
/// @param ctx The semantic context.
/// @return true if the expression evaluates to a function value.
static bool isFunctionValue(const ExprAST* expr, SemaContext& ctx) {
    if (!expr) return false;

    switch (expr->kind) {
        case ASTKind::IdentifierExpr: {
            const IdentifierExprAST* id = expr->as<IdentifierExprAST>();
            const ValueDeclAST* decl = ctx.lookupValue(id->name);
            return decl && decl->isa<FuncDeclAST>();
        }

        case ASTKind::ModuleAccessExpr: {
            const ModuleAccessExprAST* access = expr->as<ModuleAccessExprAST>();
            const ValueDeclAST* decl = ctx.lookupValueByAlias(access->moduleName, access->memberName);
            return decl && decl->isa<FuncDeclAST>();
        }

        case ASTKind::CallExpr: {
            return expr->resolvedType && expr->resolvedType->isa<FuncTypeAST>();
        }

        case ASTKind::FieldAccessExpr: {
            return expr->resolvedType && expr->resolvedType->isa<FuncTypeAST>();
        }

        case ASTKind::AnonFuncExpr: {
            return true;
        }

        default:
            return false;
    }
}

} // namespace sema