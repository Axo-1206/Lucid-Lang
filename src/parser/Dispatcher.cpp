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

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// 1. DECLARATION DISPATCH
// ============================================================================
//
// These functions dispatch to specific declaration parsers based on the
// current token and context (top‑level vs local).
// ============================================================================

DeclPtr Parser::parseTopLevelDecl() {
    return parseDeclaration(DeclContext::TopLevel);
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
    std::vector<AttributePtr> attrs = parseAttributes();

    Visibility vis = Visibility::Private;
    if (ctx == DeclContext::TopLevel) {
        vis = parseVisibility();
    } else {
        if (ts_.checkAny({TokenType::PUB, TokenType::EXPORT})) {
            errorAt(DiagCode::E2014, "visibility modifier not allowed in local declaration");
            ts_.advance();
        }
    }

    DeclPtr decl;
    
    if (ts_.check(TokenType::USE)) {
        if (ctx == DeclContext::Local) {
            errorAt(DiagCode::E2006, "'use' declaration is not allowed inside a block");
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
        decl = parseStructDecl(vis);
    } else if (ts_.check(TokenType::ENUM)) {
        decl = parseEnumDecl(vis);
    } else if (ts_.check(TokenType::TRAIT)) {
        decl = parseTraitDecl(vis);
    } else if (ts_.check(TokenType::IMPL)) {
        decl = parseImplDecl(vis);
    } else if (ts_.check(TokenType::FROM)) {
        decl = parseFromDecl(vis);
    } else if (ts_.check(TokenType::TYPE)) {
        decl = parseTypeAliasDecl(vis);
    } else if (ts_.checkAny({TokenType::LET, TokenType::CONST})) {
        Token kwTok = ts_.advance();
        DeclKeyword kw = (kwTok.type == TokenType::LET) ? DeclKeyword::Let : DeclKeyword::Const;
        if (looksLikeFuncDecl()) {
            decl = parseFuncDecl(kw, vis);
        } else {
            decl = parseVarDecl(vis);
        }
    } else {
        errorAt(DiagCode::E2002, "expected declaration");
        return nullptr;
    }

    if (decl) {
        if (!attrs.empty()) {
            auto builder = arena_.makeBuilder<AttributePtr>();
            for (auto& a : attrs) builder.push_back(std::move(a));
            decl->attributes = builder.build();
        }
        decl->loc = ts_.currentLoc();
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
    return parseTypeWithNullable();
}

/**
 * @brief Parses a type with optional `?` and `!` suffixes.
 * 
 * Grammar: base_type [ '?' ] [ '!' [ type ] ]
 * 
 * Examples: int, int?, int!string, int?!string, int!
 */
TypePtr Parser::parseTypeWithNullable() {
    TypePtr ty = parseBaseType();
    if (ty && ts_.match(TokenType::QUESTION)) {
        ty = arena_.make<NullableTypeAST>(std::move(ty));
    }
    if (ty && ts_.match(TokenType::BANG)) {
        TypePtr errorType = nullptr;
        if (looksLikeType()) {
            errorType = parseType();
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
            return parsePrimitiveType();

        case TokenType::IDENTIFIER:
            return parseNamedType();

        case TokenType::LBRACKET:
            return parseArrayType();

        case TokenType::AMPERSAND:
            return parseRefType();

        case TokenType::MUL:
            return parsePtrType();

        case TokenType::LPAREN:
        case TokenType::TILDE:
            return parseFuncType();

        default:
            errorAt(DiagCode::E2005, "expected type, got '" + ts_.peek().value + "'");
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
    // Multi-assignment (reassignment)
    if (looksLikeMultiAssignStart()) {
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
        DeclPtr decl = parseDeclaration(DeclContext::Local);
        if (!decl) return nullptr;
        
        auto ds = arena_.make<DeclStmtAST>(std::move(decl));
        ds->loc = ts_.currentLoc();
        return ds;
    }

    // 'pub' inside a block - error
    if (ts_.check(TokenType::PUB)) {
        errorAt(DiagCode::E2014, "'pub' is not valid inside a block");
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
    if (ts_.check(TokenType::IF))       return parseIfStmt();
    if (ts_.check(TokenType::SWITCH))   return parseSwitchStmt();
    if (ts_.check(TokenType::FOR))      return parseForStmt();
    if (ts_.check(TokenType::WHILE))    return parseWhileStmt();
    if (ts_.check(TokenType::DO))       return parseDoWhileStmt();
    if (ts_.check(TokenType::RETURN))   return parseReturnStmt();
    if (ts_.check(TokenType::BREAK))    return parseBreakStmt();
    if (ts_.check(TokenType::CONTINUE)) return parseContinueStmt();

    // Detect invalid variable declaration missing let/const
    if (ts_.check(TokenType::IDENTIFIER)) {
        size_t savedPos = ts_.getPos();
        ts_.advance();
        if (looksLikeType() && ts_.check(TokenType::ASSIGN)) {
            errorAt(DiagCode::E2002, "variable declaration requires 'let' or 'const'");
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
        errorAt(DiagCode::E2002, "unexpected token '" + ts_.peek().value + "'");
        auto unknown = arena_.make<UnknownStmtAST>();
        unknown->loc = ts_.currentLoc();
        if (!ts_.isAtEnd()) ts_.advance();
        return unknown;
    }

    SourceLocation loc = ts_.currentLoc();
    ExprPtr expr = parseExpr();
    if (!expr) {
        errorAt(DiagCode::E2008, "expected expression statement");
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
    return parsePrattExpr(PREC_NONE, allowStructLiteral);
}

ExprPtr Parser::parsePrattExpr(int minPrec, bool allowStructLiteral) {
    ExprPtr lhs = parsePrefixExpr(allowStructLiteral);
    if (!lhs) {
        return arena_.make<UnknownExprAST>();
    }

    lhs = parsePostfixExpr(std::move(lhs));

    while (true) {
        int prec = infixPrec(ts_.peekType());
        if (prec <= minPrec) break;

        TokenType opTok = ts_.peekType();

        if (isAssignOp(opTok)) {
            lhs = parseInfixAssign(std::move(lhs), allowStructLiteral);
            break;
        }

        if (opTok == TokenType::IS) {
            lhs = parseInfixIs(std::move(lhs));
            continue;
        }

        if (opTok == TokenType::PIPELINE) {
            lhs = parsePipelineExpr(std::move(lhs));
            continue;
        }

        if (opTok == TokenType::COMPOSE) {
            lhs = parseComposeExpr(std::move(lhs));
            continue;
        }

        if (opTok == TokenType::QUESTION_QUESTION) {
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

    switch (ts_.peekType()) {
        case TokenType::MINUS: {
            ts_.advance();
            ExprPtr operand = parsePrefixExpr(allowStructLiteral);
            if (!operand) {
                errorAt(DiagCode::E2008, "expected expression after '-'");
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
            ExprPtr operand = parsePrefixExpr(allowStructLiteral);
            if (!operand) {
                errorAt(DiagCode::E2008, "expected expression after 'not'");
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
            ExprPtr operand = parsePrefixExpr(allowStructLiteral);
            if (!operand) {
                errorAt(DiagCode::E2008, "expected expression after '~'");
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
            ExprPtr operand = parsePrefixExpr(allowStructLiteral);
            if (!operand) {
                errorAt(DiagCode::E2008, "expected expression after '&'");
                return arena_.make<UnknownExprAST>();
            }
            auto node = arena_.make<UnaryExprAST>();
            node->loc = loc;
            node->op = UnaryOp::Ref;
            node->operand = std::move(operand);
            return node;
        }
        default:
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
 *   10. unsafe cast *T(expr)
 *   11. identifier (struct literal, behavior access, or plain)
 *   12. primitive type cast T(expr)
 *   13. literal
 */
ExprPtr Parser::parsePrimaryExpr(bool allowStructLiteral) {
    SourceLocation loc = ts_.currentLoc();

    if (ts_.check(TokenType::MATCH))     return parseMatchExpr();
    if (ts_.check(TokenType::IF))        return parseIfExpr();
    if (ts_.check(TokenType::RESOLVE))   return parseResolveExpr();
    if (ts_.check(TokenType::HASH))      return parseIntrinsicCallExpr();
    if (ts_.check(TokenType::AWAIT))     return parseAwaitExpr();
    if (ts_.check(TokenType::LBRACKET))  return parseArrayLiteralExpr();

    if (ts_.check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2007, "unexpected block in expression position");
        int braceDepth = 1;
        ts_.advance();
        while (!ts_.isAtEnd() && braceDepth > 0) {
            if (ts_.match(TokenType::LBRACE)) braceDepth++;
            else if (ts_.match(TokenType::RBRACE)) braceDepth--;
            else ts_.advance();
        }
        return arena_.make<UnknownExprAST>();
    }

    if (looksLikeAnonFunc()) return parseAnonFuncExpr();

    if (ts_.check(TokenType::LPAREN)) {
        ts_.advance();
        ExprPtr inner = parsePrattExpr(PREC_NONE, allowStructLiteral);
        if (!ts_.check(TokenType::RPAREN)) {
            errorAt(DiagCode::E2001, "expected ')' to close grouped expression");
        } else {
            ts_.advance();
        }
        return inner;
    }

    if (ts_.check(TokenType::MUL) && looksLikeType()) {
        ts_.advance();
        TypePtr targetType = parseBaseType();
        if (!targetType) {
            errorAt(DiagCode::E2005, "expected type after '*' in unsafe cast");
            return arena_.make<UnknownExprAST>();
        }
        if (!ts_.check(TokenType::LPAREN)) {
            errorAt(DiagCode::E2001, "expected '(' after type in unsafe cast");
            return arena_.make<UnknownExprAST>();
        }
        return parseTypeConvExpr(true, std::move(targetType));
    }

    if (ts_.check(TokenType::IDENTIFIER)) {
        std::string name = ts_.peek().value;

        if (allowStructLiteral && looksLikeStructLiteral()) {
            ts_.advance();
            ArenaSpan<TypePtr> genericArgs;
            if (ts_.check(TokenType::LESS)) {
                genericArgs = parseGenericArgs();
            }
            return parseStructLiteralExpr(name, genericArgs);
        }

        if (looksLikeBehaviorAccess()) {
            std::string typeName = ts_.advance().value;
            ArenaSpan<TypePtr> genericArgs;
            if (ts_.check(TokenType::LESS)) {
                genericArgs = parseGenericArgs();
            }
            ts_.consume(TokenType::COLON, "expected ':' in behavior access");
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected method name after ':'");
                return arena_.make<UnknownExprAST>();
            }
            std::string method = ts_.advance().value;

            auto node = arena_.make<BehaviorAccessExprAST>();
            node->loc = loc;
            node->typeName = pool_.intern(typeName);
            node->genericArgs = genericArgs;
            node->method = pool_.intern(method);
            node->isBehaviorMember = true;
            return node;
        }

        ts_.advance();
        auto node = arena_.make<IdentifierExprAST>(pool_.intern(name));
        node->loc = loc;
        return node;
    }

    if (looksLikeType() && ts_.peekNextType() == TokenType::LPAREN) {
        TypePtr targetType = parsePrimitiveType();
        if (targetType && ts_.check(TokenType::LPAREN)) {
            return parseTypeConvExpr(false, std::move(targetType));
        }
    }

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
    while (true) {
        if (ts_.check(TokenType::RPAREN)) break;
        if (ts_.check(TokenType::PIPELINE) || ts_.check(TokenType::COMPOSE)) break;

        if (ts_.check(TokenType::LPAREN)) {
            lhs = parseCallExpr(std::move(lhs), ArenaSpan<TypePtr>());
            continue;
        }

        if (ts_.check(TokenType::LESS) && 
            (lhs->isa<IdentifierExprAST>() || lhs->isa<BehaviorAccessExprAST>())) {
            
            size_t savedPos = ts_.getPos();
            int depth = 1;
            size_t i = ts_.getPos() + 1;
            const auto& tokens = ts_.getTokens();
            size_t tokenCount = ts_.getTokenCount();
            
            while (i < tokenCount && depth > 0) {
                if (tokens[i].type == TokenType::LESS) ++depth;
                else if (tokens[i].type == TokenType::GREATER) --depth;
                else if (tokens[i].type == TokenType::EOF_TOKEN) break;
                ++i;
            }
            
            if (depth == 0 && i + 1 < tokenCount && tokens[i + 1].type == TokenType::LPAREN) {
                ArenaSpan<TypePtr> genericArgs = parseGenericArgs();
                lhs = parseCallExpr(std::move(lhs), genericArgs);
                continue;
            }
        }

        if (ts_.check(TokenType::LBRACKET)) {
            lhs = parseIndexExpr(std::move(lhs));
            continue;
        }

        if (ts_.check(TokenType::DOT)) {
            ts_.advance();
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected field name after '.'");
                break;
            }
            std::string field = ts_.advance().value;
            auto node = arena_.make<FieldAccessExprAST>();
            node->loc = lhs->loc;
            node->object = std::move(lhs);
            node->field = pool_.intern(field);
            lhs = std::move(node);
            continue;
        }

        if (ts_.check(TokenType::QUESTION_DOT)) {
            std::vector<InternedString> steps;
            ExprPtr object = std::move(lhs);
            
            while (ts_.check(TokenType::QUESTION_DOT)) {
                ts_.advance();
                if (!ts_.check(TokenType::IDENTIFIER)) {
                    errorAt(DiagCode::E2003, "expected field name after '?.'");
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