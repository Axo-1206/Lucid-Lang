/**
 * @file ParseExpr.cpp
 * @brief Implementation of expression parsers.
 * 
 * This file implements all expression parsing functions using a Pratt parser.
 * It produces AST nodes without any semantic analysis.
 * 
 * @design_decision Parser ONLY builds AST structure
 *   - No semantic analysis (resolvedType, valueState, constValue)
 *   - No type checking
 *   - Pure syntactic parsing
 * 
 * @design_decision Pratt parser with precedence climbing
 *   - parseExpr() is the main entry point
 *   - parsePrattExpr() handles precedence climbing
 *   - parsePrefixExpr() handles literals, identifiers, unary ops
 *   - Infix handlers are dispatched by parsePrattExpr()
 */

#include "../Parser.hpp"
#include "core/Tokens.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

namespace parser {

// =============================================================================
// Core Pratt Parser
// =============================================================================

ExprAST* parseExpr(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseExpr: parsing expression");
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected expression, got EOF");
        return nullptr;
    }
    
    return parsePrattExpr(stream, ctx, -1);
}

ExprAST* parsePrattExpr(TokenStream& stream, ParserContext& ctx, int minPrec) {
    LOG_PARSER_DETAIL("parsePrattExpr: min precedence: ", minPrec);
    
    ExprAST* lhs = parsePrefixExpr(stream, ctx);
    if (!lhs) {
        return nullptr;
    }
    
    while (!stream.isAtEnd()) {
        TokenType current = stream.peekType();
        int prec = infixPrec(current);
        
        if (prec < minPrec) {
            break;
        }
        
        // Composition (+>) has highest precedence
        if (current == TokenType::COMPOSE) {
            lhs = parseComposeExpr(stream, ctx, lhs);
            if (!lhs) return nullptr;
            continue;
        }
        
        // Assignment operators (right-associative)
        if (current == TokenType::ASSIGN ||
            current == TokenType::PLUS_ASSIGN ||
            current == TokenType::MINUS_ASSIGN ||
            current == TokenType::MUL_ASSIGN ||
            current == TokenType::DIV_ASSIGN ||
            current == TokenType::MOD_ASSIGN ||
            current == TokenType::POW_ASSIGN ||
            current == TokenType::BIT_AND_ASSIGN ||
            current == TokenType::BIT_OR_ASSIGN ||
            current == TokenType::BIT_XOR_ASSIGN ||
            current == TokenType::SHL_ASSIGN ||
            current == TokenType::SHR_ASSIGN) {
            
            stream.consume();
            lhs = parseInfixAssign(stream, ctx, lhs, current);
            if (!lhs) return nullptr;
            continue;
        }
        
        // Null coalesce (??)
        if (current == TokenType::QUESTION_QUESTION) {
            stream.consume();
            lhs = parseInfixNullCoalesce(stream, ctx, lhs);
            if (!lhs) return nullptr;
            continue;
        }
        
        // Binary operators
        if (prec >= 0) {
            stream.consume();
            lhs = parseInfixBinary(stream, ctx, lhs, current, prec);
            if (!lhs) return nullptr;
            continue;
        }
        
        // Postfix expressions (call, index, slice, pipeline)
        if (current == TokenType::LPAREN ||
            current == TokenType::LBRACKET ||
            current == TokenType::PIPELINE) {
            lhs = parsePostfixExpr(stream, ctx, lhs);
            if (!lhs) return nullptr;
            continue;
        }
        
        break;
    }
    
    return lhs;
}

ExprAST* parsePrefixExpr(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parsePrefixExpr");
    
    SourceLocation loc = stream.currentLoc();
    TokenType current = stream.peekType();
    
    // Unary operators
    if (current == TokenType::MINUS ||
        current == TokenType::NOT ||
        current == TokenType::BIT_NOT) {
        
        Token opTok = stream.consume();
        UnaryOp op;
        switch (opTok.type) {
            case TokenType::MINUS:   op = UnaryOp::Neg; break;
            case TokenType::NOT:     op = UnaryOp::Not; break;
            case TokenType::BIT_NOT: op = UnaryOp::BitNot; break;
            default:
                ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, loc,
                                        "expected unary operator, got '", stream.peekValue(), "'");
                return nullptr;
        }
        
        ExprAST* operand = parsePrattExpr(stream, ctx, infixPrec(current) + 1);
        if (!operand) {
            return nullptr;
        }
        
        auto* unary = ctx.arena.make<UnaryExprAST>(op);
        unary->loc = loc;
        unary->operand = operand;
        return unary;
    }
    
    return parsePrimaryExpr(stream, ctx);
}

// =============================================================================
// Primary Expressions
// =============================================================================

ExprAST* parsePrimaryExpr(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parsePrimaryExpr");
    
    SourceLocation loc = stream.currentLoc();
    TokenType current = stream.peekType();
    
    // ─── Literals ────────────────────────────────────────────────────────
    if (is_literal(current)) {
        return parseLiteralExpr(stream, ctx);
    }

    // ─── Underscore (_) - discard placeholder ──────────────────────────
    if (current == TokenType::UNDERSCORE) {
        stream.consume(); // Consume '_'
        auto* idExpr = ctx.arena.make<IdentifierExprAST>(ctx.pool.intern("_"));
        idExpr->loc = loc;
        idExpr->genericArgs = ctx.arena.makeBuilder<TypeAST*>().build();
        return idExpr;
    }
    
    // ─── Intrinsic call: #sizeof(T) ─────────────────────────────────────
    if (current == TokenType::HASH) {
        return parseIntrinsicCallExpr(stream, ctx);
    }
    
    // ─── Array literal: [1, 2, 3] ──────────────────────────────────────
    if (current == TokenType::LBRACKET) {
        return parseArrayLiteralExpr(stream, ctx);
    }
    
    // ─── If expression: if cond ?? expr else expr ──────────────────────
    if (current == TokenType::IF) {
        return parseIfExpr(stream, ctx);
    }
    
    // ─── Parenthesized expression: (expr) ──────────────────────────────
    if (current == TokenType::LPAREN) {
        stream.consume(); // Consume '('
        
        if (stream.check(TokenType::RPAREN)) {
            stream.consume(); // Consume ')'
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                    "empty parenthesized expression");
            return nullptr;
        }
        
        ExprAST* expr = parseExpr(stream, ctx);
        if (!expr) {
            return nullptr;
        }
        
        if (!stream.check(TokenType::RPAREN)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected ')', got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::RPAREN);
            if (stream.check(TokenType::RPAREN)) {
                stream.consume();
            }
            return expr;
        }
        stream.consume(); // Consume ')'
        return expr;
    }
    
    // ─── Anonymous function: (a int) -> int { ... } ────────────────────
    if (looksLikeAnonFunc(stream, ctx)) {
        return parseAnonFuncExpr(stream, ctx);
    }
    
    // ─── Module Access: module:member ───────────────────────────────────
    // Only parse as module access if the current token is IDENTIFIER followed by ':'
    // This prevents parsing 'obj.field:something' as module access
    if (current == TokenType::IDENTIFIER) {
        size_t savedPos = stream.getPos();
        stream.consume(); // Consume identifier temporarily
        bool isModuleAccess = stream.check(TokenType::COLON);
        stream.setPos(savedPos);
        
        if (isModuleAccess) {
            // ─── Additional check: Ensure the token before ':' is a valid module name ──
            // The parser already checks this via the grammar, but we add a safety check
            // to prevent things like 'obj.field:member' from being parsed as module access
            
            // This is safe because parseModuleAccessExpr will parse IDENTIFIER ':' IDENTIFIER
            return parseModuleAccessExpr(stream, ctx);
        }
    }
    
    // ─── Struct literal: Point { x = 1, y = 2 } ────────────────────────
    if (looksLikeStructLiteral(stream, ctx)) {
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, loc,
                                    "expected struct type name, got '", stream.peekValue(), "'");
            return nullptr;
        }
        Token nameTok = stream.consume();
        InternedString typeName = ctx.pool.intern(nameTok.value);
        
        ArenaSpan<TypeAST*> genericArgs;
        if (stream.check(TokenType::LESS)) {
            genericArgs = parseGenericArgs(stream, ctx);
        }
        
        if (!stream.check(TokenType::LBRACE)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                    "expected '{', got '", stream.peekValue(), "'");
            return nullptr;
        }
        return parseStructLiteralExpr(stream, ctx, typeName, genericArgs);
    }
    
    // ─── Identifier: x ──────────────────────────────────────────────────
    if (current == TokenType::IDENTIFIER) {
        return parseIdentifierExpr(stream, ctx);
    }
    
    // ─── Unknown primary expression ─────────────────────────────────────
    ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, loc,
                            "unexpected token '", stream.peekValue(), "' in expression");
    synchronizeToContext(stream, ctx);
    return nullptr;
}

LiteralExprAST* parseLiteralExpr(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    Token tok = stream.peek();
    
    LiteralKind kind;
    switch (tok.type) {
        case TokenType::INT_LITERAL:     kind = LiteralKind::Int; break;
        case TokenType::FLOAT_LITERAL:   kind = LiteralKind::Float; break;
        case TokenType::STRING_LITERAL:  kind = LiteralKind::String; break;
        case TokenType::RAW_STRING_LITERAL: kind = LiteralKind::RawString; break;
        case TokenType::CHAR_LITERAL:    kind = LiteralKind::Char; break;
        case TokenType::HEX_LITERAL:     kind = LiteralKind::Hex; break;
        case TokenType::BINARY_LITERAL:  kind = LiteralKind::Binary; break;
        case TokenType::TRUE:            kind = LiteralKind::True; break;
        case TokenType::FALSE:           kind = LiteralKind::False; break;
        case TokenType::NIL:             kind = LiteralKind::Nil; break;
        case TokenType::ERR:             kind = LiteralKind::Err; break;
        default:
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, loc,
                                    "expected literal, got '", stream.peekValue(), "'");
            synchronizeToContext(stream, ctx);
            return nullptr;
    }
    
    InternedString value = ctx.pool.intern(tok.value);
    stream.consume();
    
    auto* literal = ctx.arena.make<LiteralExprAST>(kind, value);
    literal->loc = loc;
    
    LOG_PARSER_DETAIL("parseLiteralExpr: parsed literal");
    return literal;
}

// =============================================================================
// Array Literal
// =============================================================================

ArrayLiteralExprAST* parseArrayLiteralExpr(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseArrayLiteralExpr");
    
    if (!stream.check(TokenType::LBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected '[', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.consume();
   
    if (stream.check(TokenType::RBRACKET)) {
        ArenaSpan<ExprAST*> elements = ctx.arena.makeBuilder<ExprAST*>().build();
        stream.consume();
        auto* array = ctx.arena.make<ArrayLiteralExprAST>(elements);
        array->loc = loc;
        return array;
    }
    
    if (stream.consumeTrailing(TokenType::COMMA) > 0) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_TrailingComma, stream.currentLoc(),
                                "unexpected leading comma in array literal");
    }
    
    std::vector<ExprAST*> elements;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACKET)) {
        ExprAST* elem = parseExpr(stream, ctx);
        if (elem) {
            elements.push_back(elem);
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected array element expression");
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RBRACKET);
            if (!stream.check(TokenType::COMMA) && !stream.check(TokenType::RBRACKET)) {
                break;
            }
            continue;
        }
        
        if (stream.consumeTrailing(TokenType::COMMA) > 1) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_TrailingComma, stream.currentLoc(),
                                    "unexpected consecutive commas in array literal");
        }
    }
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ']' to close array literal");
    } else {
        stream.consume(); // Consume ']'
    }
    
    auto builder = ctx.arena.makeBuilder<ExprAST*>();
    for (auto* e : elements) {
        builder.push_back(e);
    }

    auto* array = ctx.arena.make<ArrayLiteralExprAST>(builder.build());
    array->loc = loc;
    
    LOG_PARSER_DETAIL("parseArrayLiteralExpr: ", elements.size(), " elements");
    return array;
}

// =============================================================================
// If Expression
// =============================================================================

IfExprAST* parseIfExpr(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseIfExpr");
    
    if (!stream.match(TokenType::IF)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected 'if', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    ExprAST* condition = parseExpr(stream, ctx);
    if (!condition) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected if condition");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    if (!stream.match(TokenType::QUESTION_QUESTION)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '\?\?', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    ExprAST* thenBranch = parseExpr(stream, ctx);
    if (!thenBranch) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected then branch");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    if (!stream.match(TokenType::ELSE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'else', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    ExprAST* elseBranch = parseExpr(stream, ctx);
    if (!elseBranch) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected else branch");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    auto* ifExpr = ctx.arena.make<IfExprAST>(condition, thenBranch, elseBranch);
    ifExpr->loc = loc;

    LOG_PARSER_DETAIL("parseIfExpr: parsed if expression");
    return ifExpr;
}

// =============================================================================
// Struct Literal
// =============================================================================

StructLiteralExprAST* parseStructLiteralExpr(TokenStream& stream, ParserContext& ctx,
                                              InternedString typeName, ArenaSpan<TypeAST*> genericArgs) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseStructLiteralExpr: ", ctx.pool.lookup(typeName));
    
    if (!stream.check(TokenType::LBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected '{', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.consume();
    
    if (stream.check(TokenType::RBRACE)) {
        ArenaSpan<FieldInitAST*> inits = ctx.arena.makeBuilder<FieldInitAST*>().build();
        stream.consume();
        auto* structLit = ctx.arena.make<StructLiteralExprAST>(typeName, genericArgs, inits);
        structLit->loc = loc;
        return structLit;
    }
    
    if (stream.consumeTrailing(TokenType::COMMA) > 0) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_TrailingComma, stream.currentLoc(),
                                "unexpected leading comma in struct literal");
    }
    
    std::vector<FieldInitAST*> inits;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected field name, got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RBRACE);
            if (!stream.check(TokenType::COMMA) && !stream.check(TokenType::RBRACE)) {
                break;
            }
            continue;
        }
        
        Token fieldTok = stream.consume();
        InternedString fieldName = ctx.pool.intern(fieldTok.value);
        
        if (!stream.match(TokenType::ASSIGN)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected '=', got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RBRACE);
            if (!stream.check(TokenType::COMMA) && !stream.check(TokenType::RBRACE)) {
                break;
            }
            continue;
        }
        
        ExprAST* value = parseExpr(stream, ctx);
        if (!value) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected field value");
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RBRACE);
            if (!stream.check(TokenType::COMMA) && !stream.check(TokenType::RBRACE)) {
                break;
            }
            continue;
        }
        
        auto* init = ctx.arena.make<FieldInitAST>(fieldName, value);
        init->loc = stream.currentLoc();
        inits.push_back(init);
        
        if (stream.consumeTrailing(TokenType::COMMA) > 1) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_TrailingComma, stream.currentLoc(),
                                    "unexpected consecutive commas in struct literal");
        }
    }
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '}' to close struct literal");
    } else {
        stream.consume(); // Consume '}'
    }
    
    auto builder = ctx.arena.makeBuilder<FieldInitAST*>();
    for (auto* init : inits) {
        builder.push_back(init);
    }

    auto* structLit = ctx.arena.make<StructLiteralExprAST>(typeName, genericArgs, builder.build());
    structLit->loc = loc;
    
    LOG_PARSER_DETAIL("parseStructLiteralExpr: ", inits.size(), " fields");
    return structLit;
}

// =============================================================================
// Anonymous Function
// =============================================================================

AnonFuncExprAST* parseAnonFuncExpr(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseAnonFuncExpr");
    
    // ─── Parse the leading cluster (bound_cluster) - names required ────
    std::vector<ParamAST*> allParamNames;
    std::vector<TypeAST*> funcParamTypes;
    
    while (stream.check(TokenType::LPAREN)) {
        std::vector<ParamAST*> groupParams = parseParamList(stream, ctx);
        
        for (auto* p : groupParams) {
            allParamNames.push_back(p);
            funcParamTypes.push_back(p->type);
        }
    }
    
    // ─── Parse remaining clusters after '->' ─────────────────────────────
    while (stream.check(TokenType::ARROW)) {
        stream.consume(); // Consume '->'
        
        if (stream.check(TokenType::LPAREN)) {
            // Parse unnamed cluster (no names)
            std::vector<TypeAST*> groupTypes = parseParamTypeList(stream, ctx);
            for (auto* t : groupTypes) {
                funcParamTypes.push_back(t);
            }
        } else {
            break;
        }
    }
    
    // ─── Parse the final return type ──────────────────────────────────────
    TypeAST* returnType = parseType(stream, ctx);
    if (!returnType) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected return type");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // ─── Build the FuncTypeAST ─────────────────────────────────────────────
    auto paramBuilder = ctx.arena.makeBuilder<TypeAST*>();
    for (auto* t : funcParamTypes) {
        paramBuilder.push_back(t);
    }
    auto* funcType = ctx.arena.make<FuncTypeAST>(paramBuilder.build(), returnType, true);
    funcType->loc = loc;
    
    // ─── Parse the body ────────────────────────────────────────────────────
    if (!stream.check(TokenType::LBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '{', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }

    ScopedContext bodyGuard(ctx, SyntacticContext::FuncBody, stream.currentLoc());
    
    stream.consume(); // Consume '{'
    StmtAST* body = parseBlock(stream, ctx);
    
    if (!stream.check(TokenType::RBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '}', got '", stream.peekValue(), "'");
        synchronizeTo(stream, ctx, TokenType::RBRACE);
        if (stream.check(TokenType::RBRACE)) {
            stream.consume();
        }
    } else {
        stream.consume(); // Consume '}'
    }
    
    // ─── Build param groups span ──────────────────────────────────────────
    auto paramGroupBuilder = ctx.arena.makeBuilder<ParamAST*>();
    for (auto* p : allParamNames) {
        paramGroupBuilder.push_back(p);
    }
    
    auto* anonFunc = ctx.arena.make<AnonFuncExprAST>(
        funcType, 
        paramGroupBuilder.build(), 
        body
    );
    anonFunc->loc = loc;
    
    LOG_PARSER_DETAIL("parseAnonFuncExpr: parsed anonymous function");
    return anonFunc;
}

// =============================================================================
// Postfix Expressions
// =============================================================================

ExprAST* parsePostfixExpr(TokenStream& stream, ParserContext& ctx, ExprAST* lhs) {
    LOG_PARSER_DETAIL("parsePostfixExpr");
    
    if (!lhs) {
        return nullptr;
    }
    
    TokenType current = stream.peekType();
    
    // ─── Function call: f() or module:func() ────────────────────────────
    if (current == TokenType::LPAREN) {
        ArenaSpan<TypeAST*> genericArgs;
        
        // Check if the callee already has generic arguments
        if (lhs->isa<IdentifierExprAST>()) {
            auto* idExpr = lhs->as<IdentifierExprAST>();
            if (idExpr->genericArgs.size() > 0) {
                genericArgs = idExpr->genericArgs;
            }
        } else if (lhs->isa<ModuleAccessExprAST>()) {
            auto* moduleAccess = lhs->as<ModuleAccessExprAST>();
            if (moduleAccess->genericArgs.size() > 0) {
                genericArgs = moduleAccess->genericArgs;
            }
        }
        // FieldAccessExprAST doesn't store genericArgs
        
        // ─── Parse generic args before the call (if present) ──────────────
        if (genericArgs.empty() && stream.check(TokenType::LESS)) {
            // Only IdentifierExprAST and ModuleAccessExprAST can have generic args
            // FieldAccessExprAST cannot - generic args are resolved at struct declaration time
            if (lhs->isa<FieldAccessExprAST>()) {
                const FieldAccessExprAST* fieldAccess = lhs->as<FieldAccessExprAST>();
                ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                        "generic arguments are not allowed on field access '",
                                        ctx.pool.lookup(fieldAccess->fieldName),
                                        "<...>' - struct generic arguments are resolved at declaration time");
                // Skip the generic args to recover
                parseGenericArgs(stream, ctx);
            } else {
                // Valid: identifier or module access
                genericArgs = parseGenericArgs(stream, ctx);
            }
        }
        
        return parseCallExpr(stream, ctx, lhs, genericArgs);
    }
    
    // ─── Index or slice: arr[0] or arr[1..3] ────────────────────────────
    if (current == TokenType::LBRACKET) {
        size_t savedPos = stream.getPos();
        stream.consume(); // Consume '['
        
        bool isSlice = false;
        if (!stream.isAtEnd()) {
            int parenDepth = 0;
            int bracketDepth = 0;
            while (!stream.isAtEnd()) {
                if (stream.check(TokenType::LPAREN)) parenDepth++;
                if (stream.check(TokenType::RPAREN)) parenDepth--;
                if (stream.check(TokenType::LBRACKET)) bracketDepth++;
                if (stream.check(TokenType::RBRACKET)) bracketDepth--;
                if (parenDepth == 0 && bracketDepth == 0) {
                    if (stream.check(TokenType::RANGE) || stream.check(TokenType::RANGE_EXCLUSIVE)) {
                        isSlice = true;
                        break;
                    }
                    if (stream.check(TokenType::RBRACKET)) {
                        break;
                    }
                }
                stream.consume();
            }
        }
        
        stream.setPos(savedPos);
        
        if (isSlice) {
            return parseSliceExpr(stream, ctx, lhs);
        } else {
            return parseIndexExpr(stream, ctx, lhs);
        }
    }
    
    // ─── Pipeline: expr |> step ──────────────────────────────────────────
    if (current == TokenType::PIPELINE) {
        return parsePipelineExpr(stream, ctx, lhs);
    }

    // ─── Field Access: expr.field ────────────────────────────────────────
    if (current == TokenType::DOT) {
        return parseFieldAccessExpr(stream, ctx, lhs);
    }
    
    return lhs;
}



/// @brief Parse a regular identifier expression.
/// 
/// This is a helper to avoid duplicating identifier creation logic.
IdentifierExprAST* parseIdentifierExpr(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.check(TokenType::IDENTIFIER)) {
        return nullptr;
    }
    
    Token nameTok = stream.consume();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    ArenaSpan<TypeAST*> genericArgs;
    if (stream.check(TokenType::LESS)) {
        genericArgs = parseGenericArgs(stream, ctx);
    }
    
    auto* idExpr = ctx.arena.make<IdentifierExprAST>(name);
    idExpr->loc = loc;
    idExpr->genericArgs = genericArgs;
    return idExpr;
}

// =============================================================================
// Call & Index
// =============================================================================

CallExprAST* parseCallExpr(TokenStream& stream, ParserContext& ctx, 
                            ExprAST* callee, ArenaSpan<TypeAST*> genericArgs) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseCallExpr");
    
    if (!callee) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "expected callee");
        return nullptr;
    }
    
    // ─── Defensive check: FieldAccessExprAST should never have generic args ──
    // This is a safety net - parsePostfixExpr should have caught this already
    // by skip all the generics
    if (callee->isa<FieldAccessExprAST>() && !genericArgs.empty()) {
        const FieldAccessExprAST* fieldAccess = callee->as<FieldAccessExprAST>();
        ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, loc,
                                "generic arguments are not allowed on field access '",
                                ctx.pool.lookup(fieldAccess->fieldName),
                                "<...>' - struct generic arguments are resolved at declaration time");
        // Clear generic args to continue parsing
        genericArgs = ArenaSpan<TypeAST*>();
    }
    
    if (!stream.check(TokenType::LPAREN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected '(', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    ArenaSpan<ExprAST*> args = parseArgList(stream, ctx);
    bool hasArgPack = stream.match(TokenType::BANG);
    
    auto* call = ctx.arena.make<CallExprAST>(hasArgPack);
    call->loc = loc;
    call->callee = callee;
    call->genericArgs = genericArgs;
    call->args = args;
    
    LOG_PARSER_DETAIL("parseCallExpr: ", args.size(), " args");
    return call;
}

IntrinsicCallExprAST* parseIntrinsicCallExpr(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseIntrinsicCallExpr");
    
    if (!stream.check(TokenType::HASH)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected '#', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.consume();
    
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, loc,
                                "expected intrinsic name, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString intrinsicName = ctx.pool.intern(nameTok.value);
    
    ArenaSpan<ExprAST*> args = parseArgList(stream, ctx);
    
    auto* intrinsic = ctx.arena.make<IntrinsicCallExprAST>(intrinsicName);
    intrinsic->loc = loc;
    intrinsic->args = args;
    
    LOG_PARSER_DETAIL("parseIntrinsicCallExpr: #", ctx.pool.lookup(intrinsicName));
    return intrinsic;
}

IndexExprAST* parseIndexExpr(TokenStream& stream, ParserContext& ctx, ExprAST* target) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseIndexExpr");
    
    if (!target) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "expected target for index");
        return nullptr;
    }
    
    if (!stream.check(TokenType::LBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected '[', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.consume();
    
    if (stream.check(TokenType::RBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected index expression");
        stream.consume(); // Consume ']'
        return nullptr;
    }
    
    ExprAST* index = parseExpr(stream, ctx);
    if (!index) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected index expression");
        synchronizeTo(stream, ctx, TokenType::RBRACKET);
        if (stream.check(TokenType::RBRACKET)) {
            stream.consume();
        }
        return nullptr;
    }
    
    if (!stream.check(TokenType::RBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ']', got '", stream.peekValue(), "'");
        synchronizeTo(stream, ctx, TokenType::RBRACKET);
        if (stream.check(TokenType::RBRACKET)) {
            stream.consume();
        }
        auto* indexExpr = ctx.arena.make<IndexExprAST>(target, index);
        indexExpr->loc = loc;
        return indexExpr;
    }
    stream.consume(); // Consume ']'
    
    auto* indexExpr = ctx.arena.make<IndexExprAST>(target, index);
    indexExpr->loc = loc;
    
    return indexExpr;
}

SliceExprAST* parseSliceExpr(TokenStream& stream, ParserContext& ctx, ExprAST* target) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseSliceExpr");
    
    if (!target) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "expected target for slice");
        return nullptr;
    }
    
    if (!stream.check(TokenType::LBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected '[', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.consume();
    
    ExprAST* start = nullptr;
    ExprAST* end = nullptr;
    bool isExclusive = false;
    bool hasRangeOp = false;
    
    if (stream.check(TokenType::RBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected slice bounds");
        stream.consume(); // Consume ']'
        return nullptr;
    }
    
    // Check for [..] or [..<] (start omitted)
    if (stream.check(TokenType::RANGE) || stream.check(TokenType::RANGE_EXCLUSIVE)) {
        hasRangeOp = true;
        isExclusive = stream.match(TokenType::RANGE_EXCLUSIVE);
        if (!isExclusive) {
            stream.match(TokenType::RANGE);
        }
        
        if (!stream.check(TokenType::RBRACKET)) {
            end = parseExpr(stream, ctx);
            if (!end) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                        "expected slice end expression");
                synchronizeTo(stream, ctx, TokenType::RBRACKET);
                if (stream.check(TokenType::RBRACKET)) {
                    stream.consume();
                }
                auto* slice = ctx.arena.make<SliceExprAST>(target, start, end, isExclusive);
                slice->loc = loc;
                return slice;
            }
        }
    } else {
        // Parse start expression
        start = parseExpr(stream, ctx);
        if (!start) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected slice start expression");
            synchronizeTo(stream, ctx, TokenType::RANGE, TokenType::RANGE_EXCLUSIVE, TokenType::RBRACKET);
            if (stream.checkAny(TokenType::RANGE, TokenType::RANGE_EXCLUSIVE)) {
                hasRangeOp = true;
            } else if (stream.check(TokenType::RBRACKET)) {
                stream.consume();
                auto* slice = ctx.arena.make<SliceExprAST>(target, start, end, isExclusive);
                slice->loc = loc;
                return slice;
            } else {
                synchronizeTo(stream, ctx, TokenType::RBRACKET);
                if (stream.check(TokenType::RBRACKET)) {
                    stream.consume();
                }
                return nullptr;
            }
        }
        
        // Check for range operator: [start..end] or [start..<end]
        if (stream.check(TokenType::RANGE) || stream.check(TokenType::RANGE_EXCLUSIVE)) {
            hasRangeOp = true;
            isExclusive = stream.match(TokenType::RANGE_EXCLUSIVE);
            if (!isExclusive) {
                stream.match(TokenType::RANGE);
            }
            
            if (!stream.check(TokenType::RBRACKET)) {
                end = parseExpr(stream, ctx);
                if (!end) {
                    ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                            "expected slice end expression");
                    synchronizeTo(stream, ctx, TokenType::RBRACKET);
                    if (stream.check(TokenType::RBRACKET)) {
                        stream.consume();
                    }
                    auto* slice = ctx.arena.make<SliceExprAST>(target, start, end, isExclusive);
                    slice->loc = loc;
                    return slice;
                }
            }
        } else if (stream.check(TokenType::RBRACKET)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected '..' or '..<', got '", stream.peekValue(), "'");
            stream.consume(); // Consume ']'
            auto* slice = ctx.arena.make<SliceExprAST>(target, start, end, isExclusive);
            slice->loc = loc;
            return slice;
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected '..', '..<', or ']', got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::RBRACKET);
            if (stream.check(TokenType::RBRACKET)) {
                stream.consume();
            }
            auto* slice = ctx.arena.make<SliceExprAST>(target, start, end, isExclusive);
            slice->loc = loc;
            return slice;
        }
    }
    
    if (!stream.check(TokenType::RBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ']', got '", stream.peekValue(), "'");
        synchronizeTo(stream, ctx, TokenType::RBRACKET);
        if (stream.check(TokenType::RBRACKET)) {
            stream.consume();
        }
        auto* slice = ctx.arena.make<SliceExprAST>(target, start, end, isExclusive);
        slice->loc = loc;
        return slice;
    }
    stream.consume(); // Consume ']'
    
    auto* slice = ctx.arena.make<SliceExprAST>(target, start, end, isExclusive);
    slice->loc = loc;
    
    LOG_PARSER_DETAIL("parseSliceExpr: parsed slice");
    return slice;
}

/// @brief Parse a field access expression.
/// 
/// Grammar: `expr '.' IDENTIFIER`
/// 
/// @example
///   player.health         → object = player, field = "health"
///   Direction.North       → object = Direction, field = "North"
///   getPlayer().health    → object = getPlayer(), field = "health"
/// 
/// @param stream The token stream
/// @param ctx The parsing context
/// @param lhs The left-hand side expression (the object)
/// @return FieldAccessExprAST* The parsed field access expression
FieldAccessExprAST* parseFieldAccessExpr(TokenStream& stream, ParserContext& ctx, ExprAST* lhs) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseFieldAccessExpr");
    
    if (!lhs) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "expected object for field access");
        return nullptr;
    }
    
    // ─── 1. Consume '.' ──────────────────────────────────────────────────
    if (!stream.match(TokenType::DOT)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected '.', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // ─── 2. Parse field name ──────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected field name, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    Token fieldTok = stream.consume();
    InternedString fieldName = ctx.pool.intern(fieldTok.value);
    
    // ─── 3. CRITICAL: Check for invalid '.field:' syntax ──────────────────
    // After parsing '.field', if the next token is ':', that's invalid.
    // The grammar allows module:member.field but NOT obj.field:something
    // 
    // The rule: ':' can only appear after a module name (IDENTIFIER),
    // not after a field access.
    if (stream.check(TokenType::COLON)) {
        SourceLocation colonLoc = stream.currentLoc();
        
        ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedColonAfterField, colonLoc,
                                "unexpected ':' after field access. Did you mean to use '.' instead of ':'?");
        ctx.diagnostics.noteAt(loc, "field access starts here");
        
        // ─── Error Recovery: Consume the ':' and skip to the next statement ──
        stream.consume(); // Consume ':'
        
        // Skip the rest of the invalid expression
        synchronizeToContext(stream, ctx);
        
        // Return a field access with the parsed field name (partial recovery)
        auto* fieldAccess = ctx.arena.make<FieldAccessExprAST>(fieldName);
        fieldAccess->loc = loc;
        fieldAccess->object = lhs;
        return fieldAccess;
    }
    
    // ─── 4. Build the AST node ────────────────────────────────────────────
    auto* fieldAccess = ctx.arena.make<FieldAccessExprAST>(fieldName);
    fieldAccess->loc = loc;
    fieldAccess->object = lhs;
    
    LOG_PARSER_DETAIL("parseFieldAccessExpr: parsed '.", ctx.pool.lookup(fieldName), "'");
    return fieldAccess;
}

/// @brief Parse a module access expression.
/// 
/// Grammar: `IDENTIFIER ':' IDENTIFIER`
/// 
/// @example
///   math:sqrt         → module = "math", member = "sqrt"
///   std:io            → module = "std", member = "io"
///   mymod:PI          → module = "mymod", member = "PI"
/// 
/// @param stream The token stream
/// @param ctx The parsing context
/// @return ModuleAccessExprAST* The parsed module access expression
ModuleAccessExprAST* parseModuleAccessExpr(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseModuleAccessExpr");
    
    // ─── 1. Parse module name ─────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected module name, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    Token moduleTok = stream.consume();
    InternedString moduleName = ctx.pool.intern(moduleTok.value);
    
    // ─── 2. Expect ':' ────────────────────────────────────────────────────
    if (!stream.match(TokenType::COLON)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ':', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // ─── 3. Parse member name ─────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected member name, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    Token memberTok = stream.consume();
    InternedString memberName = ctx.pool.intern(memberTok.value);
    
    // ─── 4. Check for generic arguments ──────────────────────────────────
    ArenaSpan<TypeAST*> genericArgs;
    if (stream.check(TokenType::LESS)) {
        genericArgs = parseGenericArgs(stream, ctx);
    }
    
    // ─── 5. Build the AST node ────────────────────────────────────────────
    auto* moduleAccess = ctx.arena.make<ModuleAccessExprAST>(moduleName, memberName);
    moduleAccess->loc = loc;
    moduleAccess->genericArgs = genericArgs;
    
    LOG_PARSER_DETAIL("parseModuleAccessExpr: parsed '", 
                      ctx.pool.lookup(moduleName), ":", ctx.pool.lookup(memberName), "'");
    return moduleAccess;
}

// =============================================================================
// Pipeline & Composition
// =============================================================================

ExprAST* parsePipelineExpr(TokenStream& stream, ParserContext& ctx, ExprAST* seed) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parsePipelineExpr");
    
    if (!seed) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "expected pipeline seed");
        return nullptr;
    }
    
    std::vector<PipelineStepAST*> steps;
    
    while (stream.check(TokenType::PIPELINE)) {
        stream.consume(); // Consume '|>'
        
        if (stream.isAtEnd()) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected pipeline step after '|>'");
            break;
        }
        
        if (stream.consumeTrailing(TokenType::PIPELINE) > 1) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                    "unexpected consecutive '|>' operators");
            stream.consume();
            continue;
        }
        
        PipelineStepAST* step = parsePipelineStep(stream, ctx);
        if (!step) {
            break;
        }
        steps.push_back(step);
    }
    
    if (steps.empty()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "pipeline requires at least one step");
        return seed;
    }

    auto builder = ctx.arena.makeBuilder<PipelineStepAST*>();
    for (auto* s : steps) {
        builder.push_back(s);
    }

    auto* pipeline = ctx.arena.make<PipelineExprAST>(seed, builder.build());
    pipeline->loc = loc;
    
    LOG_PARSER_DETAIL("parsePipelineExpr: ", steps.size(), " steps");
    return pipeline;
}

PipelineStepAST* parsePipelineStep(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parsePipelineStep");
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "expected pipeline step");
        return nullptr;
    }
    
    // Anonymous function step
    if (looksLikeAnonFunc(stream, ctx)) {
        ExprAST* anonFunc = parseAnonFuncExpr(stream, ctx);
        if (!anonFunc) {
            return nullptr;
        }
        ArenaSpan<ExprAST*> packArgs = ctx.arena.makeBuilder<ExprAST*>().build();
        auto* step = ctx.arena.make<PipelineStepAST>(anonFunc, packArgs);
        step->loc = loc;
        return step;
    }
    
    // Expression step
    ExprAST* callable = parseExpr(stream, ctx);
    if (!callable) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected pipeline step expression");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // Check for argument pack step: fn(args)!
    ArenaSpan<ExprAST*> packArgs;
    bool hasPackArgs = false;
    
    if (stream.check(TokenType::LPAREN)) {
        size_t savedPos = stream.getPos();
        
        std::vector<ExprAST*> args;
        stream.consume(); // Consume '('
        
        if (!stream.check(TokenType::RPAREN)) {
            while (!stream.isAtEnd()) {
                if (stream.consumeTrailing(TokenType::COMMA) > 0) {
                    ctx.diagnostics.errorAt(DiagCode::Syntax_TrailingComma, stream.currentLoc(),
                                            "unexpected comma in pipeline arguments");
                }
                
                if (stream.check(TokenType::RPAREN)) {
                    break;
                }
                
                if (stream.isAtEnd()) {
                    ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                            "expected ')' to close pipeline arguments");
                    break;
                }
                
                ExprAST* arg = parseExpr(stream, ctx);
                if (arg) {
                    args.push_back(arg);
                } else {
                    ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                            "expected pipeline argument");
                    synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
                    if (stream.check(TokenType::COMMA)) {
                        stream.consume();
                        continue;
                    } else if (stream.check(TokenType::RPAREN)) {
                        break;
                    } else {
                        break;
                    }
                }
            }
        }
        
        if (!stream.check(TokenType::RPAREN)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected ')', got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::RPAREN);
            if (stream.check(TokenType::RPAREN)) {
                stream.consume();
            }
        } else {
            stream.consume(); // Consume ')'
        }
        
        if (stream.check(TokenType::BANG)) {
            stream.consume(); // Consume '!'
            hasPackArgs = true;
            
            auto builder = ctx.arena.makeBuilder<ExprAST*>();
            for (auto* arg : args) {
                builder.push_back(arg);
            }
            packArgs = builder.build();
        } else {
            stream.setPos(savedPos);
            ArenaSpan<ExprAST*> packArgs = ctx.arena.makeBuilder<ExprAST*>().build();
            auto* step = ctx.arena.make<PipelineStepAST>(callable, packArgs);
            step->loc = loc;
            return step;
        }
    }
    
    packArgs = ctx.arena.makeBuilder<ExprAST*>().build();
    auto* step = ctx.arena.make<PipelineStepAST>(callable, packArgs);
    step->loc = loc;
    
    LOG_PARSER_DETAIL("parsePipelineStep: parsed step");
    return step;
}

ExprAST* parseComposeExpr(TokenStream& stream, ParserContext& ctx, ExprAST* lhs) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseComposeExpr");
    
    if (!lhs) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "expected left-hand side");
        return nullptr;
    }
    
    if (!stream.check(TokenType::COMPOSE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected '+>', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.consume(); // Consume '+>'
    
    std::vector<ComposeOperandAST*> operands;
    
    ComposeOperandAST* operand = parseComposeOperand(stream, ctx);
    if (!operand) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected composition operand");
        synchronizeToContext(stream, ctx);
        return lhs;
    }
    operands.push_back(operand);
    
    while (stream.check(TokenType::COMPOSE)) {
        stream.consume(); // Consume '+>'
        
        operand = parseComposeOperand(stream, ctx);
        if (!operand) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected composition operand");
            synchronizeToContext(stream, ctx);
            break;
        }
        operands.push_back(operand);
    }

    auto builder = ctx.arena.makeBuilder<ComposeOperandAST*>();
    for (auto* op : operands) {
        builder.push_back(op);
    }
    
    auto* compose = ctx.arena.make<ComposeExprAST>(lhs, builder.build());
    compose->loc = loc;
    
    LOG_PARSER_DETAIL("parseComposeExpr: ", operands.size(), " operands");
    return compose;
}

ComposeOperandAST* parseComposeOperand(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseComposeOperand");
    
    ExprAST* callable = parseExpr(stream, ctx);
    if (!callable) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected composition operand");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    ArenaSpan<TypeAST*> genericArgs;
    
    if (callable->isa<IdentifierExprAST>()) {
        auto* idExpr = callable->as<IdentifierExprAST>();
        if (idExpr->genericArgs.size() > 0) {
            genericArgs = idExpr->genericArgs;
        }
    } else if (callable->isa<ModuleAccessExprAST>()) {
        auto* moduleAccess = callable->as<ModuleAccessExprAST>();
        if (moduleAccess->genericArgs.size() > 0) {
            genericArgs = moduleAccess->genericArgs;
        }
    }
    
    auto* operand = ctx.arena.make<ComposeOperandAST>(callable, genericArgs);
    operand->loc = loc;
    
    LOG_PARSER_DETAIL("parseComposeOperand: parsed operand");
    return operand;
}

// =============================================================================
// Precedence Helpers
// =============================================================================

int infixPrec(TokenType type) {
    switch (type) {
        case TokenType::DOT:            return 9;   // Field access (highest)
        case TokenType::COMPOSE:        return 8;   // Composition
        case TokenType::MUL:
        case TokenType::DIV:
        case TokenType::MOD:
        case TokenType::POW:            return 6;   // Multiplicative
        case TokenType::PLUS:
        case TokenType::MINUS:          return 5;   // Additive
        case TokenType::RANGE:
        case TokenType::RANGE_EXCLUSIVE: return 4;  // Range
        case TokenType::EQUAL_EQUAL:
        case TokenType::NOT_EQUAL:
        case TokenType::LESS:
        case TokenType::LESS_EQUAL:
        case TokenType::GREATER:
        case TokenType::GREATER_EQUAL:  return 3;   // Comparison
        case TokenType::AND:            return 2;   // Logical AND
        case TokenType::OR:             return 1;   // Logical OR
        case TokenType::QUESTION_QUESTION: return 0; // Null coalesce
        case TokenType::PIPELINE:       return -1;  // Pipeline
        default:                        return -2;  // Not an operator
    }
}

BinaryOp tokenToBinaryOp(TokenType type) {
    switch (type) {
        case TokenType::PLUS:   return BinaryOp::Add;
        case TokenType::MINUS:  return BinaryOp::Sub;
        case TokenType::MUL:    return BinaryOp::Mul;
        case TokenType::DIV:    return BinaryOp::Div;
        case TokenType::POW:    return BinaryOp::Pow;
        case TokenType::MOD:    return BinaryOp::Mod;
        case TokenType::EQUAL_EQUAL:    return BinaryOp::Eq;
        case TokenType::NOT_EQUAL:      return BinaryOp::Ne;
        case TokenType::LESS:           return BinaryOp::Lt;
        case TokenType::LESS_EQUAL:     return BinaryOp::Le;
        case TokenType::GREATER:        return BinaryOp::Gt;
        case TokenType::GREATER_EQUAL:  return BinaryOp::Ge;
        case TokenType::AND:    return BinaryOp::And;
        case TokenType::OR:     return BinaryOp::Or;
        case TokenType::BIT_AND: return BinaryOp::BitAnd;
        case TokenType::BIT_OR:  return BinaryOp::BitOr;
        case TokenType::BIT_XOR: return BinaryOp::BitXor;
        case TokenType::SHL:     return BinaryOp::Shl;
        case TokenType::SHR:     return BinaryOp::Shr;
        default: return BinaryOp::Add;
    }
}

AssignOp tokenToAssignOp(TokenType type) {
    switch (type) {
        case TokenType::ASSIGN:         return AssignOp::Assign;
        case TokenType::PLUS_ASSIGN:    return AssignOp::AddAssign;
        case TokenType::MINUS_ASSIGN:   return AssignOp::SubAssign;
        case TokenType::MUL_ASSIGN:     return AssignOp::MulAssign;
        case TokenType::DIV_ASSIGN:     return AssignOp::DivAssign;
        case TokenType::POW_ASSIGN:     return AssignOp::PowAssign;
        case TokenType::MOD_ASSIGN:     return AssignOp::ModAssign;
        case TokenType::BIT_AND_ASSIGN: return AssignOp::BitAndAssign;
        case TokenType::BIT_OR_ASSIGN:  return AssignOp::BitOrAssign;
        case TokenType::BIT_XOR_ASSIGN: return AssignOp::BitXorAssign;
        case TokenType::SHL_ASSIGN:     return AssignOp::ShlAssign;
        case TokenType::SHR_ASSIGN:     return AssignOp::ShrAssign;
        default: return AssignOp::Assign;
    }
}

// =============================================================================
// Infix Dispatch
// =============================================================================

ExprAST* parseInfixAssign(TokenStream& stream, ParserContext& ctx, ExprAST* lhs, TokenType opTok) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseInfixAssign");
    
    if (!lhs) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "expected left-hand side");
        return nullptr;
    }
    
    ExprAST* rhs = parsePrattExpr(stream, ctx, infixPrec(opTok));
    if (!rhs) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected right-hand side");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    AssignOp op = tokenToAssignOp(opTok);
    
    auto* assign = ctx.arena.make<AssignExprAST>(op);
    assign->loc = loc;
    assign->lhs = lhs;
    assign->rhs = rhs;
    
    LOG_PARSER_DETAIL("parseInfixAssign: parsed assignment");
    return assign;
}

ExprAST* parseInfixNullCoalesce(TokenStream& stream, ParserContext& ctx, ExprAST* lhs) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseInfixNullCoalesce");
    
    if (!lhs) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "expected left-hand side");
        return nullptr;
    }
    
    ExprAST* rhs = parsePrattExpr(stream, ctx, infixPrec(TokenType::QUESTION_QUESTION));
    if (!rhs) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected right-hand side");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    auto* coalesce = ctx.arena.make<NullCoalesceExprAST>(lhs, rhs);
    coalesce->loc = loc;

    LOG_PARSER_DETAIL("parseInfixNullCoalesce: parsed null coalesce");
    return coalesce;
}

ExprAST* parseInfixBinary(TokenStream& stream, ParserContext& ctx, ExprAST* lhs, TokenType opTok, int prec) {
    SourceLocation loc = stream.currentLoc();
    
    LOG_PARSER_DETAIL("parseInfixBinary: ", token_type_name(opTok));
    
    if (!lhs) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "expected left-hand side");
        return nullptr;
    }
    
    BinaryOp op = tokenToBinaryOp(opTok);
    
    bool isRangeOp = (opTok == TokenType::RANGE || opTok == TokenType::RANGE_EXCLUSIVE);
    int rhsPrec = isRangeOp ? prec : prec + 1;
    
    ExprAST* rhs = parsePrattExpr(stream, ctx, rhsPrec);
    if (!rhs) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected right-hand side");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    bool isExclusive = (opTok == TokenType::RANGE_EXCLUSIVE);
    if (isRangeOp) {
        auto* range = ctx.arena.make<RangeExprAST>(isExclusive);
        range->loc = loc;
        range->lo = lhs;
        range->hi = rhs;
        return range;
    }
    
    auto* binary = ctx.arena.make<BinaryExprAST>(op);
    binary->loc = loc;
    binary->left = lhs;
    binary->right = rhs;
    
    LOG_PARSER_DETAIL("parseInfixBinary: parsed binary expression");
    return binary;
}

} // namespace parser