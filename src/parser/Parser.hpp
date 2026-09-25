/**
 * @file Parser.hpp
 * @brief The Lucid parser: one file in, one ModuleAST out.
 *
 * ─── Design: one file per call ────────────────────────────────────────────
 * The parser does not walk imports, does not resolve module paths, and does
 * not touch the filesystem. Module resolution is the CLI's job; the
 * architecture document's pipeline puts ModuleResolver before Parsing in
 * the CLI's column. The parser's inputs are one file's path, one file's
 * source, and a ParserContext; its output is one ModuleAST*.
 *
 * Every parser function below is a piece of that single file's parse. None
 * of them recurses into another file.
 *
 * ─── Design: parse functions build AST; they do not analyze ───────────────
 * No parser function infers a type, resolves a name, checks a signature, or
 * decides whether the program is well-formed. The parser produces a tree;
 * Sema validates it. The parser's only validation is syntactic — the shape
 * of what it read against the shape the grammar allows — and its only
 * output on failure is a diagnostic and a node marked `hasSyntaxError`.
 *
 * ─── Design: every parser function has external linkage ───────────────────
 * Every function the parser defines is declared in this header and defined
 * with external linkage in one of the parser's .cpp files. There are no
 * `static` functions and no forward declarations in the .cpp files. This
 * keeps every parser function's signature visible in one place, so a
 * signature change touches one file, not five.
 *
 * The cost is a larger header. The benefit is that no parser function is
 * hidden from the header's reader, and there are no cross-file
 * forward-declaration chains to keep in sync when a signature changes.
 *
 * ─── Design: desugaring is concentrated ───────────────────────────────────
 * A few functions produce AST that is not a literal transcription of the
 * source. They are:
 *
 *   - parseFuncDecl          wraps a block or expression body in an
 *                            AnonFuncExprAST with the declared signature
 *   - parseFieldDecl         wraps a function-typed field's block default
 *                            in the same way, with a synthesized `self`
 *   - parseDefDecl           wraps a DEF's block implementation
 *   - parseStaticFnDecl      wraps a static function's body in an
 *                            AnonFuncExprAST
 *   - parseAnonFuncExpr      produces the FuncTypeAST chain and the
 *                            AnonFuncExprAST for an inline function literal
 *   - parseTypeDecl          expands `struct X { ... }` into
 *                            `TYPE X = struct { ... }` and likewise for enum
 *
 * Every other parse function is a structural transcription: it reads tokens
 * and produces the node whose fields match.
 */

#pragma once

#include "core/Tokens.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "context/ParserContext.hpp"
#include "context/TokenStream.hpp"
#include "support/ErrorRecovery.hpp"

#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

namespace lucid::parser {

// =============================================================================
// Parser-level result types
// =============================================================================
//
// Small aggregates that a parser function returns when it produces more
// than one value. These are not AST nodes; they are the parser's own
// data carriers.

/// @brief The two spans produced by parsing a struct body.
///
/// A struct body interleaves fields and static functions. The parser's
/// per-item loop produces both spans in one pass; this struct is what that
/// pass returns.
///
/// The two spans are in source order within their respective kinds. A
/// field that appears after a static and another field that appears after
/// that are in the `fields` span in the order they were written; the
/// statics are in the `statics` span in their own order. There is no
/// interleaving between the two spans; the parser does not preserve the
/// mixed order, because nothing downstream needs it.
struct StructBodyParseResult {
    ArenaSpan<FieldDeclAST*>    fields;
    ArenaSpan<StaticFnDeclAST*> statics;
};

// =============================================================================
// 1. Entry points
// =============================================================================

/// @brief Parse one source file into a ModuleAST.
///
/// This is the parser's only public entry point. It lexes the source,
/// constructs a TokenStream, parses every top-level declaration, and
/// returns the ModuleAST. It does not resolve imports, does not read other
/// files, and does not walk dependencies.
///
/// The returned pointer is never null. A file that could not be parsed
/// (lexer error, unrecoverable syntax error) still produces a real
/// ModuleAST with `hasErrors == true`. Callers detect failure by checking
/// `module->hasErrors`, not by checking for null.
///
/// The parser asserts that `ctx.contextStack` is empty on entry and on
/// exit. If either assertion fires, the parser has a bug (an unbalanced
/// pushContext/popContext), not the input.
ModuleAST* parseOneFile(std::string_view path,
                        std::string_view source,
                        ParserContext&   ctx);

/// @brief Parse a file's top-level declarations into `outDecls`.
///
/// Called by `parseOneFile`. Exposed because tooling (a REPL, an LSP
/// incremental parse) may want to parse declarations without constructing
/// the surrounding ModuleAST.
///
/// On return, `outDecls` contains every declaration that was successfully
/// collected, including any that were recovered from a syntax error.
void parseInternal(TokenStream& stream,
                   ParserContext& ctx,
                   std::vector<DeclAST*>& outDecls);

// =============================================================================
// 2. Dispatchers
// =============================================================================

/// @brief Parse one declaration. Dispatches on the current declaration
///        keyword.
///
/// The caller is responsible for having established that the current token
/// is a declaration keyword or `@` for an attribute list.
DeclAST* parseDecl(TokenStream& stream, ParserContext& ctx);

/// @brief Parse one statement. Dispatches on the current statement keyword.
StmtAST* parseStmt(TokenStream& stream, ParserContext& ctx);

// =============================================================================
// 3. Declaration parsers
// =============================================================================

ImportDeclAST* parseImportDecl(TokenStream& stream, ParserContext& ctx);
TypeDeclAST*   parseTypeDecl(TokenStream& stream, ParserContext& ctx);
VarDeclAST*    parseVarDecl(TokenStream& stream, ParserContext& ctx);
FuncDeclAST*   parseFuncDecl(TokenStream& stream, ParserContext& ctx);
TraitDeclAST*  parseTraitDecl(TokenStream& stream, ParserContext& ctx);
SatisfyDeclAST* parseSatisfyDecl(TokenStream& stream, ParserContext& ctx);
DefDeclAST*    parseDefDecl(TokenStream& stream, ParserContext& ctx);

// ─── TYPE target body parsers ───────────────────────────────────────────
//
// The `TYPE X = <target>` frame has six target shapes; two of them
// (`struct { ... }` and `enum { ... }`) have bodies. These parsers handle
// those two bodies. They are called from `parseTypeDecl` and from the
// sugar forms (`struct X { ... }`, `enum X { ... }`).
//
// Both take the already-parsed name and generic parameters, and return the
// completed declaration node.

StructDeclAST* parseStructBody(TokenStream& stream,
                               ParserContext& ctx,
                               InternedString name,
                               ArenaSpan<GenericParamDeclAST*> genericParams,
                               ArenaSpan<NamedTypeAST*> traitRefs,
                               SourceLocation loc);

EnumDeclAST* parseEnumBody(TokenStream& stream,
                           ParserContext& ctx,
                           InternedString name,
                           PrimitiveTypeAST* backingType,
                           SourceLocation loc);

// =============================================================================
// 4. Sub-declaration parsers
// =============================================================================

/// @brief Parse the body of a struct: fields and statics.
///
/// The caller has consumed the opening `{`. This function reads items
/// until `}` or EOF, producing the two spans in a `StructBodyParseResult`.
StructBodyParseResult parseStructBodyList(TokenStream& stream,
                                          ParserContext& ctx,
                                          InternedString structName);

/// @brief Parse one struct field.
FieldDeclAST* parseFieldDecl(TokenStream& stream,
                             ParserContext& ctx,
                             InternedString structName);

/// @brief Parse the body of an enum: the variants.
///
/// The caller has consumed the opening `{`. This function reads variants
/// until `}` or EOF.
ArenaSpan<EnumVariantAST*> parseEnumVariantList(TokenStream& stream,
                                                ParserContext& ctx);

/// @brief Parse one enum variant.
EnumVariantAST* parseEnumVariant(TokenStream& stream, ParserContext& ctx);

/// @brief Parse one `FIELD` clause inside a trait body.
TraitFieldDeclAST* parseTraitField(TokenStream& stream, ParserContext& ctx);

/// @brief Parse one `REQUIRE` clause inside a trait body.
TraitRequireDeclAST* parseRequireClause(TokenStream& stream, ParserContext& ctx);

/// @brief Parse one `static` function inside a struct body.
StaticFnDeclAST* parseStaticFnDecl(TokenStream& stream, ParserContext& ctx);

// =============================================================================
// 5. Statement parsers
// =============================================================================

BlockStmtAST*     parseBlock(TokenStream& stream, ParserContext& ctx);
IfStmtAST*        parseIfStmt(TokenStream& stream, ParserContext& ctx);
SwitchStmtAST*    parseSwitchStmt(TokenStream& stream, ParserContext& ctx);
SwitchCaseAST*    parseSwitchCase(TokenStream& stream, ParserContext& ctx);
ForStmtAST*       parseForStmt(TokenStream& stream, ParserContext& ctx);
WhileStmtAST*     parseWhileStmt(TokenStream& stream, ParserContext& ctx);
DoWhileStmtAST*   parseDoWhileStmt(TokenStream& stream, ParserContext& ctx);
ReturnStmtAST*    parseReturnStmt(TokenStream& stream, ParserContext& ctx);
BreakStmtAST*     parseBreakStmt(TokenStream& stream, ParserContext& ctx);
ContinueStmtAST*  parseContinueStmt(TokenStream& stream, ParserContext& ctx);
ExprStmtAST*      parseExprStmt(TokenStream& stream, ParserContext& ctx);
DeclStmtAST*      parseDeclStmt(TokenStream& stream, ParserContext& ctx);

// ─── Concurrency statements ─────────────────────────────────────────────

SpawnStmtAST* parseSpawnStmt(TokenStream& stream, ParserContext& ctx);
StartStmtAST* parseStartStmt(TokenStream& stream, ParserContext& ctx);
AwaitStmtAST* parseAwaitStmt(TokenStream& stream, ParserContext& ctx);

// =============================================================================
// 6. Expression parsers
// =============================================================================

ExprAST* parseExpr(TokenStream& stream, ParserContext& ctx);
ExprAST* parseRequiredExpr(TokenStream& stream,
                           ParserContext& ctx,
                           const char* expectedWhat);
ExprAST* parsePrattExpr(TokenStream& stream, ParserContext& ctx, int minPrec);
ExprAST* parsePrefixExpr(TokenStream& stream, ParserContext& ctx);
ExprAST* parsePrimaryExpr(TokenStream& stream, ParserContext& ctx);
ExprAST* parsePostfixExpr(TokenStream& stream, ParserContext& ctx, ExprAST* lhs);

// ─── Primary form parsers ───────────────────────────────────────────────

LiteralExprAST*       parseLiteralExpr(TokenStream& stream, ParserContext& ctx);
ArrayLiteralExprAST*  parseArrayLiteralExpr(TokenStream& stream, ParserContext& ctx);
StructLiteralExprAST* parseStructLiteralExpr(TokenStream& stream,
                                             ParserContext& ctx,
                                             InternedString typeName,
                                             ArenaSpan<TypeAST*> genericArgs);
AnonFuncExprAST*      parseAnonFuncExpr(TokenStream& stream, ParserContext& ctx);
IfExprAST*            parseIfExpr(TokenStream& stream, ParserContext& ctx);
IdentifierExprAST*    parseIdentifierExpr(TokenStream& stream, ParserContext& ctx);

// ─── Postfix form parsers ───────────────────────────────────────────────

CallExprAST*          parseCallExpr(TokenStream& stream, ParserContext& ctx, ExprAST* callee);
IndexExprAST*         parseIndexExpr(TokenStream& stream, ParserContext& ctx, ExprAST* target);
SliceExprAST*         parseSliceExpr(TokenStream& stream, ParserContext& ctx, ExprAST* target);
FieldAccessExprAST*   parseFieldAccessExpr(TokenStream& stream, ParserContext& ctx, ExprAST* lhs);
ModuleAccessExprAST*  parseModuleAccessExpr(TokenStream& stream, ParserContext& ctx);

// ─── Pipeline ───────────────────────────────────────────────────────────

ExprAST*         parsePipelineExpr(TokenStream& stream, ParserContext& ctx, ExprAST* seed);
PipelineStepAST* parsePipelineStep(TokenStream& stream, ParserContext& ctx);

// =============================================================================
// 7. Type parsers
// =============================================================================

TypeAST* parseType(TokenStream& stream, ParserContext& ctx);
TypeAST* parseBaseType(TokenStream& stream, ParserContext& ctx);
TypeAST* parseNamedType(TokenStream& stream, ParserContext& ctx);
TypeAST* parseArrayType(TokenStream& stream, ParserContext& ctx);
TypeAST* parseRefType(TokenStream& stream, ParserContext& ctx);
TypeAST* parseTypeWithQualifier(TokenStream& stream,
                                ParserContext& ctx,
                                TypeAST* type);
TypeAST* parseFuncType(TokenStream& stream, ParserContext& ctx);

// =============================================================================
// 8. Infix dispatch
// =============================================================================

ExprAST* parseInfixAssign(TokenStream& stream, ParserContext& ctx,
                          ExprAST* lhs, TokenType opTok);
ExprAST* parseInfixNullCoalesce(TokenStream& stream, ParserContext& ctx,
                                ExprAST* lhs);
ExprAST* parseInfixBinary(TokenStream& stream, ParserContext& ctx,
                          ExprAST* lhs, TokenType opTok, int prec);

// =============================================================================
// 9. Helpers
// =============================================================================

// ─── Doc comments and attributes ────────────────────────────────────────

std::optional<DocComment> harvestDocComment(TokenStream& stream,
                                            ParserContext& ctx);
ArenaSpan<AttributeAST*> parseAttributes(TokenStream& stream, ParserContext& ctx);
AttributeAST*            parseAttribute(TokenStream& stream, ParserContext& ctx);
LiteralExprAST*          parseAttributeArgLiteral(TokenStream& stream, ParserContext& ctx);

// ─── Generic parameters and arguments ───────────────────────────────────

GenericParamDeclAST* parseGenericParamDecl(TokenStream& stream, ParserContext& ctx);
ArenaSpan<GenericParamDeclAST*> parseGenericParamDecls(TokenStream& stream,
                                                       ParserContext& ctx);
ArenaSpan<TypeAST*> parseGenericArgs(TokenStream& stream, ParserContext& ctx);

// ─── Argument and parameter lists ───────────────────────────────────────

ArenaSpan<ExprAST*> parseArgList(TokenStream& stream, ParserContext& ctx);

std::vector<ParamAST*> parseParamList(TokenStream& stream,
                                      ParserContext& ctx,
                                      bool allowNames);
ParamAST* parseSingleParameter(TokenStream& stream,
                               ParserContext& ctx,
                               bool allowNames);

// ─── Import path ────────────────────────────────────────────────────────

std::vector<InternedString> parseImportPath(TokenStream& stream,
                                            ParserContext& ctx);

// ─── Trait reference list ───────────────────────────────────────────────
//
// A comma-separated list of trait names, used after a `:` in a struct's
// declaration and in a trait's inheritance clause.

ArenaSpan<NamedTypeAST*> parseTraitRefList(TokenStream& stream,
                                           ParserContext& ctx);

// ─── Host target sigils ─────────────────────────────────────────────────

bool parseHostTarget(TokenStream& stream,
                     ParserContext& ctx,
                     HostTypeKind& kind,
                     InternedString& targetName);

// ─── Small shared utilities ─────────────────────────────────────────────
//
// Helpers that are called by more than one parser function across .cpp
// files. They are declared here so no .cpp file needs a forward
// declaration.

/// @brief Consume the terminating semicolon of a declaration or
///        sub-declaration.
///
/// When `required` is true, reports an error at the current position if
/// the `;` is missing. When false, consumes a `;` if present and silently
/// accepts its absence.
void consumeDeclarationSemicolon(TokenStream& stream,
                                 ParserContext& ctx,
                                 bool required,
                                 const char* declKind);

/// @brief The same rule for sub-declarations inside a body.
///
/// Provided as a distinct function so that a future change to the
/// sub-declaration rule (for example, allowing a trailing comma) has one
/// place to change.
void consumeSubDeclSemicolon(TokenStream& stream,
                             ParserContext& ctx,
                             bool required,
                             const char* declKind);

/// @brief Build a single-stage `FuncTypeAST` from a parameter group and
///        a return type.
///
/// Used by `parseFuncDecl`, `parseDefDecl`, `parseStaticFnDecl`, and
/// `parseAnonFuncExpr` when they construct a func type from parts. The
/// parameters are moved into an arena span.
FuncTypeAST* makeFuncType(ParserContext& ctx,
                          std::vector<ParamAST*>&& params,
                          TypeAST* returnType);

/// @brief True if the token can start a struct-body item.
///
/// A struct body item starts with `IDENTIFIER` (a normal field),
/// `CONST` (a const field), or `AT_SIGN` (an attribute list). `static`
/// is handled separately by `parseStructBodyList` before this predicate
/// is consulted.
bool startsStructFieldItem(TokenType t);

/// @brief True if the token can start an enum-variant item.
bool startsEnumVariantItem(TokenType t);

// =============================================================================
// 10. Lookahead helpers
// =============================================================================

bool looksLikeFuncDecl(TokenStream& stream, ParserContext& ctx);
bool looksLikeAnonFunc(TokenStream& stream, ParserContext& ctx);

// =============================================================================
// 11. Precedence helpers
// =============================================================================

int infixPrec(TokenType type);
BinaryOp tokenToBinaryOp(TokenType type);
AssignOp tokenToAssignOp(TokenType type);

} // namespace lucid::parser