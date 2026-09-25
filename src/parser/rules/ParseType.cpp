/**
 * @file ParseType.cpp
 * @brief The type parsers.
 *
 * ─── What this file implements ────────────────────────────────────────────
 *   - parseType                the entry point; parses a base type and its
 *                              `?`/`!`/`?!` suffixes
 *   - parseBaseType            dispatches on the leading token
 *   - parseNamedType           `Vec2`, `Map<K, V>`, `mod::Type`
 *   - parseArrayType           `[*]T`, `[_]T`, `[N]T`
 *   - parseRefType             `&T`
 *   - parseTypeWithQualifier   `T?`, `T!`, `T?!`
 *   - parseFuncType            `fn (...) -> ...`
 *
 * ─── Design: no error recovery inside type parsers ────────────────────────
 * Under the current grammar's design, type parsers do NOT perform error
 * recovery. When a type cannot be parsed, the parser reports a
 * diagnostic and returns nullptr. The caller — which knows what
 * construct the type was supposed to be part of — decides how to
 * recover.
 *
 * This is a deliberate asymmetry with the expression parsers. Expressions
 * appear in many contexts and the parser is willing to invent a
 * placeholder expression to keep going. Types appear in fewer contexts,
 * each with a specific shape (a parameter, a field, a return type, a
 * generic argument), and each context has its own recovery strategy.
 * Pushing recovery up to the caller keeps the type parsers simple.
 *
 * The one exception is `parseTypeWithQualifier`: it consumes trailing
 * `?` and `!` tokens unconditionally. A type followed by a suffix is
 * always well-formed at the type level; whether the suffixed type is
 * legal in its context is a Sema concern.
 *
 * ─── Design: no primitive-type special case ───────────────────────────────
 * Under the clean-model design, primitive type names (`int`, `float`,
 * `bool`, `string`, `char`, `byte`, and their sized variants) are ordinary
 * identifiers. They resolve to core-script `TYPE` declarations via Sema.
 * The parser produces a `NamedTypeAST` for them, just as it does for any
 * user-defined type name.
 *
 * There is no `parsePrimitiveType` and no `PrimitiveTypeAST` produced at
 * parse time. Sema converts a `NamedTypeAST` that resolves to a primitive
 * declaration into whatever internal representation it uses.
 */

#include "parser/Parser.hpp"
#include "core/Tokens.hpp"
#include "core/ast/TypeAST.hpp"

using namespace lucid::diag;

namespace lucid::parser {

// =============================================================================
// parseType — the entry point
// =============================================================================

/// @brief Parse a complete type, including any `?`/`!`/`?!` suffixes.
///
/// `parseType` is the parser's single entry point for types. Every caller
/// that expects a type in the source (a parameter, a field, a return
/// type, a generic argument, a local binding's annotation) calls this
/// function. The two-phase structure — base type, then suffix — mirrors
/// the grammar's `type` production:
///
///   type := base_type [ '?' | '!' | '?!' ]
///
/// The base type's form is determined by the leading token:
///
///   - `IDENTIFIER`               → `parseNamedType`
///   - `[`                        → `parseArrayType`
///   - `&`                        → `parseRefType`
///   - `fn`                       → `parseFuncType`
///
/// Anything else is a syntax error: the caller expected a type, and
/// nothing in the token stream can start one.
///
/// On failure, reports a diagnostic and returns nullptr. On success,
/// returns a `TypeAST*` whose `loc` is the start of the type.
TypeAST* parseType(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    // ─── Base type ────────────────────────────────────────────────────────
    TypeAST* base = parseBaseType(stream, ctx);
    if (!base) {
        // parseBaseType reports its own error. The caller decides how
        // to recover; this function returns nullptr.
        return nullptr;
    }
    base->loc = loc;

    // ─── Suffix: `?`, `!`, or `?!` ────────────────────────────────────────
    //
    // The suffix parser consumes any trailing qualifier and returns the
    // (possibly wrapped) type. If no suffix is present, it returns the
    // base type unchanged.
    return parseTypeWithQualifier(stream, ctx, base);
}

// =============================================================================
// parseBaseType — dispatch on the leading token
// =============================================================================

/// @brief Parse a type's base form, before any `?`/`!` suffixes.
///
/// Dispatches on the leading token:
///
///   - `IDENTIFIER`               → a named type (possibly qualified or
///                                  generic): `Vec2`, `Map<K, V>`,
///                                  `mod::Type`
///   - `[`                        → an array type: `[*]T`, `[_]T`, `[N]T`
///   - `&`                        → a reference type: `&T`
///   - `fn`                       → a function type: `fn (...) -> ...`
///
/// On failure, reports a diagnostic and returns nullptr.
TypeAST* parseBaseType(TokenStream& stream, ParserContext& ctx) {
    const TokenType current = stream.peekType();

    // ─── Function type: `fn (...)` ────────────────────────────────────────
    if (current == TokenType::KW_FN_MARKER) {
        return parseFuncType(stream, ctx);
    }

    // ─── Array: `[` ───────────────────────────────────────────────────────
    if (current == TokenType::LBRACKET) {
        return parseArrayType(stream, ctx);
    }

    // ─── Reference: `&` ───────────────────────────────────────────────────
    if (current == TokenType::BIT_AND) {
        return parseRefType(stream, ctx);
    }

    // ─── Named type: an identifier ────────────────────────────────────────
    //
    // Under the clean-model design, primitive type names are identifiers
    // and go through this branch. There is no separate primitive-type
    // branch.
    if (current == TokenType::IDENTIFIER) {
        return parseNamedType(stream, ctx);
    }

    // ─── Not a type ───────────────────────────────────────────────────────
    ctx.diag().errorAt(DiagCode::Syntax_ExpectedType,
                       stream.currentLoc(),
                       "expected a type, got '", stream.peekValue(), "'");
    return nullptr;
}

// =============================================================================
// parseNamedType — `Vec2`, `Map<K, V>`, `mod::Type`
// =============================================================================

/// @brief Parse a named type, with optional module qualification and
///        optional generic arguments.
///
/// The three forms:
///
///   `Vec2`              a plain name
///   `Map<K, V>`         a name with generic arguments
///   `mod::Type`         a module-qualified name
///   `mod::Type<K, V>`   a module-qualified name with generic arguments
///
/// The parser does not resolve the name; it produces a `NamedTypeAST`
/// with the name's interned identifier and, if present, the generic
/// arguments. Sema resolves the name against the type namespace.
///
/// The module qualification is stored as a single `NamedTypeAST` — the
/// grammar's `NamedTypeAST` has a `name` field but no separate module
/// field. A module-qualified type `mod::Type` produces a `NamedTypeAST`
/// whose name is the last segment (`Type`) and whose `qualifier` field,
/// if the AST has one, is the module name. See the note at the end of
/// this file about how the AST currently represents module qualification.
TypeAST* parseNamedType(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           loc,
                           "expected a type name, got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    Token firstTok = stream.consume();
    InternedString firstName = ctx.pool().intern(firstTok.value);

    // ─── Module qualification: `mod::Type` ────────────────────────────────
    //
    // The `::` operator separates the module name from the type name.
    if (stream.match(TokenType::DOUBLE_COLON)) {
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                               stream.currentLoc(),
                               "expected a type name after '::', got '",
                               stream.peekValue(), "'");
            return nullptr;
        }
        Token secondTok = stream.consume();
        InternedString typeName = ctx.pool().intern(secondTok.value);

        // Generic arguments, if any.
        ArenaSpan<TypeAST*> genericArgs;
        if (stream.check(TokenType::LESS)) {
            genericArgs = parseGenericArgs(stream, ctx);
        }

        // The AST represents a module-qualified type. See the note at
        // the end of this file for the current representation.
        auto* qualified = ctx.arena().make<NamedTypeAST>(typeName);
        qualified->loc = loc;
        qualified->genericArgs = genericArgs;
        // (If NamedTypeAST gains a `qualifier` field, set it here.)
        return qualified;
    }

    // ─── Unqualified: `Type` or `Type<Args>` ──────────────────────────────
    ArenaSpan<TypeAST*> genericArgs;
    if (stream.check(TokenType::LESS)) {
        genericArgs = parseGenericArgs(stream, ctx);
    }

    auto* named = ctx.arena().make<NamedTypeAST>(firstName);
    named->loc = loc;
    named->genericArgs = genericArgs;
    return named;
}

// =============================================================================
// parseArrayType — `[*]T`, `[_]T`, `[N]T`
// =============================================================================

/// @brief Parse an array type.
///
/// The three array kinds differ in the size slot inside the brackets:
///
///   `[*]T`   dynamic array; the slot is `*`
///   `[_]T`   slice; the slot is `_`
///   `[N]T`   fixed array; the slot is an integer literal
///
/// The element type follows the closing bracket and is parsed
/// recursively.
///
/// The grammar's `array_type` production does not allow `?` or `!` on
/// the array type itself; the suffixes apply to the *element* type.
/// `[*]int?` is an array of nullable ints, not a nullable array. The
/// recursion into `parseType` for the element handles this: the element
/// type's own suffix parse consumes the `?` before the array closes.
///
/// A malformed size slot (`[abc]T`, `[]T`) reports a diagnostic and
/// recovers by producing an array with an unknown element type.
TypeAST* parseArrayType(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::LBRACKET)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected '[', got '", stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Size slot ────────────────────────────────────────────────────────
    ArrayKind arrayKind;
    uint64_t  fixedSize = 0;

    if (stream.match(TokenType::MUL)) {
        arrayKind = ArrayKind::Dynamic;
    } else if (isUnderscoreIdentifier(stream)) {
        stream.consume();
        arrayKind = ArrayKind::Slice;
    } else if (stream.check(TokenType::INT_LITERAL)) {
        Token sizeTok = stream.consume();
        arrayKind = ArrayKind::Fixed;
        // The parser does not convert the lexeme; Sema does. The
        // fixedSize field is left as 0 and Sema fills it in from the
        // lexeme. (If ArrayTypeAST currently stores a uint64_t size,
        // the parser has to convert here — see the note at the end of
        // this file.)
        (void)sizeTok;
    } else {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '*', '_', or an integer literal in "
                           "array size slot, got '", stream.peekValue(), "'");
        // Recover: assume a dynamic array with an unknown element.
        arrayKind = ArrayKind::Dynamic;
    }

    // ─── Closing `]` ──────────────────────────────────────────────────────
    if (!stream.match(TokenType::RBRACKET)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ']' to close array size, got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Element type ─────────────────────────────────────────────────────
    TypeAST* element = parseType(stream, ctx);
    if (!element) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedType,
                           stream.currentLoc(),
                           "expected an array element type after ']', got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    auto* array = ctx.arena().make<ArrayTypeAST>(arrayKind, fixedSize, element);
    array->loc = loc;
    return array;
}

// =============================================================================
// parseRefType — `&T`
// =============================================================================

/// @brief Parse a reference type: `&T`.
///
/// The `&` token is the same in type position and expression position;
/// the parser knows it is in a type position because it was called from
/// `parseBaseType`. It consumes the `&` and parses the referent type
/// recursively.
///
/// A reference may be nullable: `&T?` is a nullable reference, and the
/// `?` is consumed by the referent's own `parseTypeWithQualifier`. The
/// grammar forbids `&T??` (a nullable nullable reference); if the source
/// writes one, the inner `?` is consumed first and the outer `?` is a
/// stray token that the enclosing construct's parser reports.
TypeAST* parseRefType(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::BIT_AND)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           loc,
                           "expected '&', got '", stream.peekValue(), "'");
        return nullptr;
    }

    TypeAST* inner = parseType(stream, ctx);
    if (!inner) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedType,
                           stream.currentLoc(),
                           "expected a referent type after '&', got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    auto* ref = ctx.arena().make<RefTypeAST>(inner);
    ref->loc = loc;
    return ref;
}

// =============================================================================
// parseTypeWithQualifier — `T?`, `T!`, `T?!`
// =============================================================================

/// @brief Consume any trailing `?`, `!`, or `?!` on a type.
///
/// The suffixes are:
///
///   `T?`     nullable
///   `T!`     fallible
///   `T?!`    nullable and fallible
///
/// The `?!` form is a single token (`QUESTION_BANG`) in the lexer; the
/// parser sees it as one token and produces a `CombinedTypeAST`. The
/// `?` and `!` forms are separate tokens; the parser consumes each and
/// produces a `NullableTypeAST` or a `FallibleTypeAST`.
///
/// The order `!?` is a syntax error: `!` followed by `?` is not the
/// combined type. If the parser sees `!` and then `?`, it treats the
/// `?` as a stray token; the enclosing construct's parser reports it.
///
/// This function is idempotent on the absence of a suffix: if no suffix
/// follows, it returns the input type unchanged.
TypeAST* parseTypeWithQualifier(TokenStream& stream,
                                ParserContext& ctx,
                                TypeAST* type) {
    if (!type) return nullptr;

    // The combined form `?!` is a single token. Check it first so it is
    // not mistaken for `?` followed by `!`.
    if (stream.check(TokenType::QUESTION_BANG)) {
        const SourceLocation loc = stream.currentLoc();
        stream.consume();
        auto* combined = ctx.arena().make<CombinedTypeAST>(type);
        combined->loc = loc;
        return combined;
    }

    // `?` alone.
    if (stream.check(TokenType::QUESTION)) {
        const SourceLocation loc = stream.currentLoc();
        stream.consume();
        auto* nullable = ctx.arena().make<NullableTypeAST>(type);
        nullable->loc = loc;
        return nullable;
    }

    // `!` alone.
    if (stream.check(TokenType::BANG)) {
        const SourceLocation loc = stream.currentLoc();
        stream.consume();
        auto* fallible = ctx.arena().make<FallibleTypeAST>(type);
        fallible->loc = loc;
        return fallible;
    }

    // No suffix. Return the base type as-is.
    return type;
}

// =============================================================================
// parseFuncType — `fn (...) -> ...`
// =============================================================================

/// @brief Parse a function type.
///
/// Grammar:
///
///   func_type = stage { '->' stage } [ '->' type ]
///   stage     = 'fn' '(' [ type_list ] ')'
///
/// Every stage carries its own `fn` marker. Adjacent stages without `->`
/// between them are legal only in the leading cluster of a *declaration*
/// header, not in a bare function type. The parser accepts adjacency
/// here for uniformity; Sema enforces where adjacency is permitted.
///
/// The return type may be another function type, producing a curried
/// chain. The parser recurses into `parseType` after the final `->`.
///
/// The result is a `FuncTypeAST` chain. The outermost `FuncTypeAST` is
/// the first stage; its `returnType` is either the final type or the
/// next `FuncTypeAST`. The chain ends at a non-function type or at
/// `nullptr` (a void return).
///
/// ─── Parameter names ──────────────────────────────────────────────────────
/// A bare function type's stages have unnamed parameters (`fn (int)`).
/// The parser passes `allowNames = false` to `parseParamList`. If the
/// source writes names (`fn (x int)`), the parser accepts them and
/// marks the parameters as named; Sema rejects named parameters in
/// bare function types later. This is a shape-first choice: the parser
/// produces the AST it read; Sema enforces the rule.
///
/// ─── Void return ──────────────────────────────────────────────────────────
/// A function type with no `->` has a void return. The parser sets the
/// innermost `FuncTypeAST`'s `returnType` to `nullptr`; Sema treats a
/// null return type as `unit`.
TypeAST* parseFuncType(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    // Collect the stages.
    std::vector<std::vector<ParamAST*>> groups;

    while (!stream.isAtEnd()) {
        // Every stage begins with `fn`.
        if (!stream.check(TokenType::KW_FN_MARKER)) {
            break;
        }
        stream.consume();   // `fn`

        if (!stream.check(TokenType::LPAREN)) {
            ctx.diag().errorAt(DiagCode::Syntax_MissingFuncShapeMarker,
                               stream.currentLoc(),
                               "expected '(' after 'fn', got '",
                               stream.peekValue(), "'");
            // Return whatever we have so far, or nullptr if nothing.
            if (groups.empty()) return nullptr;
            break;
        }

        std::vector<ParamAST*> group = parseParamList(stream, ctx,
                                                      /*allowNames=*/false);
        groups.push_back(std::move(group));

        // After a group, another `fn` means another stage. An `->`
        // introduces a return type (which may itself be a function
        // type, parsed by the recursion below). Anything else ends the
        // type.
        if (stream.check(TokenType::KW_FN_MARKER)) {
            continue;   // adjacent stage
        }
        break;
    }

    if (groups.empty()) {
        ctx.diag().errorAt(DiagCode::Syntax_MissingFuncShapeMarker,
                           stream.currentLoc(),
                           "expected 'fn' before parameter group, got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    // Optional `->` return type.
    TypeAST* returnType = nullptr;
    if (stream.match(TokenType::ARROW)) {
        // The return type is a full type. If it is itself a function
        // type, it starts with `fn` and the recursion in parseType
        // dispatches to parseFuncType.
        returnType = parseType(stream, ctx);
        if (!returnType) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedType,
                               stream.currentLoc(),
                               "expected a return type after '->', got '",
                               stream.peekValue(), "'");
            // Continue with a void return; Sema will treat it as unit.
            returnType = nullptr;
        }
    }

    // ─── Build the FuncTypeAST chain ──────────────────────────────────────
    //
    // The chain is built right-to-left: the innermost stage wraps the
    // return type, the one before it wraps that, and so on. The
    // outermost FuncTypeAST is the first stage.
    TypeAST* cursor = returnType;
    for (int i = static_cast<int>(groups.size()) - 1; i >= 0; --i) {
        auto* ft = makeFuncType(ctx, std::move(groups[i]), cursor);
        ft->loc = loc;
        cursor = ft;
    }
    return cursor;
}

} // namespace lucid::parser