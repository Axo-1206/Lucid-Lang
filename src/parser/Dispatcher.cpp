/**
 * @file Dispatcher.cpp
 * @brief Central dispatch hub for the Luc parser.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file contains the main entry points that orchestrate parsing by
 * dispatching to specialized parser modules. It serves as the central hub
 * for the entire parsing system.
 * 
 * ## What This File Contains
 * 
 *   1. Declaration Dispatch   – parseTopLevelDecl(), parseDeclaration()
 *   2. Statement Dispatch     – parseStmt(), parseBlock()
 *   3. Type Dispatch          – parseType(), parseTypeWithNullable(), parseBaseType()
 *   4. Expression Dispatch    – parseExpr(), parsePrattExpr(), parsePrefixExpr(),
 *                               parsePostfixExpr(), parsePrimaryExpr()
 * 
 * ## Why Group These Together?
 * 
 *   - **Single entry point** – All parsing starts from functions in this file
 *   - **Dispatch coordination** – These functions call each other and delegate
 *     to specialized parsers (decl/expr/stmt/type modules)
 *   - **No duplication** – These are the ONLY locations of these dispatch functions
 *   - **Clear control flow** – The entire parser's control flow is visible here
 * 
 * ## Dispatch Flow
 * 
 *   parse()
 *     └── parseDeclaration(TopLevel)
 *           └── parseUseDecl / parseStructDecl / parseEnumDecl / ...
 *   
 *   parseStmt()
 *     └── parseMultiAssignStmt / parseDeclaration(Local) / parseIfStmt / ...
 *   
 *   parseExpr()
 *     └── parsePrattExpr()
 *           ├── parsePrefixExpr() → parsePrimaryExpr()
 *           ├── parsePostfixExpr()
 *           └── parseInfix*() handlers
 *   
 *   parseType()
 *     └── parseTypeWithNullable()
 *           └── parseBaseType()
 *                 └── parsePrimitiveType / parseNamedType / parseArrayType / ...
 * 
 * @see ParserDecl.cpp for declaration parsers
 * @see ParserStmt.cpp for statement parsers
 * @see ParserExpr.cpp for expression parsers
 * @see ParserType.cpp for type parsers
 */

#include "ast/BaseAST.hpp"
#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

// ============================================================================
// 1. DECLARATION DISPATCH
// ============================================================================
//
// These functions dispatch to specific declaration parsers based on the
// current token and context (top‑level vs local).
// ============================================================================

DeclPtr Parser::parseTopLevelDecl() {
    LOG_PARSER_VERBOSE("parseTopLevelDecl: entering");
    DeclPtr result = parseDeclaration(DeclContext::TopLevel);
    if (result) {
        LOG_PARSER_VERBOSE("parseTopLevelDecl: parsed " << LucDebug::kindToString(result->kind));
    } else {
        LOG_PARSER_VERBOSE("parseTopLevelDecl: returned nullptr");
    }
    return result;
}

/**
 * @brief Parses any declaration (top-level or local).
 * 
 * Grammar: [ '@' attribute ]* [ 'pub' | 'export' ]? actual_decl
 * 
 * Dispatch order:
 *   USE        → parseUseDecl
 *   STRUCT     → parseStructDecl
 *   ENUM       → parseEnumDecl
 *   TRAIT      → parseTraitDecl
 *   IMPL       → parseImplDecl
 *   FROM       → parseFromDecl
 *   TYPE       → parseTypeAliasDecl
 *   LET/CONST  → looksLikeFuncDecl ? parseFuncDecl : parseVarDecl
 *   default    → error
 * 
 * @param ctx TopLevel or Local (affects visibility and allowed declarations)
 */
DeclPtr Parser::parseDeclaration(DeclContext ctx) {
    LOG_PARSER_VERBOSE("parseDeclaration: ctx=" << (ctx == DeclContext::TopLevel ? "TopLevel" : "Local")
                           << ", current token=" << ts_.peek().value);
    
    std::vector<AttributePtr> attrs = parseAttributes();

    Visibility vis = Visibility::Private;
    if (ctx == DeclContext::TopLevel) {
        vis = parseVisibility();
        if (vis != Visibility::Private) {
            LOG_PARSER_EXTREME("parseDeclaration: visibility=" 
                                   << (vis == Visibility::Package ? "pub" : "export"));
        }
    } else {
        if (ts_.checkAny({TokenType::PUB, TokenType::EXPORT})) {
            errorAt(DiagCode::E1104, ts_.peek().value);
            ts_.advance();
        }
    }

    DeclPtr decl;
    
    if (ts_.check(TokenType::USE)) {
        LOG_PARSER_VERBOSE("parseDeclaration: dispatching to parseUseDecl");
        if (ctx == DeclContext::Local) {
            errorAt(DiagCode::E1105);
            ts_.advance();
            while (!ts_.isAtEnd() && !ts_.checkAny({TokenType::SEMICOLON, TokenType::RBRACE, 
                    TokenType::LET, TokenType::CONST, TokenType::IF, TokenType::FOR,
                    TokenType::WHILE, TokenType::RETURN})) {
                ts_.advance();
            }
            return nullptr;
        }
        decl = parseUseDecl(vis);
    } else if (ts_.check(TokenType::STRUCT)) {
        LOG_PARSER_VERBOSE("parseDeclaration: dispatching to parseStructDecl");
        decl = parseStructDecl(vis);
    } else if (ts_.check(TokenType::ENUM)) {
        LOG_PARSER_VERBOSE("parseDeclaration: dispatching to parseEnumDecl");
        decl = parseEnumDecl(vis);
    } else if (ts_.check(TokenType::TRAIT)) {
        LOG_PARSER_VERBOSE("parseDeclaration: dispatching to parseTraitDecl");
        decl = parseTraitDecl(vis);
    } else if (ts_.check(TokenType::IMPL)) {
        LOG_PARSER_VERBOSE("parseDeclaration: dispatching to parseImplDecl");
        decl = parseImplDecl(vis);
    } else if (ts_.check(TokenType::FROM)) {
        LOG_PARSER_VERBOSE("parseDeclaration: dispatching to parseFromDecl");
        decl = parseFromDecl(vis);
    } else if (ts_.check(TokenType::TYPE)) {
        LOG_PARSER_VERBOSE("parseDeclaration: dispatching to parseTypeAliasDecl");
        decl = parseTypeAliasDecl(vis);
    } else if (ts_.checkAny({TokenType::LET, TokenType::CONST})) {
        Token kwTok = ts_.advance();
        DeclKeyword kw = (kwTok.type == TokenType::LET) ? DeclKeyword::Let : DeclKeyword::Const;
        bool looksLikeFunc = looksLikeFuncDecl();
        LOG_PARSER_VERBOSE("parseDeclaration: LET/CONST, looksLikeFuncDecl=" << looksLikeFunc
                               << ", dispatching to " << (looksLikeFunc ? "parseFuncDecl" : "parseVarDecl"));
        if (looksLikeFunc) {
            decl = parseFuncDecl(kw, vis);
        } else {
            decl = parseVarDecl(vis);
        }
    } else {
        LOG_PARSER("parseDeclaration: ERROR - expected declaration, got '" << ts_.peek().value << "'");
        errorAt(DiagCode::E1002, "expected declaration");
        return nullptr;
    }

    if (decl) {
        if (!attrs.empty()) {
            LOG_PARSER_EXTREME("parseDeclaration: attaching " << attrs.size() << " attributes");
            auto builder = arena_.makeBuilder<AttributePtr>();
            for (auto& a : attrs) builder.push_back(a);
            decl->attributes = builder.build();
        }
        decl->loc = ts_.currentLoc();
        LOG_PARSER_VERBOSE("parseDeclaration: successfully parsed " 
                               << LucDebug::kindToString(decl->kind));
    }
    
    return decl;
}

// ============================================================================
// 2. TYPE DISPATCH
// ============================================================================
//
// These functions are the entry points for parsing type annotations.
// They dispatch to primitive, named, array, reference, pointer, and function
// type parsers.
// ============================================================================

TypePtr Parser::parseType() {
    LOG_TYPE_VERBOSE("parseType: entering at line " << ts_.currentLoc().line()
                         << ", col " << ts_.currentLoc().column());
    
    LOG_TYPE("parseType: current token = '" << ts_.peek().value 
                 << "' (type=" << static_cast<int>(ts_.peek().type) 
                 << ") at line " << ts_.peek().line << ", col " << ts_.peek().column);
    
    TypePtr result = parseTypeWithNullable();
    
    LOG_TYPE_VERBOSE("parseType: returning, next token = '" << ts_.peek().value 
                         << "' at line " << ts_.peek().line << ", col " << ts_.peek().column);
    return result;
}

/**
 * @brief Parses a type with optional `?` and `!` suffixes.
 * 
 * Grammar: base_type [ '?' ] [ '!' [ type ] ]
 * 
 * Examples: int, int?, int!string, int?!string, int!
 */
TypePtr Parser::parseTypeWithNullable() {
    LOG_TYPE_EXTREME("parseTypeWithNullable: current token=" << ts_.peek().value);
    
    TypePtr ty = parseBaseType();
    if (ty && ts_.match(TokenType::QUESTION)) {
        LOG_TYPE_VERBOSE("parseTypeWithNullable: adding nullable modifier");
        ty = arena_.make<NullableTypeAST>(ty);
    }
    if (ty && ts_.match(TokenType::BANG)) {
        LOG_TYPE_VERBOSE("parseTypeWithNullable: adding result type modifier");
        TypePtr errorType = nullptr;
        if (looksLikeType()) {
            errorType = parseType();
            LOG_TYPE_EXTREME("parseTypeWithNullable: with error type");
        }
        ty = arena_.make<ResultTypeAST>(ty, errorType);
    }
    return ty;
}

/**
 * @brief Dispatches to the appropriate base type parser.
 * 
 * Dispatch priority:
 *   1. Primitive keywords → parsePrimitiveType()
 *   2. IDENTIFIER → parseNamedType()
 *   3. '[' → parseArrayType()
 *   4. '&' → parseRefType()
 *   5. '*' → parsePtrType()
 *   6. '(' or '~' → parseFuncType()
 *   7. default → error + UnknownTypeAST
 */
TypePtr Parser::parseBaseType() {
    LOG_TYPE_EXTREME("parseBaseType: current token=" << ts_.peek().value 
                         << " type=" << LucDebug::tokenTypeToString(ts_.peekType()));
    
    switch (ts_.peekType()) {
        case TokenType::TYPE_BOOL: case TokenType::TYPE_BYTE:
        case TokenType::TYPE_SHORT: case TokenType::TYPE_INT:
        case TokenType::TYPE_LONG: case TokenType::TYPE_UBYTE:
        case TokenType::TYPE_USHORT: case TokenType::TYPE_UINT:
        case TokenType::TYPE_ULONG: case TokenType::TYPE_INT8:
        case TokenType::TYPE_INT16: case TokenType::TYPE_INT32:
        case TokenType::TYPE_INT64: case TokenType::TYPE_UINT8:
        case TokenType::TYPE_UINT16: case TokenType::TYPE_UINT32:
        case TokenType::TYPE_UINT64: case TokenType::TYPE_FLOAT:
        case TokenType::TYPE_DOUBLE: case TokenType::TYPE_DECIMAL:
        case TokenType::TYPE_STRING: case TokenType::TYPE_CHAR:
        case TokenType::TYPE_ANY:
            LOG_TYPE_EXTREME("parseBaseType: dispatching to parsePrimitiveType");
            return parsePrimitiveType();

        case TokenType::IDENTIFIER:
            LOG_TYPE_EXTREME("parseBaseType: dispatching to parseNamedType");
            return parseNamedType();

        case TokenType::LBRACKET:
            LOG_TYPE_EXTREME("parseBaseType: dispatching to parseArrayType");
            return parseArrayType();

        case TokenType::AMPERSAND:
            LOG_TYPE_EXTREME("parseBaseType: dispatching to parseRefType");
            return parseRefType();

        case TokenType::MUL:
            LOG_TYPE_EXTREME("parseBaseType: dispatching to parsePtrType");
            return parsePtrType();

        case TokenType::LPAREN:
        case TokenType::TILDE:
            LOG_TYPE_EXTREME("parseBaseType: dispatching to parseFuncType");
            return parseFuncType();

        default:
            LOG_TYPE("parseBaseType: ERROR - expected type, but found '" << ts_.peek().value << "'");
            errorAt(DiagCode::E1003, ts_.peek().value);
            return arena_.make<UnknownTypeAST>();
    }
}

// ============================================================================
// 3. STATEMENT DISPATCH
// ============================================================================
//
// parseStmt() is the main entry point for parsing statements. It dispatches
// to multi-assignment, multi-var declaration, local declarations, control
// flow statements, and expression statements.
// ============================================================================

/**
 * @brief Parses a single statement.
 * 
 * Dispatch priority:
 *   1. Multi-assignment (reassignment) – looksLikeMultiAssignStart()
 *   2. Multi-variable declaration – let/const with commas
 *   3. Local declarations – type, struct, enum, impl, trait, from, let, const, @, use
 *   4. 'pub' inside block – error
 *   5. Control flow – if, switch, for, while, do, return, break, continue
 *   6. Expression statement – fallback
 */
StmtPtr Parser::parseStmt() {
    LOG_STMT_VERBOSE("parseStmt: current token=" << ts_.peek().value);
    
    // Multi-assignment (reassignment)
    if (looksLikeMultiAssignStart()) {
        LOG_STMT_VERBOSE("parseStmt: looks like multi-assignment, dispatching to parseMultiAssignStmt");
        return parseMultiAssignStmt();
    }

    // Multi-variable declaration (let/const with commas)
    if (ts_.checkAny({TokenType::LET, TokenType::CONST})) {
        size_t savedPos = ts_.getPos();
        ts_.advance(); // consume keyword
        
        bool hasIdentifier = ts_.check(TokenType::IDENTIFIER);
        if (hasIdentifier) ts_.advance();
        
        bool hasType = false;
        if (looksLikeType()) {
            hasType = true;
            if (hasIdentifier && hasType && ts_.check(TokenType::COMMA)) {
                ts_.setPos(savedPos);
                LOG_STMT_VERBOSE("parseStmt: looks like multi-var declaration");
                return parseMultiVarDecl();
            }
        }
        ts_.setPos(savedPos);
    }

    // Local declarations
    if (ts_.checkAny({TokenType::TYPE, TokenType::STRUCT, TokenType::ENUM,
                      TokenType::IMPL, TokenType::TRAIT, TokenType::FROM,
                      TokenType::LET, TokenType::CONST, TokenType::AT_SIGN,
                      TokenType::USE})) {
        LOG_STMT_VERBOSE("parseStmt: local declaration, dispatching to parseDeclaration(Local)");
        DeclPtr decl = parseDeclaration(DeclContext::Local);
        if (!decl) return nullptr;
        
        auto ds = arena_.make<DeclStmtAST>(decl);
        ds->loc = ts_.currentLoc();
        LOG_STMT_VERBOSE("parseStmt: created DeclStmtAST");
        return ds;
    }

    // 'pub' inside a block - error
    if (ts_.check(TokenType::PUB)) {
        LOG_STMT("parseStmt: ERROR - 'pub' not allowed inside block");
        errorAt(DiagCode::E1104, ts_.peek().value);
        ts_.advance();
        if (ts_.checkAny({TokenType::LET, TokenType::CONST, TokenType::TYPE,
                          TokenType::STRUCT, TokenType::ENUM, TokenType::IMPL,
                          TokenType::FROM})) {
            DeclPtr decl = parseDeclaration(DeclContext::Local);
            if (decl) {
                auto ds = arena_.make<DeclStmtAST>(decl);
                ds->loc = ts_.currentLoc();
                return ds;
            }
        }
        auto unknown = arena_.make<UnknownStmtAST>();
        unknown->loc = ts_.currentLoc();
        return unknown;
    }

    // Control flow keywords
    if (ts_.check(TokenType::IF)) {
        LOG_STMT_VERBOSE("parseStmt: dispatching to parseIfStmt");
        return parseIfStmt();
    }
    if (ts_.check(TokenType::SWITCH)) {
        LOG_STMT_VERBOSE("parseStmt: dispatching to parseSwitchStmt");
        return parseSwitchStmt();
    }
    if (ts_.check(TokenType::FOR)) {
        LOG_STMT_VERBOSE("parseStmt: dispatching to parseForStmt");
        return parseForStmt();
    }
    if (ts_.check(TokenType::WHILE)) {
        LOG_STMT_VERBOSE("parseStmt: dispatching to parseWhileStmt");
        return parseWhileStmt();
    }
    if (ts_.check(TokenType::DO)) {
        LOG_STMT_VERBOSE("parseStmt: dispatching to parseDoWhileStmt");
        return parseDoWhileStmt();
    }
    if (ts_.check(TokenType::RETURN)) {
        LOG_STMT_VERBOSE("parseStmt: dispatching to parseReturnStmt");
        return parseReturnStmt();
    }
    if (ts_.check(TokenType::BREAK)) {
        LOG_STMT_VERBOSE("parseStmt: dispatching to parseBreakStmt");
        return parseBreakStmt();
    }
    if (ts_.check(TokenType::CONTINUE)) {
        LOG_STMT_VERBOSE("parseStmt: dispatching to parseContinueStmt");
        return parseContinueStmt();
    }

    // Detect invalid variable declaration missing let/const
    if (ts_.check(TokenType::IDENTIFIER)) {
        size_t savedPos = ts_.getPos();
        ts_.advance();
        if (looksLikeType() && ts_.check(TokenType::ASSIGN)) {
            LOG_STMT("parseStmt: ERROR - variable declaration missing 'let' or 'const'");
            errorAt(DiagCode::E1002, "variable declaration requires 'let' or 'const'");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::SEMICOLON) && !ts_.check(TokenType::RBRACE)) {
                ts_.advance();
            }
            if (ts_.check(TokenType::SEMICOLON)) ts_.advance();
            auto unknown = arena_.make<UnknownStmtAST>();
            unknown->loc = ts_.currentLoc();
            return unknown;
        }
        ts_.setPos(savedPos);
    }

    // Expression statement
    if (!looksLikeStmtStart()) {
        LOG_STMT("parseStmt: ERROR - unexpected token '" << ts_.peek().value << "'");
        errorAt(DiagCode::E1002, "unexpected token '" + ts_.peek().value + "'");
        auto unknown = arena_.make<UnknownStmtAST>();
        unknown->loc = ts_.currentLoc();
        if (!ts_.isAtEnd()) ts_.advance();
        return unknown;
    }

    SourceLocation loc = ts_.currentLoc();
    LOG_STMT_VERBOSE("parseStmt: falling back to expression statement");
    ExprPtr expr = parseExpr();
    if (!expr) {
        LOG_STMT("parseStmt: ERROR - expected expression statement");
        errorAt(DiagCode::E1008, "expected expression statement");
        auto unknown = arena_.make<UnknownStmtAST>();
        unknown->loc = ts_.currentLoc();
        while (!ts_.isAtEnd() && !ts_.check(TokenType::SEMICOLON) && !ts_.check(TokenType::RBRACE)) {
            ts_.advance();
        }
        if (ts_.check(TokenType::SEMICOLON)) ts_.advance();
        return unknown;
    }

    auto stmt = arena_.make<ExprStmtAST>(expr);
    stmt->loc = loc;
    LOG_STMT_VERBOSE("parseStmt: created ExprStmtAST");
    return stmt;
}

// ============================================================================
// 4. EXPRESSION DISPATCH (PRATT PARSER)
// ============================================================================
//
// The Pratt parser is a recursive‑descent operator precedence parser.
// These functions implement the core climbing algorithm.
// ============================================================================

ExprPtr Parser::parseExpr(bool allowStructLiteral) {
    LOG_EXPR_VERBOSE("parseExpr: allowStructLiteral=" << allowStructLiteral);
    ExprPtr result = parsePrattExpr(PREC_NONE, allowStructLiteral);
    if (result) {
        LOG_EXPR_EXTREME("parseExpr: parsed " << LucDebug::kindToString(result->kind));
    }
    return result;
}

ExprPtr Parser::parsePrattExpr(int minPrec, bool allowStructLiteral) {
    LOG_EXPR_EXTREME("parsePrattExpr: minPrec=" << minPrec << ", allowStructLiteral=" << allowStructLiteral);
    
    ExprPtr lhs = parsePrefixExpr(allowStructLiteral);
    if (!lhs) {
        LOG_EXPR("parsePrattExpr: parsePrefixExpr returned nullptr");
        return arena_.make<UnknownExprAST>();
    }

    lhs = parsePostfixExpr(lhs);

    while (true) {
        int prec = infixPrec(ts_.peekType());
        if (prec <= minPrec) {
            LOG_EXPR_EXTREME("parsePrattExpr: break - prec=" << prec << " <= minPrec=" << minPrec);
            break;
        }

        TokenType opTok = ts_.peekType();
        LOG_EXPR_EXTREME("parsePrattExpr: processing infix operator " << LucDebug::tokenTypeToString(opTok)
                             << " with precedence " << prec);

        if (isAssignOp(opTok)) {
            LOG_EXPR_VERBOSE("parsePrattExpr: assignment operator, calling parseInfixAssign");
            lhs = parseInfixAssign(lhs, allowStructLiteral);
            break;
        }

        if (opTok == TokenType::IS) {
            LOG_EXPR_VERBOSE("parsePrattExpr: 'is' operator, calling parseInfixIs");
            lhs = parseInfixIs(lhs);
            continue;
        }

        if (opTok == TokenType::PIPELINE) {
            LOG_EXPR_VERBOSE("parsePrattExpr: pipeline operator, calling parsePipelineExpr");
            lhs = parsePipelineExpr(lhs);
            continue;
        }

        if (opTok == TokenType::COMPOSE) {
            LOG_EXPR_VERBOSE("parsePrattExpr: compose operator, calling parseComposeExpr");
            lhs = parseComposeExpr(lhs);
            continue;
        }

        if (opTok == TokenType::QUESTION_QUESTION) {
            LOG_EXPR_VERBOSE("parsePrattExpr: null coalesce operator, calling parseInfixNullCoalesce");
            lhs = parseInfixNullCoalesce(lhs, allowStructLiteral);
            break;
        }

        lhs = parseInfixBinary(lhs, opTok, prec, allowStructLiteral);
        lhs = parsePostfixExpr(lhs);
    }

    return lhs;
}

// ============================================================================
// 5. PREFIX & PRIMARY EXPRESSIONS
// ============================================================================

ExprPtr Parser::parsePrefixExpr(bool allowStructLiteral) {
    SourceLocation loc = ts_.currentLoc();
    TokenType current = ts_.peekType();
    
    LOG_EXPR_EXTREME("parsePrefixExpr: current token=" << ts_.peek().value
                         << " (" << LucDebug::tokenTypeToString(current) << ")");

    switch (current) {
        case TokenType::MINUS: {
            ts_.advance();
            LOG_EXPR_EXTREME("parsePrefixExpr: unary minus");
            ExprPtr operand = parsePrefixExpr(allowStructLiteral);
            if (!operand) {
                errorAt(DiagCode::E1008, "expected expression after '-'");
                return arena_.make<UnknownExprAST>();
            }
            auto node = arena_.make<UnaryExprAST>();
            node->loc = loc;
            node->op = UnaryOp::Neg;
            node->operand = operand;
            return node;
        }
        case TokenType::NOT: {
            ts_.advance();
            LOG_EXPR_EXTREME("parsePrefixExpr: unary not");
            ExprPtr operand = parsePrefixExpr(allowStructLiteral);
            if (!operand) {
                errorAt(DiagCode::E1008, "expected expression after 'not'");
                return arena_.make<UnknownExprAST>();
            }
            auto node = arena_.make<UnaryExprAST>();
            node->loc = loc;
            node->op = UnaryOp::Not;
            node->operand = operand;
            return node;
        }
        case TokenType::BIT_NOT: {
            ts_.advance();
            LOG_EXPR_EXTREME("parsePrefixExpr: unary bitwise not");
            ExprPtr operand = parsePrefixExpr(allowStructLiteral);
            if (!operand) {
                errorAt(DiagCode::E1008, "expected expression after '~'");
                return arena_.make<UnknownExprAST>();
            }
            auto node = arena_.make<UnaryExprAST>();
            node->loc = loc;
            node->op = UnaryOp::BitNot;
            node->operand = operand;
            return node;
        }
        case TokenType::AMPERSAND: {
            ts_.advance();
            LOG_EXPR_EXTREME("parsePrefixExpr: unary reference");
            ExprPtr operand = parsePrefixExpr(allowStructLiteral);
            if (!operand) {
                errorAt(DiagCode::E1008, "expected expression after '&'");
                return arena_.make<UnknownExprAST>();
            }
            auto node = arena_.make<UnaryExprAST>();
            node->loc = loc;
            node->op = UnaryOp::Ref;
            node->operand = operand;
            return node;
        }
        default:
            LOG_EXPR_EXTREME("parsePrefixExpr: dispatching to parsePrimaryExpr");
            return parsePrimaryExpr(allowStructLiteral);
    }
}

/**
 * @brief Parses a primary expression (atom).
 * 
 * Dispatch priority:
 *   1. match expression
 *   2. if expression
 *   3. resolve expression
 *   4. #intrinsic call
 *   5. await expression
 *   6. array literal
 *   7. bare '{' (error)
 *   8. anonymous function
 *   9. grouped expression (expr)
 *   10. identifier (struct literal, behavior access, or plain)
 *   11. primitive type cast T(expr)
 *   12. literal
 */
ExprPtr Parser::parsePrimaryExpr(bool allowStructLiteral) {
    SourceLocation loc = ts_.currentLoc();
    TokenType current = ts_.peekType();
    
    LOG_EXPR_VERBOSE("parsePrimaryExpr: entering at line " << loc.line()
                         << ", col " << loc.column()
                         << ", current token=" << ts_.peek().value
                         << " (" << LucDebug::tokenTypeToString(current) << ")");

    if (ts_.check(TokenType::MATCH)) {
        LOG_EXPR_VERBOSE("parsePrimaryExpr: dispatching to parseMatchExpr");
        return parseMatchExpr();
    }
    if (ts_.check(TokenType::IF)) {
        LOG_EXPR_VERBOSE("parsePrimaryExpr: dispatching to parseIfExpr");
        return parseIfExpr();
    }
    if (ts_.check(TokenType::RESOLVE)) {
        LOG_EXPR_VERBOSE("parsePrimaryExpr: dispatching to parseResolveExpr");
        return parseResolveExpr();
    }
    if (ts_.check(TokenType::HASH)) {
        LOG_EXPR_VERBOSE("parsePrimaryExpr: dispatching to parseIntrinsicCallExpr");
        return parseIntrinsicCallExpr();
    }
    if (ts_.check(TokenType::AWAIT)) {
        LOG_EXPR_VERBOSE("parsePrimaryExpr: dispatching to parseAwaitExpr");
        return parseAwaitExpr();
    }
    if (ts_.check(TokenType::LBRACKET)) {
        LOG_EXPR_VERBOSE("parsePrimaryExpr: dispatching to parseArrayLiteralExpr");
        return parseArrayLiteralExpr();
    }

    if (ts_.check(TokenType::LBRACE)) {
        LOG_EXPR("parsePrimaryExpr: ERROR - unexpected block in expression position");
        errorAt(DiagCode::E1006, "unexpected block in expression position");
        int braceDepth = 1;
        ts_.advance();
        while (!ts_.isAtEnd() && braceDepth > 0) {
            if (ts_.match(TokenType::LBRACE)) braceDepth++;
            else if (ts_.match(TokenType::RBRACE)) braceDepth--;
            else ts_.advance();
        }
        return arena_.make<UnknownExprAST>();
    }

    if (looksLikeAnonFunc()) {
        LOG_EXPR_VERBOSE("parsePrimaryExpr: looks like anonymous function, dispatching to parseAnonFuncExpr");
        return parseAnonFuncExpr();
    }

    if (ts_.check(TokenType::LPAREN)) {
        LOG_EXPR_VERBOSE("parsePrimaryExpr: grouped expression");
        ts_.advance();
        ExprPtr inner = parsePrattExpr(PREC_NONE, allowStructLiteral);
        if (!ts_.check(TokenType::RPAREN)) {
            errorAt(DiagCode::E1001, "expected ')' to close grouped expression");
        } else {
            ts_.advance();
        }
        return inner;
    }

    // =================================================================
    // Primitive type names in expression context (e.g., int(42))
    // Treat them as identifiers; the call will be parsed in parsePostfixExpr.
    // =================================================================
    if (isPrimitiveTypeToken(ts_.peekType())) {
        SourceLocation primLoc = ts_.currentLoc();
        Token primTok = ts_.advance();
        InternedString name = pool_.intern(primTok.value);
        LOG_EXPR_VERBOSE("parsePrimaryExpr: primitive type name used as conversion: '" << primTok.value << "'");
        auto ident = arena_.make<IdentifierExprAST>(name);
        ident->loc = primLoc;
        return ident;
    }

    // =================================================================
    // Identifier handling (variables, type references, generic instantiations,
    // behavior access, struct literals)
    // =================================================================
    if (ts_.check(TokenType::IDENTIFIER) || isPrimitiveTypeToken(ts_.peekType())) {
        // Parse as function reference (handles identifiers, type names, method refs,
        // dotted paths, and generic arguments)
        ExprPtr expr = parseFuncRef();
        if (!expr) {
            return arena_.make<UnknownExprAST>();
        }
        
        // Check for struct literal: TypeName { ... } or TypeName<GenericArgs> { ... }
        // The parseFuncRef may have consumed generic args already (e.g., Container<int>)
        // So we need to check if the resulting expression is an IdentifierExprAST
        // with or without generic args, followed by '{'
        if (allowStructLiteral && ts_.check(TokenType::LBRACE)) {
            // For struct literal, we need the type name and any generic args
            if (auto* ident = expr->as<IdentifierExprAST>()) {
                return parseStructLiteralExpr(ident->name, ident->genericArgs);
            }
            // For field access like math.Point { ... }? Not supported currently
            errorAt(DiagCode::E1001, "expected type name before '{' for struct literal");
            return arena_.make<UnknownExprAST>();
        }
        
        return expr;
    }

    // Fallback to literal
    LOG_EXPR_VERBOSE("parsePrimaryExpr: falling back to parseLiteralExpr");
    return parseLiteralExpr();
}

// ============================================================================
// 6. POSTFIX EXPRESSIONS
// ============================================================================
//
// parsePostfixExpr applies postfix operators to an already‑parsed left‑hand
// side expression. It handles calls, indexing, field access, and nullable
// chains.
// ============================================================================

ExprPtr Parser::parsePostfixExpr(ExprPtr lhs) {
    LOG_EXPR_EXTREME("parsePostfixExpr: entering at line " << lhs->loc.line()
                         << ", col " << lhs->loc.column());
    
    while (true) {
        if (ts_.check(TokenType::RPAREN)) break;
        if (ts_.check(TokenType::PIPELINE) || ts_.check(TokenType::COMPOSE)) break;

        // Regular call expression
        if (ts_.check(TokenType::LPAREN)) {
            LOG_EXPR_EXTREME("parsePostfixExpr: call expression at line "
                                 << ts_.peek().line << ", col " << ts_.peek().column);
            lhs = parseCallExpr(lhs, ArenaSpan<TypePtr>());
            continue;
        }

        // Field access with optional generic arguments
        if (ts_.check(TokenType::DOT)) {
            ts_.advance();
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E1003, "expected field name after '.'");
                break;
            }
            Token fieldTok = ts_.advance();
            LOG_EXPR_EXTREME("parsePostfixExpr: field access ." << fieldTok.value
                                 << " at line " << fieldTok.line << ", col " << fieldTok.column);
            
            // Check for generic arguments after field name
            ArenaSpan<TypePtr> genericArgs;
            if (ts_.check(TokenType::LESS)) {
                LOG_EXPR_EXTREME("parsePostfixExpr: parsing generic arguments for field");
                ts_.advance(); // consume '<'
                genericArgs = parseGenericArgs();
                LOG_EXPR("parsePostfixExpr: parsed " << genericArgs.size() 
                             << " generic args for field '" << fieldTok.value << "'");
            }
            
            auto node = arena_.make<FieldAccessExprAST>();
            node->loc = lhs->loc;
            node->object = lhs;
            node->field = pool_.intern(fieldTok.value);
            node->genericArgs = genericArgs;
            lhs = node;
            continue;
        }

        // Index/Slice expression
        if (ts_.check(TokenType::LBRACKET)) {
            LOG_EXPR_EXTREME("parsePostfixExpr: index/slice expression at line "
                                 << ts_.peek().line << ", col " << ts_.peek().column);
            lhs = parseIndexExpr(lhs);
            continue;
        }

        // Nullable chain
        if (ts_.check(TokenType::QUESTION_DOT)) {
            LOG_EXPR_EXTREME("parsePostfixExpr: nullable chain at line "
                                 << ts_.peek().line << ", col " << ts_.peek().column);
            std::vector<InternedString> steps;
            ExprPtr object = lhs;
            
            while (ts_.check(TokenType::QUESTION_DOT)) {
                ts_.advance();
                if (!ts_.check(TokenType::IDENTIFIER)) {
                    errorAt(DiagCode::E1003, "expected field name after '?.'");
                    break;
                }
                steps.push_back(pool_.intern(ts_.advance().value));
            }
            
            auto chain = arena_.make<NullableChainExprAST>();
            chain->loc = object->loc;
            chain->object = object;
            
            auto builder = arena_.makeBuilder<InternedString>();
            for (auto& s : steps) builder.push_back(s);
            chain->steps = builder.build();
            
            lhs = chain;
            continue;
        }

        break;
    }

    return lhs;
}