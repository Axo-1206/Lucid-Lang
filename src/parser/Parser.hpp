/**
 * @file Parser.hpp
 * @brief Lucid language parser – converts token streams into AST.
 */

#pragma once

#include "core/Tokens.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/ASTArena.hpp"
#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "ModuleResolver.hpp"
#include "ParserContext.hpp"
#include "TokenStream.hpp"

#include <vector>
#include <string>
#include <optional>
#include <initializer_list>

namespace parser {

// =============================================================================
// Public API - Single Entry Point
// =============================================================================

/**
 * @brief Parse a single source file and all its imports.
 * 
 * This is the ONE and ONLY entry point for parsing.
 * 
 * @param path The file path
 * @param source The source code
 * @param ctx The parsing context (shared across all files)
 * @return ProgramAST* The complete AST, or nullptr on error
 */
ProgramAST* parse(const std::string& path, 
                  const std::string& source,
                  ParserContext& ctx);

// =============================================================================
// Error Recovery
// =============================================================================

void synchronize(TokenStream& stream, ParserContext& ctx);

template<typename... StopTokens>
void synchronizeTo(TokenStream& stream, ParserContext& ctx, StopTokens... stopTokens);

// =============================================================================
// Internal Parser Functions
// =============================================================================

/**
 * @brief Parse the internal declarations of a single file.
 * 
 * @param stream The token stream for the file
 * @param ctx The parsing context
 * @param outDecls Output vector to collect declarations
 * @return true if parsing succeeded, false on fatal error
 */
bool parseInternal(TokenStream& stream, ParserContext& ctx, std::vector<DeclPtr>& outDecls);

// ─── Declarations ──────────────────────────────────────────────────────────

DeclAST* parseDecl(TokenStream& stream, ParserContext& ctx);
UseDeclAST* parseUseDecl(TokenStream& stream, ParserContext& ctx);
VarDeclAST* parseVarDecl(TokenStream& stream, ParserContext& ctx);
FuncDeclAST* parseFuncDecl(TokenStream& stream, ParserContext& ctx);
EnumDeclAST* parseEnumDecl(TokenStream& stream, ParserContext& ctx);
TraitDeclAST* parseTraitDecl(TokenStream& stream, ParserContext& ctx);
StructDeclAST* parseStructDecl(TokenStream& stream, ParserContext& ctx);

FieldDeclPtr parseFieldDecl(TokenStream& stream, ParserContext& ctx);
EnumVariantPtr parseEnumVariant(TokenStream& stream, ParserContext& ctx);
TraitFieldPtr parseTraitField(TokenStream& stream, ParserContext& ctx);

// ─── Statements ────────────────────────────────────────────────────────────

StmtAST* parseStmt(TokenStream& stream, ParserContext& ctx);
BlockStmtAST* parseBlock(TokenStream& stream, ParserContext& ctx);
IfStmtAST* parseIfStmt(TokenStream& stream, ParserContext& ctx);
SwitchStmtAST* parseSwitchStmt(TokenStream& stream, ParserContext& ctx);
SwitchCaseAST* parseSwitchCase(TokenStream& stream, ParserContext& ctx);
ForStmtAST* parseForStmt(TokenStream& stream, ParserContext& ctx);
WhileStmtAST* parseWhileStmt(TokenStream& stream, ParserContext& ctx);
DoWhileStmtAST* parseDoWhileStmt(TokenStream& stream, ParserContext& ctx);
ReturnStmtAST* parseReturnStmt(TokenStream& stream, ParserContext& ctx);
BreakStmtAST* parseBreakStmt(TokenStream& stream, ParserContext& ctx);
ContinueStmtAST* parseContinueStmt(TokenStream& stream, ParserContext& ctx);
ExprStmtAST* parseExprStmt(TokenStream& stream, ParserContext& ctx);
DeclStmtAST* parseDeclStmt(TokenStream& stream, ParserContext& ctx);
MultiVarDeclAST* parseMultiVarDecl(TokenStream& stream, ParserContext& ctx);
MultiAssignStmtAST* parseMultiAssignStmt(TokenStream& stream, ParserContext& ctx);

// ─── Expressions ───────────────────────────────────────────────────────────

ExprAST* parseExpr(TokenStream& stream, ParserContext& ctx);
ExprAST* parsePrattExpr(TokenStream& stream, ParserContext& ctx, int minPrec);
ExprAST* parsePrefixExpr(TokenStream& stream, ParserContext& ctx);
ExprAST* parsePrimaryExpr(TokenStream& stream, ParserContext& ctx);
ExprAST* parsePostfixExpr(TokenStream& stream, ParserContext& ctx, ExprPtr lhs);

// ─── Literals ──────────────────────────────────────────────────────────────

LiteralExprAST* parseLiteralExpr(TokenStream& stream, ParserContext& ctx);
ArrayLiteralExprAST* parseArrayLiteralExpr(TokenStream& stream, ParserContext& ctx);
StructLiteralExprAST* parseStructLiteralExpr(TokenStream& stream, ParserContext& ctx, InternedString typeName, ArenaSpan<TypeAST*> genericArgs);
AnonFuncExprAST* parseAnonFuncExpr(TokenStream& stream, ParserContext& ctx);
IntrinsicCallExprAST* parseIntrinsicCallExpr(TokenStream& stream, ParserContext& ctx);
IfExprAST* parseIfExpr(TokenStream& stream, ParserContext& ctx);
RangeExprAST* parseRangeExpr(TokenStream& stream, ParserContext& ctx, ExprPtr lo);

// ─── Concurrency ───────────────────────────────────────────────────────────

AsyncExprAST* parseAsyncExpr(TokenStream& stream, ParserContext& ctx);
AwaitExprAST* parseAwaitExpr(TokenStream& stream, ParserContext& ctx);
SpawnExprAST* parseSpawnExpr(TokenStream& stream, ParserContext& ctx);
JoinExprAST* parseJoinExpr(TokenStream& stream, ParserContext& ctx);

// ─── Call & Index ──────────────────────────────────────────────────────────

CallExprAST* parseCallExpr(TokenStream& stream, ParserContext& ctx, ExprPtr callee, ArenaSpan<TypeAST*> genericArgs);
IndexExprAST* parseIndexExpr(TokenStream& stream, ParserContext& ctx, ExprPtr target);
SliceExprAST* parseSliceExpr(TokenStream& stream, ParserContext& ctx, ExprPtr target);

// ─── Pipeline & Composition ───────────────────────────────────────────────

ExprAST* parsePipelineExpr(TokenStream& stream, ParserContext& ctx, ExprPtr seed);
ExprAST* parseComposeExpr(TokenStream& stream, ParserContext& ctx, ExprPtr lhs);
PipelineStepAST* parsePipelineStep(TokenStream& stream, ParserContext& ctx);
ComposeOperandAST* parseComposeOperand(TokenStream& stream, ParserContext& ctx);

// ─── Types ──────────────────────────────────────────────────────────────────

TypeAST* parseType(TokenStream& stream, ParserContext& ctx);
TypeAST* parseBaseType(TokenStream& stream, ParserContext& ctx);
TypeAST* parsePrimitiveType(TokenStream& stream, ParserContext& ctx);
TypeAST* parseNamedType(TokenStream& stream, ParserContext& ctx);
TypeAST* parseGenericParamRef(TokenStream& stream, ParserContext& ctx);
TypeAST* parseArrayType(TokenStream& stream, ParserContext& ctx);
TypeAST* parseRefType(TokenStream& stream, ParserContext& ctx);
TypeAST* parsePtrType(TokenStream& stream, ParserContext& ctx);
TypeAST* parseFuncType(TokenStream& stream, ParserContext& ctx);
TypeAST* parseTypeWithNullable(TokenStream& stream, ParserContext& ctx);

// ─── Helpers ────────────────────────────────────────────────────────────────

std::optional<DocComment> harvestDocComment(TokenStream& stream, ParserContext& ctx);
ArenaSpan<AttributePtr> parseAttributes(TokenStream& stream, ParserContext& ctx);
AttributePtr parseAttribute(TokenStream& stream, ParserContext& ctx);
AttributeArgPtr parseAttributeArgLiteral(TokenStream& stream, ParserContext& ctx);

GenericParamDeclPtr parseGenericParamDecl(TokenStream& stream, ParserContext& ctx);
ArenaSpan<GenericParamDeclPtr> parseGenericParamDecls(TokenStream& stream, ParserContext& ctx);
ArenaSpan<TypePtr> parseGenericArgs(TokenStream& stream, ParserContext& ctx);

ArenaSpan<ExprAST*> parseArgList(TokenStream& stream, ParserContext& ctx);
ArenaSpan<TypeAST*> parseReturnList(TokenStream& stream, ParserContext& ctx);
std::vector<ParamPtr> parseParamList(TokenStream& stream, ParserContext& ctx);
std::vector<InternedString> parseUsePath(TokenStream& stream, ParserContext& ctx);

TraitRefPtr parseTraitRef(TokenStream& stream, ParserContext& ctx);

ExprPtr parseLvalue(TokenStream& stream, ParserContext& ctx);
ExprPtr parseFuncRef(TokenStream& stream, ParserContext& ctx);

// ─── Lookahead Helpers ────────────────────────────────────────────────────

bool isStartOfDeclaration(TokenStream& stream, ParserContext& ctx);
bool isStartOfStatement(TokenStream& stream, ParserContext& ctx);
bool isStartOfType(TokenStream& stream, ParserContext& ctx);
bool looksLikeType(TokenStream& stream, ParserContext& ctx);
bool looksLikeFuncDecl(TokenStream& stream, ParserContext& ctx);
bool looksLikeAnonFunc(TokenStream& stream, ParserContext& ctx);
bool looksLikeStructLiteral(TokenStream& stream, ParserContext& ctx);
bool looksLikeMultiAssignStart(TokenStream& stream, ParserContext& ctx);
bool isFunctionTypeAfterParen(TokenStream& stream, ParserContext& ctx, size_t startPos);

// ─── Precedence Helpers ────────────────────────────────────────────────────

int infixPrec(TokenType type);
BinaryOp tokenToBinaryOp(TokenType type);
AssignOp tokenToAssignOp(TokenType type);
bool isAssignOp(TokenType type);

// ─── Infix Dispatch ───────────────────────────────────────────────────────

ExprPtr parseInfixAssign(TokenStream& stream, ParserContext& ctx, ExprPtr lhs);
ExprPtr parseInfixIs(TokenStream& stream, ParserContext& ctx, ExprPtr lhs);
ExprPtr parseInfixNullCoalesce(TokenStream& stream, ParserContext& ctx, ExprPtr lhs);
ExprPtr parseInfixBinary(TokenStream& stream, ParserContext& ctx, ExprPtr lhs, TokenType opTok, int prec);

} // namespace parser