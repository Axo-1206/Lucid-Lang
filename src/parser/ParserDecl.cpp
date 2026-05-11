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
// validateAnonFuncBodySig
//
// Shared helper for parseFuncDecl and parseMethodDecl.
//
// When a function/method body is written in the verbose anon‑func form:
//   = (params) -> ret { ... }   or   = (a int)(b int) -> ret { ... }  (curried)
//
// the repeated parameter groups and optional return list must be consumed.
// This helper does exactly that, then validates the repeated return list
// against the already-declared signature (the declaration is authoritative).
//
// Anonymous functions cannot have qualifiers (~async, ~nullable, ~parallel) –
// they are plain values.  They also cannot be marked nullable with '?'.
// This helper rejects both.
//
// Cursor contract:
//   On entry : positioned at the first token after '=' (may be '(' or '{').
//   On return: positioned after the repeated signature (at '{' or a bad token).
// ─────────────────────────────────────────────────────────────────────────────
void Parser::validateAnonFuncBodySig(FuncSignature& declaredSig, const std::string& declName) {
    // ── 0. Anonymous functions cannot have qualifiers ─────────────────────
    if (check(TokenType::TILDE)) {
        errorAt(DiagCode::E2002,
                "anonymous function body cannot have qualifiers (e.g., ~async, ~nullable). "
                "Qualifiers belong on the declaration itself.");
        // Skip the qualifier(s) to recover
        while (check(TokenType::TILDE)) {
            advance(); // consume '~'
            if (!check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected qualifier name after '~'");
                break;
            }
            advance(); // consume qualifier name
        }
    }

    // ── 1. Consume ALL repeated parameter groups (curried) ─────────────────
    while (check(TokenType::LPAREN)) {
        parseParamGroup(); // discard – declared signature is authoritative
    }

    // ── 2. Optional '->' and return list ─────────────────────────────────
    bool hasArrow = match(TokenType::ARROW);
    std::vector<TypePtr> repeatedReturnTypes;

    if (hasArrow) {
        // Parse one or more return types separated by commas
        do {
            if (check(TokenType::LPAREN)) {
                // Nested function type (curried return)
                TypePtr t = parseFuncType();
                if (t) repeatedReturnTypes.push_back(std::move(t));
                else errorAt(DiagCode::E2005, "expected return type after '->'");
            } else {
                TypePtr t = parseType();
                if (t) repeatedReturnTypes.push_back(std::move(t));
                else errorAt(DiagCode::E2005, "expected return type after '->'");
            }
        } while (match(TokenType::COMMA));
    }

    // ── 3. Validate or adopt return types ────────────────────────────────
    // Case A: Declaration has no return types (void function)
    if (declaredSig.returnTypes.empty()) {
        if (hasArrow) {
            if (!repeatedReturnTypes.empty()) {
                // Adopt the repeated list (the semantic pass may still warn)
                LUC_LOG_PARSER("validateAnonFuncBodySig: adopting repeated return types for '"
                               << declName << "'");
                declaredSig.returnTypes = std::move(repeatedReturnTypes);
            } else {
                errorAt(DiagCode::E2005,
                        "expected at least one return type after '->'");
            }
        }
        // else: both are void → nothing to do
    }
    // Case B: Both have return types – compare
    else if (hasArrow) {
        if (declaredSig.returnTypes.size() != repeatedReturnTypes.size()) {
            errorAt(DiagCode::E2005,
                    "repeated return type count (" + std::to_string(repeatedReturnTypes.size()) +
                    ") does not match declared count (" + std::to_string(declaredSig.returnTypes.size()) +
                    ") for '" + declName + "'");
        } else {
            // Simple equality check (type category)
            for (size_t i = 0; i < declaredSig.returnTypes.size(); ++i) {
                if (declaredSig.returnTypes[i]->kind != repeatedReturnTypes[i]->kind) {
                    errorAt(DiagCode::W3001,
                            "repeated return type #" + std::to_string(i) +
                            " in body does not match declared return type for '" +
                            declName + "'; the declaration is authoritative");
                    break;
                }
            }
        }
    }
    // Case C: Declaration has return types but repeated signature omitted '->'
    else {
        errorAt(DiagCode::E2001,
                "expected '->' return list in repeated signature for function '" +
                declName + "' because the declaration has a return type");
    }

    // ── 4. Anonymous function cannot be nullable ──────────────────────────
    if (match(TokenType::QUESTION)) {
        errorAt(DiagCode::E2007,
                "anonymous function body cannot be marked nullable with '?'. "
                "Use the '~nullable' qualifier on the function declaration instead.");
    }
}

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
//                [ qualifier_list ] param_group { param_group }
//                [ '->' return_list ]
//                '=' func_body
//
//   qualifier_list := { '~' IDENTIFIER }            -- ~async, ~nullable, ~parallel
//   return_list     := return_type { ',' return_type }
//   return_type     := type
//                    | param_group { param_group } '->' return_list   -- curried return
//
// Examples:
//   let square (x int) -> int = { return x * x }
//   let fetch ~async (url string) -> string = { return await httpGet(url) }
//   let process ~async (data []byte) -> []byte = { ... }
//   let parse (src string) -> int, string = { ... }
//   let add (a int)(b int) -> int = { return a + b }
//   let f (a int) -> int, (s string)(n float) -> int = { ... }
//
// Returns a FuncDeclAST or nullptr on error.
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
        node->genericParams = parseGenericParams();
    }

    // ── Parse type qualifiers (~async, ~nullable, ~parallel) ─────────────────
    uint32_t qualifiersMask = 0;
    while (check(TokenType::TILDE)) {
        advance(); // consume '~'
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        std::string qualRaw = advance().value;
        if (!QualifierRegistry::instance().applyQualifier(qualifiersMask, qualRaw)) {
            errorAt(DiagCode::E2003,
                    "unknown type qualifier '~" + qualRaw + "'; known: " +
                    QualifierRegistry::instance().allNames());
        }
        node->sig.rawQualifiers.push_back(pool_.intern(qualRaw));
        LUC_LOG_PARSER_VERBOSE("\tqualifier: '~" << qualRaw << "'");
    }
    node->sig.qualifiers = qualifiersMask;

    // Parse parameter groups
    LUC_LOG_PARSER("Checking for '(' in raw token stream...");
    
    std::size_t nextNonComment = pos_;
    while (nextNonComment < tokens_.size() && tokens_[nextNonComment].type == TokenType::LINE_COMMENT) {
        nextNonComment++;
    }
    
    if (nextNonComment >= tokens_.size() || tokens_[nextNonComment].type != TokenType::LPAREN) {
        LUC_LOG_PARSER("ERROR: No '(' found for function '" << nameRaw << "'");
        errorAt(DiagCode::E2001, "expected '(' to start parameter list for function '" + nameRaw + "'");
        return nullptr;
    }
    
    // Skip comments before '('
    while (check(TokenType::LINE_COMMENT)) advance();
    
    // Parse parameter groups
    while (check(TokenType::LPAREN)) {
        LUC_LOG_PARSER("\tParsing parameter group at pos " << pos_);
        node->sig.paramGroups.push_back(parseParamGroup());
        LUC_LOG_PARSER("\t\tParsed " << node->sig.paramGroups.back().size() << " parameters");
    }

    LUC_LOG_PARSER("Total parameter groups: " << node->sig.paramGroups.size());

    // ── Parse return list after '->' ───────────────────────────────────────
    if (match(TokenType::ARROW)) {
        node->sig.returnTypes = parseReturnList();
        LUC_LOG_PARSER("\tParsed " << node->sig.returnTypes.size() << " return type(s)");
    } else {
        // Void function – no return types
        LUC_LOG_PARSER("\tNo return types (void function)");
    }

    // ── Handle extern vs normal function bodies ─────────────────────────────
    if (hasExternAttr) {
        LUC_LOG_PARSER("Processing @extern function '" << nameRaw << "'");
        
        while (check(TokenType::LINE_COMMENT)) advance();
        
        if (check(TokenType::SEMICOLON)) {
            LUC_LOG_PARSER("\tFound semicolon - valid extern declaration");
            advance();
        } else if (check(TokenType::ASSIGN)) {
            LUC_LOG_PARSER("\tERROR: extern function cannot have body");
            errorAt(DiagCode::E2002, "'@extern' function '" + nameRaw + "' must not have a body");
            
            // Skip the '=' and the body to recover
            advance(); // consume '='
            if (check(TokenType::LBRACE)) {
                parseBlock(); // discard block body
            } else {
                parseExpr(); // discard expression body
            }
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
    
    // Parse the body - block, anon‑func (verbose), or expression
    if (check(TokenType::LBRACE)) {
        // Block body: = { ... }
        node->body = parseBlock();
    } else if (check(TokenType::LPAREN)) {
        // Verbose anon-func form: = (params) -> ret { ... }
        // The declaration header is authoritative. The helper consumes the
        // repeated signature and validates it against the declared signature.
        validateAnonFuncBodySig(node->sig, nameRaw);
        if (!check(TokenType::LBRACE)) {
            errorAt(DiagCode::E2001, "expected '{' to start function body");
            return nullptr;
        }
        node->body = parseBlock();
    } else {
        // Expression body: = existingFunc
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
    
    // Optional semicolon after body (for expression bodies)
    if (match(TokenType::SEMICOLON)) {
        LUC_LOG_PARSER("Optional semicolon consumed after function body");
    }
    
    LUC_LOG_PARSER("=== parseFuncDecl END (normal) ===");
    return node;
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
//   trait_method := IDENTIFIER [ qualifier_list ] param_group { param_group }
//                  [ '->' return_list ]
//
//   qualifier_list := { '~' IDENTIFIER }            -- ~async, ~nullable, ~parallel
//   return_list     := return_type { ',' return_type }
//   return_type     := type
//                    | param_group { param_group } '->' return_list   -- curried return
//
// Signature only — no '=' and no body.
// Supports curried trait methods and multiple returns.
// Examples:
//   draw ()
//   bounds () -> Rect
//   fetch ~async (url string) -> string
//   clamp (min int)(max int)(value int) -> int
//   process (data []byte) -> int, string, bool
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
    
    // ── Parse type qualifiers (~async, ~nullable, ~parallel) ──────────────
    uint32_t qualifiersMask = 0;
    while (check(TokenType::TILDE)) {
        advance(); // consume '~'
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        std::string qualRaw = advance().value;
        if (!QualifierRegistry::instance().applyQualifier(qualifiersMask, qualRaw)) {
            errorAt(DiagCode::E2003,
                    "unknown type qualifier '~" + qualRaw + "'; known: " +
                    QualifierRegistry::instance().allNames());
        }
        method->sig.rawQualifiers.push_back(pool_.intern(qualRaw));
        LUC_LOG_PARSER_VERBOSE("\tqualifier: '~" << qualRaw << "'");
    }
    method->sig.qualifiers = qualifiersMask;
    
    // ── Parse parameter groups ────────────────────────────────────────────
    if (!check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' for trait method parameters");
        return nullptr;
    }
    
    while (check(TokenType::LPAREN)) {
        method->sig.paramGroups.push_back(parseParamGroup());
    }
    
    // ── Parse return types after '->' (if present) ───────────────────────
    if (match(TokenType::ARROW)) {
        method->sig.returnTypes = parseReturnList();
        LUC_LOG_PARSER_VERBOSE("\tparsed " << method->sig.returnTypes.size() << " return type(s)");
    } else {
        // void trait method (no return types)
        LUC_LOG_PARSER_VERBOSE("\tvoid trait method (no return types)");
    }
    
    LUC_LOG_PARSER_VERBOSE("parseTraitMethod: parsed method '" << pool_.lookup(method->name)
                           << "' with " << method->sig.paramGroups.size() << " param group(s)");
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
//   method_decl := IDENTIFIER [ qualifier_list ] param_group { param_group }
//                 [ '->' return_list ] '=' func_body
//
//   qualifier_list := { '~' IDENTIFIER }            -- ~async, ~nullable, ~parallel
//   return_list     := return_type { ',' return_type }
//   return_type     := type
//                    | param_group { param_group } '->' return_list
//
// Supports curried methods and multiple returns.
// Examples:
//   length () -> float = { ... }
//   dot (other Vec2) -> float = { ... }
//   clamp (min int)(max int)(value int) -> int = { ... }
//   fetch ~async (url string) -> string = { return await httpGet(url) }
//   process (data []byte) -> int, string = { ... }
//
// No visibility prefix per method — visibility comes from the enclosing impl block.
// ● The ~nullable qualifier marks the method binding as nullable (caller must guard).
// ● '?' is not used on function types – use ~nullable instead.
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

    // ── Parse type qualifiers (~async, ~nullable, ~parallel) ────────────────
    uint32_t qualifiersMask = 0;
    while (check(TokenType::TILDE)) {
        advance(); // consume '~'
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        std::string qualRaw = advance().value;
        if (!QualifierRegistry::instance().applyQualifier(qualifiersMask, qualRaw)) {
            errorAt(DiagCode::E2003,
                    "unknown type qualifier '~" + qualRaw + "'; known: " +
                    QualifierRegistry::instance().allNames());
        }
        method->sig.rawQualifiers.push_back(pool_.intern(qualRaw));
        LUC_LOG_PARSER_VERBOSE("\tmethod qualifier: '~" << qualRaw << "'");
    }
    method->sig.qualifiers = qualifiersMask;

    // Parse one or more parameter groups (curried method support)
    if (!check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' to start parameter list for method '" +
                std::string(pool_.lookup(method->name)) + "'");
        return nullptr;
    }

    while (check(TokenType::LPAREN)) {
        method->sig.paramGroups.push_back(parseParamGroup());
    }

    // ── Parse return types after '->' (if present) ─────────────────────────
    if (match(TokenType::ARROW)) {
        method->sig.returnTypes = parseReturnList();
        LUC_LOG_PARSER_VERBOSE("\tparsed " << method->sig.returnTypes.size() << " return type(s)");
    } else {
        // Void method (no return types)
        LUC_LOG_PARSER_VERBOSE("\tvoid method (no return types)");
    }

    // Must have '=' body
    if (!check(TokenType::ASSIGN)) {
        errorAt(DiagCode::E2001, "expected '=' before method body for '" +
                std::string(pool_.lookup(method->name)) + "'");
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
        // Verbose anon-func form: = (params) -> ret { ... }  or curried form
        // The declaration header is authoritative. The helper consumes the
        // repeated signature and validates it against the declared signature.
        std::string methodName = std::string(pool_.lookup(method->name));
        validateAnonFuncBodySig(method->sig, methodName);
        if (!check(TokenType::LBRACE)) {
            errorAt(DiagCode::E2001, "expected '{' to start method body");
            return nullptr;
        }
        method->body = parseBlock();
        LUC_LOG_PARSER("parseMethodDecl: verbose anon-func body");
    } else {
        // Expression body: = existingFunc
        SourceLocation bodyLoc = currentLoc();
        ExprPtr expr = parseExpr();
        if (!expr) {
            errorAt(DiagCode::E2008, "expected expression after '=' for method '" +
                    std::string(pool_.lookup(method->name)) + "'");
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
