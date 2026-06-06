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
    LUC_LOG_PARSER_VERBOSE("parseTopLevelDecl: entering");
    DeclPtr result = parseDeclaration(DeclContext::TopLevel);
    if (result) {
        LUC_LOG_PARSER_VERBOSE("parseTopLevelDecl: parsed " << LucDebug::kindToString(result->kind));
    } else {
        LUC_LOG_PARSER_VERBOSE("parseTopLevelDecl: returned nullptr");
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
    LUC_LOG_PARSER_VERBOSE("parseDeclaration: ctx=" << (ctx == DeclContext::TopLevel ? "TopLevel" : "Local")
                           << ", current token=" << ts_.peek().value);
    
    std::vector<AttributePtr> attrs = parseAttributes();

    Visibility vis = Visibility::Private;
    if (ctx == DeclContext::TopLevel) {
        vis = parseVisibility();
        if (vis != Visibility::Private) {
            LUC_LOG_PARSER_EXTREME("parseDeclaration: visibility=" 
                                   << (vis == Visibility::Package ? "pub" : "export"));
        }
    } else {
        if (ts_.checkAny({TokenType::PUB, TokenType::EXPORT})) {
            errorAt(DiagCode::E1014, "visibility modifier not allowed in local declaration");
            ts_.advance();
        }
    }

    DeclPtr decl;
    
    if (ts_.check(TokenType::USE)) {
        LUC_LOG_PARSER_VERBOSE("parseDeclaration: dispatching to parseUseDecl");
        if (ctx == DeclContext::Local) {
            errorAt(DiagCode::E1006, "'use' declaration is not allowed inside a block");
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
        LUC_LOG_PARSER_VERBOSE("parseDeclaration: dispatching to parseStructDecl");
        decl = parseStructDecl(vis);
    } else if (ts_.check(TokenType::ENUM)) {
        LUC_LOG_PARSER_VERBOSE("parseDeclaration: dispatching to parseEnumDecl");
        decl = parseEnumDecl(vis);
    } else if (ts_.check(TokenType::TRAIT)) {
        LUC_LOG_PARSER_VERBOSE("parseDeclaration: dispatching to parseTraitDecl");
        decl = parseTraitDecl(vis);
    } else if (ts_.check(TokenType::IMPL)) {
        LUC_LOG_PARSER_VERBOSE("parseDeclaration: dispatching to parseImplDecl");
        decl = parseImplDecl(vis);
    } else if (ts_.check(TokenType::FROM)) {
        LUC_LOG_PARSER_VERBOSE("parseDeclaration: dispatching to parseFromDecl");
        decl = parseFromDecl(vis);
    } else if (ts_.check(TokenType::TYPE)) {
        LUC_LOG_PARSER_VERBOSE("parseDeclaration: dispatching to parseTypeAliasDecl");
        decl = parseTypeAliasDecl(vis);
    } else if (ts_.checkAny({TokenType::LET, TokenType::CONST})) {
        Token kwTok = ts_.advance();
        DeclKeyword kw = (kwTok.type == TokenType::LET) ? DeclKeyword::Let : DeclKeyword::Const;
        bool looksLikeFunc = looksLikeFuncDecl();
        LUC_LOG_PARSER_VERBOSE("parseDeclaration: LET/CONST, looksLikeFuncDecl=" << looksLikeFunc
                               << ", dispatching to " << (looksLikeFunc ? "parseFuncDecl" : "parseVarDecl"));
        if (looksLikeFunc) {
            decl = parseFuncDecl(kw, vis);
        } else {
            decl = parseVarDecl(vis);
        }
    } else {
        LUC_LOG_PARSER("parseDeclaration: ERROR - expected declaration, got '" << ts_.peek().value << "'");
        errorAt(DiagCode::E1002, "expected declaration");
        return nullptr;
    }

    if (decl) {
        if (!attrs.empty()) {
            LUC_LOG_PARSER_EXTREME("parseDeclaration: attaching " << attrs.size() << " attributes");
            auto builder = arena_.makeBuilder<AttributePtr>();
            for (auto& a : attrs) builder.push_back(std::move(a));
            decl->attributes = builder.build();
        }
        decl->loc = ts_.currentLoc();
        LUC_LOG_PARSER_VERBOSE("parseDeclaration: successfully parsed " 
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
    LUC_LOG_TYPE_VERBOSE("parseType: entering at line " << ts_.currentLoc().line()
                         << ", col " << ts_.currentLoc().column());
    
    LUC_LOG_TYPE("parseType: current token = '" << ts_.peek().value 
                 << "' (type=" << static_cast<int>(ts_.peek().type) 
                 << ") at line " << ts_.peek().line << ", col " << ts_.peek().column);
    
    TypePtr result = parseTypeWithNullable();
    
    LUC_LOG_TYPE_VERBOSE("parseType: returning, next token = '" << ts_.peek().value 
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
    LUC_LOG_TYPE_EXTREME("parseTypeWithNullable: current token=" << ts_.peek().value);
    
    TypePtr ty = parseBaseType();
    if (ty && ts_.match(TokenType::QUESTION)) {
        LUC_LOG_TYPE_VERBOSE("parseTypeWithNullable: adding nullable modifier");
        ty = arena_.make<NullableTypeAST>(std::move(ty));
    }
    if (ty && ts_.match(TokenType::BANG)) {
        LUC_LOG_TYPE_VERBOSE("parseTypeWithNullable: adding result type modifier");
        TypePtr errorType = nullptr;
        if (looksLikeType()) {
            errorType = parseType();
            LUC_LOG_TYPE_EXTREME("parseTypeWithNullable: with error type");
        }
        ty = arena_.make<ResultTypeAST>(std::move(ty), std::move(errorType));
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
    LUC_LOG_TYPE_EXTREME("parseBaseType: current token=" << ts_.peek().value 
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
            LUC_LOG_TYPE_EXTREME("parseBaseType: dispatching to parsePrimitiveType");
            return parsePrimitiveType();

        case TokenType::IDENTIFIER:
            LUC_LOG_TYPE_EXTREME("parseBaseType: dispatching to parseNamedType");
            return parseNamedType();

        case TokenType::LBRACKET:
            LUC_LOG_TYPE_EXTREME("parseBaseType: dispatching to parseArrayType");
            return parseArrayType();

        case TokenType::AMPERSAND:
            LUC_LOG_TYPE_EXTREME("parseBaseType: dispatching to parseRefType");
            return parseRefType();

        case TokenType::MUL:
            LUC_LOG_TYPE_EXTREME("parseBaseType: dispatching to parsePtrType");
            return parsePtrType();

        case TokenType::LPAREN:
        case TokenType::TILDE:
            LUC_LOG_TYPE_EXTREME("parseBaseType: dispatching to parseFuncType");
            return parseFuncType();

        default:
            LUC_LOG_TYPE("parseBaseType: ERROR - expected type, got '" << ts_.peek().value << "'");
            errorAt(DiagCode::E1005, "expected type, got '" + ts_.peek().value + "'");
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
    LUC_LOG_STMT_VERBOSE("parseStmt: current token=" << ts_.peek().value);
    
    // Multi-assignment (reassignment)
    if (looksLikeMultiAssignStart()) {
        LUC_LOG_STMT_VERBOSE("parseStmt: looks like multi-assignment, dispatching to parseMultiAssignStmt");
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
                LUC_LOG_STMT_VERBOSE("parseStmt: looks like multi-var declaration");
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
        LUC_LOG_STMT_VERBOSE("parseStmt: local declaration, dispatching to parseDeclaration(Local)");
        DeclPtr decl = parseDeclaration(DeclContext::Local);
        if (!decl) return nullptr;
        
        auto ds = arena_.make<DeclStmtAST>(std::move(decl));
        ds->loc = ts_.currentLoc();
        LUC_LOG_STMT_VERBOSE("parseStmt: created DeclStmtAST");
        return ds;
    }

    // 'pub' inside a block - error
    if (ts_.check(TokenType::PUB)) {
        LUC_LOG_STMT("parseStmt: ERROR - 'pub' not allowed inside block");
        errorAt(DiagCode::E1014, "'pub' is not valid inside a block");
        ts_.advance();
        if (ts_.checkAny({TokenType::LET, TokenType::CONST, TokenType::TYPE,
                          TokenType::STRUCT, TokenType::ENUM, TokenType::IMPL,
                          TokenType::FROM})) {
            DeclPtr decl = parseDeclaration(DeclContext::Local);
            if (decl) {
                auto ds = arena_.make<DeclStmtAST>(std::move(decl));
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
        LUC_LOG_STMT_VERBOSE("parseStmt: dispatching to parseIfStmt");
        return parseIfStmt();
    }
    if (ts_.check(TokenType::SWITCH)) {
        LUC_LOG_STMT_VERBOSE("parseStmt: dispatching to parseSwitchStmt");
        return parseSwitchStmt();
    }
    if (ts_.check(TokenType::FOR)) {
        LUC_LOG_STMT_VERBOSE("parseStmt: dispatching to parseForStmt");
        return parseForStmt();
    }
    if (ts_.check(TokenType::WHILE)) {
        LUC_LOG_STMT_VERBOSE("parseStmt: dispatching to parseWhileStmt");
        return parseWhileStmt();
    }
    if (ts_.check(TokenType::DO)) {
        LUC_LOG_STMT_VERBOSE("parseStmt: dispatching to parseDoWhileStmt");
        return parseDoWhileStmt();
    }
    if (ts_.check(TokenType::RETURN)) {
        LUC_LOG_STMT_VERBOSE("parseStmt: dispatching to parseReturnStmt");
        return parseReturnStmt();
    }
    if (ts_.check(TokenType::BREAK)) {
        LUC_LOG_STMT_VERBOSE("parseStmt: dispatching to parseBreakStmt");
        return parseBreakStmt();
    }
    if (ts_.check(TokenType::CONTINUE)) {
        LUC_LOG_STMT_VERBOSE("parseStmt: dispatching to parseContinueStmt");
        return parseContinueStmt();
    }

    // Detect invalid variable declaration missing let/const
    if (ts_.check(TokenType::IDENTIFIER)) {
        size_t savedPos = ts_.getPos();
        ts_.advance();
        if (looksLikeType() && ts_.check(TokenType::ASSIGN)) {
            LUC_LOG_STMT("parseStmt: ERROR - variable declaration missing 'let' or 'const'");
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
        LUC_LOG_STMT("parseStmt: ERROR - unexpected token '" << ts_.peek().value << "'");
        errorAt(DiagCode::E1002, "unexpected token '" + ts_.peek().value + "'");
        auto unknown = arena_.make<UnknownStmtAST>();
        unknown->loc = ts_.currentLoc();
        if (!ts_.isAtEnd()) ts_.advance();
        return unknown;
    }

    SourceLocation loc = ts_.currentLoc();
    LUC_LOG_STMT_VERBOSE("parseStmt: falling back to expression statement");
    ExprPtr expr = parseExpr();
    if (!expr) {
        LUC_LOG_STMT("parseStmt: ERROR - expected expression statement");
        errorAt(DiagCode::E1008, "expected expression statement");
        auto unknown = arena_.make<UnknownStmtAST>();
        unknown->loc = ts_.currentLoc();
        while (!ts_.isAtEnd() && !ts_.check(TokenType::SEMICOLON) && !ts_.check(TokenType::RBRACE)) {
            ts_.advance();
        }
        if (ts_.check(TokenType::SEMICOLON)) ts_.advance();
        return unknown;
    }

    auto stmt = arena_.make<ExprStmtAST>(std::move(expr));
    stmt->loc = loc;
    LUC_LOG_STMT_VERBOSE("parseStmt: created ExprStmtAST");
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
    LUC_LOG_EXPR_VERBOSE("parseExpr: allowStructLiteral=" << allowStructLiteral);
    ExprPtr result = parsePrattExpr(PREC_NONE, allowStructLiteral);
    if (result) {
        LUC_LOG_EXPR_EXTREME("parseExpr: parsed " << LucDebug::kindToString(result->kind));
    }
    return result;
}

ExprPtr Parser::parsePrattExpr(int minPrec, bool allowStructLiteral) {
    LUC_LOG_EXPR_EXTREME("parsePrattExpr: minPrec=" << minPrec << ", allowStructLiteral=" << allowStructLiteral);
    
    ExprPtr lhs = parsePrefixExpr(allowStructLiteral);
    if (!lhs) {
        LUC_LOG_EXPR("parsePrattExpr: parsePrefixExpr returned nullptr");
        return arena_.make<UnknownExprAST>();
    }

    lhs = parsePostfixExpr(std::move(lhs));

    while (true) {
        int prec = infixPrec(ts_.peekType());
        if (prec <= minPrec) {
            LUC_LOG_EXPR_EXTREME("parsePrattExpr: break - prec=" << prec << " <= minPrec=" << minPrec);
            break;
        }

        TokenType opTok = ts_.peekType();
        LUC_LOG_EXPR_EXTREME("parsePrattExpr: processing infix operator " << LucDebug::tokenTypeToString(opTok)
                             << " with precedence " << prec);

        if (isAssignOp(opTok)) {
            LUC_LOG_EXPR_VERBOSE("parsePrattExpr: assignment operator, calling parseInfixAssign");
            lhs = parseInfixAssign(std::move(lhs), allowStructLiteral);
            break;
        }

        if (opTok == TokenType::IS) {
            LUC_LOG_EXPR_VERBOSE("parsePrattExpr: 'is' operator, calling parseInfixIs");
            lhs = parseInfixIs(std::move(lhs));
            continue;
        }

        if (opTok == TokenType::PIPELINE) {
            LUC_LOG_EXPR_VERBOSE("parsePrattExpr: pipeline operator, calling parsePipelineExpr");
            lhs = parsePipelineExpr(std::move(lhs));
            continue;
        }

        if (opTok == TokenType::COMPOSE) {
            LUC_LOG_EXPR_VERBOSE("parsePrattExpr: compose operator, calling parseComposeExpr");
            lhs = parseComposeExpr(std::move(lhs));
            continue;
        }

        if (opTok == TokenType::QUESTION_QUESTION) {
            LUC_LOG_EXPR_VERBOSE("parsePrattExpr: null coalesce operator, calling parseInfixNullCoalesce");
            lhs = parseInfixNullCoalesce(std::move(lhs), allowStructLiteral);
            break;
        }

        lhs = parseInfixBinary(std::move(lhs), opTok, prec, allowStructLiteral);
        lhs = parsePostfixExpr(std::move(lhs));
    }

    return lhs;
}

// ============================================================================
// 5. PREFIX & PRIMARY EXPRESSIONS
// ============================================================================

ExprPtr Parser::parsePrefixExpr(bool allowStructLiteral) {
    SourceLocation loc = ts_.currentLoc();
    TokenType current = ts_.peekType();
    
    LUC_LOG_EXPR_EXTREME("parsePrefixExpr: current token=" << ts_.peek().value
                         << " (" << LucDebug::tokenTypeToString(current) << ")");

    switch (current) {
        case TokenType::MINUS: {
            ts_.advance();
            LUC_LOG_EXPR_EXTREME("parsePrefixExpr: unary minus");
            ExprPtr operand = parsePrefixExpr(allowStructLiteral);
            if (!operand) {
                errorAt(DiagCode::E1008, "expected expression after '-'");
                return arena_.make<UnknownExprAST>();
            }
            auto node = arena_.make<UnaryExprAST>();
            node->loc = loc;
            node->op = UnaryOp::Neg;
            node->operand = std::move(operand);
            return node;
        }
        case TokenType::NOT: {
            ts_.advance();
            LUC_LOG_EXPR_EXTREME("parsePrefixExpr: unary not");
            ExprPtr operand = parsePrefixExpr(allowStructLiteral);
            if (!operand) {
                errorAt(DiagCode::E1008, "expected expression after 'not'");
                return arena_.make<UnknownExprAST>();
            }
            auto node = arena_.make<UnaryExprAST>();
            node->loc = loc;
            node->op = UnaryOp::Not;
            node->operand = std::move(operand);
            return node;
        }
        case TokenType::BIT_NOT: {
            ts_.advance();
            LUC_LOG_EXPR_EXTREME("parsePrefixExpr: unary bitwise not");
            ExprPtr operand = parsePrefixExpr(allowStructLiteral);
            if (!operand) {
                errorAt(DiagCode::E1008, "expected expression after '~'");
                return arena_.make<UnknownExprAST>();
            }
            auto node = arena_.make<UnaryExprAST>();
            node->loc = loc;
            node->op = UnaryOp::BitNot;
            node->operand = std::move(operand);
            return node;
        }
        case TokenType::AMPERSAND: {
            ts_.advance();
            LUC_LOG_EXPR_EXTREME("parsePrefixExpr: unary reference");
            ExprPtr operand = parsePrefixExpr(allowStructLiteral);
            if (!operand) {
                errorAt(DiagCode::E1008, "expected expression after '&'");
                return arena_.make<UnknownExprAST>();
            }
            auto node = arena_.make<UnaryExprAST>();
            node->loc = loc;
            node->op = UnaryOp::Ref;
            node->operand = std::move(operand);
            return node;
        }
        default:
            LUC_LOG_EXPR_EXTREME("parsePrefixExpr: dispatching to parsePrimaryExpr");
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
    
    LUC_LOG_EXPR_VERBOSE("parsePrimaryExpr: entering at line " << loc.line()
                         << ", col " << loc.column()
                         << ", current token=" << ts_.peek().value
                         << " (" << LucDebug::tokenTypeToString(current) << ")");

    if (ts_.check(TokenType::MATCH)) {
        LUC_LOG_EXPR_VERBOSE("parsePrimaryExpr: dispatching to parseMatchExpr");
        return parseMatchExpr();
    }
    if (ts_.check(TokenType::IF)) {
        LUC_LOG_EXPR_VERBOSE("parsePrimaryExpr: dispatching to parseIfExpr");
        return parseIfExpr();
    }
    if (ts_.check(TokenType::RESOLVE)) {
        LUC_LOG_EXPR_VERBOSE("parsePrimaryExpr: dispatching to parseResolveExpr");
        return parseResolveExpr();
    }
    if (ts_.check(TokenType::HASH)) {
        LUC_LOG_EXPR_VERBOSE("parsePrimaryExpr: dispatching to parseIntrinsicCallExpr");
        return parseIntrinsicCallExpr();
    }
    if (ts_.check(TokenType::AWAIT)) {
        LUC_LOG_EXPR_VERBOSE("parsePrimaryExpr: dispatching to parseAwaitExpr");
        return parseAwaitExpr();
    }
    if (ts_.check(TokenType::LBRACKET)) {
        LUC_LOG_EXPR_VERBOSE("parsePrimaryExpr: dispatching to parseArrayLiteralExpr");
        return parseArrayLiteralExpr();
    }

    if (ts_.check(TokenType::LBRACE)) {
        LUC_LOG_EXPR("parsePrimaryExpr: ERROR - unexpected block in expression position");
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
        LUC_LOG_EXPR_VERBOSE("parsePrimaryExpr: looks like anonymous function, dispatching to parseAnonFuncExpr");
        return parseAnonFuncExpr();
    }

    if (ts_.check(TokenType::LPAREN)) {
        LUC_LOG_EXPR_VERBOSE("parsePrimaryExpr: grouped expression");
        ts_.advance();
        ExprPtr inner = parsePrattExpr(PREC_NONE, allowStructLiteral);
        if (!ts_.check(TokenType::RPAREN)) {
            errorAt(DiagCode::E1001, "expected ')' to close grouped expression");
        } else {
            ts_.advance();
        }
        return inner;
    }

    if (ts_.check(TokenType::IDENTIFIER)) {
        SourceLocation idenLoc = ts_.currentLoc();
        Token identTok = ts_.advance();
        InternedString name = pool_.intern(identTok.value);
        LUC_LOG_EXPR_VERBOSE("parsePrimaryExpr: identifier '" << identTok.value 
                            << "' at line " << identTok.line << ", col " << identTok.column);
        
        // Use the robust lookahead to determine if this is a generic instantiation
        ArenaSpan<TypePtr> genericArgs;
        if (looksLikeGenericTypeInstantiation()) {
            // We are positioned after the identifier; the next token is '<'
            LUC_LOG_EXPR_EXTREME("parsePrimaryExpr: looks like generic instantiation for '" 
                                << identTok.value << "'");
            ts_.advance(); // consume '<'
            genericArgs = parseGenericArgs(); // This will now succeed cleanly
            LUC_LOG_EXPR("parsePrimaryExpr: parsed " << genericArgs.size() 
                        << " generic args for '" << identTok.value << "'");
        }
        
        // Check for behavior access (method call) – takes precedence over struct literal
        if (ts_.check(TokenType::COLON)) {
            LUC_LOG_EXPR_VERBOSE("parsePrimaryExpr: behavior access (method call)");
            ts_.advance(); // consume ':'
            
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E1003, "expected method name after ':'");
                return arena_.make<UnknownExprAST>();
            }
            Token methodTok = ts_.advance();
            LUC_LOG_EXPR_EXTREME("parsePrimaryExpr: method = '" << methodTok.value << "'");
            
            auto node = arena_.make<BehaviorAccessExprAST>();
            node->loc = idenLoc;
            node->typeName = name;
            node->method = pool_.intern(methodTok.value);
            node->isBehaviorMember = true;
            if (genericArgs.size() > 0) {
                LUC_LOG_EXPR("parsePrimaryExpr: warning – method '" << identTok.value << ":" << methodTok.value 
                            << "' has generic arguments (ignored)");
            }
            return node;
        }
        
        // Check for struct literal (with or without generic args)
        if (allowStructLiteral && ts_.check(TokenType::LBRACE)) {
            LUC_LOG_EXPR_VERBOSE("parsePrimaryExpr: struct literal '" << identTok.value 
                                << "' with " << genericArgs.size() << " generic args");
            return parseStructLiteralExpr(name, genericArgs);
        }
        
        // Plain identifier (variable, function name, or generic function reference)
        auto node = arena_.make<IdentifierExprAST>(name);
        node->loc = idenLoc;
        node->genericArgs = genericArgs;
        if (genericArgs.size() > 0) {
            LUC_LOG_EXPR("parsePrimaryExpr: generic function reference '" << identTok.value 
                        << "' with " << genericArgs.size() << " args");
        }
        return node;
    }

    LUC_LOG_EXPR_VERBOSE("parsePrimaryExpr: falling back to parseLiteralExpr");
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
    LUC_LOG_EXPR_EXTREME("parsePostfixExpr: entering at line " << lhs->loc.line()
                         << ", col " << lhs->loc.column());
    
    while (true) {
        if (ts_.check(TokenType::RPAREN)) break;
        if (ts_.check(TokenType::PIPELINE) || ts_.check(TokenType::COMPOSE)) break;

        // Regular call expression
        if (ts_.check(TokenType::LPAREN)) {
            LUC_LOG_EXPR_EXTREME("parsePostfixExpr: call expression at line "
                                 << ts_.peek().line << ", col " << ts_.peek().column);
            lhs = parseCallExpr(std::move(lhs), ArenaSpan<TypePtr>());
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
            LUC_LOG_EXPR_EXTREME("parsePostfixExpr: field access ." << fieldTok.value
                                 << " at line " << fieldTok.line << ", col " << fieldTok.column);
            
            // Check for generic arguments after field name
            ArenaSpan<TypePtr> genericArgs;
            if (ts_.check(TokenType::LESS)) {
                LUC_LOG_EXPR_EXTREME("parsePostfixExpr: parsing generic arguments for field");
                ts_.advance(); // consume '<'
                genericArgs = parseGenericArgs();
                LUC_LOG_EXPR("parsePostfixExpr: parsed " << genericArgs.size() 
                             << " generic args for field '" << fieldTok.value << "'");
            }
            
            auto node = arena_.make<FieldAccessExprAST>();
            node->loc = lhs->loc;
            node->object = std::move(lhs);
            node->field = pool_.intern(fieldTok.value);
            node->genericArgs = genericArgs;
            lhs = std::move(node);
            continue;
        }

        // Index/Slice expression
        if (ts_.check(TokenType::LBRACKET)) {
            LUC_LOG_EXPR_EXTREME("parsePostfixExpr: index/slice expression at line "
                                 << ts_.peek().line << ", col " << ts_.peek().column);
            lhs = parseIndexExpr(std::move(lhs));
            continue;
        }

        // Nullable chain
        if (ts_.check(TokenType::QUESTION_DOT)) {
            LUC_LOG_EXPR_EXTREME("parsePostfixExpr: nullable chain at line "
                                 << ts_.peek().line << ", col " << ts_.peek().column);
            std::vector<InternedString> steps;
            ExprPtr object = std::move(lhs);
            
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
            chain->object = std::move(object);
            
            auto builder = arena_.makeBuilder<InternedString>();
            for (auto& s : steps) builder.push_back(std::move(s));
            chain->steps = builder.build();
            
            lhs = std::move(chain);
            continue;
        }

        break;
    }

    return lhs;
}