/**
 * @file ParseDecl.cpp
 * @brief The top-level declaration parsers.
 *
 * ─── What this file implements ────────────────────────────────────────────
 * The parser functions that produce a DeclAST:
 *
 *   - parseDecl              dispatcher on the current keyword
 *   - parseImportDecl        `import a.b as c`
 *   - parseTypeDecl          `TYPE X = <target>` and its sugar forms
 *   - parseVarDecl           `let x T = expr` / `const x T = expr`
 *   - parseFuncDecl          `FN`/`const`/`let` bindings whose type is a function
 *   - parseTraitDecl         `trait X { FIELD ...; REQUIRE ...; }`
 *   - parseSatisfyDecl       `satisfy Trait for Type { DEF ...; }`
 *   - parseDefDecl           `DEF op_kind "sym" (params) -> type = impl;`
 *
 * ─── Desugaring in this file ──────────────────────────────────────────────
 * Two functions here produce AST that is not a literal transcription:
 *
 *   - parseFuncDecl: for a block-body or expression-body declaration, it
 *     wraps the body in an AnonFuncExprAST whose funcType is the
 *     declaration's declared signature. For a curried declaration, it
 *     builds a chain of AnonFuncExprASTs, one per stage. For a
 *     reference body (init = another function's name), it stores the
 *     reference directly.
 *
 *   - parseTypeDecl: `struct X { ... }` and `enum X { ... }` are sugar
 *     for `TYPE X = struct { ... }` and `TYPE X = enum { ... }`. The
 *     parser detects which form the source used and produces the same
 *     StructDeclAST / EnumDeclAST either way.
 *
 * Every other function in this file is a structural transcription.
 *
 * ─── Semicolon convention ─────────────────────────────────────────────────
 * Every declaration parser in this file consumes its own terminating `;`.
 * The one exception is `parseTypeDecl`'s sugar forms for `struct` and
 * `enum`, which end with `}` and have no trailing `;` (the `TYPE X = struct
 * { ... }` form does, because it ends with the `TYPE` frame's own `;`).
 * The distinction is handled inside `parseTypeDecl`.
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

/// @brief Consume the optional trailing semicolon of a declaration.
///
/// Most declarations require a `;`; the sugar forms of `struct` and `enum`
/// do not. When `required` is true, the function reports an error if the
/// `;` is missing. When false, it consumes one if present and silently
/// accepts its absence.
void consumeDeclarationSemicolon(TokenStream& stream,
                                 ParserContext& ctx,
                                 bool required,
                                 const char* declKind) {
    if (stream.match(TokenType::SEMICOLON)) return;
    if (!required) return;

    ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken,
                       stream.currentLoc(),
                       "expected ';' after ", declKind, " declaration");
}

/// @brief Synthesize a function type from a leading group and a return
///        type, for building the `funcType` of a `FuncDeclAST`.
///
/// The parser builds the declared signature of a function declaration from
/// the parsed groups. For a non-curried declaration, there is one group;
/// for a curried one, `parseFuncDecl` calls this repeatedly and chains the
/// results.
FuncTypeAST* makeFuncType(ParserContext& ctx,
                          std::vector<ParamAST*>&& params,
                          TypeAST* returnType) {
    auto* ft = ctx.arena().make<FuncTypeAST>();
    auto pb = ctx.arena().makeBuilder<ParamAST*>(params.size());
    for (ParamAST* p : params) pb.push_back(p);
    ft->params = pb.build();
    ft->returnType = returnType;
    return ft;
}

} // namespace

// =============================================================================
// parseDecl — the dispatcher
// =============================================================================

DeclAST* parseDecl(TokenStream& stream, ParserContext& ctx) {
    // The caller (parseInternal) has established that the current token is
    // a declaration keyword or `@`. We do not re-check; a wrong dispatch
    // here is a caller bug, not a user error.
    //
    // An attribute list, if present, precedes the declaration itself. It
    // is parsed first and attached to the declaration at the end.
    auto docOpt = harvestDocComment(stream, ctx);
    ArenaSpan<AttributeAST*> attrs = parseAttributes(stream, ctx);

    SourceLocation declLoc = stream.currentLoc();

    // ─── Dispatch on the declaration's leading keyword ────────────────────
    DeclAST* decl = nullptr;

    if (stream.check(TokenType::KW_IMPORT)) {
        decl = parseImportDecl(stream, ctx);
    } else if (stream.check(TokenType::KW_TYPE) ||
               stream.check(TokenType::KW_STRUCT) ||
               stream.check(TokenType::KW_ENUM)) {
        // `TYPE X = ...`, plus the sugar forms `struct X { ... }` and
        // `enum X { ... }`. All three route through parseTypeDecl, which
        // detects the form from the first token.
        decl = parseTypeDecl(stream, ctx);
    } else if (stream.check(TokenType::KW_FN) ||
               stream.check(TokenType::KW_CONST) ||
               stream.check(TokenType::KW_LET)) {
        // A value declaration. The shape is one of two:
        //   - a function declaration, if the header begins with `fn`
        //     after the name and generic params
        //   - a variable declaration, otherwise
        //
        // looksLikeFuncDecl performs the lookahead. It is a shape check,
        // not a validation; a malformed function declaration still routes
        // to parseFuncDecl so that parser can produce a targeted error.
        if (looksLikeFuncDecl(stream, ctx)) {
            decl = parseFuncDecl(stream, ctx);
        } else {
            decl = parseVarDecl(stream, ctx);
        }
    } else if (stream.check(TokenType::KW_TRAIT)) {
        decl = parseTraitDecl(stream, ctx);
    } else if (stream.check(TokenType::KW_SATISFY)) {
        decl = parseSatisfyDecl(stream, ctx);
    } else if (stream.check(TokenType::KW_DEF)) {
        decl = parseDefDecl(stream, ctx);
    } else {
        // Should not happen: parseInternal pre-checks the dispatch.
        ctx.diag().errorAt(diag::DiagCode::Syntax_UnexpectedToken,
                           declLoc,
                           "internal error: parseDecl called with token '",
                           stream.peekValue(), "'");
        stream.consume();   // avoid an infinite loop
        return nullptr;
    }

    // ─── Attach common fields ─────────────────────────────────────────────
    //
    // Every declaration shares the same `doc`, `attributes`, and `loc`
    // fields on DeclAST. The specific parser fills its own fields; this
    // block fills the shared ones.
    if (decl != nullptr) {
        decl->loc = declLoc;
        decl->attributes = attrs;
        if (docOpt.has_value()) {
            decl->doc = docOpt;
        }
    }

    return decl;
}

// =============================================================================
// parseImportDecl — `import a.b as c`
// =============================================================================

ImportDeclAST* parseImportDecl(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    // `import`
    if (!stream.match(TokenType::KW_IMPORT)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'import', got '", stream.peekValue(), "'");
        return nullptr;
    }

    // The dotted module path: `a`, `a.b`, `a.b.c`.
    std::vector<InternedString> parts = parseImportPath(stream, ctx);
    if (parts.empty()) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedModulePath,
                           stream.currentLoc(),
                           "expected a module path after 'import'");
        consumeDeclarationSemicolon(stream, ctx, /*required=*/true, "import");
        return nullptr;
    }

    // Combine the path segments into a single InternedString. The combined
    // string is the module's identity as written in source; the CLI's
    // resolver translates it to a file path.
    std::string combined;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) combined += '.';
        combined += std::string(ctx.pool().lookupView(parts[i]));
    }
    InternedString path = ctx.pool().intern(combined);

    // The alias. If `as` is present, the next token must be an identifier.
    // Otherwise, the alias is the last path segment.
    InternedString alias;
    if (stream.match(TokenType::KW_AS)) {
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedIdentifier,
                               stream.currentLoc(),
                               "expected alias name after 'as', got '",
                               stream.peekValue(), "'");
            consumeDeclarationSemicolon(stream, ctx, /*required=*/true, "import");
            return nullptr;
        }
        Token aliasTok = stream.consume();
        alias = ctx.pool().intern(aliasTok.value);
    } else {
        alias = parts.back();
    }

    // The terminating `;`.
    consumeDeclarationSemicolon(stream, ctx, /*required=*/true, "import");

    // The parser does not resolve the import. The path is stored as
    // written; the CLI's linking step will match it against the resolved
    // module table. See Parser.hpp for the design.
    auto* importDecl = ctx.arena().make<ImportDeclAST>(path, alias);
    importDecl->loc = loc;
    return importDecl;
}

// =============================================================================
// parseTypeDecl — `TYPE X = <target>`, plus `struct X { ... }` and
//                 `enum X { ... }` sugar
// =============================================================================

/// @brief The six target shapes of a `TYPE` declaration.
///
/// Only `parseTypeDecl` dispatches on this. Each target parser produces
/// the corresponding TypeDeclAST node.
enum class TypeTargetKind {
    Host,       // #host(name)
    Native,     // #native(name)
    Builtin,    // #builtin(name)
    Alias,      // = SomeType
    Struct,     // = struct { ... }
    Enum,       // = enum { ... }
};

TypeDeclAST* parseTypeDecl(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    // ─── Determine which form we're parsing ───────────────────────────────
    //
    //   `TYPE X = ...`        the explicit form
    //   `struct X { ... }`    sugar for `TYPE X = struct { ... }`
    //   `enum X { ... }`      sugar for `TYPE X = enum { ... }`
    //
    // The sugar forms are detected from the first token. The explicit form
    // consumes `TYPE`, the name, the generic params, and the `=`.
    bool isSugarStruct = stream.check(TokenType::KW_STRUCT);
    bool isSugarEnum   = stream.check(TokenType::KW_ENUM);

    if (!isSugarStruct && !isSugarEnum && !stream.match(TokenType::KW_TYPE)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'TYPE', 'struct', or 'enum', got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Name ─────────────────────────────────────────────────────────────
    InternedString name;
    if (stream.check(TokenType::IDENTIFIER)) {
        Token nameTok = stream.consume();
        name = ctx.pool().intern(nameTok.value);
    } else {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected type name, got '", stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Generic parameters ───────────────────────────────────────────────
    ArenaSpan<GenericParamDeclAST*> genericParams;
    if (stream.check(TokenType::LESS)) {
        genericParams = parseGenericParamDecls(stream, ctx);
    }

    // ─── Sugar: `struct X : T1, T2 { ... }` or `enum X : int32 { ... }` ──
    if (isSugarStruct) {
        // Trait conformance, if present.
        ArenaSpan<NamedTypeAST*> traitRefs;
        if (stream.match(TokenType::COLON)) {
            // parseTraitRefList is a local helper: reads `A, B, C` until `{`.
            traitRefs = parseTraitRefList(stream, ctx);
        }

        auto* structDecl = parseStructBody(stream, ctx, name,
                                           genericParams, traitRefs, loc);
        // The sugar form ends at `}` with no trailing `;`.
        consumeDeclarationSemicolon(stream, ctx, /*required=*/false, "struct");
        return structDecl;
    }

    if (isSugarEnum) {
        // Backing type, if present.
        PrimitiveTypeAST* backingType = nullptr;
        if (stream.match(TokenType::COLON)) {
            TypeAST* parsed = parseType(stream, ctx);
            if (parsed && parsed->isa<PrimitiveTypeAST>()) {
                backingType = parsed->as<PrimitiveTypeAST>();
            } else {
                ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedType,
                                   stream.currentLoc(),
                                   "expected integer backing type, got '",
                                   stream.peekValue(), "'");
                // Fall through with a null backing type; Sema will
                // report if the enum really needed one.
            }
        }

        auto* enumDecl = parseEnumBody(stream, ctx, name, backingType, loc);
        consumeDeclarationSemicolon(stream, ctx, /*required=*/false, "enum");
        return enumDecl;
    }

    // ─── Explicit form: expect `=` ────────────────────────────────────────
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '=' after '", ctx.pool().lookup(name),
                           "', got '", stream.peekValue(), "'");
        synchronizeToBoundary(stream, ctx, {TokenType::SEMICOLON});
        consumeDeclarationSemicolon(stream, ctx, /*required=*/true, "TYPE");
        return nullptr;
    }

    // ─── Dispatch on the target shape ─────────────────────────────────────
    //
    //   `#host(...)` / `#native(...)` / `#builtin(...)`   → HostTypeDeclAST
    //   `struct { ... }`                                   → StructDeclAST
    //   `enum { ... }`                                     → EnumDeclAST
    //   an identifier                                      → TypeAliasDeclAST
    //
    // The first three are unambiguous from the next token. The last is the
    // fallback: an identifier at the start of a type target is an alias
    // to another named type.
    TypeTargetKind targetKind;
    HostTypeKind hostKind = HostTypeKind::Host;
    InternedString hostTargetName;

    if (stream.check(TokenType::HASH)) {
        // Read the sigil and the parenthesized identifier. `parseHostTarget`
        // returns the specific kind (host / native / builtin) and the target
        // name.
        if (!parseHostTarget(stream, ctx, hostKind, hostTargetName)) {
            consumeDeclarationSemicolon(stream, ctx, /*required=*/true, "TYPE");
            return nullptr;
        }
        targetKind = (hostKind == HostTypeKind::Host)    ? TypeTargetKind::Host
                   : (hostKind == HostTypeKind::Native)  ? TypeTargetKind::Native
                                                         : TypeTargetKind::Builtin;
    } else if (stream.check(TokenType::KW_STRUCT)) {
        targetKind = TypeTargetKind::Struct;
    } else if (stream.check(TokenType::KW_ENUM)) {
        targetKind = TypeTargetKind::Enum;
    } else if (stream.check(TokenType::IDENTIFIER)) {
        targetKind = TypeTargetKind::Alias;
    } else {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedType,
                           stream.currentLoc(),
                           "expected a type target after '=', got '",
                           stream.peekValue(), "'");
        synchronizeToBoundary(stream, ctx, {TokenType::SEMICOLON});
        consumeDeclarationSemicolon(stream, ctx, /*required=*/true, "TYPE");
        return nullptr;
    }

    // ─── Parse the specific target ────────────────────────────────────────
    TypeDeclAST* decl = nullptr;

    switch (targetKind) {
        case TypeTargetKind::Host:
        case TypeTargetKind::Native:
        case TypeTargetKind::Builtin: {
            auto* hostDecl = ctx.arena().make<HostTypeDeclAST>(
                name, hostKind, hostTargetName);
            hostDecl->genericParams = genericParams;
            hostDecl->loc = loc;
            decl = hostDecl;
            break;
        }

        case TypeTargetKind::Alias: {
            TypeAST* target = parseType(stream, ctx);
            if (!target) {
                ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedType,
                                   stream.currentLoc(),
                                   "expected an aliased type, got '",
                                   stream.peekValue(), "'");
                synchronizeToBoundary(stream, ctx, {TokenType::SEMICOLON});
                consumeDeclarationSemicolon(stream, ctx, /*required=*/true,
                                            "TYPE");
                return nullptr;
            }
            auto* alias = ctx.arena().make<TypeAliasDeclAST>(name, target);
            alias->genericParams = genericParams;
            alias->loc = loc;
            decl = alias;
            break;
        }

        case TypeTargetKind::Struct: {
            // `TYPE X = struct { ... }`: consume the `struct` keyword
            // (checked above but not consumed), then parse the body.
            stream.consume();   // KW_STRUCT
            decl = parseStructBody(stream, ctx, name, genericParams,
                                   /*traitRefs=*/{}, loc);
            break;
        }

        case TypeTargetKind::Enum: {
            stream.consume();   // KW_ENUM
            decl = parseEnumBody(stream, ctx, name,
                                 /*backingType=*/nullptr, loc);
            break;
        }
    }

    // The explicit `TYPE X = ...` form ends with `;`.
    consumeDeclarationSemicolon(stream, ctx, /*required=*/true, "TYPE");
    return decl;
}

// =============================================================================
// parseStructBody — the body of a struct declaration
// =============================================================================

StructDeclAST* parseStructBody(
        TokenStream& stream,
        ParserContext& ctx,
        InternedString name,
        ArenaSpan<GenericParamDeclAST*> genericParams,
        ArenaSpan<NamedTypeAST*> traitRefs,
        SourceLocation loc) {
    // `{`
    if (!stream.match(TokenType::LBRACE)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '{' for struct body, got '",
                           stream.peekValue(), "'");
        synchronizeToBoundary(stream, ctx);
        auto* empty = ctx.arena().make<StructDeclAST>(
            name, genericParams,
            ctx.arena().makeBuilder<FieldDeclAST*>().build(),
            traitRefs);
        empty->loc = loc;
        empty->hasSyntaxError = true;
        return empty;
    }

    ArenaSpan<FieldDeclAST*> fields = parseStructFieldList(stream, ctx, name);

    // `}`
    if (!stream.check(TokenType::RBRACE)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '}' to close struct body, got '",
                           stream.peekValue(), "'");
        // parseStructFieldList leaves the cursor at whatever stopped it;
        // if it stopped on `}`, consume; otherwise leave for the caller.
        stream.match(TokenType::RBRACE);
    } else {
        stream.consume();   // RBRACE
    }

    auto* decl = ctx.arena().make<StructDeclAST>(
        name, genericParams, fields, traitRefs);
    decl->loc = loc;
    return decl;
}

// =============================================================================
// parseEnumBody — the body of an enum declaration
// =============================================================================

EnumDeclAST* parseEnumBody(
        TokenStream& stream,
        ParserContext& ctx,
        InternedString name,
        PrimitiveTypeAST* backingType,
        SourceLocation loc) {
    // `{`
    if (!stream.match(TokenType::LBRACE)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '{' for enum body, got '",
                           stream.peekValue(), "'");
        synchronizeToBoundary(stream, ctx);
        auto* empty = ctx.arena().make<EnumDeclAST>(
            name,
            ctx.arena().makeBuilder<EnumVariantAST*>().build(),
            backingType);
        empty->loc = loc;
        empty->hasSyntaxError = true;
        return empty;
    }

    ArenaSpan<EnumVariantAST*> variants = parseEnumVariantList(stream, ctx);

    // `}`
    if (!stream.check(TokenType::RBRACE)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '}' to close enum body, got '",
                           stream.peekValue(), "'");
        stream.match(TokenType::RBRACE);
    } else {
        stream.consume();   // RBRACE
    }

    auto* decl = ctx.arena().make<EnumDeclAST>(name, variants, backingType);
    decl->loc = loc;
    return decl;
}

// =============================================================================
// parseVarDecl — `let x T = expr` / `const x T = expr`
// =============================================================================

VarDeclAST* parseVarDecl(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    // ─── Keyword ──────────────────────────────────────────────────────────
    DeclKeyword keyword;
    if (stream.match(TokenType::KW_CONST)) {
        keyword = DeclKeyword::Const;
    } else if (stream.match(TokenType::KW_LET)) {
        keyword = DeclKeyword::Let;
    } else {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'let' or 'const', got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Name ─────────────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected variable name, got '",
                           stream.peekValue(), "'");
        synchronizeToBoundary(stream, ctx, {TokenType::SEMICOLON});
        consumeDeclarationSemicolon(stream, ctx, /*required=*/true, "variable");
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool().intern(nameTok.value);

    // ─── Type ─────────────────────────────────────────────────────────────
    // The type is required. A missing type is a targeted error.
    TypeAST* type = parseType(stream, ctx);
    if (!type) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedType,
                           stream.currentLoc(),
                           "expected type for variable '",
                           ctx.pool().lookup(name), "', got '",
                           stream.peekValue(), "'");
        // Continue parsing the initializer if there is one, so we produce
        // as much AST as possible. The type is an UnknownTypeAST.
        type = ctx.arena().make<UnknownTypeAST>();
        type->loc = stream.currentLoc();
        type->hasSyntaxError = true;
    }

    // ─── Initializer (optional for `let`, required for `const`) ──────────
    ExprAST* init = nullptr;
    if (stream.match(TokenType::ASSIGN)) {
        init = parseRequiredExpr(stream, ctx, "initializer expression");
    } else if (keyword == DeclKeyword::Const) {
        ctx.diag().errorAt(diag::DiagCode::Sem_MissingInitializer,
                           stream.currentLoc(),
                           "const variable '", ctx.pool().lookup(name),
                           "' requires an initializer");
        init = ctx.arena().make<UnknownExprAST>();
        init->loc = stream.currentLoc();
        init->hasSyntaxError = true;
    }

    // ─── Terminator ───────────────────────────────────────────────────────
    consumeDeclarationSemicolon(stream, ctx, /*required=*/true, "variable");

    auto* decl = ctx.arena().make<VarDeclAST>(name, keyword, type, init);
    decl->loc = loc;

    // Mark the declaration as having a syntax error if any of its parts
    // did. This is the convention every parser follows: a node whose
    // subtree contains a recovered error is flagged at the root.
    if (type->hasSyntaxError || (init && init->hasSyntaxError)) {
        decl->hasSyntaxError = true;
    }
    return decl;
}

// =============================================================================
// parseFuncDecl — function declarations
// =============================================================================

FuncDeclAST* parseFuncDecl(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    // ─── Keyword: `let` or `const` ────────────────────────────────────────
    DeclKeyword keyword;
    if (stream.match(TokenType::KW_CONST)) {
        keyword = DeclKeyword::Const;
    } else if (stream.match(TokenType::KW_LET)) {
        keyword = DeclKeyword::Let;
    } else {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'let' or 'const', got '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Name ─────────────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected function name, got '",
                           stream.peekValue(), "'");
        synchronizeToBoundary(stream, ctx, {TokenType::SEMICOLON});
        consumeDeclarationSemicolon(stream, ctx, /*required=*/true, "function");
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool().intern(nameTok.value);

    // ─── Generic parameters ───────────────────────────────────────────────
    ArenaSpan<GenericParamDeclAST*> genericParams;
    if (stream.check(TokenType::LESS)) {
        genericParams = parseGenericParamDecls(stream, ctx);
    }

    // A generic function must be `const`. If `let` was written, report it
    // here so the diagnostic points at the keyword. Sema enforces the same
    // rule with the same code; the parser reports it early because the
    // keyword's location is here.
    if (keyword == DeclKeyword::Let && !genericParams.empty()) {
        ctx.diag().errorAt(diag::DiagCode::Sem_GenericRequiresConst, loc,
                           "a generic function must be declared 'const'");
    }

    // ─── The declared signature ───────────────────────────────────────────
    //
    // A function declaration's header is a sequence of `fn (params)` stages
    // separated by `->`, ending with an optional `-> return_type`.
    //
    //   fn (a int) -> fn (b int) -> int
    //
    // The first `fn (params)` group is the "leading cluster". Adjacent
    // groups before the first `->` are also part of the leading cluster;
    // they desugar to nested stages.
    //
    //   fn (a int) fn (b int) -> int
    //
    // is equivalent to
    //
    //   fn (a int) -> fn (b int) -> int
    //
    // The parser builds the FuncTypeAST chain from the parsed groups.

    // Collect the leading cluster: one or more `fn (params)` groups.
    std::vector<std::vector<ParamAST*>> leadingGroups;

    while (stream.check(TokenType::KW_FN_MARKER)) {
        stream.consume();   // `fn`
        if (!stream.check(TokenType::LPAREN)) {
            ctx.diag().errorAt(diag::DiagCode::Syntax_MissingFuncShapeMarker,
                               stream.currentLoc(),
                               "expected '(' after 'fn', got '",
                               stream.peekValue(), "'");
            synchronizeToBoundary(stream, ctx,
                                  {TokenType::ASSIGN, TokenType::SEMICOLON});
            consumeDeclarationSemicolon(stream, ctx, /*required=*/true,
                                        "function");
            return nullptr;
        }
        std::vector<ParamAST*> group = parseParamList(stream, ctx,
                                                      /*allowNames=*/true);
        leadingGroups.push_back(std::move(group));

        // If the next token is `fn`, another adjacent group follows.
        // Otherwise the leading cluster ends here, and the return type
        // (if any) is parsed by parseType after the `->`.
        if (!stream.check(TokenType::KW_FN_MARKER)) break;
    }

    if (leadingGroups.empty()) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_MissingFuncShapeMarker,
                           stream.currentLoc(),
                           "expected 'fn' before parameter group, got '",
                           stream.peekValue(), "'");
        synchronizeToBoundary(stream, ctx,
                              {TokenType::ASSIGN, TokenType::SEMICOLON});
        consumeDeclarationSemicolon(stream, ctx, /*required=*/true, "function");
        return nullptr;
    }

    // After the leading cluster, an optional `->` introduces either the
    // return type or a subsequent curry stage (which parseType handles,
    // because a `fn (params)` after `->` is a function type).
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

    // Build the declared FuncTypeAST chain, innermost last. The chain is
    // built right-to-left: the last leading group wraps the return type,
    // the one before it wraps that, and so on. The outermost FuncTypeAST
    // is the declaration's declared type.
    TypeAST* cursor = returnType;
    std::vector<FuncTypeAST*> stages(leadingGroups.size());
    for (int i = static_cast<int>(leadingGroups.size()) - 1; i >= 0; --i) {
        auto* ft = makeFuncType(ctx, std::move(leadingGroups[i]), cursor);
        ft->loc = loc;
        stages[i] = ft;
        cursor = ft;
    }
    FuncTypeAST* declaredType = stages.front();

    // ─── `=` and the body ─────────────────────────────────────────────────
    //
    // The `=` is required unless the declaration is foreign (marked
    // `@[host_only]` or carrying a `#host`-family target). Under the new
    // grammar, foreign bindings are written with `FN`, whose target is
    // `#host(...)` directly — so this parser handles the Lucid-bodied
    // cases, and the foreign case is handled by `parseValueDecl`'s
    // dispatch reaching a different path.
    //
    // The declaration grammar for `FN`/`const`/`let` with a signature is:
    //
    //   ('const' | 'let') NAME [generics] header '=' body ';'
    //
    // The body is one of:
    //
    //   { ... }             a block body
    //   expr                an expression body
    //   identifier          a reference body (init = another function)
    //
    // The block and expression bodies are wrapped in an AnonFuncExprAST
    // whose funcType is the declared signature. The reference body is
    // stored as-is; no wrapping.

    if (!stream.match(TokenType::ASSIGN)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '=' after function signature, got '",
                           stream.peekValue(), "'");
        synchronizeToBoundary(stream, ctx, {TokenType::SEMICOLON});
        consumeDeclarationSemicolon(stream, ctx, /*required=*/true, "function");
        auto* decl = ctx.arena().make<FuncDeclAST>(
            name, keyword, genericParams, declaredType, /*init=*/nullptr);
        decl->loc = loc;
        decl->hasSyntaxError = true;
        return decl;
    }

    ExprAST* init = nullptr;

    if (stream.check(TokenType::LBRACE)) {
        // Block body. The body is a single StmtAST (a BlockStmtAST). For
        // a curried declaration, the block belongs to the outermost
        // function's body; the inner stages' bodies are ReturnStmts that
        // return the inner closures.
        //
        // The construction is bottom-up: for stages [s0, s1, ..., sN],
        // the block is sN's body, wrapped in a ReturnStmt that becomes
        // s(N-1)'s body, and so on up to s0.
        StmtAST* block = parseBlock(stream, ctx);
        if (!block) {
            block = ctx.arena().make<UnknownStmtAST>();
            block->loc = stream.currentLoc();
            block->hasSyntaxError = true;
        }

        // Build the chain bottom-up.
        StmtAST* bodyCursor = block;
        AnonFuncExprAST* anon = nullptr;
        for (int i = static_cast<int>(stages.size()) - 1; i >= 0; --i) {
            anon = ctx.arena().make<AnonFuncExprAST>(stages[i], bodyCursor);
            anon->loc = loc;
            if (i == 0) break;

            // Wrap the inner anon in a ReturnStmt, which becomes the body
            // of the next-outer stage.
            auto* ret = ctx.arena().make<ReturnStmtAST>();
            ret->loc = loc;
            ret->value = anon;
            bodyCursor = ret;
        }
        init = anon;

    } else if (looksLikeAnonFunc(stream, ctx)) {
        // An anonymous function literal at the declaration site is a
        // syntax error: the body's signature would have to be
        // re-specified, and the parser cannot tell which one wins.
        ctx.diag().errorAt(diag::DiagCode::Syntax_AnonymousFunctionAtDeclaration,
                           stream.currentLoc(),
                           "anonymous function is not allowed at a "
                           "declaration site; use a block body '{ ... }' or "
                           "the declared signature");
        synchronizeToBoundary(stream, ctx, {TokenType::SEMICOLON});
        init = ctx.arena().make<UnknownExprAST>();
        init->loc = stream.currentLoc();
        init->hasSyntaxError = true;

    } else {
        // Expression body or reference body. Parse as an expression. If
        // it's an identifier, module access, field access, or call, treat
        // it as a reference body — no wrapping. Otherwise it's an
        // expression body: wrap it in an AnonFuncExprAST whose body is a
        // single ReturnStmt with the expression.
        ExprAST* expr = parseRequiredExpr(stream, ctx, "function body");

        if (expr->isa<IdentifierExprAST>() ||
            expr->isa<ModuleAccessExprAST>() ||
            expr->isa<FieldAccessExprAST>() ||
            expr->isa<CallExprAST>()) {
            init = expr;
        } else {
            // Expression body. Wrap it in a ReturnStmt (the innermost
            // stage's body), then build the chain the same way as the
            // block case.
            auto* ret = ctx.arena().make<ReturnStmtAST>();
            ret->loc = expr->loc;
            ret->value = expr;

            StmtAST* bodyCursor = ret;
            AnonFuncExprAST* anon = nullptr;
            for (int i = static_cast<int>(stages.size()) - 1; i >= 0; --i) {
                anon = ctx.arena().make<AnonFuncExprAST>(stages[i], bodyCursor);
                anon->loc = loc;
                if (i == 0) break;

                auto* innerRet = ctx.arena().make<ReturnStmtAST>();
                innerRet->loc = loc;
                innerRet->value = anon;
                bodyCursor = innerRet;
            }
            init = anon;
        }
    }

    // ─── Terminator ───────────────────────────────────────────────────────
    consumeDeclarationSemicolon(stream, ctx, /*required=*/true, "function");

    auto* decl = ctx.arena().make<FuncDeclAST>(
        name, keyword, genericParams, declaredType, init);
    decl->loc = loc;
    if (init && init->hasSyntaxError) {
        decl->hasSyntaxError = true;
    }
    return decl;
}

// =============================================================================
// parseTraitDecl — `trait X { FIELD ...; REQUIRE ...; }`
// =============================================================================

TraitDeclAST* parseTraitDecl(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_TRAIT)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'trait', got '", stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Name ─────────────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected trait name, got '",
                           stream.peekValue(), "'");
        synchronizeToBoundary(stream, ctx, {TokenType::LBRACE});
        // Recover by synthesizing an empty trait body.
        auto* decl = ctx.arena().make<TraitDeclAST>(
            ctx.pool().intern(""),
            /*genericParams=*/{},
            /*parentTraits=*/{},
            /*fields=*/{},
            /*requires=*/{});
        decl->loc = loc;
        decl->hasSyntaxError = true;
        return decl;
    }
    Token nameTok = stream.consume();
    InternedString name = ctx.pool().intern(nameTok.value);

    // ─── Generic parameters ───────────────────────────────────────────────
    ArenaSpan<GenericParamDeclAST*> genericParams;
    if (stream.check(TokenType::LESS)) {
        genericParams = parseGenericParamDecls(stream, ctx);
    }

    // ─── Parent traits ────────────────────────────────────────────────────
    ArenaSpan<NamedTypeAST*> parentTraits;
    if (stream.match(TokenType::COLON)) {
        parentTraits = parseTraitRefList(stream, ctx);
    }

    // ─── Body ─────────────────────────────────────────────────────────────
    if (!stream.match(TokenType::LBRACE)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '{' for trait body, got '",
                           stream.peekValue(), "'");
        synchronizeToBoundary(stream, ctx);
        auto* decl = ctx.arena().make<TraitDeclAST>(
            name, genericParams, parentTraits, /*fields=*/{}, /*requires=*/{});
        decl->loc = loc;
        decl->hasSyntaxError = true;
        consumeDeclarationSemicolon(stream, ctx, /*required=*/false, "trait");
        return decl;
    }

    // The body is a mixed list of FIELD and REQUIRE clauses. The two
    // clause-list parsers below each read until a token that is not a
    // clause of their kind. Since a trait body may interleave them, we
    // need a single loop that dispatches on the leading keyword.

    std::vector<TraitFieldDeclAST*>   fields;
    std::vector<TraitRequireDeclAST*> requires;

    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE) &&
           ctx.canContinue()) {
        // Skip stray `;`.
        if (stream.match(TokenType::SEMICOLON)) continue;

        // Dispatch on the clause keyword.
        if (stream.check(TokenType::KW_FIELD_MARKER)) {
            // FIELD clause. parseTraitField reads the clause and its
            // terminating `;`.
            TraitFieldDeclAST* field = parseTraitField(stream, ctx);
            if (field) fields.push_back(field);
        } else if (stream.check(TokenType::KW_REQUIRE)) {
            // REQUIRE clause. parseRequireClause reads the clause and its
            // terminating `;`.
            TraitRequireDeclAST* req = parseRequireClause(stream, ctx);
            if (req) requires.push_back(req);
        } else {
            ctx.diag().errorAt(diag::DiagCode::Syntax_UnexpectedToken,
                               stream.currentLoc(),
                               "expected 'FIELD' or 'REQUIRE' inside trait "
                               "body, got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx,
                          TokenType::KW_FIELD_MARKER,
                          TokenType::KW_REQUIRE,
                          TokenType::RBRACE);
            if (stream.check(TokenType::RBRACE) || stream.isAtEnd()) break;
        }
    }

    // `}`
    if (!stream.check(TokenType::RBRACE)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '}' to close trait body");
    } else {
        stream.consume();
    }

    // Build the spans.
    auto fieldBuilder = ctx.arena().makeBuilder<TraitFieldDeclAST*>(fields.size());
    for (auto* f : fields) fieldBuilder.push_back(f);
    auto requireBuilder = ctx.arena().makeBuilder<TraitRequireDeclAST*>(requires.size());
    for (auto* r : requires) requireBuilder.push_back(r);

    auto* decl = ctx.arena().make<TraitDeclAST>(
        name, genericParams, parentTraits,
        fieldBuilder.build(), requireBuilder.build());
    decl->loc = loc;

    consumeDeclarationSemicolon(stream, ctx, /*required=*/false, "trait");
    return decl;
}

// =============================================================================
// parseSatisfyDecl — `satisfy Trait for Type { DEF ...; }`
// =============================================================================

SatisfyDeclAST* parseSatisfyDecl(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_SATISFY)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'satisfy', got '", stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Trait name ───────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected trait name after 'satisfy', got '",
                           stream.peekValue(), "'");
        synchronizeToBoundary(stream, ctx, {TokenType::LBRACE});
        auto* decl = ctx.arena().make<SatisfyDeclAST>(ctx.pool().intern(""));
        decl->loc = loc;
        decl->hasSyntaxError = true;
        consumeDeclarationSemicolon(stream, ctx, /*required=*/false, "satisfy");
        return decl;
    }
    Token traitTok = stream.consume();
    InternedString traitName = ctx.pool().intern(traitTok.value);

    // ─── Trait generic arguments ──────────────────────────────────────────
    ArenaSpan<TypeAST*> traitGenericArgs;
    if (stream.check(TokenType::LESS)) {
        traitGenericArgs = parseGenericArgs(stream, ctx);
    }

    // ─── `for` ────────────────────────────────────────────────────────────
    if (!stream.match(TokenType::KW_FOR)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected 'for' in satisfy clause, got '",
                           stream.peekValue(), "'");
        synchronizeToBoundary(stream, ctx, {TokenType::LBRACE});
    }

    // ─── Target type ──────────────────────────────────────────────────────
    TypeAST* targetType = parseType(stream, ctx);
    if (!targetType) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedType,
                           stream.currentLoc(),
                           "expected a type after 'for', got '",
                           stream.peekValue(), "'");
        targetType = ctx.arena().make<UnknownTypeAST>();
        targetType->loc = stream.currentLoc();
        targetType->hasSyntaxError = true;
    }

    // ─── Body ─────────────────────────────────────────────────────────────
    if (!stream.match(TokenType::LBRACE)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '{' for satisfy body, got '",
                           stream.peekValue(), "'");
        synchronizeToBoundary(stream, ctx);
        auto* decl = ctx.arena().make<SatisfyDeclAST>(traitName);
        decl->traitGenericArgs = traitGenericArgs;
        decl->targetType = targetType;
        decl->loc = loc;
        decl->hasSyntaxError = true;
        consumeDeclarationSemicolon(stream, ctx, /*required=*/false, "satisfy");
        return decl;
    }

    // The body contains only DEF declarations. Each is parsed by
    // parseDefDecl, the same function used for top-level DEF declarations.
    // The parser does not distinguish top-level DEF from satisfy-body DEF;
    // both produce a DefDeclAST. Sema knows which context it is in.
    std::vector<DefDeclAST*> defs;

    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE) &&
           ctx.canContinue()) {
        if (stream.match(TokenType::SEMICOLON)) continue;

        if (stream.check(TokenType::KW_DEF)) {
            DeclAST* def = parseDefDecl(stream, ctx);
            if (def && def->isa<DefDeclAST>()) {
                defs.push_back(def->as<DefDeclAST>());
            } else if (def) {
                ctx.diag().errorAt(diag::DiagCode::Syntax_UnexpectedToken, def->loc,
                                   "only DEF declarations are allowed inside "
                                   "a satisfy block");
            }
        } else {
            ctx.diag().errorAt(diag::DiagCode::Syntax_UnexpectedToken,
                               stream.currentLoc(),
                               "expected 'DEF' inside satisfy block, got '",
                               stream.peekValue(), "'");
            synchronizeTo(stream, ctx,
                          TokenType::KW_DEF,
                          TokenType::RBRACE);
            if (stream.check(TokenType::RBRACE) || stream.isAtEnd()) break;
        }
    }

    // `}`
    if (!stream.check(TokenType::RBRACE)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedBlock,
                           stream.currentLoc(),
                           "expected '}' to close satisfy body");
    } else {
        stream.consume();
    }

    auto defsBuilder = ctx.arena().makeBuilder<DefDeclAST*>(defs.size());
    for (auto* d : defs) defsBuilder.push_back(d);

    auto* decl = ctx.arena().make<SatisfyDeclAST>(traitName);
    decl->traitGenericArgs = traitGenericArgs;
    decl->targetType = targetType;
    decl->defs = defsBuilder.build();
    decl->loc = loc;

    consumeDeclarationSemicolon(stream, ctx, /*required=*/false, "satisfy");
    return decl;
}

// =============================================================================
// parseDefDecl — `DEF op_kind "sym" (params) -> type = impl;`
// =============================================================================

DefDeclAST* parseDefDecl(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_DEF)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'DEF', got '", stream.peekValue(), "'");
        return nullptr;
    }

    // ─── op_kind ──────────────────────────────────────────────────────────
    // An identifier. Under the grammar, the op_kind is data, not a
    // keyword; Sema resolves it against the OpKind values declared in the
    // core script.
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected an operation kind after 'DEF', got '",
                           stream.peekValue(), "'");
        synchronizeToBoundary(stream, ctx, {TokenType::SEMICOLON});
        consumeDeclarationSemicolon(stream, ctx, /*required=*/true, "DEF");
        return nullptr;
    }
    Token opKindTok = stream.consume();
    InternedString opKindName = ctx.pool().intern(opKindTok.value);

    // ─── Symbol ───────────────────────────────────────────────────────────
    // A string literal. The symbol is the operator's spelling (`"+"`,
    // `"=="`) or the call's name (`"toStr"`).
    InternedString symbol;
    if (stream.check(TokenType::STRING_HEAD)) {
        // A string literal is a sequence of STRING_HEAD / STRING_MIDDLE /
        // STRING_END tokens. For a DEF symbol, only the simple case (one
        // STRING_HEAD and one STRING_END with no interpolation) is
        // meaningful. The parser accepts only the simple case; a string
        // with an interpolation is a syntax error.
        Token head = stream.consume();
        if (!stream.check(TokenType::STRING_END)) {
            ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedLiteral,
                               stream.currentLoc(),
                               "the symbol after an operation kind must be a "
                               "simple string literal (no interpolation)");
            // Skip to the string's end.
            while (!stream.isAtEnd() &&
                   !stream.check(TokenType::STRING_END)) {
                stream.consume();
            }
            if (stream.check(TokenType::STRING_END)) stream.consume();
            symbol = ctx.pool().intern("");
        } else {
            stream.consume();   // STRING_END
            symbol = ctx.pool().intern(head.value);
        }
    } else {
        // Some op kinds (`INDEX_GET`, `INDEX_SET`) have no symbol. A `(`
        // immediately after the op_kind means the symbol was omitted.
        if (!stream.check(TokenType::LPAREN)) {
            ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedLiteral,
                               stream.currentLoc(),
                               "expected a symbol string or '(' after the "
                               "operation kind, got '", stream.peekValue(), "'");
            // Fall through with an empty symbol.
        }
    }

    // ─── Generic parameters ───────────────────────────────────────────────
    ArenaSpan<GenericParamDeclAST*> genericParams;
    if (stream.check(TokenType::LESS)) {
        genericParams = parseGenericParamDecls(stream, ctx);
    }

    // ─── Parameter list ───────────────────────────────────────────────────
    if (!stream.check(TokenType::LPAREN)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '(' for DEF parameter list, got '",
                           stream.peekValue(), "'");
        synchronizeToBoundary(stream, ctx, {TokenType::SEMICOLON});
        consumeDeclarationSemicolon(stream, ctx, /*required=*/true, "DEF");
        return nullptr;
    }
    std::vector<ParamAST*> params = parseParamList(stream, ctx,
                                                   /*allowNames=*/true);
    auto paramsBuilder = ctx.arena().makeBuilder<ParamAST*>(params.size());
    for (ParamAST* p : params) paramsBuilder.push_back(p);

    // ─── `->` return type ─────────────────────────────────────────────────
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
    // An omitted return type means `unit`; the AST stores null and Sema
    // treats it as `unit` in the DEF's context.

    // ─── `=` and implementation ───────────────────────────────────────────
    if (!stream.match(TokenType::ASSIGN)) {
        ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '=' before DEF implementation, got '",
                           stream.peekValue(), "'");
        synchronizeToBoundary(stream, ctx, {TokenType::SEMICOLON});
        consumeDeclarationSemicolon(stream, ctx, /*required=*/true, "DEF");
        auto* decl = ctx.arena().make<DefDeclAST>(opKindName, symbol);
        decl->genericParams = genericParams;
        decl->params = paramsBuilder.build();
        decl->returnType = returnType;
        decl->loc = loc;
        decl->hasSyntaxError = true;
        return decl;
    }

    ExprAST* impl = nullptr;

    if (stream.check(TokenType::LBRACE)) {
        // Block body. The block belongs to a synthesized AnonFuncExprAST
        // whose funcType is the DEF's declared signature. The wrapping is
        // the same as for parseFuncDecl's block body.
        StmtAST* block = parseBlock(stream, ctx);
        if (!block) {
            block = ctx.arena().make<UnknownStmtAST>();
            block->loc = stream.currentLoc();
            block->hasSyntaxError = true;
        }

        // Build a single-stage FuncTypeAST for the DEF's signature.
        auto* ft = makeFuncType(ctx, std::vector<ParamAST*>(), returnType);
        auto pb = ctx.arena().makeBuilder<ParamAST*>(params.size());
        for (ParamAST* p : params) pb.push_back(p);
        ft->params = pb.build();
        ft->loc = loc;

        auto* anon = ctx.arena().make<AnonFuncExprAST>(ft, block);
        anon->loc = loc;
        impl = anon;
    } else if (stream.check(TokenType::HASH)) {
        // A `#host`/`#native`/`#builtin` target. The DEF's impl is a call
        // to the registered operation.
        HostTypeKind hostKind = HostTypeKind::Host;
        InternedString targetName;
        if (!parseHostTarget(stream, ctx, hostKind, targetName)) {
            consumeDeclarationSemicolon(stream, ctx, /*required=*/true, "DEF");
            return nullptr;
        }

        // Synthesize an IdentifierExprAST naming the target. Sema will
        // resolve it against the host registry and reject if the target
        // is not registered under the expected kind. The node itself is
        // a plain identifier; the parser does not distinguish `#host`
        // from `#native` from `#builtin` in the AST.
        auto* idExpr = ctx.arena().make<IdentifierExprAST>(targetName);
        idExpr->loc = loc;
        impl = idExpr;
    } else {
        // An expression. Either a reference to another function or a call
        // to a host/builtin target.
        impl = parseRequiredExpr(stream, ctx, "DEF implementation");
    }

    // ─── Terminator ───────────────────────────────────────────────────────
    consumeDeclarationSemicolon(stream, ctx, /*required=*/true, "DEF");

    auto* decl = ctx.arena().make<DefDeclAST>(opKindName, symbol);
    decl->genericParams = genericParams;
    decl->params = paramsBuilder.build();
    decl->returnType = returnType;
    decl->impl = impl;
    decl->loc = loc;
    if (impl && impl->hasSyntaxError) decl->hasSyntaxError = true;
    return decl;
}

// =============================================================================
// parseTraitRefList — a comma-separated list of trait names
// =============================================================================

ArenaSpan<NamedTypeAST*> parseTraitRefList(TokenStream& stream,
                                                  ParserContext& ctx) {
    // Reads `A, B, C` until a token that cannot continue the list
    // (typically `{` or `<` for generics on a later clause). Produces a
    // span of NamedTypeASTs. Each `A`, `B`, `C` may itself carry generic
    // arguments (`Container<int>`), which parseNamedType handles.
    //
    // The caller is responsible for having consumed the `:` that
    // introduces the list. This function returns when the current token
    // is not a comma following a successful ref.
    std::vector<NamedTypeAST*> refs;

    while (true) {
        TypeAST* parsed = parseNamedType(stream, ctx);
        if (!parsed || !parsed->isa<NamedTypeAST>()) {
            ctx.diag().errorAt(diag::DiagCode::Syntax_ExpectedType,
                               stream.currentLoc(),
                               "expected a trait name, got '",
                               stream.peekValue(), "'");
            // Recovery: consume the offending token and continue if a
            // comma follows. Otherwise stop.
            if (!stream.match(TokenType::COMMA)) break;
            continue;
        }

        refs.push_back(parsed->as<NamedTypeAST>());

        if (!stream.match(TokenType::COMMA)) break;
    }

    auto builder = ctx.arena().makeBuilder<NamedTypeAST*>(refs.size());
    for (auto* r : refs) builder.push_back(r);
    return builder.build();
}

} // namespace lucid::parser