/**
 * @file ParserDecl.cpp
 *
 * @responsibility Parses all top-level declarations (let, func, struct, etc.).
 *
 * @related src/diagnostics/DiagnosticEngine.hpp, DiagnosticCodes.hpp
 */

#include "Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// ParserDecl.cpp
//
// Implements every declaration parse function declared in Parser.hpp.
// This file handles all top-level constructs: use, var, func, struct,
// enum, trait, impl (with from/method bodies), type alias, and the
// '@' compiler directive attributes.
//
// Entry points (called from Parser.cpp::parseTopLevelDecl):
//   parseAttributes()        — zero or more '@' directives
//   parseAttribute()         — one '@' IDENTIFIER [ '(' args ')' ]
//   parseUseDecl(vis)
//   parseVarDecl(vis)
//   parseFuncDecl(kw, vis)
//   parseStructDecl(vis)
//   parseEnumDecl(vis)
//   parseTraitDecl(vis)
//   parseImplDecl(vis)
//   parseFromDecl(vis)
//   parseTypeAliasDecl(vis)
//
// Internal helpers used across multiple parsers:
//   parseParamGroup()        — one '(' param* ')'
//   parseParam()             — IDENTIFIER [ '...' ] type
//   parseGenericParams()     — '<' generic_param { ',' generic_param } '>'
//   parseGenericParam()      — IDENTIFIER [ ':' constraint { '+' constraint } ]
//   parseFieldDecl()         — IDENTIFIER type [ '=' expr ]
//   parseEnumVariant()       — IDENTIFIER [ '=' INT_LITERAL | HEX_LITERAL ]
//   parseTraitMethod()       — IDENTIFIER '(' params ')' [ returnType ]
//   parseMethodDecl()        — IDENTIFIER '(' params ')' [ returnType ] '=' body
//   parseFromDecl()          — 'from' '(' name type ')' returnType '=' body
//   parseTraitRef()          — IDENTIFIER [ generic_args ]
//   parseFuncBody()          — '=' ( block | anon_func )
//
// Grammar source: LUC_GRAMMAR.md
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseAttributes / parseAttribute
//
// Grammar:
//   attributes    := { attribute }
//   attribute     := '@' IDENTIFIER [ '(' attr_arg_list ')' ]
//   attr_arg_list := attr_arg { ',' attr_arg }
//   attr_arg      := STRING_LITERAL
//                  | INT_LITERAL | HEX_LITERAL | BINARY_LITERAL
//                  | 'true' | 'false'
//                  | IDENTIFIER      -- type name used in @sizeof(T)
//
// Examples:
//   @extern("malloc")
//   @extern("vkCreateInstance", "C")
//   @inline
//   @packed
//   @deprecated("Use newAlloc instead")
//
// Parameters are intentionally restricted to compile-time literals and type
// identifiers — no runtime expressions inside attribute argument lists.
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// validateAnonFuncBodySig
//
// Shared helper for parseFuncDecl and parseMethodDecl.
//
// When a function/method body is written in the verbose anon-func form:
//   = (params) RetType { ... }
// or, for curried declarations:
//   = (a int) (b int) RetType { ... }
//
// the repeated parameter groups and optional return type must be consumed.
// This helper does exactly that, then validates the repeated return type
// against the already-declared signature (which is the authoritative source).
//
// Cursor contract:
//   On entry : positioned at the first token of the repeated signature.
//              This may be '~' (qualifier) or '(' (first param group) —
//              both forms are handled correctly.
//   On return: positioned at '{' (or at a bad token — caller owns that check).
// ─────────────────────────────────────────────────────────────────────────────
void Parser::validateAnonFuncBodySig(FuncSignature& declaredSig,
                                     const std::string& declName) {
    // ── 0. Consume any repeated type qualifiers (~async, ~noinline, etc.) ──
    //
    // Without this step the helper would reach looksLikeType() with '~' as the
    // current token.  looksLikeType() returns true for '~IDENTIFIER' (function
    // type qualifier), so it would call parseType() → parseBaseType().
    // parseBaseType() has no TILDE case and falls through to default:, which
    // returns UnknownTypeAST while consuming ZERO tokens.  The '~' is left
    // unconsumed, check(LBRACE) fails, and the caller reports "expected '{'"
    // and returns nullptr — the entire function body is silently dropped.
    //
    // The fix: discard repeated qualifiers here, before the param-group loop,
    // exactly mirroring the qualifier loop in parseFuncDecl's declaration header.
    // We do not validate consistency against declaredSig.rawQualifiers here;
    // the semantic pass is the right place for that check.
    while (check(TokenType::TILDE)) {
        advance(); // consume '~'
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003,
                    "expected qualifier name after '~' in repeated signature for '" +
                    declName + "'");
            break; // stop; let the param-group loop handle whatever comes next
        }
        advance(); // consume qualifier name (e.g. 'async', 'noinline')
    }

    // ── 1. Consume ALL repeated parameter groups ─────────────────────────
    // This correctly handles both single-group and curried (multi-group) forms.
    // Without this loop the previous code only consumed one group, leaving the
    // second (and subsequent) groups to be misidentified as the return type.
    while (check(TokenType::LPAREN)) {
        parseParamGroup(); // discard — declared signature is authoritative
    }

    // ── 2. Optional repeated return type ─────────────────────────────────
    if (looksLikeType() && !check(TokenType::LBRACE)) {
        SourceLocation repeatedRetLoc = currentLoc();
        TypePtr repeatedRet = parseType();

        if (declaredSig.returnType && repeatedRet) {
            // Both declared and repeated return types are present — compare.
            // ASTKind is the fast first-pass check; it catches mismatches
            // between entirely different type categories (e.g. int vs MyStruct).
            bool mismatch = (declaredSig.returnType->kind != repeatedRet->kind);

            // For named types, kinds match but names might differ (Foo vs Bar).
            if (!mismatch &&
                declaredSig.returnType->isa<NamedTypeAST>() &&
                repeatedRet->isa<NamedTypeAST>()) {
                mismatch = (declaredSig.returnType->as<NamedTypeAST>()->name !=
                            repeatedRet->as<NamedTypeAST>()->name);
            }
            // Primitive types: each primitive has a unique ASTKind, so the
            // kind comparison above is already sufficient — no extra check.

            if (mismatch) {
                error(repeatedRetLoc, DiagCode::W3001,
                      "repeated return type in body does not match the declared "
                      "return type for '" + declName + "'; "
                      "the declaration's type is authoritative");
                LUC_LOG_PARSER("validateAnonFuncBodySig: return-type MISMATCH for '" << declName << "'");
            } else {
                LUC_LOG_PARSER("validateAnonFuncBodySig: return-type matches for '" << declName << "'");
            }
        } else if (!declaredSig.returnType && repeatedRet) {
            // No return type in the header — adopt the body's annotation.
            // This is a permissive fallback; the semantic pass will confirm.
            LUC_LOG_PARSER("validateAnonFuncBodySig: adopting repeated return type for '" << declName << "'");
            declaredSig.returnType = std::move(repeatedRet);
        }
        // If repeatedRet is null (parse error), we leave declaredSig unchanged.
    }
}


std::vector<AttributePtr> Parser::parseAttributes() {
    LUC_LOG_PARSER_VERBOSE("parseAttributes: starting");
    std::vector<AttributePtr> attrs;
    while (check(TokenType::AT_SIGN)) {
        std::size_t savedPos = pos_;
        AttributePtr attr = parseAttribute();
        if (attr) {
            LUC_LOG_PARSER_VERBOSE("\tParsed attribute: @" << pool_.lookup(attr->name));
            attrs.push_back(std::move(attr));
        } else {
            // parseAttribute failed. Ensure we make progress.
            if (pos_ == savedPos && !isAtEnd()) {
                errorAt(DiagCode::E2002, "invalid attribute syntax, skipping '@'");
                advance(); // consume the '@' to avoid infinite loop
            }
        }
    }
    LUC_LOG_PARSER_VERBOSE("parseAttributes: found " << attrs.size() << " attributes");
    return attrs;
}

AttributePtr Parser::parseAttribute() {
    if (!check(TokenType::AT_SIGN))
        return nullptr;

    SourceLocation loc = currentLoc();
    advance(); // consume '@'

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected attribute name after '@'");
        return nullptr;
    }

    // 1. Intern the attribute name
    auto attr = arena_.make<AttributeAST>();
    attr->loc  = loc;
    attr->name = pool_.intern(advance().value);

    // Optional argument list: '(' attr_arg { ',' attr_arg } ')'
    if (match(TokenType::LPAREN)) {
        while (!check(TokenType::RPAREN) && !isAtEnd()) {
            match(TokenType::COMMA); // optional separator
            if (check(TokenType::RPAREN)) break;

            std::size_t savedPos = pos_;
            SourceLocation argLoc = currentLoc();

            if (check(TokenType::STRING_LITERAL)) {
                std::string raw = advance().value;
                auto arg = arena_.make<AttributeArgAST>(
                    AttributeArgKind::StringLit,
                    pool_.intern(raw)
                );
                arg->loc = argLoc;
                attr->args.push_back(std::move(arg));
            }
            else if (checkAny({TokenType::INT_LITERAL, TokenType::HEX_LITERAL,
                            TokenType::BINARY_LITERAL})) {
                std::string raw = advance().value;
                auto arg = arena_.make<AttributeArgAST>(
                    AttributeArgKind::IntLit,
                    pool_.intern(raw)
                );
                arg->loc = argLoc;
                attr->args.push_back(std::move(arg));
            }
            else if (check(TokenType::TRUE)) {
                advance();
                auto arg = arena_.make<AttributeArgAST>(
                    AttributeArgKind::BoolLit,
                    pool_.intern("true")
                );
                arg->loc = argLoc;
                attr->args.push_back(std::move(arg));
            }
            else if (check(TokenType::FALSE)) {
                advance();
                auto arg = arena_.make<AttributeArgAST>(
                    AttributeArgKind::BoolLit,
                    pool_.intern("false")
                );
                arg->loc = argLoc;
                attr->args.push_back(std::move(arg));
            }
            else if (check(TokenType::IDENTIFIER)) {
                // Type identifier: @sizeof(Vec2), @extern("sym", "C")
                std::string raw = advance().value;
                auto arg = arena_.make<AttributeArgAST>(
                    AttributeArgKind::TypeIdent,
                    pool_.intern(raw)
                );
                arg->loc = argLoc;
                attr->args.push_back(std::move(arg));
            }
            else {
                errorAt(DiagCode::E2009,
                        "attribute argument must be a string, integer, boolean, or type name");
                // Skip to closing parenthesis for recovery
                while (!check(TokenType::RPAREN) && !isAtEnd()) advance();
                break;
            }

            // Ensure progress was made
            if (pos_ == savedPos) {
                errorAt(DiagCode::E2009, "invalid attribute argument – no progress");
                // consume the offending token to avoid infinite loop
                if (!isAtEnd()) advance();
                break;
            }
        }
        consume(TokenType::RPAREN, "expected ')' to close attribute argument list");
    }

    return attr;
}

// ─────────────────────────────────────────────────────────────────────────────
// parsePackageDecl
//
// Grammar:
//   package_decl := 'package' IDENTIFIER
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<PackageDeclAST> Parser::parsePackageDecl() {
    SourceLocation loc = currentLoc();
    consume(TokenType::PACKAGE, "expected 'package'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected package name");
        // Return a valid node with error name instead of nullptr
        auto node = arena_.make<PackageDeclAST>(pool_.intern("<error>"));
        node->loc = loc;
        return node;
    }
    
    std::string nameRaw = advance().value;
    InternedString name = pool_.intern(nameRaw);
    auto node = arena_.make<PackageDeclAST>(name);
    node->loc = loc;
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseUseDecl
//
// Grammar:
//   use_decl := 'use' module_path [ 'as' IDENTIFIER ]
//   module_path := IDENTIFIER { '.' IDENTIFIER }
//
// Examples:
//   use math.vec2
//   use renderer.types
//   use math as m
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<UseDeclAST> Parser::parseUseDecl(Visibility vis) {
    SourceLocation loc = currentLoc();
    consume(TokenType::USE, "expected 'use'");

    auto node = arena_.make<UseDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    // Parse module path: IDENTIFIER { '.' IDENTIFIER }
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected module path after 'use'");
        return node;
    }

    node->path.push_back(pool_.intern(advance().value));

    while (check(TokenType::DOT) && peekNext().type == TokenType::IDENTIFIER) {
        advance(); // consume '.'
        node->path.push_back(pool_.intern(advance().value));
    }

    // Optional alias: 'as' IDENTIFIER
    if (match(TokenType::AS)) {
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected alias name after 'as'");
        } else {
            node->alias = pool_.intern(advance().value);
        }
    }

    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseVarDecl
//
// Grammar:
//   var_decl := decl_keyword IDENTIFIER type_ann [ '=' expr ]
//
// Called from Parser.cpp after the keyword token has ALREADY been consumed.
// pos_ sits on the IDENTIFIER (the variable name).
//
// isPub is passed in from the outer loop (was 'pub' seen before the keyword?).
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<VarDeclAST> Parser::parseVarDecl(Visibility vis, std::vector<AttributePtr> attrs) {
    // The keyword was consumed by parseTopLevelDecl before this call.
    const Token &kwTok = tokens_[pos_ - 1];
    DeclKeyword kw;
    switch (kwTok.type) {
    case TokenType::LET:
        kw = DeclKeyword::Let;
        break;
    default:
        kw = DeclKeyword::Const;
        break;
    }

    SourceLocation loc = currentLoc();

    // Check for @extern attribute in the attributes list
    bool hasExternAttr = false;
    std::string externName;
    InternedString externStr = kw_extern;
    for (const auto& attr : attrs) {
        if (attr->name == externStr) {
            hasExternAttr = true;
            if (!attr->args.empty()) {
                const auto& arg = attr->args[0];
                if (arg->kind == AttributeArgKind::StringLit) {  
                    externName = pool_.lookup(arg->value);    
                }
            }
            LUC_LOG_PARSER("\t*** @extern attribute detected on variable! C name: '" << externName << "' ***");
            break;
        }
    }

    // Name
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected variable name");
        return nullptr;
    }
    std::string nameRaw = advance().value;
    InternedString name = pool_.intern(nameRaw);
    LUC_LOG_PARSER("Variable name: '" << nameRaw << "'");

    // Type annotation (required)
    if (!looksLikeType()) {
        errorAt(DiagCode::E2005, "expected type annotation for '" + nameRaw + "'");
        return nullptr;
    }

    TypePtr type = parseType();
    if (!type) {
        errorAt(DiagCode::E2005, "expected type for variable '" + nameRaw + "'");
        return nullptr;
    }

    // Optional initialiser
    ExprPtr init;
    if (match(TokenType::ASSIGN)) {
        if (hasExternAttr) {
            errorAt(DiagCode::E3002, 
                    "'@extern' variable '" + nameRaw + "' must not have an initialiser — "
                    "the symbol is resolved by the linker");
            // Consume the initializer expression to recover (ignore the result)
            parseExpr();
            // After the expression, consume a semicolon if present to keep the token stream clean
            match(TokenType::SEMICOLON);
        } else {
            init = parseExpr();
            if (!init) {
                errorAt(DiagCode::E2008, "expected expression after '=' in variable declaration");
            }
        }
    }

    // Warn: @extern with 'let' instead of 'const'
    if (hasExternAttr && kw == DeclKeyword::Let) {
        errorAt(DiagCode::W3001, 
                "'@extern' variable '" + nameRaw + "' should be declared as 'const', not 'let' — "
                "extern symbols are resolved permanently by the linker and cannot be reassigned");
    }

    // Additional validation: '@packed' only valid on structs
    InternedString packedStr = kw_packed;
    InternedString inlineStr = kw_inline;
    InternedString noinlineStr = kw_noinline;
    InternedString deprecatedStr = kw_deprecated;
    for (const auto& attr : attrs) {
        if (attr->name == packedStr) {
            errorAt(DiagCode::E2010, 
                    "'@packed' attribute is only valid on 'struct' declarations, not on variables");
        }
        if (attr->name == inlineStr || attr->name == noinlineStr) {
            errorAt(DiagCode::E2010, 
                    "'@" + std::string(pool_.lookup(attr->name)) + "' attribute is only valid on function declarations");
        }
        if (attr->name == deprecatedStr) {
            // @deprecated is allowed on variables - keep as warning later
            LUC_LOG_PARSER("\t'@deprecated' attribute on variable '" << nameRaw << "'");
        }
    }

    // Allocate node via arena
    auto node = arena_.make<VarDeclAST>();
    node->loc = loc;
    node->keyword = kw;
    node->name = name;  
    node->type = std::move(type);
    node->init = std::move(init);
    node->visibility = vis;
    node->attributes = std::move(attrs);   // Attach all attributes

    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseFuncDecl
//
// Grammar:
//   func_decl := decl_keyword IDENTIFIER [ generic_params ]
//               [ qualifier_list ] param_group { param_group } [ return_type ] [ '?' ]
//               '=' block
//
//   qualifier_list := { '~' IDENTIFIER }
//
// Examples:
//   let square (x int) int = { return x * x }
//   let fetch ~async (url string) string = { return await httpGet(url) }
//   let process ~async ~noinline (data []byte) []byte = { ... }
//
// The async-ness of a function is now part of its type (via ~async qualifier),
// not a separate flag on the declaration. The 'async' keyword is no longer used
// before the body.
//
// Returns a unique_ptr<FuncDeclAST> or nullptr on error.
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<FuncDeclAST> Parser::parseFuncDecl(DeclKeyword kw, Visibility vis, std::vector<AttributePtr> attrs) {
    SourceLocation loc = currentLoc();

    LUC_LOG_PARSER("=== parseFuncDecl START ===");
    LUC_LOG_PARSER("Current token (peek filtered): '" << peek().value << "' (type: " << static_cast<int>(peek().type) << ")");
    
    // Check for @extern early
    bool hasExternAttr = false;
    std::string externName;
    InternedString externStr = kw_extern;
    for (const auto& attr : attrs) {
        if (attr->name == externStr) {
            hasExternAttr = true;
            if (!attr->args.empty()) {
                const auto& arg = attr->args[0];
                if (arg->kind == AttributeArgKind::StringLit) {
                    externName = pool_.lookup(arg->value);
                }
            }
            LUC_LOG_PARSER("\t*** @extern attribute detected! C name: '" << externName << "' ***");
            break;
        }
    }

    // Name
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected function name");
        return nullptr;
    }
    std::string nameRaw = advance().value;
    InternedString name = pool_.intern(nameRaw);
    LUC_LOG_PARSER("Function name: '" << nameRaw << "'");
    
    // Allocate via arena
    auto node = arena_.make<FuncDeclAST>();
    node->loc = loc;
    node->keyword = kw;
    node->name = name;
    node->visibility = vis;
    node->attributes = std::move(attrs);

    // Optional generic params
    if (check(TokenType::LESS)) {
        LUC_LOG_PARSER("Found generic parameters");
        node->genericParams = parseGenericParams();  // already returns vector<GenericParamPtr>
    }

    // ── Parse type qualifiers (~async, ~noinline, etc.) into FuncSignature ──────
    while (check(TokenType::TILDE)) {
        advance(); // consume '~'
        
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        
        std::string qualRaw = advance().value;
        node->sig.rawQualifiers.push_back(pool_.intern(qualRaw));
        LUC_LOG_PARSER_VERBOSE("\tqualifier: '~" << qualRaw << "'");
    }

    // Parse parameter groups into FuncSignature
    LUC_LOG_PARSER("Checking for '(' in raw token stream...");
    
    // Find the next non-comment token
    std::size_t nextNonComment = pos_;
    while (nextNonComment < tokens_.size() && tokens_[nextNonComment].type == TokenType::LINE_COMMENT) {
        nextNonComment++;
    }
    
    if (nextNonComment >= tokens_.size() || tokens_[nextNonComment].type != TokenType::LPAREN) {
        LUC_LOG_PARSER("ERROR: No '(' found for function '" << nameRaw << "'");
        errorAt(DiagCode::E2001, "expected '(' to start parameter list for function '" + nameRaw + "'");
        return nullptr;
    }
    
    // If we have LINE_COMMENTs before the '(', advance through them
    while (check(TokenType::LINE_COMMENT)) {
        advance();
    }
    
    // Parse parameter groups - parseParamGroup returns vector<ASTPtr<ParamAST>>
    while (check(TokenType::LPAREN)) {
        LUC_LOG_PARSER("\tParsing parameter group at pos " << pos_);
        node->sig.paramGroups.push_back(parseParamGroup());
        LUC_LOG_PARSER("\t\tParsed " << node->sig.paramGroups.back().size() << " parameters");
    }

    LUC_LOG_PARSER("Total parameter groups: " << node->sig.paramGroups.size());
    for (size_t i = 0; i < node->sig.paramGroups.size(); i++) {
        LUC_LOG_PARSER("\tGroup " << i << " has " << node->sig.paramGroups[i].size() << " params");
        for (const auto& param : node->sig.paramGroups[i]) {
            LUC_LOG_PARSER("\t\tparam: " << pool_.lookup(param->name));
        }
    }

    // Optional return type
    LUC_LOG_PARSER("Checking for return type...");
    LUC_LOG_PARSER("\tCurrent token: '" << peek().value << "' (type: " << static_cast<int>(peek().type) << ")");

    if (looksLikeType() && !check(TokenType::ASSIGN) && !check(TokenType::SEMICOLON)) {
        LUC_LOG_PARSER("\tParsing return type...");
        node->sig.returnType = parseType();  // returns TypePtr
        if (node->sig.returnType) {
            LUC_LOG_PARSER("\t\tReturn type parsed: " << static_cast<int>(node->sig.returnType->kind));
        }
    }

    // Optional nullable function suffix '?'
    node->sig.isNullable = match(TokenType::QUESTION);

    // Handle extern vs normal function bodies
    if (hasExternAttr) {
        LUC_LOG_PARSER("Processing @extern function '" << nameRaw << "'");
        
        // Skip any LINE_COMMENTs before checking for semicolon
        while (check(TokenType::LINE_COMMENT)) advance();
        
        if (check(TokenType::SEMICOLON)) {
            LUC_LOG_PARSER("\tFound semicolon - valid extern declaration");
            advance(); // Consume semicolon
        } else if (check(TokenType::ASSIGN)) {
            LUC_LOG_PARSER("\tERROR: extern function cannot have body");
            errorAt(DiagCode::E2002, "'@extern' function '" + nameRaw + "' must not have a body");
            
            // Consume the '=' and skip the body to recover
            advance(); // consume '='
            if (check(TokenType::LBRACE)) {
                parseBlock(); // discard the block body
            } else {
                parseExpr(); // discard the expression body
            }
            // Optionally consume a following semicolon (if present)
            match(TokenType::SEMICOLON);
        } else {
            LUC_LOG_PARSER("\tWarning: extern declaration missing semicolon, current token: '" << peek().value << "'");
        }
        
        LUC_LOG_PARSER("=== parseFuncDecl END (extern) ===");
        return node;
    }

    // Non-extern functions must have '=' and body
    if (!check(TokenType::ASSIGN)) {
        LUC_LOG_PARSER("\tERROR: expected '=' before function body ");
        errorAt(DiagCode::E2001, "expected '=' before function body for '" + nameRaw + "'");
        return nullptr;
    }
    advance(); // Consume '='

    LUC_LOG_PARSER("Parsing function body...");
    
    // Parse the body - determine if block or expression
    if (check(TokenType::LBRACE)) {
        // Block body: = { ... }
        node->body = parseBlock();  // returns StmtPtr (BlockStmtAST)
    } else if (check(TokenType::LPAREN)) {
        // Anon func form with repeated signature (verbose/curried form):
        //   = (params) ret { ... }
        //   = (a int) (b int) ret { ... }   ← curried
        // The declaration header is authoritative. The helper consumes all
        // repeated param groups (loop) and validates the return type.
        validateAnonFuncBodySig(node->sig, nameRaw);
        if (!check(TokenType::LBRACE)) {
            errorAt(DiagCode::E2001, "expected '{' to start function body");
            return nullptr;
        }
        node->body = parseBlock();
    } else {
        // Expression body - function assignment: = existingFunc
        // We desugar this into a BlockStmtAST containing a ReturnStmtAST.
        SourceLocation bodyLoc = currentLoc();
        ExprPtr expr = parseExpr();
        if (!expr) {
            LUC_LOG_PARSER("\tERROR: expected expression after '='");
            errorAt(DiagCode::E2008, "expected expression after '='");
            return nullptr;
        }

        auto ret = arena_.make<ReturnStmtAST>();
        ret->loc = bodyLoc;
        ret->value = std::move(expr);

        auto block = arena_.make<BlockStmtAST>();
        block->loc = bodyLoc;
        block->stmts.push_back(std::move(ret));
        node->body = std::move(block);
    }
    
    // Optional semicolon after body (for expression bodies in certain contexts)
    if (match(TokenType::SEMICOLON)) {
        LUC_LOG_PARSER("Optional semicolon consumed after function body");
    }
    
    LUC_LOG_PARSER("=== parseFuncDecl END (normal) ===");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseParamGroup
//
// Grammar:
//   param_group := '(' [ param_list ] ')'
//   param_list  := param { [','] param } [ [','] variadic_param ]
//
// Returns the list of params for a single group. Variadic must be last.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<ASTPtr<ParamAST>> Parser::parseParamGroup() {
    LUC_LOG_PARSER_VERBOSE("parseParamGroup: parsing parameter group");
    SourceLocation loc = currentLoc();
    consume(TokenType::LPAREN, "expected '(' to start parameter group");
    
    std::vector<ASTPtr<ParamAST>> group;
    
    while (!check(TokenType::RPAREN) && !isAtEnd()) {
        match(TokenType::COMMA);
        if (check(TokenType::RPAREN)) break;

        SourceLocation paramLoc = currentLoc();

        // Parse parameter name
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected parameter name");
            break;
        }
        InternedString paramName = pool_.intern(advance().value);
        
        // Parse variadic '...' if present
        bool isVariadic = match(TokenType::VARIADIC);

        // Save position right before parsing the type
        std::size_t savedPos = pos_;
        TypePtr paramType = parseType();

        // Case 1: No progress → infinite loop risk
        if (pos_ == savedPos) {
            errorAt(DiagCode::E2005, "expected parameter type, no token consumed");
            if (!isAtEnd()) advance(); // consume offending token to avoid infinite loop
            break;
        }

        // Case 2: parseType returned an unknown type (invalid syntax)
        if (paramType->isa<UnknownTypeAST>()) {
            errorAt(DiagCode::E2005, "invalid parameter type");
            break;  // do NOT add this parameter
        }
        
        auto paramNode = arena_.make<ParamAST>();
        paramNode->loc = paramLoc;
        paramNode->name = paramName;
        paramNode->type = std::move(paramType);
        paramNode->isVariadic = isVariadic;
        group.push_back(std::move(paramNode));
    }
    
    consume(TokenType::RPAREN, "expected ')' to close parameter group");
    return group;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseGenericParams
//
// Grammar:
//   generic_params := '<' generic_param { [','] generic_param } '>'
//
// Called on the declaration side (func, struct, trait, impl, type alias).
// For the use side (call sites, named types), use parseGenericArgs() in
// ParserType.cpp.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<GenericParamPtr> Parser::parseGenericParams() {
    LUC_LOG_PARSER_VERBOSE("parseGenericParams: starting");
    std::vector<GenericParamPtr> params;

    consume(TokenType::LESS, "expected '<' to open generic parameters");

    if (check(TokenType::GREATER)) {
        advance();
        return params;
    }

    bool stalled = false;
    do {
        match(TokenType::COMMA);
        if (check(TokenType::GREATER))
            break;

        // Record position before parsing the generic parameter
        std::size_t savedPos = pos_;
        GenericParamPtr gp = parseGenericParam();

        if (!gp) {
            if (pos_ == savedPos) {
                errorAt(DiagCode::E2002, "expected generic parameter, skipping token '" + peek().value + "'");
                advance(); // consume the unexpected token to avoid infinite loop
                stalled = true;
                break;      // exit loop early to avoid cascading errors
            }
            // Continue to next iteration (loop condition will be re-evaluated)
            continue;
        }

        params.push_back(std::move(gp));

    } while (!check(TokenType::GREATER) && !isAtEnd());

    // If we stalled, we might not be at GREATER. Skip to it or to end.
    if (stalled) {
        while (!isAtEnd() && !check(TokenType::GREATER)) {
            advance();
        }
    }

    // Now consume the closing '>'
    consume(TokenType::GREATER, DiagCode::E2001, "expected '>' to close generic parameters");
    
    LUC_LOG_PARSER_VERBOSE("parseGenericParams: found " << params.size() << " generic params");
    return params;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseGenericParam
//
// Grammar:
//   generic_param   := IDENTIFIER
//                    | IDENTIFIER ':' IDENTIFIER
//                    | IDENTIFIER ':' IDENTIFIER { '+' IDENTIFIER }
//
// Examples:  T     K : Hashable     V : Hashable + Comparable
// ─────────────────────────────────────────────────────────────────────────────
GenericParamPtr Parser::parseGenericParam() {
    LUC_LOG_PARSER_VERBOSE("parseGenericParam: parsing");
    SourceLocation loc = currentLoc();

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected type parameter name");
        return nullptr;
    }
    std::string nameRaw = advance().value;
    InternedString name = pool_.intern(nameRaw);

    // Allocate via arena
    auto gp = arena_.make<GenericParamAST>(name);
    gp->loc = loc;

    // Optional constraints: ':' IDENTIFIER { '+' IDENTIFIER }
    if (match(TokenType::COLON)) {
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected trait name after ':' in generic parameter");
        } else {
            std::string traitRaw = advance().value;
            gp->constraints.push_back(pool_.intern(traitRaw));

            while (match(TokenType::PLUS)) {
                if (!check(TokenType::IDENTIFIER)) {
                    errorAt(DiagCode::E2003, "expected trait name after '+' in generic constraint");
                    break;
                }
                traitRaw = advance().value;
                gp->constraints.push_back(pool_.intern(traitRaw));
            }
        }
    }

    LUC_LOG_PARSER_VERBOSE("\tgeneric param: '" << nameRaw << "' with " << gp->constraints.size() << " constraints");
    return gp;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseStructDecl
//
// Grammar:
//   struct_decl := [ 'pub' ] 'struct' IDENTIFIER [ generic_params ]
//                  '{' { field_decl } '}'
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<StructDeclAST> Parser::parseStructDecl(Visibility vis) {
    LUC_LOG_PARSER("parseStructDecl: parsing struct");
    SourceLocation loc = currentLoc();
    consume(TokenType::STRUCT, "expected 'struct'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected struct name");
        return nullptr;
    }
    std::string name = advance().value;
    LUC_LOG_PARSER("\tstruct name: '" << name << "'");

    auto node = arena_.make<StructDeclAST>();
    node->loc = loc;
    node->name = std::move(pool_.intern(name));
    node->visibility = vis;

    // Optional generic params
    if (check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    consume(TokenType::LBRACE, "expected '{' to open struct body");

    // Parse fields until '}'
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        match(TokenType::SEMICOLON); // optional separator
        match(TokenType::COMMA);     // optional separator
        if (check(TokenType::RBRACE))
            break;

        // Harvest trailing/stacked doc comment for the field
        std::optional<DocComment> fdoc = harvestDocComment();

        FieldDeclPtr field = parseFieldDecl();
        if (field) {
            attachDoc(*field, std::move(fdoc));
            node->fields.push_back(std::move(field));
        } else {
            synchronize();
        }
    }

    consume(TokenType::RBRACE, "expected '}' to close struct body");
    LUC_LOG_PARSER("\tparsed " << node->fields.size() << " fields");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseFieldDecl
//
// Grammar:
//   field_decl := IDENTIFIER type [ '=' expr ]
// ─────────────────────────────────────────────────────────────────────────────
FieldDeclPtr Parser::parseFieldDecl() {
    SourceLocation loc = currentLoc();

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected field name");
        return nullptr;
    }
    std::string name = advance().value;

    TypePtr type = parseType();
    if (!type) {
        errorAt(DiagCode::E2005, "expected type for field '" + name + "'");
        return nullptr;
    }

    ExprPtr defaultVal;
    if (match(TokenType::ASSIGN)) {
        defaultVal = parseExpr();
        if (!defaultVal) {
            errorAt(DiagCode::E2008, "expected expression after '=' in field default value");
        }
    }

    auto field = arena_.make<FieldDeclAST>();
    field->loc = loc;
    field->name = std::move(pool_.intern(name));
    field->type = std::move(type);
    field->defaultVal = std::move(defaultVal);
    return field;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseEnumDecl
//
// Grammar:
//   enum_decl := [ 'pub' ] 'enum' IDENTIFIER '{' enum_variant { [','] enum_variant } '}'
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<EnumDeclAST> Parser::parseEnumDecl(Visibility vis) {
    LUC_LOG_PARSER("parseEnumDecl: parsing enum");
    SourceLocation loc = currentLoc();
    consume(TokenType::ENUM, "expected 'enum'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected enum name");
        return nullptr;
    }
    std::string name = advance().value;
    LUC_LOG_PARSER("\tenum name: '" << name << "'");

    auto node = arena_.make<EnumDeclAST>();
    node->loc = loc;
    node->name = std::move(pool_.intern(name));
    node->visibility = vis;

    consume(TokenType::LBRACE, "expected '{' to open enum body");

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        match(TokenType::COMMA);
        match(TokenType::SEMICOLON);
        if (check(TokenType::RBRACE))
            break;

        std::optional<DocComment> vdoc = harvestDocComment();

        EnumVariantPtr variant = parseEnumVariant();
        if (variant) {
            attachDoc(*variant, std::move(vdoc));
            node->variants.push_back(std::move(variant));
        } else {
            synchronize();
        }
    }

    consume(TokenType::RBRACE, "expected '}' to close enum body");
    LUC_LOG_PARSER("\tparsed " << node->variants.size() << " variants");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseEnumVariant
//
// Grammar:
//   enum_variant := IDENTIFIER [ '=' ( INT_LITERAL | HEX_LITERAL ) ]
//
// Explicit values may be decimal (INT_LITERAL) or hex (HEX_LITERAL) —
// both are common in Vulkan flag enums (e.g. Vertex = 0x01).
// ─────────────────────────────────────────────────────────────────────────────
EnumVariantPtr Parser::parseEnumVariant() {
    SourceLocation loc = currentLoc();

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected enum variant name");
        return nullptr;
    }
    std::string nameRaw = advance().value;
    InternedString name = pool_.intern(nameRaw);

    auto variant = arena_.make<EnumVariantAST>(name);
    variant->loc = loc;

    if (match(TokenType::ASSIGN)) {
        if (check(TokenType::INT_LITERAL) || check(TokenType::HEX_LITERAL)) {
            Token valTok = advance();
            std::string raw = valTok.value;

            // Strip underscores used as visual separators (e.g. 0xFF_FF)
            raw.erase(std::remove(raw.begin(), raw.end(), '_'), raw.end());

            int base = (valTok.type == TokenType::HEX_LITERAL) ? 16 : 10;
            char *endPtr = nullptr;
            errno = 0;
            long long val = std::strtoll(raw.c_str(), &endPtr, base);

            if (endPtr != raw.c_str() && *endPtr == '\0' && errno != ERANGE) {
                variant->explicitValue = val;
            } else {
                error(locOf(valTok), DiagCode::E2009,
                      "enum variant value '" + valTok.value + "' is not a valid integer or overflows 64-bit");
            }
        } else {
            errorAt(DiagCode::E2009, "expected integer literal after '=' in enum variant");
        }
    }

    return variant;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseTraitDecl
//
// Grammar:
//   trait_decl := [ 'pub' ] 'trait' IDENTIFIER [ generic_params ]
//                 '{' { trait_method } '}'
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<TraitDeclAST> Parser::parseTraitDecl(Visibility vis) {
    LUC_LOG_PARSER("parseTraitDecl: parsing trait");
    SourceLocation loc = currentLoc();
    consume(TokenType::TRAIT, "expected 'trait'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected trait name");
        return nullptr;
    }
    std::string name = advance().value;

    auto node = arena_.make<TraitDeclAST>();
    node->loc = loc;
    node->name = std::move(pool_.intern(name));
    node->visibility = vis;

    // Optional generic params
    if (check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    consume(TokenType::LBRACE, "expected '{' to open trait body");

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        match(TokenType::SEMICOLON);
        match(TokenType::COMMA);
        if (check(TokenType::RBRACE))
            break;

        std::optional<DocComment> mdoc = harvestDocComment();

        TraitMethodPtr method = parseTraitMethod();
        if (method) {
            attachDoc(*method, std::move(mdoc));
            node->methods.push_back(std::move(method));
        } else {
            synchronize();
        }
    }

    consume(TokenType::RBRACE, "expected '}' to close trait body");
    LUC_LOG_PARSER("\ttrait name: '" << name << "', methods: " << node->methods.size());
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseTraitMethod
//
// Grammar:
//   trait_method :=  IDENTIFIER [ qualifier_list ] '(' [ param_list ] ')' [ return_type ]
//
//   qualifier_list := { '~' IDENTIFIER }
//
// Signature only — no '=' and no body.
// Supports curried trait methods: IDENTIFIER param_group { param_group } return_type
//
// Examples:
//   draw ()
//   bounds () Rect
//   fetch ~async (url string) string
//   clamp (min int) (max int) (value int) int
// ─────────────────────────────────────────────────────────────────────────────
TraitMethodPtr Parser::parseTraitMethod() {
    SourceLocation loc = currentLoc();
    
    auto method = arena_.make<TraitMethodAST>();
    method->loc = loc;

    // Parse method name
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected trait method name");
        return nullptr;
    }
    method->name = pool_.intern(advance().value);
    
    // Parse qualifiers (~async, etc.) - store as raw strings
    while (check(TokenType::TILDE)) {
        advance();
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        method->sig.rawQualifiers.push_back(pool_.intern(advance().value));
    }
    
    // Parse parameter groups
    if (!check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' for trait method parameters");
        return nullptr;
    }
    
    while (check(TokenType::LPAREN)) {
        method->sig.paramGroups.push_back(parseParamGroup());
    }
    
    // Optional return type
    if (looksLikeType() && !check(TokenType::SEMICOLON) && !check(TokenType::RBRACE)) {
        method->sig.returnType = parseType();
    }
    
    LUC_LOG_PARSER_VERBOSE("parseTraitMethod: parsed method '" << pool_.lookup(method->name )
                           << "' with " << method->sig.paramGroups.size() << " param groups");
    return method;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseImplDecl
//
// Grammar:
//   impl_decl := [ 'pub' ] 'impl' [ generic_params ] IDENTIFIER [ generic_args ]
//                [ ':' trait_ref ] '{' { impl_member } '}'
//
//   impl_member := method_decl | from_decl
//
// Notes:
//   - generic_params on the impl itself: <T : Drawable> in  impl Scene<T : Drawable>
//   - generic_args on the struct name:   <T>            in  impl Scene<T : Drawable>
//   - trait conformance:                 : Drawable      in  impl Circle : Drawable
//   - from_decl is only valid inside pub impl — recorded as error otherwise
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<ImplDeclAST> Parser::parseImplDecl(Visibility vis) {
    LUC_LOG_PARSER("parseImplDecl: parsing impl");
    SourceLocation loc = currentLoc();
    consume(TokenType::IMPL, "expected 'impl'");

    auto node = arena_.make<ImplDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    // 1. Struct name comes first in the new syntax
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected struct name after 'impl'");
        return nullptr;
    }
    node->structName = pool_.intern(advance().value);
    LUC_LOG_PARSER("\timpl for struct: '" << pool_.lookup(node->structName) << "'");

    // 2. Optional generic params (definition style): impl Scene<T : Drawable>
    if (check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();

        // 3. Synthesis: Populate structGenericArgs with NamedTypeAST nodes.
        // This maintains the existing AST structure where ImplDeclAST expects
        // a list of type arguments to bind to the struct's parameters.
        for (const auto& gp : node->genericParams) {
            auto nt = arena_.make<NamedTypeAST>(gp->name);
            nt->loc = gp->loc;
            node->structGenericArgs.push_back(std::move(nt));
        }
    }

    // 4. Optional trait conformance: ':' trait_ref
    if (check(TokenType::COLON)) {
        node->traitRef = parseTraitRef();
    }

    consume(TokenType::LBRACE, "expected '{' to open impl body");

    // Parse impl members
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        match(TokenType::SEMICOLON);
        match(TokenType::COMMA);
        if (check(TokenType::RBRACE))
            break;

        std::optional<DocComment> mdoc = harvestDocComment();

        // method_decl — regular method body
        if (check(TokenType::IDENTIFIER)) {
            MethodDeclPtr md = parseMethodDecl();
            if (md) {
                attachDoc(*md, std::move(mdoc));
                node->methods.push_back(std::move(md));
            } else {
                synchronize();
            }
            continue;
        }

        // Unrecognised token inside impl block
        errorAt(DiagCode::E2002, "expected method declaration inside impl block");
        synchronize();
    }

    consume(TokenType::RBRACE, "expected '}' to close impl body");
    LUC_LOG_PARSER("\tparsed " << node->methods.size() << " methods");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseTraitRef
//
// Grammar:
//   trait_ref := ':' IDENTIFIER [ generic_args ]
//
// Called when ':' is seen in an impl header. Consumes the ':' itself.
// ─────────────────────────────────────────────────────────────────────────────
TraitRefPtr Parser::parseTraitRef() {
    SourceLocation loc = currentLoc();
    consume(TokenType::COLON, "expected ':' before trait name");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected trait name after ':'");
        return nullptr;
    }

    auto ref = arena_.make<TraitRefAST>();
    ref->loc = loc;
    ref->name = pool_.intern(advance().value);

    // Optional generic arguments: Comparable<int>
    if (check(TokenType::LESS)) {
        ref->genericArgs = parseGenericArgs();
    }

    return ref;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseMethodDecl
//
// Grammar:
//   method_decl := IDENTIFIER [ qualifier_list ] param_group { param_group } [ return_type ] '=' func_body
//
//   qualifier_list := { '~' IDENTIFIER }
//
// Supports curried methods:
//   length () float = { ... }
//   dot (other Vec2) float = { ... }
//   clamp (min int) (max int) (value int) int = { ... }
//   fetch ~async (url string) string = { return await httpGet(url) }
//
// No visibility prefix per method — visibility comes from the enclosing impl block.
// Async-ness is now specified via ~async qualifier, not by 'async' keyword before body.
// ─────────────────────────────────────────────────────────────────────────────
MethodDeclPtr Parser::parseMethodDecl() {
    SourceLocation loc = currentLoc();

    auto method = arena_.make<MethodDeclAST>();
    method->loc = loc;

    // Method name
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected method name");
        return nullptr;
    }
    method->name = pool_.intern(advance().value);

    // ── Parse type qualifiers (~async, ~noinline, etc.) into FuncSignature ──────
    while (check(TokenType::TILDE)) {
        advance(); // consume '~'
        
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        
        method->sig.rawQualifiers.push_back(pool_.intern(advance().value));
        LUC_LOG_PARSER_VERBOSE("\tmethod qualifier: '~" << pool_.lookup(method->sig.rawQualifiers.back()) << "'");
    }

    // Parse one or more parameter groups (curried method support)
    if (!check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' to start parameter list for method '" + std::string( pool_.lookup(method->name)) + "'");
        return nullptr;
    }
    
    while (check(TokenType::LPAREN)) {
        method->sig.paramGroups.push_back(parseParamGroup());
    }

    // Optional return type
    if (looksLikeType() && !check(TokenType::ASSIGN)) {
        method->sig.returnType = parseType();
    }

    if (!check(TokenType::ASSIGN)) {
        errorAt(DiagCode::E2001, "expected '=' before method body for '" + std::string(pool_.lookup(method->name)) + "'");
        return nullptr;
    }
    advance(); // Consume '='

    LUC_LOG_PARSER("parseMethodDecl: parsing body for method '" << pool_.lookup(method->name) << "'");

    // Determine body type
    if (check(TokenType::LBRACE)) {
        // Block body: = { ... }
        method->body = parseBlock();
        LUC_LOG_PARSER("parseMethodDecl: block body");
    } else if (check(TokenType::LPAREN)) {
        // Anon func form with repeated signature (verbose/curried form):
        //   = (params) ret { ... }
        //   = (a int) (b int) ret { ... }   ← curried
        std::string methodName = std::string(pool_.lookup(method->name));
        validateAnonFuncBodySig(method->sig, methodName);
        if (!check(TokenType::LBRACE)) {
            errorAt(DiagCode::E2001, "expected '{' to start method body");
            return nullptr;
        }
        method->body = parseBlock();
        LUC_LOG_PARSER("parseMethodDecl: anon func body");
    } else {
        // Expression body - method assignment: = existingFunc
        SourceLocation bodyLoc = currentLoc();
        ExprPtr expr = parseExpr();
        if (!expr) {
            errorAt(DiagCode::E2008, "expected expression after '=' for method '" + std::string(pool_.lookup(method->name)) + "'");
            return nullptr;
        }

        auto ret = arena_.make<ReturnStmtAST>();
        ret->loc = bodyLoc;
        ret->value = std::move(expr);

        auto block = arena_.make<BlockStmtAST>();
        block->loc = bodyLoc;
        block->stmts.push_back(std::move(ret));
        method->body = std::move(block);

        LUC_LOG_PARSER("parseMethodDecl: expression body");
    }

    // Optional semicolon after body (for expression bodies in certain contexts)
    if (match(TokenType::SEMICOLON)) {
        LUC_LOG_PARSER("parseMethodDecl: optional semicolon consumed");
    }

    LUC_LOG_PARSER("parseMethodDecl: success for method '" << std::string(pool_.lookup(method->name)) << "'");
    return method;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseFromDecl
//
// Grammar:
//   from_block  := [ visibility_mod ] 'from' IDENTIFIER '{' from_entry* '}'
//
//   from_entry  := param_group { param_group } IDENTIFIER '=' func_body
//                  -- one or more param groups   return type
//
// Supports curried conversions:
//   (c Celsius) Fahrenheit = { ... }
//   (c Celsius) (scale float) Fahrenheit = { ... }
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<FromDeclAST> Parser::parseFromDecl(Visibility vis) {
    SourceLocation loc = currentLoc();
    consume(TokenType::FROM, "expected 'from'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected target struct name after 'from'");
        return nullptr;
    }
    std::string targetName = advance().value;

    auto node = arena_.make<FromDeclAST>();
    node->loc = loc;
    node->visibility = vis;
    node->targetTypeName = pool_.intern(targetName);

    consume(TokenType::LBRACE, "expected '{' after target name '" + targetName + "'");

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        match(TokenType::SEMICOLON);
        match(TokenType::COMMA);
        if (check(TokenType::RBRACE))
            break;

        SourceLocation entryLoc = currentLoc();

        // Parse one or more parameter groups
        if (!check(TokenType::LPAREN)) {
            errorAt(DiagCode::E2001, "expected '(' to start parameter list for conversion entry");
            synchronize();
            // If we landed on a declaration start or RBRACE, abort this block
            if (looksLikeDeclStart() || check(TokenType::RBRACE))
                break;
            continue;
        }

        auto entry = arena_.make<FromEntryAST>();
        entry->loc = entryLoc;

        while (check(TokenType::LPAREN)) {
            entry->sig.paramGroups.push_back(parseParamGroup());
        }

        // Return type name
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected target type name after parameter list");
            synchronize();
            if (looksLikeDeclStart() || check(TokenType::RBRACE))
                break;
            continue;
        }
        entry->returnTypeName = pool_.intern(advance().value);

        if (!check(TokenType::ASSIGN)) {
            errorAt(DiagCode::E2001, "expected '=' before body for conversion entry");
            synchronize();
            if (looksLikeDeclStart() || check(TokenType::RBRACE))
                break;
            continue;
        }
        advance();

        // Normalized body parsing
        SourceLocation bodyLoc = currentLoc();
        if (check(TokenType::LBRACE)) {
            entry->body = parseBlock();
        } else {
            // Expression body: = expr
            ExprPtr expr = parseExpr();
            if (expr) {
                auto ret = arena_.make<ReturnStmtAST>();
                ret->loc = bodyLoc;
                ret->value = std::move(expr);
                
                auto block = arena_.make<BlockStmtAST>();
                block->loc = bodyLoc;
                block->stmts.push_back(std::move(ret));
                entry->body = std::move(block);
            } else {
                errorAt(DiagCode::E2008, "expected expression after '=' in conversion entry");
            }
        }

        node->entries.push_back(std::move(entry));
    }

    consume(TokenType::RBRACE, "expected '}' to close from block");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseTypeAliasDecl
//
// Grammar:
//   type_decl := [ 'pub' ] 'type' IDENTIFIER [ generic_params ] '=' type
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<TypeAliasDeclAST> Parser::parseTypeAliasDecl(Visibility vis) {
    SourceLocation loc = currentLoc();
    consume(TokenType::TYPE, "expected 'type'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected type alias name");
        return nullptr;
    }
    std::string name = advance().value;

    auto node = arena_.make<TypeAliasDeclAST>();
    node->loc = loc;
    node->name = std::move(pool_.intern(name));
    node->visibility = vis;

    // Optional generic params: type Transform<T> = (value T) T
    if (check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    consume(TokenType::ASSIGN, "expected '=' in type alias");

    node->aliasedType = parseType();
    if (!node->aliasedType) {
        errorAt(DiagCode::E2005, "expected type on the right-hand side of type alias");
    }

    return node;
}
