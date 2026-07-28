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
#include "support/SemaLookup.hpp"
#include "support/SemaType.hpp"

#include <vector>
#include <optional>

namespace sema {

// =============================================================================
// Module-Level Analysis
// =============================================================================

/// @brief Analyze all modules in the program.
/// The ONLY entry point for semantic analysis.
void analyze(std::vector<ModuleAST*>& modules, SemaContext& ctx);

/// Analyze a module's top-level declarations in source order.
void analyzeModuleDecls(ModuleAST* module, SemaContext& ctx);

// =============================================================================
// Declaration Analysis
// =============================================================================

void analyzeDecl(const DeclAST* decl, SemaContext& ctx);

// ─── Specific Declaration Analyzers ──────────────────────────────────────

void analyzeImportDecl(const ImportDeclAST* decl, SemaContext& ctx);
void analyzeVarDecl(const VarDeclAST* decl, SemaContext& ctx);
void analyzeFuncDecl(const FuncDeclAST* decl, SemaContext& ctx);
void analyzeParam(const ParamAST* param, SemaContext& ctx);
void analyzeGenericParamDecl(const GenericParamDeclAST* param, SemaContext& ctx);

// ─── Type Declaration Analyzers (Two-Pass for structs) ──────────────────

void analyzeStructDecl(const StructDeclAST* decl, SemaContext& ctx);
void analyzeEnumDecl(const EnumDeclAST* decl, SemaContext& ctx);
void analyzeTraitDecl(const TraitDeclAST* decl, SemaContext& ctx);

// =============================================================================
// Shared Function Analysis
// =============================================================================

/// @brief Analyze a function body with optional extra parameters (e.g., self).
bool analyzeFunctionBody(FuncTypeAST* funcType,
                          StmtPtr body,
                          const std::vector<ParamAST*>& extraParams,
                          const BaseAST* node,
                          SemaContext& ctx);

// =============================================================================
// STATEMENTS - Control flow analysis
// =============================================================================

bool analyzeStmt(const StmtAST* stmt, SemaContext& ctx);
bool analyzeBlock(const BlockStmtAST* block, SemaContext& ctx);
bool analyzeIfStmt(const IfStmtAST* stmt, SemaContext& ctx);
bool analyzeSwitchStmt(const SwitchStmtAST* stmt, SemaContext& ctx);
bool analyzeSwitchCase(const SwitchCaseAST* switchCase, SemaContext& ctx);
bool analyzeForStmt(const ForStmtAST* stmt, SemaContext& ctx);
bool analyzeWhileStmt(const WhileStmtAST* stmt, SemaContext& ctx);
bool analyzeDoWhileStmt(const DoWhileStmtAST* stmt, SemaContext& ctx);
bool analyzeReturnStmt(const ReturnStmtAST* stmt, SemaContext& ctx);
bool analyzeBreakStmt(const BreakStmtAST* stmt, SemaContext& ctx);
bool analyzeContinueStmt(const ContinueStmtAST* stmt, SemaContext& ctx);
bool analyzeExprStmt(const ExprStmtAST* stmt, SemaContext& ctx);
bool analyzeDeclStmt(const DeclStmtAST* stmt, SemaContext& ctx);

// ─── Concurrency ─────────────────────────────────────────────────────────

bool analyzeAsyncStmt(const AsyncStmtAST* stmt, SemaContext& ctx);
bool analyzeAwaitStmt(const AwaitStmtAST* stmt, SemaContext& ctx);
bool analyzeSpawnStmt(const SpawnStmtAST* stmt, SemaContext& ctx);
bool analyzeJoinStmt(const JoinStmtAST* stmt, SemaContext& ctx);

// =============================================================================
// EXPRESSIONS - Type checking
// =============================================================================

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

// =============================================================================
// GENERICS & TRAITS - Validation
// =============================================================================

/// Verify all generic parameters are used.
void validateGenericParamUsage(const DeclAST* owner, SemaContext& ctx);

/// Verify struct implements all trait fields.
bool validateTraitImplementation(const StructDeclAST* structDecl, SemaContext& ctx);

/// Check generic argument arity matches parameters.
void checkGenericArgs(ArenaSpan<TypePtr> args,
                       ArenaSpan<GenericParamDeclPtr> params,
                       const BaseAST* useSite,
                       SemaContext& ctx);

// =============================================================================
// FFI VALIDATION
// =============================================================================

/// Validate @[foreign("C")] function against FFI manifest.
void validateForeignFunc(const FuncDeclAST* decl, const AttributeAST* foreignAttr, SemaContext& ctx);

/// True if type is legal at FFI boundary.
bool isValidFFIType(const TypeAST* type, SemaContext& ctx);

// =============================================================================
// ATTRIBUTES
// =============================================================================

/// Validate attributes on a declaration.
void validateAttributes(ArenaSpan<AttributePtr> attrs, const DeclAST* owner, SemaContext& ctx);
void validateAttribute(const AttributeAST* attr, const DeclAST* owner, SemaContext& ctx);

} // namespace sema