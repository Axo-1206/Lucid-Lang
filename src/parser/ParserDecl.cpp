/**
 * @file ParserDecl.cpp
 *
 * @responsibility Parses all top-level declarations (let, func, struct, etc.).
 *
 * @related src/diagnostics/DiagnosticEngine.hpp, DiagnosticCodes.hpp
 */

#include "Parser.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// ParserDecl.cpp
//
// Implements every declaration parse function declared in Parser.hpp.
// This file handles all top-level constructs: use, module, var, func, struct,
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
//   parseFuncBody()          — '=' ( block | anon_func | async … )
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
std::vector<AttributePtr> Parser::parseAttributes() {
    LUC_LOG_PARSER_VERBOSE("parseAttributes: starting");
    std::vector<AttributePtr> attrs;
    while (check(TokenType::AT_SIGN)) {
        LUC_LOG_PARSER_VERBOSE("\tFound '@', parsing attribute");
        AttributePtr attr = parseAttribute();
        if (attr) {
            LUC_LOG_PARSER_VERBOSE("\t\tParsed attribute: @" << attr->name);
            attrs.push_back(std::move(attr));
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

    auto attr = std::make_unique<AttributeAST>();
    attr->loc  = loc;
    attr->name = advance().value; // consume attribute name

    // Optional argument list: '(' attr_arg { ',' attr_arg } ')'
    if (match(TokenType::LPAREN)) {
        while (!check(TokenType::RPAREN) && !isAtEnd()) {
            match(TokenType::COMMA); // optional separator
            if (check(TokenType::RPAREN)) break;

            SourceLocation argLoc = currentLoc();
            AttributeArgAST arg;
            arg.loc = argLoc;

            if (check(TokenType::STRING_LITERAL)) {
                arg.argKind = AttributeArgAST::ArgKind::StringLit;
                arg.value   = advance().value;
            } else if (checkAny({TokenType::INT_LITERAL, TokenType::HEX_LITERAL,
                                  TokenType::BINARY_LITERAL})) {
                arg.argKind = AttributeArgAST::ArgKind::IntLit;
                arg.value   = advance().value;
            } else if (check(TokenType::TRUE)) {
                arg.argKind = AttributeArgAST::ArgKind::BoolLit;
                arg.value   = "true";
                advance();
            } else if (check(TokenType::FALSE)) {
                arg.argKind = AttributeArgAST::ArgKind::BoolLit;
                arg.value   = "false";
                advance();
            } else if (check(TokenType::IDENTIFIER)) {
                // Type identifier: @sizeof(Vec2), @extern("sym", "C")
                arg.argKind = AttributeArgAST::ArgKind::TypeIdent;
                arg.value   = advance().value;
            } else {
                errorAt(DiagCode::E2009,
                        "attribute argument must be a string, integer, boolean, or type name");
                // Skip to closing paren.
                while (!check(TokenType::RPAREN) && !isAtEnd()) advance();
                break;
            }

            attr->args.push_back(std::move(arg));
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
std::unique_ptr<PackageDeclAST> Parser::parsePackageDecl() {
    SourceLocation loc = currentLoc();
    consume(TokenType::PACKAGE, "expected 'package'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected package name");
        // Return a dummy node so the parser can continue without crashing
        auto node = std::make_unique<PackageDeclAST>("<error>");
        node->loc = loc;
        return node;
    }
    std::string name = advance().value;

    auto node = std::make_unique<PackageDeclAST>(std::move(name));
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
std::unique_ptr<UseDeclAST> Parser::parseUseDecl(Visibility vis) {
    SourceLocation loc = currentLoc();
    consume(TokenType::USE, "expected 'use'");

    auto node = std::make_unique<UseDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    // Parse module path: IDENTIFIER { '.' IDENTIFIER }
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected module path after 'use'");
        return node;
    }

    node->path.push_back(advance().value);

    while (check(TokenType::DOT) && peekNext().type == TokenType::IDENTIFIER) {
        advance(); // consume '.'
        node->path.push_back(advance().value);
    }

    // Optional alias: 'as' IDENTIFIER
    if (match(TokenType::AS)) {
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected alias name after 'as'");
        } else {
            node->alias = advance().value;
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
std::unique_ptr<VarDeclAST> Parser::parseVarDecl(Visibility vis, std::vector<AttributePtr> attrs) {
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
    for (const auto& attr : attrs) {
        if (attr->name == "extern") {
            hasExternAttr = true;
            if (!attr->args.empty()) {
                const auto& arg = attr->args[0];
                if (arg.argKind == AttributeArgAST::ArgKind::StringLit) {
                    externName = arg.value;
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
    std::string name = advance().value;
    LUC_LOG_PARSER("Variable name: '" << name << "'");

    // Type annotation (required)
    if (!looksLikeType()) {
        errorAt(DiagCode::E2005, "expected type annotation for '" + name + "'");
        return nullptr;
    }

    TypePtr type = parseType();
    if (!type) {
        errorAt(DiagCode::E2005, "expected type for variable '" + name + "'");
        return nullptr;
    }

    // Optional initialiser
    ExprPtr init;
    if (match(TokenType::ASSIGN)) {
        if (hasExternAttr) {
            errorAt(DiagCode::E3002, 
                    "'@extern' variable '" + name + "' must not have an initialiser — "
                    "the symbol is resolved by the linker");
            // Skip the initialiser to recover
            int parenDepth = 0;
            while (!isAtEnd() && !check(TokenType::SEMICOLON) && 
                   !check(TokenType::RBRACE) && 
                   !(parenDepth == 0 && checkAny({TokenType::SEMICOLON, TokenType::RBRACE}))) {
                if (check(TokenType::LPAREN)) parenDepth++;
                else if (check(TokenType::RPAREN) && parenDepth > 0) parenDepth--;
                advance();
            }
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
                "'@extern' variable '" + name + "' should be declared as 'const', not 'let' — "
                "extern symbols are resolved permanently by the linker and cannot be reassigned");
    }

    // Additional validation: '@packed' only valid on structs
    for (const auto& attr : attrs) {
        if (attr->name == "packed") {
            errorAt(DiagCode::E2010, 
                    "'@packed' attribute is only valid on 'struct' declarations, not on variables");
        }
        if (attr->name == "inline" || attr->name == "noinline") {
            errorAt(DiagCode::E2010, 
                    "'@" + attr->name + "' attribute is only valid on function declarations");
        }
        if (attr->name == "deprecated") {
            // @deprecated is allowed on variables - keep as warning later
            LUC_LOG_PARSER("\t'@deprecated' attribute on variable '" << name << "'");
        }
    }

    auto node = std::make_unique<VarDeclAST>();
    node->loc = loc;
    node->keyword = kw;
    node->name = std::move(name);
    node->type = std::move(type);
    node->init = std::move(init);
    node->visibility = vis;
    node->attributes = std::move(attrs);  // Attach all attributes

    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseFuncDecl
//
// Grammar:
//   func_decl := decl_keyword IDENTIFIER [ generic_params ]
//                param_group { param_group } [ return_type ]
//                '=' func_body
//
// Multiple param_groups = curried function.
// Called from Parser.cpp after the keyword has been consumed.
// pos_ is on the IDENTIFIER (function name).
// ─────────────────────────────────────────────────────────────────────────────
std::unique_ptr<FuncDeclAST> Parser::parseFuncDecl(DeclKeyword kw, Visibility vis, std::vector<AttributePtr> attrs) {
    SourceLocation loc = currentLoc();

    LUC_LOG_PARSER("=== parseFuncDecl START ===");
    LUC_LOG_PARSER("Current token (peek filtered): '" << peek().value << "' (type: " << static_cast<int>(peek().type) << ")");
    
    // Check for @extern early
    bool hasExternAttr = false;
    std::string externName;
    for (const auto& attr : attrs) {
        if (attr->name == "extern") {
            hasExternAttr = true;
            if (!attr->args.empty()) {
                const auto& arg = attr->args[0];
                if (arg.argKind == AttributeArgAST::ArgKind::StringLit) {
                    externName = arg.value;
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
    std::string name = advance().value;
    LUC_LOG_PARSER("Function name: '" << name << "'");
    
    // DEBUG: Show raw token at current position (after consuming name)
    std::size_t rawPos = pos_;
    while (rawPos < tokens_.size() && tokens_[rawPos].type == TokenType::LINE_COMMENT) rawPos++;
    if (rawPos < tokens_.size()) {
        LUC_LOG_PARSER("Raw token at position " << rawPos << ": '" << tokens_[rawPos].value 
                       << "' (type: " << static_cast<int>(tokens_[rawPos].type) << ")");
    }

    auto node = std::make_unique<FuncDeclAST>();
    node->loc = loc;
    node->keyword = kw;
    node->name = std::move(name);
    node->visibility = vis;
    node->attributes = std::move(attrs);

    // Optional generic params
    if (check(TokenType::LESS)) {
        LUC_LOG_PARSER("Found generic parameters");
        node->genericParams = parseGenericParams();
    }

    // Parse parameter groups
    LUC_LOG_PARSER("Checking for '(' in raw token stream...");
    
    // Find the next non-comment token
    std::size_t nextNonComment = pos_;
    while (nextNonComment < tokens_.size() && tokens_[nextNonComment].type == TokenType::LINE_COMMENT) {
        nextNonComment++;
    }
    
    if (nextNonComment >= tokens_.size() || tokens_[nextNonComment].type != TokenType::LPAREN) {
        LUC_LOG_PARSER("ERROR: No '(' found for function '" << node->name << "'");
        errorAt(DiagCode::E2001, "expected '(' to start parameter list for function '" + node->name + "'");
        return nullptr;
    }
    
    // If we have LINE_COMMENTs before the '(', advance through them
    while (check(TokenType::LINE_COMMENT)) {
        advance();
    }
    
    // Parse parameter groups
    while (check(TokenType::LPAREN)) {
        LUC_LOG_PARSER("\tParsing parameter group at pos " << pos_);
        std::vector<ParamPtr> paramGroup = parseParamGroup();
        LUC_LOG_PARSER("\t\tParsed " << paramGroup.size() << " parameters");
        node->paramGroups.push_back(std::move(paramGroup));
    }

    LUC_LOG_PARSER("Total parameter groups: " << node->paramGroups.size());
    for (size_t i = 0; i < node->paramGroups.size(); i++) {
        LUC_LOG_PARSER("\tGroup " << i << " has " << node->paramGroups[i].size() << " params");
        for (const auto& param : node->paramGroups[i]) {
            if (param) {
                LUC_LOG_PARSER("\t\tparam: " << param->name);
            }
        }
    }

    // Optional return type
    LUC_LOG_PARSER("Checking for return type...");
    LUC_LOG_PARSER("\tCurrent token: '" << peek().value << "' (type: " << static_cast<int>(peek().type) << ")");

    if (looksLikeType() && !check(TokenType::ASSIGN) && !check(TokenType::SEMICOLON)) {
        LUC_LOG_PARSER("\tParsing return type...");
        node->returnType = parseType();
        if (node->returnType) {
            LUC_LOG_PARSER("\t\tReturn type parsed: " << static_cast<int>(node->returnType->kind));
        }
    }

    // Handle extern vs normal function bodies
    if (hasExternAttr) {
        LUC_LOG_PARSER("Processing @extern function '" << node->name << "'");
        
        // Skip any LINE_COMMENTs before checking for semicolon
        while (check(TokenType::LINE_COMMENT)) advance();
        
        if (check(TokenType::SEMICOLON)) {
            LUC_LOG_PARSER("\tFound semicolon - valid extern declaration");
            advance(); // Consume semicolon
        } else if (check(TokenType::ASSIGN)) {
            LUC_LOG_PARSER("\tERROR: extern function cannot have body");
            errorAt(DiagCode::E2002, "'@extern' function '" + node->name + "' must not have a body");
        } else {
            LUC_LOG_PARSER("\tWarning: extern declaration missing semicolon, current token: '" << peek().value << "'");
        }
        
        LUC_LOG_PARSER("=== parseFuncDecl END (extern) ===");
        return node;
    }

    // Non-extern functions must have '=' and body
    if (!check(TokenType::ASSIGN)) {
        LUC_LOG_PARSER("\tERROR: expected '=' before function body ");
        errorAt(DiagCode::E2001, "expected '=' before function body for '" + node->name + "'");
        return nullptr;
    }
    advance(); // Consume '='

    LUC_LOG_PARSER("Parsing function body...");
    
    // Parse async keyword if present
    bool isAsync = false;
    if (match(TokenType::ASYNC)) {
        isAsync = true;
        node->isAsync = true;
        ++asyncDepth_;
        LUC_LOG_PARSER("Async function detected");
    }
    
    // Parse the body - determine if block or expression
    if (check(TokenType::LBRACE)) {
        // Block body
        node->bodyKind = FuncBodyKind::Block;
        node->body = parseBlock();
        node->exprBody = nullptr;
    } else if (check(TokenType::LPAREN)) {
        // Anon func form with repeated signature
        node->bodyKind = FuncBodyKind::AnonFunc;
        // Parse the repeated signature (parseFuncBody will handle it)
        node->body = parseFuncBody(node->bodyKind, node->isAsync);
        node->exprBody = nullptr;
    } else {
        // Expression body - NO WRAPPING
        node->bodyKind = FuncBodyKind::ExprBody;
        node->exprBody = parseExpr();
        node->body = nullptr;
        
        if (!node->exprBody) {
            LUC_LOG_PARSER("\tERROR: expected expression after '='");
            errorAt(DiagCode::E2008, "expected expression after '='");
            return nullptr;
        }
    }

    if (isAsync) {
        --asyncDepth_;
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
std::vector<ParamPtr> Parser::parseParamGroup() {
    LUC_LOG_PARSER_VERBOSE("parseParamGroup: starting at pos " << pos_);
    std::vector<ParamPtr> params;

    consume(TokenType::LPAREN, "expected '('");

    if (check(TokenType::RPAREN)) {
        advance(); // empty param list
        LUC_LOG_PARSER_VERBOSE("parseParamGroup: empty parameter group");
        return params;
    }

    bool seenVariadic = false;

    do {
        match(TokenType::COMMA); // optional separator
        if (check(TokenType::RPAREN))
            break;               // trailing comma

        if (seenVariadic) {
            errorAt(DiagCode::E2006, "variadic parameter must be the last parameter");
        }

        ParamPtr p = parseParam();
        if (p) {
            seenVariadic = p->isVariadic;
            params.push_back(std::move(p));
        } else {
            // Panic recovery: advance at least one token or skip to the next separator/terminator
            // to ensure we don't get stuck in an infinite loop if parseParam fails without
            // consuming anything.
            if (!check(TokenType::COMMA) && !check(TokenType::RPAREN)) {
                advance();
            }
            while (!check(TokenType::COMMA) && !check(TokenType::RPAREN) && !isAtEnd()) {
                advance();
            }
        }

    } while (!check(TokenType::RPAREN) && !isAtEnd());

    consume(TokenType::RPAREN, "expected ')' to close parameter list");

    LUC_LOG_PARSER_VERBOSE("parseParamGroup: returning " << params.size() << " params");
    return params;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseParam
//
// Grammar:
//   param          := IDENTIFIER type
//   variadic_param := IDENTIFIER '...' type
//
// Parameter name is always required. The '...' variadic marker appears between
// the name and the type.
// ─────────────────────────────────────────────────────────────────────────────
ParamPtr Parser::parseParam() {
    SourceLocation loc = currentLoc();
    LUC_LOG_PARSER_VERBOSE("parseParam: parsing at pos " << pos_);

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected parameter name");
        return nullptr;
    }
    std::string name = advance().value;
    LUC_LOG_PARSER_VERBOSE("\tparam name: '" << name << "'");

    bool isVariadic = match(TokenType::VARIADIC);
    if (isVariadic) {
        LUC_LOG_PARSER_VERBOSE("\tparam is variadic");
    }

    TypePtr type;
    // Check if we are at a keyword that is NOT a valid type start.
    // In Tokens.hpp, keywords are grouped between PUB and FALSE.
    if (!looksLikeType() && peek().type >= TokenType::PUB && peek().type <= TokenType::FALSE) {
        errorAt(DiagCode::E2012, "unexpected keyword '" + peek().value + "' used as a parameter type");
        return nullptr;
    }
    
    type = parseType();
    if (!type) {
        errorAt(DiagCode::E2005, "expected type for parameter '" + name + "'");
        return nullptr;
    }

    auto p = std::make_unique<ParamAST>();
    p->loc = loc;
    p->name = std::move(name);
    p->type = std::move(type);
    p->isVariadic = isVariadic;

    LUC_LOG_PARSER_VERBOSE("\tparam type parsed successfully");
    return p;
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
        advance(); // empty — unusual but not illegal
        return params;
    }

    do {
        match(TokenType::COMMA);
        if (check(TokenType::GREATER))
            break; // trailing comma

        GenericParamPtr gp = parseGenericParam();
        if (gp)
            params.push_back(std::move(gp));

    } while (!check(TokenType::GREATER) && !isAtEnd());

    consume(TokenType::GREATER, "expected '>' to close generic parameters");
    
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
    std::string name = advance().value;

    auto gp = std::make_unique<GenericParamAST>(std::move(name));
    gp->loc = loc;

    // Optional constraints: ':' IDENTIFIER { '+' IDENTIFIER }
    if (match(TokenType::COLON)) {
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected trait name after ':' in generic parameter");
        } else {
            gp->constraints.push_back(advance().value);

            while (match(TokenType::PLUS)) {
                if (!check(TokenType::IDENTIFIER)) {
                    errorAt(DiagCode::E2003, "expected trait name after '+' in generic constraint");
                    break;
                }
                gp->constraints.push_back(advance().value);
            }
        }
    }

    LUC_LOG_PARSER_VERBOSE("\tgeneric param: '" << name << "' with " << gp->constraints.size() << " constraints");
    return gp;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseStructDecl
//
// Grammar:
//   struct_decl := [ 'pub' ] 'struct' IDENTIFIER [ generic_params ]
//                  '{' { field_decl } '}'
// ─────────────────────────────────────────────────────────────────────────────
std::unique_ptr<StructDeclAST> Parser::parseStructDecl(Visibility vis) {
    LUC_LOG_PARSER("parseStructDecl: parsing struct");
    SourceLocation loc = currentLoc();
    consume(TokenType::STRUCT, "expected 'struct'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected struct name");
        return nullptr;
    }
    std::string name = advance().value;
    LUC_LOG_PARSER("\tstruct name: '" << name << "'");

    auto node = std::make_unique<StructDeclAST>();
    node->loc = loc;
    node->name = std::move(name);
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

    auto field = std::make_unique<FieldDeclAST>();
    field->loc = loc;
    field->name = std::move(name);
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
std::unique_ptr<EnumDeclAST> Parser::parseEnumDecl(Visibility vis) {
    LUC_LOG_PARSER("parseEnumDecl: parsing enum");
    SourceLocation loc = currentLoc();
    consume(TokenType::ENUM, "expected 'enum'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected enum name");
        return nullptr;
    }
    std::string name = advance().value;
    LUC_LOG_PARSER("\tenum name: '" << name << "'");

    auto node = std::make_unique<EnumDeclAST>();
    node->loc = loc;
    node->name = std::move(name);
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
    std::string name = advance().value;

    auto variant = std::make_unique<EnumVariantAST>(std::move(name));
    variant->loc = loc;

    if (match(TokenType::ASSIGN)) {
        // Accept INT_LITERAL or HEX_LITERAL
        if (check(TokenType::INT_LITERAL) || check(TokenType::HEX_LITERAL)) {
            Token valTok = advance();

            int base = (valTok.type == TokenType::HEX_LITERAL) ? 16 : 10;

            // Strip underscores used as visual separators (e.g. 0xFF_FF)
            std::string raw = valTok.value;
            raw.erase(std::remove(raw.begin(), raw.end(), '_'), raw.end());

            char *endPtr = nullptr;
            errno = 0; // Reset errno before parsing
            long val = std::strtoll(raw.c_str(), &endPtr, base);

            if (endPtr != raw.c_str() && *endPtr == '\0' && errno != ERANGE) {
                variant->explicitValue = static_cast<int>(val);
            } else {
                error(locOf(valTok), DiagCode::E2009,
                      "enum variant value '" + valTok.value + "' is not a valid integer");
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
std::unique_ptr<TraitDeclAST> Parser::parseTraitDecl(Visibility vis) {
    LUC_LOG_PARSER("parseTraitDecl: parsing trait");
    SourceLocation loc = currentLoc();
    consume(TokenType::TRAIT, "expected 'trait'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected trait name");
        return nullptr;
    }
    std::string name = advance().value;

    auto node = std::make_unique<TraitDeclAST>();
    node->loc = loc;
    node->name = std::move(name);
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
//   trait_method := IDENTIFIER param_group { param_group } [ return_type ]
//
// Signature only — no '=' and no body. Supports curried signatures:
//   draw   ()
//   compareTo (other T) int
//   clamp (min int) (max int) int
// ─────────────────────────────────────────────────────────────────────────────
TraitMethodPtr Parser::parseTraitMethod() {
    SourceLocation loc = currentLoc();

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected method name in trait");
        return nullptr;
    }
    std::string name = advance().value;

    auto method = std::make_unique<TraitMethodAST>();
    method->loc = loc;
    method->name = std::move(name);

    // Parse one or more parameter groups (curried signature support).
    if (!check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' to start parameter list for trait method '" + method->name + "'");
        return nullptr;
    }
    while (check(TokenType::LPAREN)) {
        method->paramGroups.push_back(parseParamGroup());
    }

    // Optional return type — present if current token looks like a type
    // and is not '=' (which would indicate an impl method body, not a trait sig)
    if (looksLikeType() && !check(TokenType::ASSIGN)) {
        method->returnType = parseType();
    }

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
std::unique_ptr<ImplDeclAST> Parser::parseImplDecl(Visibility vis) {
    LUC_LOG_PARSER("parseImplDecl: parsing impl");
    SourceLocation loc = currentLoc();
    consume(TokenType::IMPL, "expected 'impl'");

    auto node = std::make_unique<ImplDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    // 1. Struct name comes first in the new syntax
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected struct name after 'impl'");
        return nullptr;
    }
    node->structName = advance().value;
    LUC_LOG_PARSER("\timpl for struct: '" << node->structName << "'");

    // 2. Optional generic params (definition style): impl Scene<T : Drawable>
    if (check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();

        // 3. Synthesis: Populate structGenericArgs with NamedTypeAST nodes.
        // This maintains the existing AST structure where ImplDeclAST expects
        // a list of type arguments to bind to the struct's parameters.
        for (const auto& gp : node->genericParams) {
            auto nt = std::make_unique<NamedTypeAST>(gp->name);
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

    auto ref = std::make_unique<TraitRefAST>();
    ref->loc = loc;
    ref->name = advance().value;

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
//   method_decl := IDENTIFIER param_group { param_group } [ return_type ] '=' func_body
//
// Supports curried methods:
//   length () float = { ... }
//   dot (other Vec2) float = { ... }
//   clamp (min int) (max int) (value int) int = { ... }
//
// No visibility prefix per method — visibility comes from the enclosing impl block.
// ─────────────────────────────────────────────────────────────────────────────
MethodDeclPtr Parser::parseMethodDecl() {
    SourceLocation loc = currentLoc();

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected method name");
        return nullptr;
    }
    std::string name = advance().value;

    auto method = std::make_unique<MethodDeclAST>();
    method->loc = loc;
    method->name = std::move(name);

    // Parse one or more parameter groups (curried method support).
    if (!check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' to start parameter list for method '" + method->name + "'");
        return nullptr;
    }
    while (check(TokenType::LPAREN)) {
        method->paramGroups.push_back(parseParamGroup());
    }

    // Optional return type
    if (looksLikeType() && !check(TokenType::ASSIGN)) {
        method->returnType = parseType();
    }

    if (!check(TokenType::ASSIGN)) {
        errorAt(DiagCode::E2001, "expected '=' before method body for '" + method->name + "'");
        return nullptr;
    }
    advance(); // Consume '='

    LUC_LOG_PARSER("parseMethodDecl: parsing body for method '" << method->name << "'");

    // Parse async keyword if present
    bool isAsync = false;
    if (match(TokenType::ASYNC)) {
        isAsync = true;
        method->isAsync = true;
        LUC_LOG_PARSER("parseMethodDecl: async method");
    }

    // Determine body type
    if (check(TokenType::LBRACE)) {
        // Block body
        method->bodyKind = FuncBodyKind::Block;
        method->body = parseBlock();
        method->exprBody = nullptr;
        LUC_LOG_PARSER("parseMethodDecl: block body");
    } else if (check(TokenType::LPAREN)) {
        // Anon func form with repeated signature - parse via parseFuncBody
        method->bodyKind = FuncBodyKind::AnonFunc;
        method->body = parseFuncBody(method->bodyKind, method->isAsync);
        method->exprBody = nullptr;
        LUC_LOG_PARSER("parseMethodDecl: anon func body");
    } else {
        // Expression body - store directly, NO WRAPPING
        method->bodyKind = FuncBodyKind::ExprBody;
        method->exprBody = parseExpr();
        method->body = nullptr;  // No block body for expression bodies

        if (!method->exprBody) {
            errorAt(DiagCode::E2008, "expected expression after '=' for method '" + method->name + "'");
            return nullptr;
        }

        LUC_LOG_PARSER("parseMethodDecl: expression body, kind=" << LucDebug::kindToString(method->exprBody->kind));
    }

    // Optional semicolon after body (for expression bodies in certain contexts)
    if (match(TokenType::SEMICOLON)) {
        LUC_LOG_PARSER("parseMethodDecl: optional semicolon consumed");
    }

    LUC_LOG_PARSER("parseMethodDecl: success for method '" << method->name << "'");
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
std::unique_ptr<FromDeclAST> Parser::parseFromDecl(Visibility vis) {
    SourceLocation loc = currentLoc();
    consume(TokenType::FROM, "expected 'from'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected target struct name after 'from'");
        return nullptr;
    }
    std::string targetName = advance().value;

    auto node = std::make_unique<FromDeclAST>();
    node->loc = loc;
    node->visibility = vis;
    node->targetTypeName = targetName;

    consume(TokenType::LBRACE, "expected '{' after target name '" + targetName + "'");

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        match(TokenType::SEMICOLON); // optional separator
        match(TokenType::COMMA);     // optional separator
        if (check(TokenType::RBRACE))
            break;

        SourceLocation entryLoc = currentLoc();

        auto entry = std::make_unique<FromEntryAST>();
        entry->loc  = entryLoc;

        // Parse one or more parameter groups (curried conversion support).
        if (!check(TokenType::LPAREN)) {
            errorAt(DiagCode::E2001,
                    "expected '(' to start parameter list for conversion entry");
            synchronize();
            continue;
        }
        while (check(TokenType::LPAREN)) {
            entry->paramGroups.push_back(parseParamGroup());
        }

        // Return type name — must match the block target type (semantic pass enforces this).
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected target type name after parameter list");
        } else {
            entry->returnTypeName = advance().value;
        }

        if (!check(TokenType::ASSIGN)) {
            errorAt(DiagCode::E2001, "expected '=' before body for conversion entry");
            synchronize();
            continue;
        }
        advance(); // Consume the '=' token

        bool isAsync = false;
        entry->body = parseFuncBody(entry->bodyKind, isAsync);

        if (isAsync) {
            errorAt(DiagCode::E2006, "conversion bodies cannot be async");
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
std::unique_ptr<TypeAliasDeclAST> Parser::parseTypeAliasDecl(Visibility vis) {
    SourceLocation loc = currentLoc();
    consume(TokenType::TYPE, "expected 'type'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected type alias name");
        return nullptr;
    }
    std::string name = advance().value;

    auto node = std::make_unique<TypeAliasDeclAST>();
    node->loc = loc;
    node->name = std::move(name);
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

// ─────────────────────────────────────────────────────────────────────────────
// parseFuncBody
//
// Consumes the '=' that precedes the body, then parses the body in one of
// four forms:
//
//   Block form:     '=' '{' stmts '}'
//     → bodyKind = Block,   isAsync = false
//
//   Async block:    '=' 'async' '{' stmts '}'
//     → bodyKind = Block,   isAsync = true
//
//   Anon func form: '=' '(' params ')' [ ret ] '{' stmts '}'
//     → bodyKind = AnonFunc, isAsync = false
//
//   Async anon:     '=' 'async' '(' params ')' [ ret ] '{' stmts '}'
//     → bodyKind = AnonFunc, isAsync = true
//
//   Expression form: '=' expr
//     → bodyKind = ExprBody, isAsync = false/true
//   (Used for function assignments like: let f = existingFunc)
//
// The expression form is desugared into a BlockStmtAST containing a
// ReturnStmtAST with the expression as the return value.
//
// Returns a StmtPtr (always a BlockStmtAST).
// Sets outBodyKind and outIsAsync on the caller's fields.
//
// Callers:  parseFuncDecl, parseMethodDecl, parseFromDecl
// ─────────────────────────────────────────────────────────────────────────────
StmtPtr Parser::parseFuncBody(FuncBodyKind &outBodyKind, bool &outIsAsync) {
    LUC_LOG_PARSER_VERBOSE("parseFuncBody: starting");
    
    // NOTE: Caller has already consumed '='
    
    outIsAsync = false;
    outBodyKind = FuncBodyKind::Block;

    // async modifier
    if (match(TokenType::ASYNC)) {
        outIsAsync = true;
        ++asyncDepth_;
    }

    StmtPtr body = nullptr;

    if (check(TokenType::LBRACE)) {
        // Block form: { ... }
        outBodyKind = FuncBodyKind::Block;
        body = parseBlock();
        LUC_LOG_PARSER_VERBOSE("parseFuncBody: block form");
    } else if (check(TokenType::LPAREN)) {
        // Anon func form: (params) [ret] { ... }
        outBodyKind = FuncBodyKind::AnonFunc;
        
        LUC_LOG_PARSER_VERBOSE("parseFuncBody: anon func form");
        
        // Parse and discard the repeated param list
        parseParamGroup();
        
        // Parse and discard the optional repeated return type
        if (looksLikeType() && !check(TokenType::LBRACE)) {
            parseType();
        }
        
        if (!check(TokenType::LBRACE)) {
            errorAt(DiagCode::E2001, "expected '{' to start function body");
            if (outIsAsync) --asyncDepth_;
            return nullptr;
        }
        
        body = parseBlock();
    } else {
        // No brace, no parens - expression bodies are handled by the caller
        errorAt(DiagCode::E2008, "expected '{' or '(' to start function body");
        if (outIsAsync) --asyncDepth_;
        return nullptr;
    }

    if (outIsAsync) {
        --asyncDepth_;
    }

    return body;
}
