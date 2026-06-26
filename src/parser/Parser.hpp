/**
 * @file Parser.hpp
 * @brief Lucid language parser – converts token streams into AST.
 * 
 * The parser is implemented as a namespace with pure functions.
 * All mutable state is explicitly passed via ParserState.
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
#include "ParserState.hpp"

#include <vector>
#include <string>
#include <unordered_map>
#include <optional>
#include <initializer_list>

namespace parser {

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Parse a complete translation unit.
 * 
 * @param state Parser state (contains token stream and allocators)
 * @return ProgramAST* Root AST node (arena-allocated), or nullptr on fatal error
 */
ProgramAST* parse(ParserState& state);

/**
 * @brief Parse a single file into a ProgramAST.
 * 
 * Convenience wrapper that creates a ParserState internally.
 * 
 * @param path File path (for error reporting)
 * @param source Source code
 * @param pool String pool (for interning)
 * @param arena AST arena (for allocation)
 * @return ProgramAST* Root AST node, or nullptr on error
 */
ProgramAST* parseFile(const std::string& path, 
                      const std::string& source,
                      StringPool& pool, 
                      ASTArena& arena);

// ─────────────────────────────────────────────────────────────────────────────
// Internal Parser Functions (declared here for organization)
// ─────────────────────────────────────────────────────────────────────────────

// ─── Declarations ────────────────────────────────────────────────────────────

DeclAST* parseTopLevelDecl(ParserState& state);
UseDeclAST* parseUseDecl(ParserState& state);
VarDeclAST* parseVarDecl(ParserState& state);
FuncDeclAST* parseFuncDecl(ParserState& state);
StructDeclAST* parseStructDecl(ParserState& state);
EnumDeclAST* parseEnumDecl(ParserState& state);
TraitDeclAST* parseTraitDecl(ParserState& state);

// ─── Statements ──────────────────────────────────────────────────────────────

StmtAST* parseStmt(ParserState& state);
BlockStmtAST* parseBlock(ParserState& state);
IfStmtAST* parseIfStmt(ParserState& state);
SwitchStmtAST* parseSwitchStmt(ParserState& state);
SwitchCaseAST* parseSwitchCase(ParserState& state);
ForStmtAST* parseForStmt(ParserState& state);
WhileStmtAST* parseWhileStmt(ParserState& state);
DoWhileStmtAST* parseDoWhileStmt(ParserState& state);
ReturnStmtAST* parseReturnStmt(ParserState& state);
BreakStmtAST* parseBreakStmt(ParserState& state);
ContinueStmtAST* parseContinueStmt(ParserState& state);
ExprStmtAST* parseExprStmt(ParserState& state);
DeclStmtAST* parseDeclStmt(ParserState& state);
MultiVarDeclAST* parseMultiVarDecl(ParserState& state);
MultiAssignStmtAST* parseMultiAssignStmt(ParserState& state);

// ─── Expressions (Pratt Parser) ─────────────────────────────────────────────

ExprAST* parseExpr(ParserState& state);
ExprAST* parsePrattExpr(ParserState& state, int minPrec);
ExprAST* parsePrefixExpr(ParserState& state);
ExprAST* parsePrimaryExpr(ParserState& state);
ExprAST* parsePostfixExpr(ParserState& state, ExprPtr lhs);

// ─── Literals ────────────────────────────────────────────────────────────────

LiteralExprAST* parseLiteralExpr(ParserState& state);
ArrayLiteralExprAST* parseArrayLiteralExpr(ParserState& state);
StructLiteralExprAST* parseStructLiteralExpr(ParserState& state, InternedString typeName, ArenaSpan<TypeAST*> genericArgs);
AnonFuncExprAST* parseAnonFuncExpr(ParserState& state);
IntrinsicCallExprAST* parseIntrinsicCallExpr(ParserState& state);
IfExprAST* parseIfExpr(ParserState& state);
RangeExprAST* parseRangeExpr(ParserState& state, ExprPtr lo);

// ─── Concurrency ─────────────────────────────────────────────────────────────

AsyncExprAST* parseAsyncExpr(ParserState& state);
AwaitExprAST* parseAwaitExpr(ParserState& state);
SpawnExprAST* parseSpawnExpr(ParserState& state);
JoinExprAST* parseJoinExpr(ParserState& state);

// ─── Call & Index ────────────────────────────────────────────────────────────

CallExprAST* parseCallExpr(ParserState& state, ExprPtr callee, ArenaSpan<TypeAST*> genericArgs);
IndexExprAST* parseIndexExpr(ParserState& state, ExprPtr target);
SliceExprAST* parseSliceExpr(ParserState& state, ExprPtr target);

// ─── Pipeline & Composition ──────────────────────────────────────────────────

ExprAST* parsePipelineExpr(ParserState& state, ExprPtr seed);
ExprAST* parseComposeExpr(ParserState& state, ExprPtr lhs);
PipelineStepAST* parsePipelineStep(ParserState& state);
ComposeOperandAST* parseComposeOperand(ParserState& state);

// ─── Types ──────────────────────────────────────────────────────────────────

TypeAST* parseType(ParserState& state);
TypeAST* parseBaseType(ParserState& state);
TypeAST* parsePrimitiveType(ParserState& state);
TypeAST* parseNamedType(ParserState& state);
TypeAST* parseGenericParamRef(ParserState& state);
TypeAST* parseArrayType(ParserState& state);
TypeAST* parseRefType(ParserState& state);
TypeAST* parsePtrType(ParserState& state);
TypeAST* parseFuncType(ParserState& state);
TypeAST* parseTypeWithNullable(ParserState& state);

// ─── Helpers ─────────────────────────────────────────────────────────────────

std::optional<DocComment> harvestDocComment(ParserState& state);
ArenaSpan<AttributePtr> parseAttributes(ParserState& state);
AttributePtr parseAttribute(ParserState& state);
AttributeArgPtr parseAttributeArgLiteral(ParserState& state);

ArenaSpan<ExprAST*> parseArgList(ParserState& state);
ArenaSpan<TypeAST*> parseReturnList(ParserState& state);
std::vector<ParamPtr> parseParamList(ParserState& state);
std::vector<InternedString> parseUsePath(ParserState& state);

GenericParamDeclPtr parseGenericParamDecl(ParserState& state);
ArenaSpan<GenericParamDeclPtr> parseGenericParamDecls(ParserState& state);
TypePtr parseGenericArg(ParserState& state);
ArenaSpan<TypePtr> parseGenericArgs(ParserState& state);

FieldDeclPtr parseFieldDecl(ParserState& state);
EnumVariantPtr parseEnumVariant(ParserState& state);
TraitFieldPtr parseTraitField(ParserState& state);
TraitRefPtr parseTraitRef(ParserState& state);

ExprPtr parseLvalue(ParserState& state);
ExprPtr parseFuncRef(ParserState& state);

// ─── Lookahead Helpers ──────────────────────────────────────────────────────

bool isStartOfDeclaration(ParserState& state);
bool isStartOfStatement(ParserState& state);
bool isStartOfType(ParserState& state);
bool looksLikeType(ParserState& state);
bool looksLikeFuncDecl(ParserState& state);
bool looksLikeAnonFunc(ParserState& state);
bool looksLikeStructLiteral(ParserState& state);
bool looksLikeMultiAssignStart(ParserState& state);
bool isFunctionTypeAfterParen(ParserState& state, size_t startPos);

// ─── Precedence Helpers ──────────────────────────────────────────────────────

int infixPrec(TokenType type);
BinaryOp tokenToBinaryOp(TokenType type);
AssignOp tokenToAssignOp(TokenType type);
bool isAssignOp(TokenType type);

// ─── Infix Dispatch ─────────────────────────────────────────────────────────

ExprPtr parseInfixAssign(ParserState& state, ExprPtr lhs);
ExprPtr parseInfixIs(ParserState& state, ExprPtr lhs);
ExprPtr parseInfixNullCoalesce(ParserState& state, ExprPtr lhs);
ExprPtr parseInfixBinary(ParserState& state, ExprPtr lhs, TokenType opTok, int prec);

// ─── Error Recovery ─────────────────────────────────────────────────────────

void synchronize(ParserState& state);
void synchronizeTo(ParserState& state, std::initializer_list<TokenType> stopTokens);

} // namespace parser