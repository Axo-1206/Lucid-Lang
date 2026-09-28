/**
 * @file Parser.hpp
 * @brief The Lucid parser: one file in, one ModuleAST out.
 *
 * ─── Design: one file per call ────────────────────────────────────────────
 * The parser does not walk imports, does not resolve module paths, and
 * does not touch the filesystem. Module resolution is the CLI's job. The
 * parser's inputs are one file's path, one file's source, and a
 * ParserContext; its output is one ModuleAST*.
 *
 * ─── Design: parse functions build AST; they do not analyze ───────────────
 * No parser function infers a type, resolves a name, checks a signature,
 * or decides whether the program is well-formed. The parser produces a
 * tree; Sema validates it. The parser's only validation is syntactic — the
 * shape of what it read against the shape the grammar allows — and its
 * only output on failure is a diagnostic and a node marked
 * `hasSyntaxError`.
 *
 * ─── Design: the two error behaviors ──────────────────────────────────────
 * A parser function that fails does one of two things:
 *
 *   1. PARTIAL-PARSE. It reports a diagnostic and returns an AST node with
 *      `hasSyntaxError = true`. The node is structurally valid; Sema skips
 *      it. Every function that can partial-parse does so — it produces the
 *      most complete tree it can, marked.
 *
 *   2. SKIP. It reports a diagnostic and returns `nullptr`. There is no
 *      honest node to produce. The caller — which knows what construct the
 *      missing node was supposed to be part of — runs a synchronizer from
 *      `ErrorRecovery.hpp` and continues at the next plausible construct
 *      start.
 *
 * `nullptr` is a rare return; a function only returns it when it cannot
 * produce even a marked node. Every function's error behavior is documented
 * in the .cpp that defines it.
 *
 * ─── Design: attributes and doc comments are the caller's job ─────────────
 * A declaration is optionally preceded by a doc comment and by a sequence
 * of juxtaposed attributes (`@name`, `@name(args)`, ...). Neither is the
 * declaration parser's concern:
 *
 *   - `parseFile`'s top-level loop harvests the doc comment attached to
 *     the next declaration and attaches it to the returned DeclAST.
 *   - `parseDecl` reads the attribute sequence before dispatching to the
 *     specific declaration parser, and attaches the span to the returned
 *     DeclAST.
 *   - `parseColumnDecl` reads its own attribute sequence, because a
 *     column's attributes are part of the column's own parse.
 *
 * A specific parser (`parseTableDecl`, `parseFnDecl`, `parseVarDecl`,
 * `parseImportDecl`) never sees a `@` and never sees a doc comment. Its
 * cursor is on its own keyword; its output is its own node.
 *
 * ─── Design: every parser function has external linkage ───────────────────
 * Every function the parser defines is declared here and defined with
 * external linkage in one of the parser's .cpp files. There are no
 * `static` functions and no forward declarations in the .cpp files. This
 * keeps every parser function's signature visible in one place, so a
 * signature change touches one file, not five.
 *
 * ─── Design: the parser is not a validator ────────────────────────────────
 * Many constructs have rules the parser does not enforce because the
 * parser does not have the context to check them:
 *
 *   - A `switch` case's value must be a constant expression.
 *   - A range's bounds must be the same integer type.
 *   - A `for` binding must match the iterable's element type.
 *   - A `suspend_stmt` must appear only inside a `@sequence` function.
 *   - A `@sequence` function must not return a value or have a `host`
 *     body.
 *   - An assignment's left-hand side must be an lvalue.
 *
 * The parser produces the shape; Sema checks the rule.
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

#include <optional>
#include <string_view>
#include <vector>

namespace lucid::parser {

// =============================================================================
// 1. Entry point
// =============================================================================

/// @brief Parse one source file into a ModuleAST.
///
/// This is the parser's only public entry point. It lexes the source,
/// constructs a TokenStream, parses every top-level declaration, and
/// returns the ModuleAST. It does not resolve imports, does not read
/// other files, and does not walk dependencies.
///
/// The returned pointer is never null. A file that could not be parsed
/// (lexer error, unrecoverable syntax error) still produces a real
/// ModuleAST with `hasErrors == true`. Callers detect failure by
/// checking `module->hasErrors`, not by checking for null.
ModuleAST* parseFile(std::string_view path,
                     std::string_view source,
                     ParserContext&   ctx);

// =============================================================================
// 2. Dispatchers
// =============================================================================

/// @brief Parse one top-level declaration.
///
/// Reads an optional attribute sequence, then dispatches on the
/// declaration keyword that follows. The returned node's `loc` is the
/// location of the first attribute, or of the declaration keyword if
/// there were none.
///
/// The caller (parseFile's loop) harvests the doc comment and attaches it
/// after this returns. This function does not touch doc comments.
///
/// Error behavior: if the attribute sequence is followed by a token that
/// is not a declaration keyword, reports "expected a declaration after
/// the attribute(s)" and returns a marked UnknownDeclAST — not nullptr.
/// The caller's loop only synchronizes on nullptr, so this keeps the
/// broken construct in the loop as a real declaration slot.
DeclAST* parseDecl(TokenStream& stream, ParserContext& ctx);

/// @brief Parse one statement.
///
/// Dispatches on the current token:
///   - a declaration keyword (`let`, `const`) → parseVarDeclStmt
///   - a control-flow keyword → the matching parser
///   - a jump keyword → the matching parser
///   - a suspend keyword (`wait`, `waitFrames`, ...) → the matching parser
///   - an expression start → parseAssignOrExprStmt (see below)
///   - a top-level declaration keyword (`TABLE`, `FIXED`, `FN`) or `@`
///     → reports "not allowed inside a block" and consumes the whole
///       declaration, then returns nullptr
///   - anything else → "expected a statement" and nullptr
///
/// The caller (`parseBlock`) has established that the current token can
/// start a statement.
///
/// Error behavior: returns nullptr for a token that cannot begin a
/// statement. Individual statement parsers may partial-parse; see their
/// declarations below. The `TABLE`/`FN`/`@` case returns nullptr *after*
/// consuming the declaration, so the block loop's recovery sees the token
/// after the declaration, not the declaration itself.
StmtAST* parseStmt(TokenStream& stream, ParserContext& ctx);

// =============================================================================
// 3. Declaration parsers
// =============================================================================
//
// Each is called with the cursor on its keyword; attributes
// have already been read by parseDecl.

/// @brief Parse `import a.b.c` or `import a.b.c as alias`.
///
/// Resolves nothing. The alias is the alias the source wrote, or the last
/// path segment if none was written. The path is the dotted form as a
/// single InternedString ("a.b.c"). The CLI's import-linking step does
/// the resolution.
///
/// Error behavior: if the module path or the alias is missing, reports a
/// diagnostic and returns nullptr. There is no meaningful partial parse
/// of an import with no target.
ImportDeclAST* parseImportDecl(TokenStream& stream, ParserContext& ctx);

/// @brief Parse a table declaration.
///
/// Two forms:
///
///   `TABLE X { col: T; ... }`          a columned table
///   `TABLE X { ... } = [ row; ... ]`   a fixed table with inline rows
///   `TABLE X = host("name")`           a host-backed table
///
/// The parser produces a TableDeclAST in every case. It does not resolve
/// the table's name, does not check column types against each other, and
/// does not decode the table's attributes. Sema does all of that.
///
/// Error behavior: partial-parse. If the body is malformed, the returned
/// TableDeclAST contains the columns and rows that were read, marked
/// `hasSyntaxError`. If the table's name is missing, returns nullptr.
TableDeclAST* parseTableDecl(TokenStream& stream, ParserContext& ctx);

/// @brief Parse one column inside a table body.
///
/// The form is `[attrs] name: type`. The parser reads the column's own
/// attribute sequence, then the name and the type. Sema decodes the
/// attributes.
///
/// Error behavior: partial-parse. If the type is missing, the returned
/// ColumnDeclAST has an UnknownTypeAST type, marked. If the name is
/// missing, returns nullptr.
ColumnDeclAST* parseColumnDecl(TokenStream& stream, ParserContext& ctx);

/// @brief Parse one inline row inside a fixed table's initializer.
///
/// The form is `{ expr, expr, ... }`. The parser produces a RowAST with
/// one cell per comma-separated expression. The expressions are
/// restricted to constant expressions (§4.1.1a); the parser produces
/// ordinary ExprAST* nodes and Sema validates.
///
/// Error behavior: partial-parse. Cells that could not be parsed are
/// replaced by UnknownExprAST nodes, marked. If the opening `{` is
/// missing, returns nullptr.
RowAST* parseRow(TokenStream& stream, ParserContext& ctx);

/// @brief Parse a function declaration.
///
/// Two forms:
///
///   `FN name(params) -> Ret { ... }`          a Lucid-bodied function
///   `FN name(params) -> Ret = host("name")`   a host-bound function
///
/// The return type is optional; a missing return type means `unit`. The
/// parser produces a FnDeclAST; Sema checks the parameters against the
/// body, resolves `@sequence` and `@on` attributes, and validates the
/// host binding.
///
/// Error behavior: partial-parse. If the body is malformed, the returned
/// FnDeclAST has a null body or a marked block, marked `hasSyntaxError`.
/// If the function's name is missing, returns nullptr.
FnDeclAST* parseFnDecl(TokenStream& stream, ParserContext& ctx);

/// @brief Parse a variable declaration: `let x: T = expr` or
///        `const x: T = expr`.
///
/// Appears both as a top-level declaration and, wrapped in a
/// VarDeclStmtAST, inside a block. The VarDeclAST produced here is the
/// same node in both contexts.
///
/// Error behavior: partial-parse. If the type or initializer is missing,
/// the returned VarDeclAST has an UnknownTypeAST or UnknownExprAST in the
/// corresponding field, marked. If the name is missing, returns nullptr.
VarDeclAST* parseVarDecl(TokenStream& stream, ParserContext& ctx);

/// @brief Parse one parameter inside a `FN` parameter list or a lambda.
///
/// The form is `[const] name: type` or `[const] name: ...type`. The
/// parser does not enforce "variadic must be last"; Sema checks.
///
/// Error behavior: partial-parse. If the type is missing, the returned
/// ParamAST has an UnknownTypeAST type, marked. If the name is missing,
/// returns nullptr.
ParamAST* parseParam(TokenStream& stream, ParserContext& ctx);

// =============================================================================
// 4. Statement parsers
// =============================================================================

/// @brief Parse a `{ ... }` block.
///
/// A block is a sequence of statements. The parser reads statements until
/// `}` or EOF, recovering from malformed statements by synchronizing to
/// the next statement start.
///
/// Error behavior: partial-parse. If the closing `}` is missing, the
/// returned BlockStmtAST contains the statements that were read, marked
/// `hasSyntaxError`. Always returns a non-null BlockStmtAST.
BlockStmtAST* parseBlock(TokenStream& stream, ParserContext& ctx);

/// @brief Parse a local variable declaration used as a statement.
///
/// Wraps parseVarDecl's result in a VarDeclStmtAST. The only declaration
/// that can appear inside a block is `let`/`const`; §12.5 forbids
/// `TABLE` and `FN` inside a block.
///
/// Error behavior: inherits parseVarDecl's. Returns nullptr only if
/// parseVarDecl returns nullptr.
VarDeclStmtAST* parseVarDeclStmt(TokenStream& stream, ParserContext& ctx);

/// @brief Parse an assignment statement: `lvalue op expr` or a compound
///        form, or an expression statement.
///
/// The parser parses an expression. If an assignment operator follows, it
/// produces an AssignStmtAST; otherwise it produces an ExprStmtAST. The
/// assignment operators (`=`, `+=`, `-=`, ...) are not binary operators,
/// so parseExpr stops before them cleanly.
///
/// The left-hand side's lvalue-ness is a Sema check; the parser accepts
/// any expression on the left.
///
/// Error behavior: returns nullptr if no expression could be parsed.
StmtAST* parseAssignOrExprStmt(TokenStream& stream, ParserContext& ctx);

/// @brief Parse `return;` or `return expr;`.
///
/// Error behavior: partial-parse. A bare `return` produces a ReturnStmtAST
/// with a null value. If the return value is malformed, the value is an
/// UnknownExprAST, marked.
ReturnStmtAST* parseReturnStmt(TokenStream& stream, ParserContext& ctx);

/// @brief Parse `break;` or `break label;`.
BreakStmtAST* parseBreakStmt(TokenStream& stream, ParserContext& ctx);

/// @brief Parse `continue;` or `continue label;`.
ContinueStmtAST* parseContinueStmt(TokenStream& stream, ParserContext& ctx);

/// @brief Parse `if cond { ... } [else ...]`.
///
/// Error behavior: partial-parse. If the then-branch is not a block, the
/// returned IfStmtAST has an UnknownStmtAST then-branch, marked.
IfStmtAST* parseIfStmt(TokenStream& stream, ParserContext& ctx);

/// @brief Parse `switch expr { case ...: { ... } default: { ... } }`.
///
/// The parser requires a `default` clause. If the source omitted it, the
/// parser reports a diagnostic and produces a placeholder default body.
///
/// Error behavior: partial-parse. A malformed case produces a marked
/// SwitchCaseAST; a malformed switch produces a marked SwitchStmtAST.
SwitchStmtAST* parseSwitchStmt(TokenStream& stream, ParserContext& ctx);

/// @brief Parse one `case` clause inside a switch.
///
/// The form is `case value, value, ... : { ... }`. The case body must
/// be a block.
///
/// Error behavior: partial-parse. If the body is not a block, the
/// returned SwitchCaseAST has a null body, marked.
SwitchCaseAST* parseSwitchCase(TokenStream& stream, ParserContext& ctx);

/// @brief Parse `[label:] while cond { ... }`.
WhileStmtAST* parseWhileStmt(TokenStream& stream, ParserContext& ctx);

/// @brief Parse `[label:] for x: T [, y: U] in iterable { ... }`.
///
/// The iterable may be a range expression (`0..10`, `0..<10`), a table,
/// a column view, or an array. The number of bindings the parser produces
/// is whatever the source wrote; Sema validates the count against the
/// iterable's type.
ForStmtAST* parseForStmt(TokenStream& stream, ParserContext& ctx);

/// @brief Parse one `for` binding.
///
/// A binding is either `_` (a discard; the parser produces a nullptr) or
/// `name: type`. The parser does not check the binding's role against the
/// iterable; Sema does.
///
/// Error behavior: partial-parse. If the type is missing, the returned
/// ParamAST has an UnknownTypeAST type, marked. If the name is missing,
/// returns nullptr.
ParamAST* parseForBinding(TokenStream& stream, ParserContext& ctx);

// ─── Sequence suspend points (§9.2) ─────────────────────────────────────

/// @brief Parse `wait(seconds);`.
WaitStmtAST* parseWaitStmt(TokenStream& stream, ParserContext& ctx);

/// @brief Parse `waitFrames(n);`.
WaitFramesStmtAST* parseWaitFramesStmt(TokenStream& stream, ParserContext& ctx);

/// @brief Parse `waitUntil(pred, arg);`.
WaitUntilStmtAST* parseWaitUntilStmt(TokenStream& stream, ParserContext& ctx);

/// @brief Parse `waitForEvent(EventKind.Member);`.
WaitForEventStmtAST* parseWaitForEventStmt(TokenStream& stream,
                                           ParserContext& ctx);

/// @brief Parse `waitForRequest(req);`.
WaitForRequestStmtAST* parseWaitForRequestStmt(TokenStream& stream,
                                               ParserContext& ctx);

// =============================================================================
// 5. Expression parsers
// =============================================================================

/// @brief Parse an expression. Entry point for the Pratt loop.
///
/// Error behavior: returns nullptr if the current token cannot begin an
/// expression. Callers that need a placeholder use parseRequiredExpr.
ExprAST* parseExpr(TokenStream& stream, ParserContext& ctx);

/// @brief Parse an expression, or produce a marked UnknownExprAST.
///
/// Never returns null. Used wherever the grammar requires an expression
/// and the caller wants to keep going after an error without checking the
/// return value at every call site.
ExprAST* parseRequiredExpr(TokenStream& stream,
                           ParserContext& ctx,
                           const char* expectedWhat);

/// @brief The Pratt loop.
///
/// `minPrec` is the minimum binding power the loop will consume.
/// Operators with lower precedence are left for the enclosing recursive
/// call.
///
/// Error behavior: returns nullptr if the prefix position is empty or a
/// malformed prefix expression was found. Partial-parse behavior of the
/// prefix and postfix parsers applies.
ExprAST* parsePrattExpr(TokenStream& stream,
                        ParserContext& ctx,
                        int minPrec);

/// @brief Parse a prefix form: unary operator, literal, identifier,
///        parenthesized, array literal, lambda, `start`.
ExprAST* parsePrefixExpr(TokenStream& stream, ParserContext& ctx);

/// @brief Parse a primary form.
///
/// A primary is a literal, an identifier, an array literal, a
/// parenthesized expression, a lambda, or a `start` expression. Anything
/// that starts an expression and is not a unary operator reaches this
/// dispatcher.
ExprAST* parsePrimaryExpr(TokenStream& stream, ParserContext& ctx);

/// @brief Parse a postfix form: call, index, or field access.
///
/// The postfix forms bind tighter than any infix operator.
ExprAST* parsePostfixExpr(TokenStream& stream, ParserContext& ctx,
                          ExprAST* lhs);

// ─── Primary form parsers ───────────────────────────────────────────────

/// @brief Parse a literal expression.
LiteralExprAST* parseLiteralExpr(TokenStream& stream, ParserContext& ctx);

/// @brief Parse an identifier expression.
IdentifierExprAST* parseIdentifierExpr(TokenStream& stream,
                                       ParserContext& ctx);

/// @brief Parse an array literal: `[a, b, c]`.
ArrayLiteralExprAST* parseArrayLiteralExpr(TokenStream& stream,
                                           ParserContext& ctx);

/// @brief Parse a parenthesized expression: `( expr )`.
ParenExprAST* parseParenExpr(TokenStream& stream, ParserContext& ctx);

/// @brief Parse a lambda: `(p1, p2) -> expr`.
LambdaExprAST* parseLambdaExpr(TokenStream& stream, ParserContext& ctx);

/// @brief Parse a `start` expression: `start call_expr`.
StartExprAST* parseStartExpr(TokenStream& stream, ParserContext& ctx);

// ─── Postfix form parsers ───────────────────────────────────────────────

/// @brief Parse a call: `callee(args)`.
CallExprAST* parseCallExpr(TokenStream& stream, ParserContext& ctx,
                           ExprAST* callee);

/// @brief Parse an index: `container[index]`.
IndexExprAST* parseIndexExpr(TokenStream& stream, ParserContext& ctx,
                             ExprAST* target);

/// @brief Parse a field access: `object.field`.
FieldAccessExprAST* parseFieldAccessExpr(TokenStream& stream,
                                         ParserContext& ctx,
                                         ExprAST* object);

// ─── Unary and binary ───────────────────────────────────────────────────

/// @brief Parse a prefix unary operation: `-x`, `not x`, `~x`.
UnaryExprAST* parseUnaryExpr(TokenStream& stream, ParserContext& ctx,
                             UnaryOp op);

/// @brief Parse the right-hand side of an infix binary operator.
///
/// The caller (the Pratt loop) has already consumed the operator and
/// computed its precedence.
ExprAST* parseInfixBinary(TokenStream& stream, ParserContext& ctx,
                          ExprAST* lhs, TokenType opTok, int prec);

// =============================================================================
// 6. Type parsers
// =============================================================================

/// @brief Parse a complete type.
///
/// A type is one of:
///   - a primitive type name (`int`, `float`, `string`, ...)
///   - a named type (`Person`, `SpriteRef`, or `alias.Name`)
///   - an array (`[T]` or `[N]T`)
///   - a row reference (`&T`)
///   - a function type (`(T, U) -> R`)
TypeAST* parseType(TokenStream& stream, ParserContext& ctx);

/// @brief Parse a primitive type: `int`, `float`, `bool`, ...
///
/// The parser maps the spelling to a PrimitiveKind, folding aliases
/// (`int` and `int32` both produce PrimitiveKind::Int32).
PrimitiveTypeAST* parsePrimitiveType(TokenStream& stream,
                                     ParserContext& ctx);

/// @brief Parse a named type: `Person`, `alias.Person`, `SpriteRef`.
///
/// The parser reads the name and an optional module qualifier. It does
/// not resolve the name; Sema does.
NamedTypeAST* parseNamedType(TokenStream& stream, ParserContext& ctx);

/// @brief Parse an array type: `[T]` or `[N]T`.
ArrayTypeAST* parseArrayType(TokenStream& stream, ParserContext& ctx);

/// @brief Parse a row reference: `&T`.
RowRefTypeAST* parseRowRefType(TokenStream& stream, ParserContext& ctx);

/// @brief Parse a function type: `(T, U) -> R`.
///
/// The parameter types are unnamed. The parser produces a
/// FunctionTypeAST with a span of TypeAST* parameters.
FunctionTypeAST* parseFunctionType(TokenStream& stream, ParserContext& ctx);

// =============================================================================
// 7. Attribute parsers
// =============================================================================
//
// Attributes are juxtaposed: a declaration may be preceded by any number
// of `@name` or `@name(args)` tokens, with no separator between them and
// no brackets around the list. The parser reads them as a sequence.

/// @brief Parse a sequence of `@name` or `@name(args)` attributes.
///
/// If the current token is not `@`, returns an empty span and consumes
/// nothing. Otherwise reads attributes until the next non-`@` token.
///
/// Error behavior: partial-parse. If an attribute's argument list is
/// malformed, the attribute is produced with the arguments it managed to
/// read, marked. If the sequence ends cleanly, the returned span contains
/// every attribute read.
ArenaSpan<AttributeAST*> parseAttributes(TokenStream& stream,
                                         ParserContext& ctx);

/// @brief Parse one attribute: `@name` or `@name(arg, arg, ...)`.
///
/// The caller has consumed the `@`. This function reads the identifier
/// and the optional argument list.
AttributeAST* parseAttribute(TokenStream& stream, ParserContext& ctx);

/// @brief Parse one attribute argument.
///
/// An attribute argument is a literal, an identifier, or a dotted
/// identifier (`EventKind.KeyDown`). The parser produces the
/// corresponding expression node.
ExprAST* parseAttributeArg(TokenStream& stream, ParserContext& ctx);

// =============================================================================
// 8. Helpers
// =============================================================================

// ─── Lists ──────────────────────────────────────────────────────────────

/// @brief Parse a parenthesized, comma-separated argument list.
///
/// The caller has consumed the opening `(`. This function reads the
/// arguments (if any) and consumes the closing `)`.
///
/// The empty list `()` produces an empty span.
ArenaSpan<ExprAST*> parseArgList(TokenStream& stream, ParserContext& ctx);

/// @brief Parse a parenthesized parameter list for a `FN` declaration or
///        a lambda.
///
/// The caller has consumed the opening `(`. This function reads the
/// parameters (if any) and consumes the closing `)`.
///
/// A variadic parameter (`...type`) must be last; the parser produces the
/// ParamAST and Sema enforces the placement rule.
std::vector<ParamAST*> parseParamList(TokenStream& stream,
                                      ParserContext& ctx);

/// @brief Parse a type-only parameter list for a function type.
///
/// The form is `(T, U, V)`. The parser reads types, not ParamASTs,
/// because function type parameters are unnamed.
ArenaSpan<TypeAST*> parseFunctionTypeParamList(TokenStream& stream,
                                               ParserContext& ctx);

/// @brief Parse a dotted import path: `a`, `a.b`, `a.b.c`.
///
/// Returns the sequence of identifiers. Does not combine them into a
/// single InternedString; the caller does that.
std::vector<InternedString> parseImportPath(TokenStream& stream,
                                            ParserContext& ctx);

// ─── Host targets ───────────────────────────────────────────────────────

/// @brief Parse a `host("name")` target.
///
/// The caller has established that the current token is the `host`
/// keyword. This function consumes the keyword, the `(`, the string
/// literal, and the `)`.
///
/// On success, `targetName` is set to the interned string inside the
/// parentheses and the function returns true. On failure, a diagnostic is
/// reported and the function returns false.
bool parseHostTarget(TokenStream& stream,
                     ParserContext& ctx,
                     InternedString& targetName);

// ─── Doc comments ───────────────────────────────────────────────────────

/// @brief Recover the doc comment attached to the declaration at the
///        current stream position.
///
/// Scans backward from the current position through the raw token vector
/// to find a DOC_COMMENT token immediately preceding the declaration, or
/// a run of line comments above it. Returns nullopt when no comment is
/// attached.
///
/// The caller (parseFile's top-level loop) attaches the returned comment
/// to the declaration's `doc` field after the declaration parser returns.
std::optional<DocComment> harvestDocComment(TokenStream& stream,
                                            ParserContext& ctx);

// =============================================================================
// 9. Lookahead
// =============================================================================
//
// One lookahead: lambda-versus-parenthesized-expression. The other two
// lookaheads from the previous design (`looksLikeFuncDecl`,
// `looksLikeSliceStart`) named constructs that do not exist in this
// grammar.

/// @brief True if the current position begins a lambda.
///
/// A lambda is a parenthesized parameter list followed by `->`. The
/// lookahead skips the parenthesized group and checks whether the next
/// token is `->`. A parenthesized expression like `(a + b)` has no `->`
/// after its closing `)`, so the lookahead returns false.
bool looksLikeLambda(TokenStream& stream, ParserContext& ctx);

} // namespace lucid::parser