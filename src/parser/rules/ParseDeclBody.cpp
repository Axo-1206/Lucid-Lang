/**
 * @file ParseDeclBody.cpp
 * @brief The sub-declaration parsers: the items inside a declaration body.
 *
 * ─── What this file implements ────────────────────────────────────────────
 *   - parseStructFieldList     the body of `struct X { ... }`
 *   - parseFieldDecl           one field inside a struct
 *   - parseEnumVariantList     the body of `enum X { ... }`
 *   - parseEnumVariant         one variant inside an enum
 *   - parseTraitField         one `FIELD` clause inside a trait
 *   - parseRequireClause       one `REQUIRE` clause inside a trait
 *   - parseStaticFnDecl        one `static` function inside a struct
 *
 * The list parsers (`parseStructFieldList`, `parseEnumVariantList`) and the
 * trait-body clause parsers (`parseTraitField`, `parseRequireClause`) share
 * a design: they loop over items until a terminating `}`, recover from a
 * bad item by synchronizing to the next item boundary, and stop when the
 * diagnostic engine says to stop.
 *
 * ─── Desugaring in this file ──────────────────────────────────────────────
 * `parseFieldDecl` produces an `AnonFuncExprAST` for a function-typed
 * field's block default. The synthesized signature is the field's declared
 * type with a `self: &StructName` parameter prepended. This is the only
 * place in the language where a `self` parameter is implicit; see the
 * grammar's "The implicit self for block defaults" section.
 *
 * ─── Semicolon convention ─────────────────────────────────────────────────
 * Every parser in this file consumes its own terminating `;`. The
 * list parsers do not consume anything beyond the items they produce.
 */

#include "parser/Parser.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"

#include <vector>

namespace lucid::parser {

// =============================================================================
// Local helpers
// =============================================================================

namespace {

/// @brief Consume the optional trailing semicolon of a sub-declaration.
///
/// Every sub-declaration in this file requires a `;`. When `required` is
/// false, the caller is a context that tolerates its absence (currently
/// none; reserved for a future sugar form).
void consumeSubDeclSemicolon(TokenStream& stream,
                             ParserContext& ctx,
                             bool required,
                             const char* declKind) {
    if (stream.match(TokenType::SEMICOLON)) return;
    if (!required) return;

    ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken,
                       stream.currentLoc(),
                       "expected ';' after ", declKind);
}

/// @brief Check whether a token can start a struct-field item.
///
/// A struct body item starts with `IDENTIFIER` (a normal field), `CONST`
/// (a const field), `AT_SIGN` (an attribute list before a field), or the
/// `static` keyword. Semicolons are skipped by the list parser before
/// this check.
bool startsStructFieldItem(TokenType t) {
    return t == TokenType::IDENTIFIER
        || t == TokenType::KW_CONST
        || t == TokenType::AT_SIGN
        || t == TokenType::KW_STATIC;
}

/// @brief Check whether a token can start an enum-variant item.
///
/// An enum body item starts with `IDENTIFIER` (a variant) or `AT_SIGN`
/// (an attribute list before a variant). Semicolons are skipped by the
/// list parser before this check.
bool startsEnumVariantItem(TokenType t) {
    return t == TokenType::IDENTIFIER
        || t == TokenType::AT_SIGN;
}

} // namespace

// =============================================================================
// parseStructFieldList — the body of a struct
// =============================================================================

ArenaSpan<FieldDeclAST*> parseStructFieldList(TokenStream& stream,
                                              ParserContext& ctx,
                                              InternedString structName) {
    // The caller has consumed `{`. We read items until `}` or EOF. The
    // list is a mixed sequence of fields and static functions; the
    // dispatch is on the leading keyword of each item.
    //
    // The parser does not require at least one item; an empty struct is
    // legal (a zero-sized type).
    std::vector<FieldDeclAST*> fields;

    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE) &&
           ctx.canContinue()) {
        // Skip stray `;`. A `;` at the start of an item position is a
        // no-op; the same check the top-level parser does.
        if (stream.match(TokenType::SEMICOLON)) continue;

        // Check that we're on something that can start a field. Anything
        // else is an error; report it and synchronize to the next item.
        if (!startsStructFieldItem(stream.peekType())) {
            ctx.diag().errorAt(diag::DiagCode::Syntax_UnexpectedToken,
                               stream.currentLoc(),
                               "expected a field declaration or 'static' "
                               "inside struct body, got '",
                               stream.peekValue(), "'");
            synchronizeTo(stream, ctx,
                          TokenType::IDENTIFIER,
                          TokenType::KW_CONST,
                          TokenType::AT_SIGN,
                          TokenType::KW_STATIC,
                          TokenType::RBRACE);
            if (stream.check(TokenType::RBRACE) || stream.isAtEnd()) break;
            continue;
        }

        // Static function: `static Name (params) -> ret = { ... };`
        //
        // Static functions are parsed by a dedicated function. They are
        // not fields; the list parser collects them separately in a
        // future change, or the containing struct's parse collects them
        // in a second pass. For now, the parser produces a FieldDeclAST
        // placeholder; the containing parser will need to distinguish.
        //
        // NOTE: as currently written, the struct body only collects
        // fields. Static functions need their own collection. See the
        // "Known limitation" note at the end of this file.

        // Field: `[const] Name Type [= default];`
        FieldDeclAST* field = parseFieldDecl(stream, ctx, structName);
        if (field) {
            fields.push_back(field);
        }
    }

    auto builder = ctx.arena().makeBuilder<FieldDeclAST*>(fields.size());
    for (auto* f : fields) builder.push_back(f);
    return builder.build();
}

StructBodyParseResult parseStructBodyList(TokenStream& stream,
                                          ParserContext& ctx,
                                          InternedString structName) {
    std::vector<FieldDeclAST*>    fields;
    std::vector<StaticFnDeclAST*> statics;

    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE) &&
           ctx.canContinue()) {
        if (stream.match(TokenType::SEMICOLON)) continue;

        // A `static` item.
        if (stream.check(TokenType::KW_STATIC)) {
            StaticFnDeclAST* stat = parseStaticFnDecl(stream, ctx);
            if (stat) statics.push_back(stat);
            continue;
        }

        // A field item.
        if (!startsStructFieldItem(stream.peekType())) {
            ctx.diag().errorAt(diag::DiagCode::Syntax_UnexpectedToken,
                               stream.currentLoc(),
                               "expected a field declaration or 'static' "
                               "inside struct body, got '",
                               stream.peekValue(), "'");
            synchronizeTo(stream, ctx,
                          TokenType::IDENTIFIER,
                          TokenType::KW_CONST,
                          TokenType::AT_SIGN,
                          TokenType::KW_STATIC,
                          TokenType::RBRACE);
            if (stream.check(TokenType::RBRACE) || stream.isAtEnd()) break;
            continue;
        }

        FieldDeclAST* field = parseFieldDecl(stream, ctx, structName);
        if (field) fields.push_back(field);
    }

    auto fieldBuilder = ctx.arena().makeBuilder<FieldDeclAST*>(fields.size());
    for (auto* f : fields) fieldBuilder.push_back(f);

    auto staticBuilder = ctx.arena().makeBuilder<StaticFnDeclAST*>(statics.size());
    for (auto* s : statics) staticBuilder.push_back(s);

    return { fieldBuilder.build(), staticBuilder.build() };
}

// =============================================================================
// parseFieldDecl — one struct field
// =============================================================================

FieldDeclAST* parseFieldDecl(TokenStream& stream,
                             ParserContext& ctx,
                             InternedString structName) {
    const SourceLocation loc = stream.currentLoc();

    // ─── Doc comment and attributes ───────────────────────────────────────
    auto docOpt = harvestDocComment(stream, ctx);
    ArenaSpan<AttributeAST*> attrs = parseAttributes(stream, ctx);

    // ─── `const` modifier ─────────────────────────────────────────────────
    bool isConstField = stream.match(TokenType::KW_CONST);

    // ─── Field name ───────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected field name, got '",
                           stream.peekValue(), "'");
        synchronizeTo(stream, ctx,
                      TokenType::SEMICOLON,
                      TokenType::RBRACE);
        consumeSubDeclSemicolon(stream, ctx, /*required=*/true, "field");
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool().intern(nameTok.value);

    // ─── Type ─────────────────────────────────────────────────────────────
    TypeAST* type = parseType(stream, ctx);
    if (!type) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedType,
                           stream.currentLoc(),
                           "expected a type for field '",
                           ctx.pool().lookup(name), "', got '",
                           stream.peekValue(), "'");
        type = ctx.arena().make<UnknownTypeAST>();
        type->loc = stream.currentLoc();
        type->hasSyntaxError = true;
    }

    // ─── Default value ────────────────────────────────────────────────────
    //
    // Two cases by the field's type:
    //
    //   - Function-typed field with a block default: the block belongs to
    //     a synthesized AnonFuncExprAST whose funcType is the field's
    //     declared type with `self: &StructName` prepended.
    //
    //   - Any other field with a block default: syntactically the block
    //     is allowed, but the field's type is not a function type, so the
    //     block cannot be a function body. The parser still produces an
    //     AnonFuncExprAST (for AST uniformity); Sema rejects the type
    //     mismatch later.
    //
    //   - Any field with a non-block default: the default is an ordinary
    //     expression producing a value of the field's type.

    ExprAST* defaultVal = nullptr;

    if (stream.match(TokenType::ASSIGN)) {
        if (stream.check(TokenType::LBRACE)) {
            // ─── Block default ────────────────────────────────────────────
            //
            // Determine the effective funcType for the AnonFuncExprAST.
            // If the field's type is a FuncTypeAST and the enclosing
            // context has a struct name, prepend a `self` parameter.
            // Otherwise, use an empty one-parameter funcType; Sema will
            // report the mismatch.
            FuncTypeAST* effectiveFuncType = nullptr;

            if (type->isa<FuncTypeAST>() && structName.isValid()) {
                FuncTypeAST* fieldFuncType = type->as<FuncTypeAST>();

                // Synthesize `self: &StructName`.
                auto* selfNamed = ctx.arena().make<NamedTypeAST>(structName);
                selfNamed->loc = loc;
                auto* selfRef = ctx.arena().make<RefTypeAST>(selfNamed);
                selfRef->loc = loc;

                InternedString selfName = ctx.pool().intern("self");
                auto* selfParam = ctx.arena().make<ParamAST>(
                    selfName, selfRef, /*isVariadic=*/false, /*isConstParam=*/false);
                selfParam->loc = loc;

                // Build the new funcType: self parameter first, then the
                // field's declared parameters.
                auto* newFuncType = ctx.arena().make<FuncTypeAST>();
                auto pb = ctx.arena().makeBuilder<ParamAST*>(
                    fieldFuncType->params.size() + 1);
                pb.push_back(selfParam);
                for (ParamAST* p : fieldFuncType->params) pb.push_back(p);
                newFuncType->params = pb.build();
                newFuncType->returnType = fieldFuncType->returnType;
                newFuncType->loc = fieldFuncType->loc;

                effectiveFuncType = newFuncType;
            } else {
                // Non-function field with a block default. Produce a
                // minimal funcType so the AST is well-formed; Sema will
                // reject.
                effectiveFuncType = ctx.arena().make<FuncTypeAST>();
                effectiveFuncType->params =
                    ctx.arena().makeBuilder<ParamAST*>().build();
                effectiveFuncType->returnType = nullptr;
                effectiveFuncType->loc = loc;
            }

            // Parse the block body.
            StmtAST* block = parseBlock(stream, ctx);
            if (!block) {
                block = ctx.arena().make<UnknownStmtAST>();
                block->loc = stream.currentLoc();
                block->hasSyntaxError = true;
            }

            auto* anon = ctx.arena().make<AnonFuncExprAST>(
                effectiveFuncType, block);
            anon->loc = loc;
            defaultVal = anon;

        } else {
            // ─── Expression default ───────────────────────────────────────
            //
            // An anonymous function literal at a field-default position is
            // a syntax error. The block form is the way to write a
            // function-typed default; the parenthesized form would require
            // the parser to synthesize the enclosing funcType from the
            // literal's own signature, and that's a different construct.
            if (looksLikeAnonFunc(stream, ctx)) {
                ctx.diag().errorAt(
                    diag::DiagCode::Syntax_AnonymousFunctionAtDeclaration,
                    stream.currentLoc(),
                    "anonymous function literal is not allowed as a field "
                    "default; use a block body '{ ... }' instead");
                defaultVal = ctx.arena().make<UnknownExprAST>();
                defaultVal->loc = stream.currentLoc();
                defaultVal->hasSyntaxError = true;
            } else {
                defaultVal = parseRequiredExpr(stream, ctx, "field default");
            }
        }
    }

    // ─── Terminator ───────────────────────────────────────────────────────
    consumeSubDeclSemicolon(stream, ctx, /*required=*/true, "field");

    // ─── Build the FieldDeclAST ───────────────────────────────────────────
    auto* field = ctx.arena().make<FieldDeclAST>(
        name, type, defaultVal, isConstField);
    field->loc = loc;
    field->attributes = attrs;
    if (docOpt.has_value()) field->doc = docOpt;

    if (type->hasSyntaxError ||
        (defaultVal && defaultVal->hasSyntaxError)) {
        field->hasSyntaxError = true;
    }
    return field;
}

// =============================================================================
// parseEnumVariantList — the body of an enum
// =============================================================================

ArenaSpan<EnumVariantAST*> parseEnumVariantList(TokenStream& stream,
                                                ParserContext& ctx) {
    std::vector<EnumVariantAST*> variants;

    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE) &&
           ctx.canContinue()) {
        // Skip stray `;`.
        if (stream.match(TokenType::SEMICOLON)) continue;

        // Check that we're on something that can start a variant.
        if (!startsEnumVariantItem(stream.peekType())) {
            ctx.diag().errorAt(diag::DiagCode::Syntax_UnexpectedToken,
                               stream.currentLoc(),
                               "expected an enum variant inside enum body, "
                               "got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx,
                          TokenType::IDENTIFIER,
                          TokenType::AT_SIGN,
                          TokenType::RBRACE);
            if (stream.check(TokenType::RBRACE) || stream.isAtEnd()) break;
            continue;
        }

        EnumVariantAST* variant = parseEnumVariant(stream, ctx);
        if (variant) variants.push_back(variant);
    }

    auto builder = ctx.arena().makeBuilder<EnumVariantAST*>(variants.size());
    for (auto* v : variants) builder.push_back(v);
    return builder.build();
}

// =============================================================================
// parseEnumVariant — one enum variant
// =============================================================================

EnumVariantAST* parseEnumVariant(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    // ─── Doc comment and attributes ───────────────────────────────────────
    auto docOpt = harvestDocComment(stream, ctx);
    ArenaSpan<AttributeAST*> attrs = parseAttributes(stream, ctx);

    // ─── Variant name ─────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected variant name, got '",
                           stream.peekValue(), "'");
        synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
        consumeSubDeclSemicolon(stream, ctx, /*required=*/true, "enum variant");
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool().intern(nameTok.value);

    // ─── Variant form ─────────────────────────────────────────────────────
    //
    // Two forms, distinguished by the token after the name:
    //
    //   `Name = INT;`    integer-valued variant
    //   `Name(Type);`    payload-carrying variant
    //
    // The `(` after the name introduces a payload type. The `=` after the
    // name introduces an integer value.

    EnumVariantAST* variant = nullptr;

    if (stream.match(TokenType::ASSIGN)) {
        // ─── Integer-valued form ──────────────────────────────────────────
        if (!stream.check(TokenType::INT_LITERAL)) {
            ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedLiteral,
                               stream.currentLoc(),
                               "expected an integer literal after '=', got '",
                               stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
            consumeSubDeclSemicolon(stream, ctx, /*required=*/true,
                                    "enum variant");
            return nullptr;
        }
        Token valueTok = stream.consume();

        // The literal is lexed as text; Sema converts it to an integer.
        // The parser does not parse the value; it stores it as a raw
        // lexeme on the node and lets Sema handle radix, range, and
        // fit-to-backing-type checks.
        //
        // For the AST field, we store the value's textual representation
        // as an InternedString (the `value` field of the grammar's
        // `enum_variant` production is `= INT_LIT`, not `= expr`). Sema
        // parses the lexeme to produce the discriminant.
        //
        // NOTE: In the current AST, EnumVariantAST's integer constructor
        // takes an int64_t. The parser cannot provide that without
        // parsing the literal. To keep the parser purely syntactic, the
        // AST should carry the raw lexeme for the value, and Sema
        // should compute the int64_t. If the AST is to be left as-is,
        // the parser must do the conversion here, and errors that would
        // be Sema's (radix, range) would have to be reported by the
        // parser. See the "Known limitation" note at the end of this
        // file.
        variant = ctx.arena().make<EnumVariantAST>(
            name, /*value=*/0);   // placeholder; see note
        variant->loc = loc;
        variant->attributes = attrs;
        if (docOpt.has_value()) variant->doc = docOpt;

    } else if (stream.match(TokenType::LPAREN)) {
        // ─── Payload-carrying form ────────────────────────────────────────
        TypeAST* payloadType = parseType(stream, ctx);
        if (!payloadType) {
            ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedType,
                               stream.currentLoc(),
                               "expected a payload type inside '(', got '",
                               stream.peekValue(), "'");
            payloadType = ctx.arena().make<UnknownTypeAST>();
            payloadType->loc = stream.currentLoc();
            payloadType->hasSyntaxError = true;
        }

        if (!stream.match(TokenType::RPAREN)) {
            ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken,
                               stream.currentLoc(),
                               "expected ')' to close variant payload, got '",
                               stream.peekValue(), "'");
        }

        variant = ctx.arena().make<EnumVariantAST>(name, payloadType);
        variant->loc = loc;
        variant->attributes = attrs;
        if (docOpt.has_value()) variant->doc = docOpt;

    } else {
        // ─── Neither form ─────────────────────────────────────────────────
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '=' or '(' after variant name '",
                           ctx.pool().lookup(name), "', got '",
                           stream.peekValue(), "'");
        synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
        consumeSubDeclSemicolon(stream, ctx, /*required=*/true, "enum variant");
        return nullptr;
    }

    // ─── Terminator ───────────────────────────────────────────────────────
    consumeSubDeclSemicolon(stream, ctx, /*required=*/true, "enum variant");

    // ─── Mark the node if its payload errored ─────────────────────────────
    if (variant->payloadType && variant->payloadType->hasSyntaxError) {
        variant->hasSyntaxError = true;
    }
    return variant;
}

// =============================================================================
// parseTraitField — one FIELD clause in a trait
// =============================================================================

TraitFieldDeclAST* parseTraitField(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    // The caller has established that the current token is `FIELD`.

    // Consume the `FIELD` keyword. The token type is expected to be a
    // marker (KW_FIELD_MARKER). If the token type is spelled differently
    // in Tokens.hpp, adjust this match.
    if (!stream.match(TokenType::KW_FIELD_MARKER)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'FIELD', got '", stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Doc comment and attributes ───────────────────────────────────────
    // (harvestDocComment does nothing here, since a FIELD clause is
    // unlikely to carry one. But the call is harmless and consistent.)
    auto docOpt = harvestDocComment(stream, ctx);
    (void)docOpt;

    // ─── `const` modifier ─────────────────────────────────────────────────
    bool isConstField = stream.match(TokenType::KW_CONST);

    // ─── Field name ───────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected a field name in FIELD clause, got '",
                           stream.peekValue(), "'");
        synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
        consumeSubDeclSemicolon(stream, ctx, /*required=*/true,
                                "FIELD clause");
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool().intern(nameTok.value);

    // ─── Type ─────────────────────────────────────────────────────────────
    TypeAST* type = parseType(stream, ctx);
    if (!type) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedType,
                           stream.currentLoc(),
                           "expected a type in FIELD clause for '",
                           ctx.pool().lookup(name), "', got '",
                           stream.peekValue(), "'");
        type = ctx.arena().make<UnknownTypeAST>();
        type->loc = stream.currentLoc();
        type->hasSyntaxError = true;
    }

    // ─── Terminator ───────────────────────────────────────────────────────
    consumeSubDeclSemicolon(stream, ctx, /*required=*/true, "FIELD clause");

    auto* clause = ctx.arena().make<TraitFieldDeclAST>(
        name, type, isConstField);
    clause->loc = loc;
    if (type->hasSyntaxError) clause->hasSyntaxError = true;
    return clause;
}

// =============================================================================
// parseRequireClause — one REQUIRE clause in a trait
// =============================================================================

TraitRequireDeclAST* parseRequireClause(TokenStream& stream,
                                        ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_REQUIRE)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'REQUIRE', got '", stream.peekValue(), "'");
        return nullptr;
    }

    // ─── op_kind ──────────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected an operation kind after 'REQUIRE', got '",
                           stream.peekValue(), "'");
        synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
        consumeSubDeclSemicolon(stream, ctx, /*required=*/true,
                                "REQUIRE clause");
        return nullptr;
    }
    Token opKindTok = stream.consume();
    InternedString opKindName = ctx.pool().intern(opKindTok.value);

    // ─── Symbol string ────────────────────────────────────────────────────
    InternedString symbol;
    if (stream.check(TokenType::STRING_HEAD)) {
        Token head = stream.consume();
        if (!stream.check(TokenType::STRING_END)) {
            ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedLiteral,
                               stream.currentLoc(),
                               "the symbol after an operation kind must be a "
                               "simple string literal (no interpolation)");
            while (!stream.isAtEnd() &&
                   !stream.check(TokenType::STRING_END)) {
                stream.consume();
            }
            if (stream.check(TokenType::STRING_END)) stream.consume();
        } else {
            stream.consume();
            symbol = ctx.pool().intern(head.value);
        }
    } else if (!stream.check(TokenType::LPAREN)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedLiteral,
                           stream.currentLoc(),
                           "expected a symbol string or '(' after the "
                           "operation kind, got '", stream.peekValue(), "'");
    }

    // ─── Parameter list ───────────────────────────────────────────────────
    std::vector<ParamAST*> params;
    if (stream.check(TokenType::LPAREN)) {
        params = parseParamList(stream, ctx, /*allowNames=*/true);
    } else {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '(' for REQUIRE parameter list, got '",
                           stream.peekValue(), "'");
        synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
        consumeSubDeclSemicolon(stream, ctx, /*required=*/true,
                                "REQUIRE clause");
        return nullptr;
    }

    // ─── Return type ──────────────────────────────────────────────────────
    TypeAST* returnType = nullptr;
    if (stream.match(TokenType::ARROW)) {
        returnType = parseType(stream, ctx);
        if (!returnType) {
            ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedType,
                               stream.currentLoc(),
                               "expected a return type after '->'");
            returnType = ctx.arena().make<UnknownTypeAST>();
            returnType->loc = stream.currentLoc();
            returnType->hasSyntaxError = true;
        }
    }

    // ─── Terminator ───────────────────────────────────────────────────────
    consumeSubDeclSemicolon(stream, ctx, /*required=*/true, "REQUIRE clause");

    auto* clause = ctx.arena().make<TraitRequireDeclAST>(opKindName, symbol);
    clause->loc = loc;
    auto pb = ctx.arena().makeBuilder<ParamAST*>(params.size());
    for (ParamAST* p : params) pb.push_back(p);
    clause->params = pb.build();
    clause->returnType = returnType;
    if (returnType && returnType->hasSyntaxError) {
        clause->hasSyntaxError = true;
    }
    return clause;
}

// =============================================================================
// parseStaticFnDecl — one static function inside a struct
// =============================================================================

StaticFnDeclAST* parseStaticFnDecl(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_STATIC)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'static', got '", stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Name ─────────────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected a name after 'static', got '",
                           stream.peekValue(), "'");
        synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
        consumeSubDeclSemicolon(stream, ctx, /*required=*/true,
                                "static function");
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool().intern(nameTok.value);

    // ─── Parameters ───────────────────────────────────────────────────────
    // A static function has no implicit `self`. Its parameter list is
    // the ordinary named-parameter form.
    std::vector<ParamAST*> params;
    if (stream.check(TokenType::LPAREN)) {
        params = parseParamList(stream, ctx, /*allowNames=*/true);
    } else {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '(' for static function parameter list, "
                           "got '", stream.peekValue(), "'");
        synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
        consumeSubDeclSemicolon(stream, ctx, /*required=*/true,
                                "static function");
        return nullptr;
    }

    // ─── Return type ──────────────────────────────────────────────────────
    TypeAST* returnType = nullptr;
    if (stream.match(TokenType::ARROW)) {
        returnType = parseType(stream, ctx);
        if (!returnType) {
            ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedType,
                               stream.currentLoc(),
                               "expected a return type after '->'");
            returnType = ctx.arena().make<UnknownTypeAST>();
            returnType->loc = stream.currentLoc();
            returnType->hasSyntaxError = true;
        }
    }

    // ─── `=` and body ─────────────────────────────────────────────────────
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '=' before static function body, got '",
                           stream.peekValue(), "'");
        synchronizeTo(stream, ctx, TokenType::SEMICOLON, TokenType::RBRACE);
        consumeSubDeclSemicolon(stream, ctx, /*required=*/true,
                                "static function");
        auto* decl = ctx.arena().make<StaticFnDeclAST>(name);
        auto pb = ctx.arena().makeBuilder<ParamAST*>(params.size());
        for (ParamAST* p : params) pb.push_back(p);
        decl->params = pb.build();
        decl->returnType = returnType;
        decl->loc = loc;
        decl->hasSyntaxError = true;
        return decl;
    }

    // Block body. The body is wrapped in an AnonFuncExprAST whose
    // funcType is the static function's declared signature — same shape
    // as a function declaration, but with no `self`.
    StmtAST* body = nullptr;
    if (stream.check(TokenType::LBRACE)) {
        body = parseBlock(stream, ctx);
        if (!body) {
            body = ctx.arena().make<UnknownStmtAST>();
            body->loc = stream.currentLoc();
            body->hasSyntaxError = true;
        }
    } else {
        // Expression body: wrap in a ReturnStmt.
        ExprAST* expr = parseRequiredExpr(stream, ctx,
                                          "static function body");
        auto* ret = ctx.arena().make<ReturnStmtAST>();
        ret->loc = expr->loc;
        ret->value = expr;
        body = ret;
    }

    // ─── Terminator ───────────────────────────────────────────────────────
    consumeSubDeclSemicolon(stream, ctx, /*required=*/true, "static function");

    // ─── Build the StaticFnDeclAST ────────────────────────────────────────
    //
    // The AST node holds `params`, `returnType`, and `body`. The body is
    // wrapped in an AnonFuncExprAST, per the AST's contract that every
    // function body lives inside an AnonFuncExprAST.
    auto* decl = ctx.arena().make<StaticFnDeclAST>(name);
    auto pb = ctx.arena().makeBuilder<ParamAST*>(params.size());
    for (ParamAST* p : params) pb.push_back(p);
    decl->params = pb.build();
    decl->returnType = returnType;

    auto* ft = ctx.arena().make<FuncTypeAST>();
    auto ftPb = ctx.arena().makeBuilder<ParamAST*>(params.size());
    for (ParamAST* p : params) ftPb.push_back(p);
    ft->params = ftPb.build();
    ft->returnType = returnType;
    ft->loc = loc;

    auto* anon = ctx.arena().make<AnonFuncExprAST>(ft, body);
    anon->loc = loc;
    decl->body = anon;
    decl->loc = loc;

    if (body->hasSyntaxError ||
        (returnType && returnType->hasSyntaxError)) {
        decl->hasSyntaxError = true;
    }
    return decl;
}

} // namespace lucid::parser