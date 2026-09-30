/**
 * @file ParseDecl.cpp
 * @brief The top-level declaration parsers.
 *
 * ─── What this file implements ────────────────────────────────────────────
 *   - parseDecl          dispatch on the current declaration keyword
 *   - parseImportDecl    `import a.b.c [as d]`
 *   - parseTableDecl     `TABLE X { ... } [= [ rows ]]` or
 *                        `TABLE X = host("name")`
 *   - parseTableBody     (file-local) the `{ column* }` loop
 *   - parseTableInit     (file-local) the `= [ row* ]` loop
 *   - parseColumnDecl    `[attrs] name: type`
 *   - parseRow           `{ expr, expr, ... }`
 *   - parseFnDecl        `FN name(params) -> Ret { ... }` or
 *                        `FN name(params) -> Ret = host("name")`
 *   - parseVarDecl       `let x: T = expr;` / `const x: T = expr;`
 *   - parseParam         `[const] name: type` or `[const] name: ...type`
 *   - parseHostTarget    `host("name")` (used by table and function
 *                        declarations)
 *
 * ─── Design: attributes are the caller's job ──────────────────────────────
 * A declaration may be preceded by a juxtaposed sequence of attributes
 * (`@name`, `@name(args)`, ...). parseDecl reads the sequence before
 * dispatching, and attaches the span to the returned node. The specific
 * declaration parsers never see an `@`; their cursor starts on the
 * declaration keyword.
 *
 * parseColumnDecl is the exception: a column's attributes are part of the
 * column's own parse. The table-body loop sees `@` and calls
 * parseColumnDecl, which reads the attributes then the column.
 *
 * ─── Design: `;` is optional ─────────────────────────────────────────────
 * A declaration ends at the first token that cannot continue it (§12.6).
 * `;` is an optional empty declaration, skipped by the enclosing loop
 * (`parseFile`'s top-level loop, or `parseBlock`'s statement loop). No
 * parser function in this file consumes a `;`.
 *
 * `parseFile`'s loop and `parseBlock`'s loop are the only places a `;`
 * is consumed. A declaration parser stops before one, and the loop's
 * next iteration skips it.
 *
 * ─── Design: loc points at the first attribute ────────────────────────────
 * parseDecl captures the declaration's `loc` before reading the attribute
 * sequence, so a declaration with `@export` in front has its `loc`
 * pointing at the `@`. This is what the LSP wants: a selection that
 * starts at `@` selects the whole declaration.
 *
 * ─── Design: partial-parse vs. skip ───────────────────────────────────────
 * Each parser either partial-parses (returns a marked node) or skips
 * (returns nullptr) on error. parseDecl wraps a nullptr from a specific
 * parser in a marked UnknownDeclAST so that the top-level loop always
 * receives a node — the attributes are preserved on the placeholder, and
 * the LSP sees a declaration slot for every attempted declaration.
 */

#include "parser/Parser.hpp"
#include "parser/support/ErrorRecovery.hpp"
#include "parser/support/GrammarPositions.hpp"

#include "core/Tokens.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"

#include <charconv>
#include <string>
#include <vector>

using namespace lucid::diag;

namespace lucid::parser {

// =============================================================================
// parseDecl — the dispatcher
// =============================================================================

DeclAST* parseDecl(TokenStream& stream, ParserContext& ctx) {
    // Capture the declaration's location before consuming anything. If
    // the declaration has attributes, this points at the first `@`; that
    // is the "start of the declaration" from the LSP's point of view.
    const SourceLocation declLoc = stream.currentLoc();

    // ─── Attributes ───────────────────────────────────────────────────────
    //
    // parseAttributes returns an empty span and consumes nothing if the
    // current token is not `@`. Otherwise it consumes the whole
    // juxtaposed sequence. The specific parsers below never see `@`.
    ArenaSpan<AttributeAST*> attrs = parseAttributes(stream, ctx);

    // ─── Dispatch on the declaration keyword ──────────────────────────────
    const TokenType keyword = stream.peekType();

    DeclAST* decl = nullptr;

    switch (keyword) {
        case TokenType::KW_IMPORT:
            decl = parseImportDecl(stream, ctx);
            break;

        case TokenType::KW_TABLE:
            decl = parseTableDecl(stream, ctx);
            break;

        case TokenType::KW_FN:
            decl = parseFnDecl(stream, ctx);
            break;

        case TokenType::KW_LET:
        case TokenType::KW_CONST:
            decl = parseVarDecl(stream, ctx);
            break;

        default:
            // The caller (parseFile's loop) checked isDeclarationStart
            // before calling us. If we reach this case, the source had
            // attributes and no declaration keyword followed them.
            ctx.diag.errorAt(
                DiagCode::Syntax_ExpectedDeclTarget,
                stream.currentLoc(),
                "expected a declaration after the attribute(s), got '",
                stream.peekValueView(ctx.pool), "'");

            // Synchronize to the next declaration so the loop does not
            // immediately re-report the same problem.
            synchronizeUntilDepth(stream, isTopLevelRecoveryStop);
            break;
    }

    // ─── Wrap a nullptr in an UnknownDeclAST ──────────────────────────────
    //
    // A specific parser returns nullptr when it cannot produce even a
    // marked node (a missing name, say). For the LSP's benefit, we still
    // want a slot in the AST with the attributes attached. An
    // UnknownDeclAST is the honest representation of "there was a
    // declaration here, it had these attributes, and the declaration
    // itself was unrecoverable".
    if (decl == nullptr) {
        auto* unk = ctx.arena.make<UnknownDeclAST>();
        unk->loc = declLoc;
        unk->attributes = attrs;
        unk->hasSyntaxError = true;
        return unk;
    }

    // ─── Attach common fields ─────────────────────────────────────────────
    //
    // Every declaration has the same loc and attributes fields on
    // DeclAST. The specific parser set its own fields; this block fills
    // the shared ones.
    //
    // The loc is overwritten to point at the first attribute (if any),
    // not at the declaration keyword.
    decl->loc = declLoc;
    decl->attributes = attrs;
    return decl;
}

// =============================================================================
// parseImportDecl — `import a.b.c [as d]`
// =============================================================================

ImportDeclAST* parseImportDecl(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_IMPORT)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'import', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    // Dotted module path: `a`, `a.b`, `a.b.c`.
    std::vector<InternedString> parts = parseImportPath(stream, ctx);
    if (parts.empty()) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedModulePath,
                           stream.currentLoc(),
                           "expected a module path after 'import'");
        return nullptr;
    }

    // Combine the path segments into one InternedString. The combined
    // string is the module's identity as written in source; the CLI's
    // resolver translates it to a file path.
    std::string combined;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) combined += '.';
        combined += std::string(ctx.pool.lookupView(parts[i]));
    }
    InternedString path = ctx.pool.intern(combined);

    // The alias. If `as` is present, the next token must be an
    // identifier. Otherwise, the alias is the last path segment.
    InternedString alias;
    if (stream.match(TokenType::KW_AS)) {
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diag.errorAt(DiagCode::Syntax_ExpectedIdentifier,
                               stream.currentLoc(),
                               "expected an alias name after 'as', got '",
                               stream.peekValueView(ctx.pool), "'");
            return nullptr;
        }
        Token aliasTok = stream.consume();
        alias = aliasTok.value;
    } else {
        alias = parts.back();
    }

    // No `;`: the grammar's import_decl production has no terminator.

    auto* importDecl = ctx.arena.make<ImportDeclAST>(path, alias);
    importDecl->loc = loc;
    return importDecl;
}

// =============================================================================
// parseTableBody — `{ column* }`
// =============================================================================

namespace {

/// @brief Parse a table body. The caller has established that the current
///        token is `{`; on return, the cursor is past the closing `}`.
///
/// Columns are juxtaposed: no separator, no `;`, no comma. A stray `;`
/// or `,` between columns is skipped silently (§2.7 allows them freely).
///
/// Error behavior: partial-parse. A column that fails to parse is
/// recorded with its own marked node (see parseColumnDecl); if the
/// column parser returns nullptr, the body's loop synchronizes to the
/// next column start.
void parseTableBody(TokenStream& stream,
                    ParserContext& ctx,
                    std::vector<ColumnDeclAST*>& columns) {
    const SourceLocation openLoc = stream.currentLoc();

    if (!stream.match(TokenType::LBRACE)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedBlock, openLoc,
                           "expected '{' to open the table body, got '",
                           stream.peekValueView(ctx.pool), "'");
        return;
    }

    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE) &&
           ctx.canContinue()) {
        // Skip free separators.
        if (stream.match(TokenType::SEMICOLON) ||
            stream.match(TokenType::COMMA)) {
            continue;
        }

        if (!isColumnStart(stream.peekType())) {
            ctx.diag.errorAt(
                DiagCode::Syntax_ExpectedColumn,
                stream.currentLoc(),
                "expected a column declaration, got '",
                stream.peekValueView(ctx.pool), "'");

            synchronizeUntil(stream, [](TokenType t) {
                return isColumnStart(t)
                    || t == TokenType::RBRACE
                    || t == TokenType::SEMICOLON;
            });
            if (stream.check(TokenType::RBRACE) || stream.isAtEnd()) break;
            continue;
        }

        const size_t posBefore = stream.getPos();
        ColumnDeclAST* column = parseColumnDecl(stream, ctx);
        if (column != nullptr) {
            columns.push_back(column);
            continue;
        }

        // parseColumnDecl returned nullptr. If it did not advance, we
        // would loop forever; synchronize to make progress.
        if (stream.getPos() == posBefore) {
            synchronizeUntil(stream, [](TokenType t) {
                return isColumnStart(t)
                    || t == TokenType::RBRACE
                    || t == TokenType::SEMICOLON;
            });
            if (stream.check(TokenType::RBRACE) || stream.isAtEnd()) break;
        }
    }

    if (!stream.match(TokenType::RBRACE)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '}' to close the table body, got '",
                           stream.peekValueView(ctx.pool), "'");
    }
}

/// @brief Parse a table's inline row initializer. The caller has
///        established that the current token is `=`; on return, the
///        cursor is past the closing `]`.
///
/// Rows are juxtaposed: no required separator. A comma or `;` between
/// rows is skipped silently (§2.7 allows them freely); the examples
/// use commas for readability.
///
/// Error behavior: partial-parse. A row that fails to parse triggers a
/// synchronize to the next row start.
void parseTableInit(TokenStream& stream,
                    ParserContext& ctx,
                    std::vector<RowAST*>& rows) {
    if (!stream.match(TokenType::ASSIGN)) {
        return;   // caller checked; defensive only
    }

    if (!stream.match(TokenType::LBRACKET)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '[' to open the table's row list, got '",
                           stream.peekValueView(ctx.pool), "'");
        return;
    }

    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACKET) &&
           ctx.canContinue()) {
        // Skip free separators.
        if (stream.match(TokenType::SEMICOLON) ||
            stream.match(TokenType::COMMA)) {
            continue;
        }

        if (!isRowStart(stream.peekType())) {
            ctx.diag.errorAt(
                DiagCode::Syntax_ExpectedRow,
                stream.currentLoc(),
                "expected a row '{ ... }', got '",
                stream.peekValueView(ctx.pool), "'");

            synchronizeUntil(stream, [](TokenType t) {
                return isRowStart(t)
                    || t == TokenType::RBRACKET
                    || t == TokenType::SEMICOLON;
            });
            if (stream.check(TokenType::RBRACKET) || stream.isAtEnd()) break;
            continue;
        }

        const size_t posBefore = stream.getPos();
        RowAST* row = parseRow(stream, ctx);
        if (row != nullptr) {
            rows.push_back(row);
            continue;
        }

        if (stream.getPos() == posBefore) {
            synchronizeUntil(stream, [](TokenType t) {
                return isRowStart(t)
                    || t == TokenType::RBRACKET
                    || t == TokenType::SEMICOLON;
            });
            if (stream.check(TokenType::RBRACKET) || stream.isAtEnd()) break;
        }
    }

    if (!stream.match(TokenType::RBRACKET)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ']' to close the table's row list, got '",
                           stream.peekValueView(ctx.pool), "'");
    }
}

} // namespace

// =============================================================================
// parseTableDecl — `TABLE X { ... } [= [ rows ]]` or
//                  `TABLE X = host("name")`
// =============================================================================

TableDeclAST* parseTableDecl(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_TABLE)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                            "expected 'TABLE', got '",
                            stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    // ─── Name ─────────────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected a table name, got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;   // no name, no table
    }
    Token nameTok = stream.consume();
    InternedString name = nameTok.value;

    auto* table = ctx.arena.make<TableDeclAST>(name);
    table->loc = loc;

    // ─── Body or host target ──────────────────────────────────────────────
    //
    // Two shapes after the name:
    //   `TABLE X { ... } [= [ rows ]]`   a columned table, optionally
    //                                     with an inline row list
    //   `TABLE X = host("name")`          a host-backed table
    if (stream.check(TokenType::LBRACE)) {
        // Columned table.
        std::vector<ColumnDeclAST*> columns;
        parseTableBody(stream, ctx, columns);

        // Optional `= [ rows ]` initializer.
        std::vector<RowAST*> rows;
        if (stream.check(TokenType::ASSIGN)) {
            parseTableInit(stream, ctx, rows);
        }

        // Build the column span.
        if (!columns.empty()) {
            auto colBuilder =
                ctx.arena.makeBuilder<ColumnDeclAST*>(columns.size());
            for (ColumnDeclAST* c : columns) colBuilder.push_back(c);
            table->columns = colBuilder.build();
        }

        // Build the row span.
        if (!rows.empty()) {
            auto rowBuilder = ctx.arena.makeBuilder<RowAST*>(rows.size());
            for (RowAST* r : rows) rowBuilder.push_back(r);
            table->rows = rowBuilder.build();
        }

        return table;
    }

    if (stream.check(TokenType::ASSIGN)) {
        // Host-backed table: `TABLE X = host("name")`.
        stream.consume();   // `=`

        InternedString hostName;
        if (!parseHostTarget(stream, ctx, hostName)) {
            table->hasSyntaxError = true;
            return table;   // partial-parse: name survive
        }

        table->isHostBacked = true;
        table->hostName = hostName;
        return table;
    }

    // Neither `{` nor `=`: the table has no body and no target.
    ctx.diag.errorAt(
        DiagCode::Syntax_ExpectedTableBody,
        stream.currentLoc(),
        "expected '{' or '=' after the table name, got '",
        stream.peekValueView(ctx.pool), "'");

    // Synchronize to the next declaration.
    synchronizeUntilDepth(stream, isTopLevelRecoveryStop);

    table->hasSyntaxError = true;
    return table;   // partial-parse: the name survive
}

// =============================================================================
// parseColumnDecl — `[attrs] name: type`
// =============================================================================

ColumnDeclAST* parseColumnDecl(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    // A column may carry attributes. Unlike a table or a function, whose
    // attributes are read by parseDecl, a column's attributes are read
    // here: the table-body loop dispatches on `@` directly to
    // parseColumnDecl.
    ArenaSpan<AttributeAST*> attrs = parseAttributes(stream, ctx);

    // ─── Name ─────────────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected a column name, got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;   // no name, no column
    }
    Token nameTok = stream.consume();
    InternedString name = nameTok.value;

    // ─── `:` ──────────────────────────────────────────────────────────────
    if (!stream.match(TokenType::COLON)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ':' after column name '",
                           ctx.pool.lookupView(name), "', got '",
                           stream.peekValueView(ctx.pool), "'");

        // Partial-parse: produce the column with an unknown type.
        auto* unkType = ctx.arena.make<UnknownTypeAST>();
        unkType->loc = stream.currentLoc();
        unkType->hasSyntaxError = true;

        auto* column = ctx.arena.make<ColumnDeclAST>(name, unkType);
        column->loc = loc;
        column->attributes = attrs;
        column->hasSyntaxError = true;
        return column;
    }

    // ─── Type ─────────────────────────────────────────────────────────────
    TypeAST* type = parseType(stream, ctx);
    if (type == nullptr) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedType,
                           stream.currentLoc(),
                           "expected a type for column '",
                           ctx.pool.lookupView(name), "', got '",
                           stream.peekValueView(ctx.pool), "'");

        type = ctx.arena.make<UnknownTypeAST>();
        type->loc = stream.currentLoc();
        type->hasSyntaxError = true;
    }

    // No `;`: columns are juxtaposed inside a table body.

    auto* column = ctx.arena.make<ColumnDeclAST>(name, type);
    column->loc = loc;
    column->attributes = attrs;
    if (type->hasSyntaxError) column->hasSyntaxError = true;
    return column;
}

// =============================================================================
// parseRow — `{ expr, expr, ... }`
// =============================================================================

RowAST* parseRow(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::LBRACE)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected '{' to open a row, got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;   // no row at all
    }

    std::vector<ExprAST*> cells;

    // Empty row: `{}`.
    if (stream.match(TokenType::RBRACE)) {
        auto* row = ctx.arena.make<RowAST>();
        row->cells = ctx.arena.makeBuilder<ExprAST*>().build();
        return row;
    }

    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE) &&
           ctx.canContinue()) {
        ExprAST* cell = parseRequiredExpr(stream, ctx, "row cell expression");
        cells.push_back(cell);

        if (stream.match(TokenType::COMMA)) {
            if (stream.check(TokenType::RBRACE)) {
                ctx.diag.errorAt(DiagCode::Syntax_TrailingComma,
                                   stream.currentLoc(),
                                   "trailing comma in row");
                break;
            }
            continue;
        }
        if (stream.check(TokenType::RBRACE)) {
            break;
        }

        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ',' or '}' in row, got '",
                           stream.peekValueView(ctx.pool), "'");

        synchronizeUntil(stream, [](TokenType t) {
            return t == TokenType::COMMA
                || t == TokenType::RBRACE;
        });
        if (stream.match(TokenType::COMMA)) continue;
        break;
    }

    if (!stream.match(TokenType::RBRACE)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '}' to close the row, got '",
                           stream.peekValueView(ctx.pool), "'");
    }

    auto* row = ctx.arena.make<RowAST>();
    if (!cells.empty()) {
        auto builder = ctx.arena.makeBuilder<ExprAST*>(cells.size());
        for (ExprAST* c : cells) builder.push_back(c);
        row->cells = builder.build();
    }
    return row;
}

// =============================================================================
// parseFnDecl — `FN name(params) -> Ret { ... }` or
//               `FN name(params) -> Ret = host("name")`
// =============================================================================

FnDeclAST* parseFnDecl(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_FN)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'FN', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    // ─── Name ─────────────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected a function name, got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;   // no name, no function
    }
    Token nameTok = stream.consume();
    InternedString name = nameTok.value;

    auto* fn = ctx.arena.make<FnDeclAST>(name);
    fn->loc = loc;

    // ─── Parameters ───────────────────────────────────────────────────────
    if (!stream.match(TokenType::LPAREN)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '(' for the parameter list, got '",
                           stream.peekValueView(ctx.pool), "'");

        // Synchronize to the body or the next declaration.
        synchronizeUntilDepth(stream, isFunctionDeclRecoveryStop);

        fn->hasSyntaxError = true;
    } else {
        std::vector<ParamAST*> params = parseParamList(stream, ctx);
        if (!params.empty()) {
            auto builder = ctx.arena.makeBuilder<ParamAST*>(params.size());
            for (ParamAST* p : params) builder.push_back(p);
            fn->params = builder.build();
        }
    }

    // ─── Return type (mandatory) ──────────────────────────────────────────
    //
    // Every FN writes `-> T` explicitly. A function that returns nothing
    // writes `-> void`.
    if (!stream.match(TokenType::ARROW)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '->' and a return type after the "
                           "parameter list, got '",
                           stream.peekValueView(ctx.pool), "'");

        auto* unk = ctx.arena.make<UnknownTypeAST>();
        unk->loc = stream.currentLoc();
        unk->hasSyntaxError = true;
        fn->returnType = unk;
        fn->hasSyntaxError = true;
    } else {
        TypeAST* ret = parseType(stream, ctx);
        if (ret == nullptr) {
            ctx.diag.errorAt(DiagCode::Syntax_ExpectedType,
                               stream.currentLoc(),
                               "expected a return type after '->', got '",
                               stream.peekValueView(ctx.pool), "'");

            ret = ctx.arena.make<UnknownTypeAST>();
            ret->loc = stream.currentLoc();
            ret->hasSyntaxError = true;
            fn->hasSyntaxError = true;
        }
        fn->returnType = ret;
    }

    // ─── Body: `{ ... }` or `= host("name")` ─────────────────────────────
    if (stream.check(TokenType::LBRACE)) {
        BlockStmtAST* body = parseBlock(stream, ctx);
        fn->body = body;
        if (body != nullptr && body->hasSyntaxError) {
            fn->hasSyntaxError = true;
        }
        return fn;
    }

    if (stream.match(TokenType::ASSIGN)) {
        InternedString hostName;
        if (!parseHostTarget(stream, ctx, hostName)) {
            fn->hasSyntaxError = true;
            return fn;   // partial-parse: the signature survives
        }
        fn->isHostBound = true;
        fn->hostName = hostName;
        return fn;
    }

    // Neither `{` nor `=`: no body.
    ctx.diag.errorAt(
        DiagCode::Syntax_ExpectedBlock,
        stream.currentLoc(),
        "expected '{' or '=' for the function body, got '",
        stream.peekValueView(ctx.pool), "'");

    fn->hasSyntaxError = true;
    return fn;   // partial-parse: the signature survives
}

// =============================================================================
// parseVarDecl — `let x: T = expr;` / `const x: T = expr;`
// =============================================================================

VarDeclAST* parseVarDecl(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    // ─── Keyword: `let` or `const` ────────────────────────────────────────
    bool isConst = false;
    if (stream.match(TokenType::KW_CONST)) {
        isConst = true;
    } else if (!stream.match(TokenType::KW_LET)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'let' or 'const', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    // ─── Name ─────────────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected a variable name, got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;   // no name, no variable
    }
    Token nameTok = stream.consume();
    InternedString name = nameTok.value;

    auto* decl = ctx.arena.make<VarDeclAST>(
        name, /*type=*/nullptr, isConst, /*init=*/nullptr);
    decl->loc = loc;

    // ─── Type annotation ──────────────────────────────────────────────────
    //
    // The type annotation is required.
    if (!stream.match(TokenType::COLON)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ':' after variable name '",
                           ctx.pool.lookupView(name), "', got '",
                           stream.peekValueView(ctx.pool), "'");
        decl->hasSyntaxError = true;
    } else {
        TypeAST* type = parseType(stream, ctx);
        if (type == nullptr) {
            ctx.diag.errorAt(DiagCode::Syntax_ExpectedType,
                               stream.currentLoc(),
                               "expected a type for variable '",
                               ctx.pool.lookupView(name), "', got '",
                               stream.peekValueView(ctx.pool), "'");

            type = ctx.arena.make<UnknownTypeAST>();
            type->loc = stream.currentLoc();
            type->hasSyntaxError = true;
        }
        decl->type = type;
        if (type->hasSyntaxError) decl->hasSyntaxError = true;
    }

    // ─── Initializer ──────────────────────────────────────────────────────
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '=' for the initializer of '",
                           ctx.pool.lookupView(name), "', got '",
                           stream.peekValueView(ctx.pool), "'");
        decl->hasSyntaxError = true;
    } else {
        ExprAST* init = parseRequiredExpr(stream, ctx, "initializer");
        decl->init = init;
        if (init != nullptr && init->hasSyntaxError) {
            decl->hasSyntaxError = true;
        }
    }

    return decl;
}

// =============================================================================
// parseParam — `[const] name: type` or `[const] name: ...type`
// =============================================================================

ParamAST* parseParam(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    // ─── Optional `const` ─────────────────────────────────────────────────
    bool isConst = stream.match(TokenType::KW_CONST);

    // ─── Name ─────────────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected a parameter name, got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;   // no name, no parameter
    }
    Token nameTok = stream.consume();
    InternedString name = nameTok.value;

    // ─── `:` ──────────────────────────────────────────────────────────────
    if (!stream.match(TokenType::COLON)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ':' after parameter name '",
                           ctx.pool.lookupView(name), "', got '",
                           stream.peekValueView(ctx.pool), "'");

        auto* unkType = ctx.arena.make<UnknownTypeAST>();
        unkType->loc = stream.currentLoc();
        unkType->hasSyntaxError = true;

        auto* param = ctx.arena.make<ParamAST>(
            name, unkType, /*isVariadic=*/false, isConst);
        param->loc = loc;
        param->hasSyntaxError = true;
        return param;
    }

    // ─── Optional `...` before the type ───────────────────────────────────
    //
    // §4.2.2: "The last parameter may be variadic: `...type` collects zero
    // or more trailing arguments into a `[type]` array." The grammar
    // writes this as `name: ...type`.
    bool isVariadic = stream.match(TokenType::VARIADIC);

    // ─── Type ─────────────────────────────────────────────────────────────
    TypeAST* type = parseType(stream, ctx);
    if (type == nullptr) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedType,
                           stream.currentLoc(),
                           "expected a type for parameter '",
                           ctx.pool.lookupView(name), "', got '",
                           stream.peekValueView(ctx.pool), "'");

        type = ctx.arena.make<UnknownTypeAST>();
        type->loc = stream.currentLoc();
        type->hasSyntaxError = true;
    }

    // For a variadic parameter, ParamAST stores the element type and the
    // isVariadic flag. Sema synthesizes the `[T]` array type; the parser
    // does not.
    auto* param = ctx.arena.make<ParamAST>(
        name, type, isVariadic, isConst);
    param->loc = loc;
    if (type->hasSyntaxError) param->hasSyntaxError = true;
    return param;
}

// =============================================================================
// parseHostTarget — `host("name")`
// =============================================================================

bool parseHostTarget(TokenStream& stream,
                     ParserContext& ctx,
                     InternedString& targetName) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_HOST)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedHostTarget, loc,
                           "expected 'host', got '",
                           stream.peekValueView(ctx.pool), "'");
        return false;
    }

    if (!stream.match(TokenType::LPAREN)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '(' after 'host', got '",
                           stream.peekValueView(ctx.pool), "'");
        return false;
    }

    // The target name is a string literal. A raw string is not accepted:
    // a host target is an identifier-like token, not a block of text.
    if (!stream.check(TokenType::STRING_LITERAL)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedLiteral,
                           stream.currentLoc(),
                           "expected a string literal for the host target, "
                           "got '",
                           stream.peekValueView(ctx.pool), "'");
        return false;
    }

    Token nameTok = stream.consume();
    targetName = nameTok.value;

    if (!stream.match(TokenType::RPAREN)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ')' to close the host target, got '",
                           stream.peekValueView(ctx.pool), "'");
        return false;
    }

    return true;
}

} // namespace lucid::parser