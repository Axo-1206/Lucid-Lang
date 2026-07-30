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
#include "support/SemaStructField.hpp"
#include "support/TraitValidation.hpp"
#include "support/GenericValidation.hpp"
#include "support/LiteralHelpers.hpp"
#include "types/SemaType.hpp"
#include "registry/AttributeRegistry.hpp"
#include "registry/IntrinsicRegistry.hpp"

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
void registerModuleNames(ModuleAST* module, SemaContext& ctx);

/// @brief Register a declaration's name only (no type resolution).
void registerDeclName(const DeclAST* decl, SemaContext& ctx);

// ─── Specific Name Registration Functions ──────────────────────────────

void registerImportName(const ImportDeclAST* decl, SemaContext& ctx);
void registerVarName(const VarDeclAST* decl, SemaContext& ctx);
void registerFuncName(const FuncDeclAST* decl, SemaContext& ctx);
void registerParamName(const ParamAST* param, SemaContext& ctx);
void registerGenericParamName(const GenericParamDeclAST* param, SemaContext& ctx);
void registerStructName(const StructDeclAST* decl, SemaContext& ctx);
void registerEnumName(const EnumDeclAST* decl, SemaContext& ctx);
void registerTraitName(const TraitDeclAST* decl, SemaContext& ctx);

// ─── Struct Field Registration (Phase 1 of struct two-pass) ────────────

/// @brief Register all field names in a struct (no type resolution).
void registerStructFieldNames(const StructDeclAST* decl, SemaContext& ctx);

// ─── Statement Name Registration ────────────────────────────────────────

/// @brief Register names in a statement (for local scopes).
void registerStmtNames(const StmtAST* stmt, SemaContext& ctx);

// =============================================================================
// TYPE RESOLUTION (Phase 2)
// =============================================================================

/// @brief Resolve all types in a module (after all names are registered).
void resolveModuleDecls(ModuleAST* module, SemaContext& ctx);

/// @brief Resolve a declaration's type and check its body.
void resolveDecl(const DeclAST* decl, SemaContext& ctx);

// ─── Specific Declaration Resolvers ─────────────────────────────────────
// These are the existing analyze*Decl functions, renamed for Phase 2.

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

/// @brief Check if a let initializer references the variable being declared.
void checkLetSelfReference(const ExprAST* expr, InternedString varName, SemaContext& ctx);

// =============================================================================
// LEGACY: Declaration Analysis (DEPRECATED - kept for compatibility during transition)
// =============================================================================

/// @brief [DEPRECATED] Use registerDeclName() + resolveDecl() instead.
[[deprecated("Use registerDeclName() + resolveDecl() instead")]]
void analyzeDecl(const DeclAST* decl, SemaContext& ctx);

/// @brief [DEPRECATED] Use resolveModuleDecls() instead.
[[deprecated("Use resolveModuleDecls() instead")]]
void analyzeModuleDecls(ModuleAST* module, SemaContext& ctx);

/// @brief [DEPRECATED] Use resolveStmt() instead.
[[deprecated("Use resolveStmt() instead")]]
bool analyzeStmt(const StmtAST* stmt, SemaContext& ctx);

/// @brief [DEPRECATED] Use resolveBlock() instead.
[[deprecated("Use resolveBlock() instead")]]
bool analyzeBlock(const BlockStmtAST* block, SemaContext& ctx);

// =============================================================================
// STATEMENTS - Control flow analysis (Phase 2)
// =============================================================================

bool resolveBlock(const BlockStmtAST* block, SemaContext& ctx);
bool resolveIfStmt(const IfStmtAST* stmt, SemaContext& ctx);
bool resolveSwitchStmt(const SwitchStmtAST* stmt, SemaContext& ctx);
bool resolveSwitchCase(const SwitchCaseAST* switchCase, SemaContext& ctx);
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
// EXPRESSIONS - Type checking
// =============================================================================

// These remain unchanged - checkExpr uses lookup which now works in Phase 2

bool checkExpr(ExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Literal Expressions ─────────────────────────────────────────────────

bool checkLiteralExpr(LiteralExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Identifier Expressions ──────────────────────────────────────────────

bool checkIdentifierExpr(IdentifierExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Array and Struct Literals ───────────────────────────────────────────

bool checkArrayLiteralExpr(ArrayLiteralExprAST* expr, const TypeAST* targetType, SemaContext& ctx);
bool checkStructLiteralExpr(StructLiteralExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Binary Expressions ──────────────────────────────────────────────────

bool checkBinaryExpr(BinaryExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Unary Expressions ───────────────────────────────────────────────────

bool checkUnaryExpr(UnaryExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Call Expressions ────────────────────────────────────────────────────

bool checkCallExpr(CallExprAST* expr, const TypeAST* targetType, SemaContext& ctx);
bool checkIntrinsicCallExpr(IntrinsicCallExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Index and Slice Expressions ─────────────────────────────────────────

bool checkIndexExpr(IndexExprAST* expr, const TypeAST* targetType, SemaContext& ctx);
bool checkSliceExpr(SliceExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Field Access Expressions ────────────────────────────────────────────

bool checkFieldAccessExpr(FieldAccessExprAST* expr, const TypeAST* targetType, SemaContext& ctx);
bool checkModuleAccessExpr(ModuleAccessExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Nullable Chain ──────────────────────────────────────────────────────

bool checkNullableChainExpr(NullableChainExprAST* expr, const TypeAST* targetType, SemaContext& ctx);
bool checkNullCoalesceExpr(NullCoalesceExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Assignment ──────────────────────────────────────────────────────────

bool checkAssignExpr(AssignExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Pipeline ────────────────────────────────────────────────────────────

bool checkPipelineExpr(PipelineExprAST* expr, const TypeAST* targetType, SemaContext& ctx);
bool checkPipelineStep(PipelineStepAST* step, const TypeAST* inputType, const TypeAST* targetType, SemaContext& ctx);

// ─── Composition ─────────────────────────────────────────────────────────

bool checkComposeExpr(ComposeExprAST* expr, const TypeAST* targetType, SemaContext& ctx);
bool checkComposeOperand(ComposeOperandAST* operand, const TypeAST* targetType, SemaContext& ctx);

// ─── Anonymous Function ──────────────────────────────────────────────────

bool checkAnonFuncExpr(AnonFuncExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── If Expression ──────────────────────────────────────────────────────

bool checkIfExpr(IfExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─── Range Expression ────────────────────────────────────────────────────

bool checkRangeExpr(RangeExprAST* expr, const TypeAST* targetType, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// CONST EVALUATION
// =============================================================================

/// @brief Evaluate all const declarations in the modules.
/// 
/// Called after type checking. Replaces const expressions with their
/// evaluated values (stored in ExprAST::constValue).
void evaluateConstDeclarations(std::vector<ModuleAST*>& modules, SemaContext& ctx);

} // namespace sema