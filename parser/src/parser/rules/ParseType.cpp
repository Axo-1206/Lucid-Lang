/**
 * @file ParseType.cpp
 * @brief The type parsers.
 *
 * ─── What this file implements ────────────────────────────────────────────
 *   - parseType            the entry point; dispatches on the leading token
 *   - parsePrimitiveType   `int`, `float`, `bool`, `string`, `char`, `void`
 *   - parseNamedType       `Person`, `alias.Person`, `SpriteRef`
 *   - parseArrayType       `[T]` (dynamic) or `[N, T]` (fixed-size)
 *   - parseRowRefType      `&T`
 *   - parseFunctionType    `(T, U) -> R`
 *
 * ─── Design: five type forms ──────────────────────────────────────────────
 * §5's `type` production has exactly five branches:
 *
 *   primitive_type     a primitive keyword
 *   table_type         a qualified table name (`Person`, `alias.Person`)
 *   row_ref_type       `&` followed by a qualified table name
 *   array_type         `[T]` or `[N, T]`
 *   function_type      `(T, U) -> R`
 *
 * parseType dispatches on the leading token; each branch has its own
 * parser below.
 *
 * ─── Design: a `?` suffix ─────────────────────────────────────────────────
 * A type is a base type with an optional `?` suffix (§5.3). The suffix
 * makes the base type nilable: `int?`, `SpriteRef?`, `[int]?`. Nilability
 * is meaningful for primitives, host types, and arrays; it is redundant
 * on `&T` (which is already nilable) and an error on bare table types,
 * function types, and `void`.
 *
 * The parser produces a `NullableTypeAST` wrapping the base type
 * uniformly, without checking whether the suffix is meaningful at that
 * position. Sema reports the error for the cases where it is not.
 *
 * ─── Design: primitive names are keywords ─────────────────────────────────
 * The primitive type names are keywords (§2.2), recognized by the lexer.
 * parseType dispatches to parsePrimitiveType based on the token type,
 * not on identifier text. Aliases (`int` and `int32`, `float` and
 * `float32`) produce the same PrimitiveKind.
 *
 * ─── Design: the module qualifier is one level deep ───────────────────────
 * A `qualified_table` is `[alias '.'] Name` (§5). The parser reads the
 * optional alias and stores it in `NamedTypeAST::qualifier`. It does not
 * resolve the alias; Sema does.
 *
 * ─── Design: fixed-array sizes are evaluated at parse time ────────────────
 * A fixed-size array type is `[N, T]` where `N` is an INT_LITERAL (§5's
 * grammar). Because the grammar restricts the token to a plain decimal
 * integer, the parser converts it to a `uint64_t` and stores it in
 * `ArrayTypeAST::fixedSize`. Sema does not have to re-parse the token.
 *
 * A size that does not fit in `uint64_t` is reported and the node is
 * marked; Sema skips it.
 *
 * ─── Design: no error recovery inside type parsers ────────────────────────
 * Type parsers do not perform their own recovery. When a type cannot be
 * parsed, the parser reports a diagnostic and returns nullptr. The
 * caller — which knows what construct the type was supposed to be part
 * of — decides how to recover.
 *
 * This is a deliberate asymmetry with the expression parsers. An
 * expression can be replaced by an UnknownExprAST and the parse can
 * continue; a type is a smaller piece of a larger construct, and the
 * larger construct is the right place to decide whether to continue
 * with an unknown type or skip the whole construct.
 */

#include "parser/Parser.hpp"

#include "core/Tokens.hpp"
#include "core/ast/TypeAST.hpp"

#include <charconv>
#include <string_view>

using namespace lucid::diag;

namespace lucid::parser {

// =============================================================================
// parseBaseType — the dispatcher, file-local
// =============================================================================

namespace {

/// @brief Parse a type's base form, before any `?` suffix.
///
/// Dispatches on the leading token:
///
///   - a primitive keyword       → parsePrimitiveType
///   - IDENTIFIER                → parseNamedType
///   - `&`                       → parseRowRefType
///   - `[`                       → parseArrayType
///   - `(`                       → parseFunctionType
///   - anything else             → error
///
/// The `?` suffix is handled by the caller (`parseType`), not here.
/// This function returns the base type without any nilability wrapper.
///
/// File-local because it has no external caller: `parseType` is the
/// only entry point for types, and every recursive type parse goes
/// through `parseType`.
TypeAST* parseBaseType(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();
    const TokenType current = stream.peekType();

    // ─── Primitive type ───────────────────────────────────────────────────
    //
    // The primitive keywords are their own token category. A primitive is
    // not an identifier; `int` is `KW_INT`, never an `IDENTIFIER` whose
    // text happens to be "int".
    if (isPrimitiveTypeKeyword(current)) {
        return parsePrimitiveType(stream, ctx);
    }

    // ─── Other forms ──────────────────────────────────────────────────────
    switch (current) {
        case TokenType::IDENTIFIER:
            return parseNamedType(stream, ctx);

        case TokenType::BIT_AND:
            return parseRowRefType(stream, ctx);

        case TokenType::LBRACKET:
            return parseArrayType(stream, ctx);

        case TokenType::LPAREN:
            return parseFunctionType(stream, ctx);

        default:
            ctx.diag.errorAt(DiagCode::Syntax_ExpectedType, loc,
                               "expected a type, got '",
                               stream.peekValueView(ctx.pool), "'");
            return nullptr;
    }
}

} // namespace

// =============================================================================
// parseType — the entry point
// =============================================================================

/// @brief Parse a complete type: a base type, then an optional `?` suffix.
///
/// §5's grammar: `type ::= base_type [ '?' ]`.
///
/// The `?` suffix makes the type nilable. It is only meaningful on
/// primitives, host types, and arrays; on a row reference it is
/// redundant (accepted but ignored by Sema); on a bare table type or a
/// function type it is a type error (Sema reports it). The parser
/// produces the `NullableTypeAST` wrapper uniformly and lets Sema
/// enforce the applicability rules.
///
/// The recursion into `parseBaseType` handles the four base forms
/// (primitive, named, array, row-ref, function). The `?` check runs
/// after the base type is complete.
///
/// On failure, reports a diagnostic and returns nullptr.
TypeAST* parseType(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    TypeAST* base = parseBaseType(stream, ctx);
    if (base == nullptr) {
        return nullptr;
    }

    // Optional `?` suffix.
    if (stream.match(TokenType::QUESTION)) {
        auto* nullable = ctx.arena.make<NullableTypeAST>(base);
        nullable->loc = loc;
        return nullable;
    }

    return base;
}

// =============================================================================
// parsePrimitiveType
// =============================================================================

PrimitiveTypeAST* parsePrimitiveType(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();
    const TokenType current = stream.peekType();

    PrimitiveKind kind;
    switch (current) {
        case TokenType::KW_BOOL:    kind = PrimitiveKind::Bool;    break;
        case TokenType::KW_CHAR:    kind = PrimitiveKind::Char;    break;
        case TokenType::KW_STRING:  kind = PrimitiveKind::String;  break;
        case TokenType::KW_VOID:    kind = PrimitiveKind::Void;    break;

        case TokenType::KW_INT8:    kind = PrimitiveKind::Int8;    break;
        case TokenType::KW_INT16:   kind = PrimitiveKind::Int16;   break;
        case TokenType::KW_INT32:   kind = PrimitiveKind::Int32;   break;
        case TokenType::KW_INT64:   kind = PrimitiveKind::Int64;   break;
        case TokenType::KW_INT:     kind = PrimitiveKind::Int32;   break;
        case TokenType::KW_LONG:    kind = PrimitiveKind::Int64;   break;

        case TokenType::KW_UINT8:   kind = PrimitiveKind::Uint8;   break;
        case TokenType::KW_UINT16:  kind = PrimitiveKind::Uint16;  break;
        case TokenType::KW_UINT32:  kind = PrimitiveKind::Uint32;  break;
        case TokenType::KW_UINT64:  kind = PrimitiveKind::Uint64;  break;
        case TokenType::KW_UINT:    kind = PrimitiveKind::Uint32;  break;
        case TokenType::KW_ULONG:   kind = PrimitiveKind::Uint64;  break;

        case TokenType::KW_FLOAT32: kind = PrimitiveKind::Float32; break;
        case TokenType::KW_FLOAT64: kind = PrimitiveKind::Float64; break;
        case TokenType::KW_FLOAT:   kind = PrimitiveKind::Float32; break;
        case TokenType::KW_DOUBLE:  kind = PrimitiveKind::Float64; break;

        default:
            // The caller (parseType) checked isPrimitiveTypeKeyword
            // before dispatching, so this is unreachable.
            ctx.diag.errorAt(DiagCode::Syntax_ExpectedType, loc,
                               "expected a primitive type name, got '",
                               stream.peekValueView(ctx.pool), "'");
            return nullptr;
    }

    stream.consume();

    auto* prim = ctx.arena.make<PrimitiveTypeAST>(kind);
    prim->loc = loc;
    return prim;
}

// =============================================================================
// parseNamedType — `Person`, `alias.Person`, `SpriteRef`
// =============================================================================

NamedTypeAST* parseNamedType(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedIdentifier, loc,
                           "expected a type name, got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    Token firstTok = stream.consume();
    InternedString firstName = firstTok.value;

    // ─── Module qualifier: `alias.Name` ───────────────────────────────────
    //
    // A `qualified_table` is `[alias '.'] Name` (§5). One level of
    // qualification: an alias, a dot, a name. The alias is the local name
    // the source wrote (from an `import ... as alias`); the parser does
    // not resolve it.
    if (stream.check(TokenType::DOT)) {
        stream.consume();   // `.`

        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diag.errorAt(DiagCode::Syntax_ExpectedIdentifier,
                               stream.currentLoc(),
                               "expected a type name after '.', got '",
                               stream.peekValueView(ctx.pool), "'");

            // Partial-parse: keep the first identifier as the name, mark
            // the node. The qualifier is left invalid.
            auto* nt = ctx.arena.make<NamedTypeAST>(firstName);
            nt->loc = loc;
            nt->hasSyntaxError = true;
            return nt;
        }

        Token secondTok = stream.consume();
        InternedString secondName = secondTok.value;

        auto* nt = ctx.arena.make<NamedTypeAST>(secondName);
        nt->loc = loc;
        nt->qualifier = firstName;
        return nt;
    }

    // ─── Unqualified ──────────────────────────────────────────────────────
    auto* nt = ctx.arena.make<NamedTypeAST>(firstName);
    nt->loc = loc;
    return nt;
}

// =============================================================================
// parseArrayType — `[T]` (dynamic) or `[N, T]` (fixed-size)
// =============================================================================

ArrayTypeAST* parseArrayType(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::LBRACKET)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected '[', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    // ─── Fixed-size form: `[N, T]` ────────────────────────────────────────
    //
    // The size slot holds a plain decimal integer literal, followed by a
    // comma, followed by the element type. The grammar restricts the size
    // token to INT_LITERAL (§5), so the parser converts the lexeme to a
    // uint64_t directly.
    if (stream.check(TokenType::INT_LITERAL)) {
        Token sizeTok = stream.consume();
        const std::string_view lexeme = ctx.pool.lookupView(sizeTok.value);

        uint64_t fixedSize = 0;
        const auto [ptr, ec] = std::from_chars(
            lexeme.data(), lexeme.data() + lexeme.size(), fixedSize);

        bool sizeIsValid = (ec == std::errc{}) &&
                           (ptr == lexeme.data() + lexeme.size());

        if (!sizeIsValid) {
            ctx.diag.errorAt(DiagCode::Syntax_ExpectedLiteral,
                               sizeTok.location,
                               "array size is not a valid unsigned "
                               "integer");
            fixedSize = 0;
        }

        // `,` between the size and the element type.
        if (!stream.match(TokenType::COMMA)) {
            ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                               stream.currentLoc(),
                               "expected ',' after the array size, got '",
                               stream.peekValueView(ctx.pool), "'");
            sizeIsValid = false;
        }

        // Element type.
        TypeAST* element = parseType(stream, ctx);
        if (element == nullptr) {
            ctx.diag.errorAt(DiagCode::Syntax_ExpectedType,
                               stream.currentLoc(),
                               "expected an element type after ',', got '",
                               stream.peekValueView(ctx.pool), "'");
            element = ctx.arena.make<UnknownTypeAST>();
            element->loc = stream.currentLoc();
            element->hasSyntaxError = true;
        }

        // Closing `]`.
        if (!stream.match(TokenType::RBRACKET)) {
            ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                               stream.currentLoc(),
                               "expected ']' to close the array type, got '",
                               stream.peekValueView(ctx.pool), "'");
            sizeIsValid = false;
        }

        auto* arr = ctx.arena.make<ArrayTypeAST>(
            ArrayKind::Fixed, fixedSize, element);
        arr->loc = loc;
        if (!sizeIsValid || element->hasSyntaxError) {
            arr->hasSyntaxError = true;
        }
        return arr;
    }

    // ─── Dynamic form: `[T]` ──────────────────────────────────────────────
    //
    // The element type is inside the brackets: `[int]`, `[string]`,
    // `[&Person]`.
    TypeAST* element = parseType(stream, ctx);
    if (element == nullptr) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedType,
                           stream.currentLoc(),
                           "expected an element type inside '[...]', got '",
                           stream.peekValueView(ctx.pool), "'");
        element = ctx.arena.make<UnknownTypeAST>();
        element->loc = stream.currentLoc();
        element->hasSyntaxError = true;
    }

    if (!stream.match(TokenType::RBRACKET)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ']' to close the array type, got '",
                           stream.peekValueView(ctx.pool), "'");
    }

    auto* arr = ctx.arena.make<ArrayTypeAST>(
        ArrayKind::Dynamic, /*fixedSize=*/0, element);
    arr->loc = loc;
    if (element->hasSyntaxError) arr->hasSyntaxError = true;
    return arr;
}

// =============================================================================
// parseRowRefType — `&T`
// =============================================================================

RowRefTypeAST* parseRowRefType(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::BIT_AND)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected '&', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    // The referent is a qualified table name, not a full type. §5's
    // grammar: `row_ref_type ::= '&' qualified_table`.
    NamedTypeAST* inner = parseNamedType(stream, ctx);
    if (inner == nullptr) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedType,
                           stream.currentLoc(),
                           "expected a table name after '&', got '",
                           stream.peekValueView(ctx.pool), "'");

        // Partial-parse: an inner named type with a placeholder name.
        // Actually, we need a TypeAST for `RowRefTypeAST::inner`. If we
        // have no name, we can't build a NamedTypeAST. Use an
        // UnknownTypeAST.
        auto* unk = ctx.arena.make<UnknownTypeAST>();
        unk->loc = stream.currentLoc();
        unk->hasSyntaxError = true;

        auto* ref = ctx.arena.make<RowRefTypeAST>(unk);
        ref->loc = loc;
        ref->hasSyntaxError = true;
        return ref;
    }

    auto* ref = ctx.arena.make<RowRefTypeAST>(inner);
    ref->loc = loc;
    if (inner->hasSyntaxError) ref->hasSyntaxError = true;
    return ref;
}

// =============================================================================
// parseFunctionType — `(T, U) -> R`
// =============================================================================

FunctionTypeAST* parseFunctionType(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::LPAREN)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected '(' to open the function type's "
                           "parameters, got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    // parseFunctionTypeParamList consumes the whole `( T, U, ... )` group.
    // Its parameters are unnamed types (a function type names a signature,
    // not a binding).
    ArenaSpan<TypeAST*> params = parseFunctionTypeParamList(stream, ctx);

    auto* ft = ctx.arena.make<FunctionTypeAST>();
    ft->loc = loc;
    ft->params = params;

    // ─── `->` and the return type ─────────────────────────────────────────
    //
    // The arrow is required: a function type is `(T, U) -> R`, and there
    // is no default return. A bare `(T, U)` is not a function type.
    if (!stream.match(TokenType::ARROW)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '->' after the function type's "
                           "parameters, got '",
                           stream.peekValueView(ctx.pool), "'");
        ft->hasSyntaxError = true;
        return ft;
    }

    TypeAST* ret = parseType(stream, ctx);
    if (ret == nullptr) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedType,
                           stream.currentLoc(),
                           "expected a return type after '->', got '",
                           stream.peekValueView(ctx.pool), "'");
        ft->hasSyntaxError = true;
        return ft;
    }

    ft->returnType = ret;
    if (ret->hasSyntaxError) ft->hasSyntaxError = true;
    return ft;
}

} // namespace lucid::parser