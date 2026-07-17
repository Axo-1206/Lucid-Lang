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

/**
 * @brief Parse a root file and every module it transitively imports,
 *        returning the whole program as one dependency-ordered array.
 *
 * This is NOT a second entry point with its own parsing logic — it is a
 * thin, non-recursive wrapper around `parse()`: it calls `parse()` exactly
 * once, for `rootPath`, and lets that call recurse through `import`s
 * exactly as it always has (see `parse()`'s own doc comment for that
 * recursive flow). Everything this function adds is what happens
 * *after* `parse()` returns:
 *
 * ```cpp
 * ModuleAST* root = parse(rootPath, rootSource, ctx);
 * // ctx.resolver now holds every module `parse()` visited — root and all
 * // of its transitive imports — cached and in dependency (post-)order via
 * // ModuleResolver::getModuleOrder(). This function just reads that back
 * // into a plain vector.
 * ```
 *
 * ## Why this exists instead of changing parse()'s return type
 *
 * `parse()` is re-entered once per `import`, not once per program — a
 * recursive call for an imported file has no use for "the whole program",
 * only for "did this path resolve to a module." Returning a
 * `std::vector<ModuleAST*>` from `parse()` itself would mean every one of
 * those recursive calls also returns a vector, which either wraps a single
 * element every time or duplicates the transitive set at every nesting
 * level — neither matches how the return value is actually used today
 * (`parseImportDecl` discards it once the module is cached; only `path` +
 * `alias` are stored in the resulting `ImportDeclAST`). Making `parse()`
 * return `void` instead has the opposite problem: every resolver access in
 * `parse()` is already `if (ctx.resolver)`-guarded, meaning single-file
 * parsing with no resolver is a supported mode today (tests, snippets) —
 * `void` would leave that mode with no way to get its `ModuleAST*` back at
 * all. Layering this function on top, rather than changing `parse()`,
 * keeps both of those working exactly as before.
 *
 * ## Relationship to ModuleResolver
 *
 * This does not introduce a new place to store modules — it reads
 * `ctx.resolver->getModuleOrder()` / `getParsedModule()`, which `parse()`
 * already populates via `cacheModule()` on every call, recursive or not.
 * There is exactly one array of "every module in the program" — the
 * resolver's own cache — and this function's return value is a copy of it
 * at a single point in time (immediately after the root parse completes),
 * not a second source of truth.
 *
 * @param rootPath   The root/main file's path.
 * @param rootSource The root file's source code.
 * @param ctx        The parsing context. If `ctx.resolver` is null, there is
 *        no import graph to walk — the returned vector contains only the
 *        root module, exactly what a resolver-less `parse()` call would
 *        have returned on its own.
 * @return Every module `parse()` visited while parsing the root file and
 *         its transitive imports, in dependency order (see
 *         `ModuleResolver::getModuleOrder()` — a module's imports always
 *         precede it; the root module is always last). Suitable to pass
 *         directly to `SemaContext`'s constructor.
 */
std::vector<ModuleAST*> parseProgram(const std::string& rootPath,
                                      const std::string& rootSource,
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
IfExprAST* parseIfExpr(TokenStream& stream, ParserContext& ctx);

// ─── Concurrency ───────────────────────────────────────────────────────────

AsyncStmtAST* parseAsyncStmt(TokenStream& stream, ParserContext& ctx);
AwaitStmtAST* parseAwaitStmt(TokenStream& stream, ParserContext& ctx);
SpawnStmtAST* parseSpawnStmt(TokenStream& stream, ParserContext& ctx);
JoinStmtAST* parseJoinStmt(TokenStream& stream, ParserContext& ctx);

// ─── Call & Index ──────────────────────────────────────────────────────────

CallExprAST* parseCallExpr(TokenStream& stream, ParserContext& ctx, ExprPtr callee, ArenaSpan<TypeAST*> genericArgs);
IntrinsicCallExprAST* parseIntrinsicCallExpr(TokenStream& stream, ParserContext& ctx);
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

// ─── Lookahead Helpers ────────────────────────────────────────────────────

bool looksLikeFuncDecl(TokenStream& stream, ParserContext& ctx);
bool looksLikeAnonFunc(TokenStream& stream, ParserContext& ctx);
bool looksLikeMultiAssignStart(TokenStream& stream, ParserContext& ctx);
bool looksLikeMultiAssignTargets(TokenStream& stream, ParserContext& ctx);
bool looksLikeStructLiteral(TokenStream& stream, ParserContext& ctx);

// ─── Precedence Helpers ────────────────────────────────────────────────────

int infixPrec(TokenType type);
BinaryOp tokenToBinaryOp(TokenType type);
AssignOp tokenToAssignOp(TokenType type);

// ─── Infix Dispatch ───────────────────────────────────────────────────────

ExprPtr parseInfixAssign(TokenStream& stream, ParserContext& ctx, ExprPtr lhs,  TokenType opTok);
ExprPtr parseInfixNullCoalesce(TokenStream& stream, ParserContext& ctx, ExprPtr lhs);
ExprPtr parseInfixBinary(TokenStream& stream, ParserContext& ctx, ExprPtr lhs, TokenType opTok, int prec);

} // namespace parser