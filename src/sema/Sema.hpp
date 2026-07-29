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
// TRAIT VALIDATION
// =============================================================================

/// @brief Validate that a struct implements a trait.
/// 
/// Checks:
///   1. All trait fields exist in the struct
///   2. Field types match exactly
///   3. Const-ness matches (trait const → struct const required)
/// 
/// @param structDecl The struct to validate.
/// @param traitDecl The trait to validate against.
/// @param ctx The semantic context.
/// @return true if the struct correctly implements the trait.
bool validateTraitImplementation(const StructDeclAST* structDecl,
                                  const TraitDeclAST* traitDecl,
                                  SemaContext& ctx);

/// @brief Validate all trait implementations for a struct.
/// 
/// This is the main entry point called during struct analysis.
/// It validates each trait and registers successful implementations.
/// 
/// @param structDecl The struct to validate.
/// @param ctx The semantic context.
/// @return true if all trait implementations are valid.
bool validateAllTraitImplementations(const StructDeclAST* structDecl,
                                      SemaContext& ctx);

/// @brief Check for conflicting field names across traits.
/// 
/// If two traits require the same field name with different types,
/// or if two traits require the same field name with different const-ness,
/// this is a conflict and should be reported as an error.
/// 
/// @param structDecl The struct with traits to check.
/// @param ctx The semantic context.
/// @return true if no conflicts found.
bool checkTraitFieldConflicts(const StructDeclAST* structDecl,
                               SemaContext& ctx);

// =============================================================================
// GENERIC VALIDATION
// =============================================================================

/// @brief Validate generic arguments against generic parameters.
/// 
/// Checks:
///   1. Arity matches
///   2. Each argument satisfies its parameter's constraints
/// 
/// @param args The provided generic arguments.
/// @param params The declared generic parameters.
/// @param useSite The AST node where the instantiation occurs.
/// @param ctx The semantic context.
/// @return true if all arguments are valid.
bool validateGenericArguments(ArenaSpan<TypePtr> args,
                              ArenaSpan<GenericParamDeclPtr> params,
                              const BaseAST* useSite,
                              SemaContext& ctx);

/// @brief Validate that all generic parameters are used.
/// 
/// @param params The generic parameters to check.
/// @param types The types to search in.
/// @param ctx The semantic context.
/// @param useSite The AST node for error reporting.
/// @return true if all parameters are used.
bool validateGenericParameterUsage(ArenaSpan<GenericParamDeclPtr> params,
                                    const std::vector<const TypeAST*>& types,
                                    const BaseAST* useSite,
                                    SemaContext& ctx);

/// @brief Check if a field is accessible on a generic type.
/// 
/// For generic parameters with constraints, only fields from the trait
/// constraints are accessible.
/// 
/// @param genericType The generic type (may be a named type with generic args).
/// @param fieldName The field name to check.
/// @param ctx The semantic context.
/// @return true if the field is accessible.
bool isFieldAccessibleOnGenericType(const TypeAST* genericType,
                                    InternedString fieldName,
                                    SemaContext& ctx);

/// @brief Get the type of a field on a generic type.
/// 
/// @param genericType The generic type.
/// @param fieldName The field name.
/// @param ctx The semantic context.
/// @return The field type, or nullptr if not accessible.
const TypeAST* getFieldTypeOnGenericType(const TypeAST* genericType,
                                         InternedString fieldName,
                                         SemaContext& ctx);

} // namespace sema