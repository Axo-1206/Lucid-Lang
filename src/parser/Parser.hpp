/**
 * @file Parser.hpp
 * @brief Lucid language parser – converts token streams into AST.
 * 
 * The parser has a single entry point: parse().
 * - parse() is the entry point - parses the root file and all imports
 * - parseFile() is a helper - parses a single file
 * - parseInternal() parses internal declarations of a file
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
#include <optional>
#include <initializer_list>

namespace parser {

// =============================================================================
// Public API - Entry Point
// =============================================================================

/**
 * @brief Parse the root file and all imported files.
 * 
 * This is the ONE entry point for parsing. It:
 * 1. Parses the root file
 * 2. Recursively parses all imported files via parseUseDecl()
 * 3. Collects all declarations into a single ProgramAST
 * 
 * @param state Parser state for the root file
 * @return ProgramAST* The complete AST with all declarations from all files
 */
ProgramAST* parse(ParserState& state);

// =============================================================================
// Helper Functions
// =============================================================================

/**
 * @brief Parse a single source file into a ProgramAST.
 * 
 * This is a helper function that parses ONE file. It handles:
 * - Lexing the source
 * - Creating a ParserState with TokenStream
 * - Checking cyclic dependencies via ModuleResolver
 * - Parsing the file's internal declarations
 * 
 * @param path The file path
 * @param source The source code
 * @param pool The string pool
 * @param arena The AST arena
 * @param resolver The module resolver (for cyclic detection)
 * @return ProgramAST* The AST for this file, or nullptr on error
 */
ProgramAST* parseFile(const std::string& path, 
                      const std::string& source,
                      StringPool& pool, 
                      ASTArena& arena,
                      ModuleResolver* resolver = nullptr);

// =============================================================================
// Error Recovery
// =============================================================================

void synchronize(ParserState& state);
void synchronizeTo(ParserState& state, std::initializer_list<TokenType> stopTokens);

// =============================================================================
// Internal Parser Functions
// =============================================================================

// ─── Parse Internal Declarations ──────────────────────────────────────────

/**
 * @brief Parse the internal declarations of a single file.
 * 
 * This does NOT handle imports - it only parses the declarations within
 * the current file. Imports are handled by parse() and parseUseDecl().
 * 
 * @param state The parser state
 * @return ProgramAST* The AST for this file's internal declarations
 */
ProgramAST* parseInternal(ParserState& state);

// ─── Declarations ──────────────────────────────────────────────────────────

DeclAST* parseTopLevelDecl(ParserState& state);
UseDeclAST* parseUseDecl(ParserState& state);
VarDeclAST* parseVarDecl(ParserState& state);
FuncDeclAST* parseFuncDecl(ParserState& state);
StructDeclAST* parseStructDecl(ParserState& state);
EnumDeclAST* parseEnumDecl(ParserState& state);
TraitDeclAST* parseTraitDecl(ParserState& state);

// ─── Statements ────────────────────────────────────────────────────────────

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

// ─── Expressions ───────────────────────────────────────────────────────────

ExprAST* parseExpr(ParserState& state);
ExprAST* parsePrattExpr(ParserState& state, int minPrec);
ExprAST* parsePrefixExpr(ParserState& state);
ExprAST* parsePrimaryExpr(ParserState& state);
ExprAST* parsePostfixExpr(ParserState& state, ExprPtr lhs);

// ─── Literals ──────────────────────────────────────────────────────────────

LiteralExprAST* parseLiteralExpr(ParserState& state);
ArrayLiteralExprAST* parseArrayLiteralExpr(ParserState& state);
StructLiteralExprAST* parseStructLiteralExpr(ParserState& state, InternedString typeName, ArenaSpan<TypeAST*> genericArgs);
AnonFuncExprAST* parseAnonFuncExpr(ParserState& state);
IntrinsicCallExprAST* parseIntrinsicCallExpr(ParserState& state);
IfExprAST* parseIfExpr(ParserState& state);
RangeExprAST* parseRangeExpr(ParserState& state, ExprPtr lo);

// ─── Concurrency ───────────────────────────────────────────────────────────

AsyncExprAST* parseAsyncExpr(ParserState& state);
AwaitExprAST* parseAwaitExpr(ParserState& state);
SpawnExprAST* parseSpawnExpr(ParserState& state);
JoinExprAST* parseJoinExpr(ParserState& state);

// ─── Call & Index ──────────────────────────────────────────────────────────

CallExprAST* parseCallExpr(ParserState& state, ExprPtr callee, ArenaSpan<TypeAST*> genericArgs);
IndexExprAST* parseIndexExpr(ParserState& state, ExprPtr target);
SliceExprAST* parseSliceExpr(ParserState& state, ExprPtr target);

// ─── Pipeline & Composition ───────────────────────────────────────────────

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

// ─── Helpers ────────────────────────────────────────────────────────────────

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

// ─── Lookahead Helpers ────────────────────────────────────────────────────

bool isStartOfDeclaration(ParserState& state);
bool isStartOfStatement(ParserState& state);
bool isStartOfType(ParserState& state);
bool looksLikeType(ParserState& state);
bool looksLikeFuncDecl(ParserState& state);
bool looksLikeAnonFunc(ParserState& state);
bool looksLikeStructLiteral(ParserState& state);
bool looksLikeMultiAssignStart(ParserState& state);
bool isFunctionTypeAfterParen(ParserState& state, size_t startPos);

// ─── Precedence Helpers ────────────────────────────────────────────────────

int infixPrec(TokenType type);
BinaryOp tokenToBinaryOp(TokenType type);
AssignOp tokenToAssignOp(TokenType type);
bool isAssignOp(TokenType type);

// ─── Infix Dispatch ───────────────────────────────────────────────────────

ExprPtr parseInfixAssign(ParserState& state, ExprPtr lhs);
ExprPtr parseInfixIs(ParserState& state, ExprPtr lhs);
ExprPtr parseInfixNullCoalesce(ParserState& state, ExprPtr lhs);
ExprPtr parseInfixBinary(ParserState& state, ExprPtr lhs, TokenType opTok, int prec);

// ─── Error Recovery ─────────────────────────────────────────────────────────

void synchronize(ParserState& state);
void synchronizeTo(ParserState& state, std::initializer_list<TokenType> stopTokens);

} // namespace parser