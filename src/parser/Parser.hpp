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
#include "support/ParserContext.hpp"
#include "support/TokenStream.hpp"

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
 * @return ModuleAST* Never nullptr — even a file that couldn't be
 *         meaningfully parsed (failed lex, circular import) still
 *         produces a real ModuleAST. Check ->hasErrors, not the
 *         pointer, to detect failure. See parse()'s own doc comment
 *         in Parser.cpp for the full design.
 */
ModuleAST* parse(const std::string& path, 
                  const std::string& source,
                  ParserContext& ctx);

// =============================================================================
// Error Recovery
// =============================================================================

template<typename Predicate>
void synchronizeUntil(TokenStream& stream, ParserContext& ctx, Predicate stopAt);

template<typename... StopTokens>
void synchronizeTo(TokenStream& stream, ParserContext& ctx, StopTokens... stopTokens);

/**
 * @brief What kind of token synchronizeToContext() actually stopped at.
 *
 * See synchronizeToContext()'s own doc comment (in Parser.cpp) for the
 * full explanation. Short version: Continuable means it landed on the
 * current construct's own comma/closer (safe to keep parsing more list
 * items); Abandoned means it hit the semantic escape valve (';' or a
 * declaration keyword) or EOF (this construct cannot continue at all).
 */
enum class SyncOutcome {
    Continuable,
    Abandoned,
};

SyncOutcome synchronizeToContext(TokenStream& stream, ParserContext& ctx);

// =============================================================================
// Internal Parser Functions
// =============================================================================

/**
 * @brief Parse the internal declarations of a single file.
 * 
 * @param stream The token stream for the file
 * @param ctx The parsing context
 * @param outDecls Output vector to collect declarations. Always contains
 *        whatever was successfully collected, even if a fatal-failure
 *        threshold was hit partway through — check ctx.hasErrors, not a
 *        return value, to tell a clean parse from one that stopped early.
 */
void parseInternal(TokenStream& stream, ParserContext& ctx, std::vector<DeclPtr>& outDecls);

// @NOTE '// X' means not fully implemented or still need to be finalize(bug free)

// ─── Declarations ──────────────────────────────────────────────────────────

DeclAST* parseDecl(TokenStream& stream, ParserContext& ctx);
ImportDeclAST* parseImportDecl(TokenStream& stream, ParserContext& ctx);
VarDeclAST* parseVarDecl(TokenStream& stream, ParserContext& ctx);
FuncDeclAST* parseFuncDecl(TokenStream& stream, ParserContext& ctx);
EnumDeclAST* parseEnumDecl(TokenStream& stream, ParserContext& ctx);
TraitDeclAST* parseTraitDecl(TokenStream& stream, ParserContext& ctx);
StructDeclAST* parseStructDecl(TokenStream& stream, ParserContext& ctx);

FieldDeclPtr parseFieldDecl(TokenStream& stream, ParserContext& ctx);
EnumVariantPtr parseEnumVariant(TokenStream& stream, ParserContext& ctx);
TraitFieldPtr parseTraitField(TokenStream& stream, ParserContext& ctx);

// ─── Statements ──────────────────────────────────────────────────────────── // X

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

// ─── Expressions ─────────────────────────────────────────────────────────── // X

ExprAST* parseExpr(TokenStream& stream, ParserContext& ctx);
ExprAST* parsePrattExpr(TokenStream& stream, ParserContext& ctx, int minPrec);
ExprAST* parsePrefixExpr(TokenStream& stream, ParserContext& ctx);
ExprAST* parsePrimaryExpr(TokenStream& stream, ParserContext& ctx);
ExprAST* parsePostfixExpr(TokenStream& stream, ParserContext& ctx, ExprPtr lhs);

// ─── Literals ────────────────────────────────────────────────────────────── // X

LiteralExprAST* parseLiteralExpr(TokenStream& stream, ParserContext& ctx);
ArrayLiteralExprAST* parseArrayLiteralExpr(TokenStream& stream, ParserContext& ctx);
StructLiteralExprAST* parseStructLiteralExpr(TokenStream& stream, ParserContext& ctx, InternedString typeName, ArenaSpan<TypeAST*> genericArgs);
AnonFuncExprAST* parseAnonFuncExpr(TokenStream& stream, ParserContext& ctx);
IfExprAST* parseIfExpr(TokenStream& stream, ParserContext& ctx);
RangeExprAST* parseRangeExpr(TokenStream& stream, ParserContext& ctx, ExprPtr lo);

// ─── Concurrency ─────────────────────────────────────────────────────────── // X

AsyncStmtAST* parseAsyncStmt(TokenStream& stream, ParserContext& ctx);
AwaitStmtAST* parseAwaitStmt(TokenStream& stream, ParserContext& ctx);
SpawnStmtAST* parseSpawnStmt(TokenStream& stream, ParserContext& ctx);
JoinStmtAST* parseJoinStmt(TokenStream& stream, ParserContext& ctx);

// ─── Call & Index ────────────────────────────────────────────────────────── // X

CallExprAST* parseCallExpr(TokenStream& stream, ParserContext& ctx, ExprPtr callee, ArenaSpan<TypeAST*> genericArgs);
IntrinsicCallExprAST* parseIntrinsicCallExpr(TokenStream& stream, ParserContext& ctx);
IndexExprAST* parseIndexExpr(TokenStream& stream, ParserContext& ctx, ExprPtr target);
SliceExprAST* parseSliceExpr(TokenStream& stream, ParserContext& ctx, ExprPtr target);

// ─── Pipeline & Composition ─────────────────────────────────────────────── // X

ExprAST* parsePipelineExpr(TokenStream& stream, ParserContext& ctx, ExprPtr seed);
ExprAST* parseComposeExpr(TokenStream& stream, ParserContext& ctx, ExprPtr lhs);
PipelineStepAST* parsePipelineStep(TokenStream& stream, ParserContext& ctx);
ComposeOperandAST* parseComposeOperand(TokenStream& stream, ParserContext& ctx);

// ─── Types ──────────────────────────────────────────────────────────────────

TypeAST* parseType(TokenStream& stream, ParserContext& ctx);
TypeAST* parseBaseType(TokenStream& stream, ParserContext& ctx);
TypeAST* parsePrimitiveType(TokenStream& stream, ParserContext& ctx);
TypeAST* parseNamedType(TokenStream& stream, ParserContext& ctx); // for both named type and generic param ref
TypeAST* parseArrayType(TokenStream& stream, ParserContext& ctx);
TypeAST* parseRefType(TokenStream& stream, ParserContext& ctx);
TypeAST* parsePtrType(TokenStream& stream, ParserContext& ctx);
TypeAST* parseFuncType(TokenStream& stream, ParserContext& ctx);
TypeAST* parseTypeWithQualifier(TokenStream& stream, ParserContext& ctx, TypeAST* type); // handle nullable/fallible

// ─── Helpers ────────────────────────────────────────────────────────────────

std::optional<DocComment> harvestDocComment(TokenStream& stream, ParserContext& ctx);
ArenaSpan<AttributePtr> parseAttributes(TokenStream& stream, ParserContext& ctx);
AttributePtr parseAttribute(TokenStream& stream, ParserContext& ctx);
AttributeArgPtr parseAttributeArgLiteral(TokenStream& stream, ParserContext& ctx);

GenericParamDeclPtr parseGenericParamDecl(TokenStream& stream, ParserContext& ctx);
ArenaSpan<GenericParamDeclPtr> parseGenericParamDecls(TokenStream& stream, ParserContext& ctx);
ArenaSpan<TypePtr> parseGenericArgs(TokenStream& stream, ParserContext& ctx);

ArenaSpan<ExprAST*> parseArgList(TokenStream& stream, ParserContext& ctx);
ArenaSpan<TypeAST*> parseReturnList(TokenStream& stream, ParserContext& ctx); // Use when parse function type
std::vector<ParamPtr> parseParamList(TokenStream& stream, ParserContext& ctx);
std::vector<InternedString> parseImportPath(TokenStream& stream, ParserContext& ctx);

TraitRefPtr parseTraitRef(TokenStream& stream, ParserContext& ctx);

ExprPtr parseLvalue(TokenStream& stream, ParserContext& ctx); // X
ExprPtr parseFuncRef(TokenStream& stream, ParserContext& ctx); // X

// ─── Lookahead Helpers ──────────────────────────────────────────────────── // X

bool looksLikeFuncDecl(TokenStream& stream, ParserContext& ctx);
bool looksLikeAnonFunc(TokenStream& stream, ParserContext& ctx);
bool looksLikeMultiAssignStart(TokenStream& stream, ParserContext& ctx);
bool isFunctionTypeAfterParen(TokenStream& stream, ParserContext& ctx, size_t startPos);
bool looksLikeStructLiteral(TokenStream& stream, ParserContext& ctx);

// ─── Precedence Helpers ──────────────────────────────────────────────────── // X

int infixPrec(TokenType type);
BinaryOp tokenToBinaryOp(TokenType type);
AssignOp tokenToAssignOp(TokenType type);
// bool isAssignOp(TokenType type);

// ─── Infix Dispatch ─────────────────────────────────────────────────────── // X

ExprPtr parseInfixAssign(TokenStream& stream, ParserContext& ctx, ExprPtr lhs,  TokenType opTok);
ExprPtr parseInfixNullCoalesce(TokenStream& stream, ParserContext& ctx, ExprPtr lhs);
ExprPtr parseInfixBinary(TokenStream& stream, ParserContext& ctx, ExprPtr lhs, TokenType opTok, int prec);

} // namespace parser