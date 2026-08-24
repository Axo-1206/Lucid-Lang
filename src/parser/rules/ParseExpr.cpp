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
#include "debug/DebugUtils.hpp"

namespace parser {

// =============================================================================
// Core Pratt Parser
// =============================================================================

ExprAST* parseExpr(TokenStream& stream, ParserContext& ctx) {
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected expression, got EOF");
        return nullptr;
    }
    
    return parsePrattExpr(stream, ctx, -1);
}

ExprAST* parsePrattExpr(TokenStream& stream, ParserContext& ctx, int minPrec) {
    
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
            return nullptr;
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
    if (stream.check(TokenType::SEMICOLON)) {
        // The call site (parse declaration) will handle error diagnostic for this case
        return nullptr;
    } else {
        ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, loc,
                        "unexpected token '", stream.peekValue(), "' in expression");
    }

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
            return nullptr;
    }
    
    InternedString value = ctx.pool.intern(tok.value);
    stream.consume();
    
    auto* literal = ctx.arena.make<LiteralExprAST>(kind, value);
    literal->loc = loc;
    
    return literal;
}

// =============================================================================
// Array Literal
// =============================================================================

ArrayLiteralExprAST* parseArrayLiteralExpr(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.check(TokenType::LBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected '[', got '", stream.peekValue(), "'");
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
            return nullptr;
        }
        
        int count = stream.consumeTrailing(TokenType::COMMA);
        if (count == 0) {
            if (stream.check(TokenType::RBRACKET)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                    "expected array element after ',' in array");
            } else {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected ',' to separate array elements");
            }
        } else if (count == 2) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.previousLoc(),
                                    "expected array element after ',' in array");
        } else if (count > 3) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
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
    
    return array;
}

// =============================================================================
// If Expression
// =============================================================================

IfExprAST* parseIfExpr(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.match(TokenType::IF)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected 'if', got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    ExprAST* condition = parseExpr(stream, ctx);
    if (!condition) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected if condition");
        return nullptr;
    }
    
    if (!stream.match(TokenType::QUESTION_QUESTION)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '\?\?', got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    ExprAST* thenBranch = parseExpr(stream, ctx);
    if (!thenBranch) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected then branch");
        return nullptr;
    }
    
    if (!stream.match(TokenType::ELSE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected 'else', got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    ExprAST* elseBranch = parseExpr(stream, ctx);
    if (!elseBranch) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected else branch");
        return nullptr;
    }
    
    auto* ifExpr = ctx.arena.make<IfExprAST>(condition, thenBranch, elseBranch);
    ifExpr->loc = loc;

    return ifExpr;
}

// =============================================================================
// Struct Literal
// =============================================================================

StructLiteralExprAST* parseStructLiteralExpr(TokenStream& stream, ParserContext& ctx, InternedString typeName, ArenaSpan<TypeAST*> genericArgs) {
    SourceLocation loc = stream.currentLoc();
    
    // ─── Parse opening brace ──────────────────────────────────────────────
    if (!stream.check(TokenType::LBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected '{', got '", stream.peekValue(), "'");
        return nullptr;
    }
    stream.consume();
    
    // ─── Handle empty struct literal ──────────────────────────────────────
    if (stream.check(TokenType::RBRACE)) {
        ArenaSpan<FieldInitAST*> inits = ctx.arena.makeBuilder<FieldInitAST*>().build();
        stream.consume();
        auto* structLit = ctx.arena.make<StructLiteralExprAST>(typeName, genericArgs, inits);
        structLit->loc = loc;
        return structLit;
    }
    
    // ─── Parse field initializers ────────────────────────────────────────
    std::vector<FieldInitAST*> inits;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACE)) {
        // ─── Filter invalid tokens in this context ──────────────────────
        // A struct literal field starts with IDENTIFIER. We also skip stray
        // semicolons and commas.
        if (!stream.checkAny(TokenType::SEMICOLON, TokenType::COMMA, TokenType::IDENTIFIER)) {
            
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                    "unexpected token '", stream.peekValue(), "' inside struct literal");
            
            // Synchronize to nearest valid field to recover
            synchronizeTo(stream, ctx, 
                TokenType::IDENTIFIER,     // Field name
                TokenType::SEMICOLON,      // Skip stray semicolons
                TokenType::COMMA,          // Skip stray commas
                TokenType::RBRACE          // End of struct literal
            );
            
            if (stream.check(TokenType::RBRACE) || stream.isAtEnd()) {
                break;
            }
        }

        // ─── Skip stray semicolons and commas ───────────────────────────
        if (stream.checkAny(TokenType::SEMICOLON, TokenType::COMMA)) {
            stream.consume();
            continue;
        }

        // ─── Parse field name ──────────────────────────────────────────────
        if (!stream.check(TokenType::IDENTIFIER)) {
            // This should not happen due to filtering, but just in case
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected field name, got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::SEMICOLON, TokenType::RBRACE);
            if (stream.checkAny(TokenType::COMMA, TokenType::SEMICOLON, TokenType::RBRACE)) {
                stream.consume();
                continue;
            }
            break;
        }
        
        Token fieldTok = stream.consume();
        InternedString fieldName = ctx.pool.intern(fieldTok.value);
        
        // ─── Parse '=' ─────────────────────────────────────────────────────
        if (!stream.match(TokenType::ASSIGN)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected '=', got '", stream.peekValue(), "'");
            
            // Try to recover by skipping to the next field or closing brace
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::SEMICOLON, TokenType::RBRACE);
            if (stream.checkAny(TokenType::COMMA, TokenType::SEMICOLON, TokenType::RBRACE)) {
                stream.consume();
                continue;
            }
            break;
        }
        
        // ─── Parse field value ─────────────────────────────────────────────
        ExprAST* value = parseExpr(stream, ctx);
        if (!value) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected field value");
            
            // Try to recover to the next field or closing brace
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::SEMICOLON, TokenType::RBRACE);
            if (stream.checkAny(TokenType::COMMA, TokenType::SEMICOLON, TokenType::RBRACE)) {
                stream.consume();
                continue;
            }
            break;
        }
        
        // ─── Build and store field initializer ───────────────────────────
        auto* init = ctx.arena.make<FieldInitAST>(fieldName, value);
        init->loc = stream.currentLoc();
        inits.push_back(init);
        
        // ─── Handle trailing comma ──────────────────────────────────────
        if (stream.checkAny(TokenType::COMMA, TokenType::SEMICOLON)) {
            // Consume the ',' or ';' and continue to the next field
            // This is valid, so we don't need to report an error
            stream.consume();
            // Continue loop to parse next field
        } else if (!stream.check(TokenType::RBRACE)) {
            // If we're not at a comma or closing brace, something's wrong
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected ',' or '}', got '", stream.peekValue(), "'");
            
            // Try to recover to the next field or closing brace
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::SEMICOLON, TokenType::RBRACE);
            if (stream.checkAny(TokenType::COMMA, TokenType::SEMICOLON, TokenType::RBRACE)) {
                stream.consume();
                // Continue loop to parse next field
            } else {
                break;
            }
        }
    }
    
    // ─── Parse closing brace ──────────────────────────────────────────────
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '}' to close struct literal");
    } else {
        stream.consume(); // Consume '}'
    }
    
    // ─── Build AST ────────────────────────────────────────────────────────
    auto builder = ctx.arena.makeBuilder<FieldInitAST*>();
    for (auto* init : inits) {
        builder.push_back(init);
    }

    auto* structLit = ctx.arena.make<StructLiteralExprAST>(typeName, genericArgs, builder.build());
    structLit->loc = loc;
    
    return structLit;
}

// =============================================================================
// Anonymous Function
// =============================================================================

AnonFuncExprAST* parseAnonFuncExpr(TokenStream& stream, ParserContext& ctx) {
    SourceLocation funcTypeLoc = stream.currentLoc();
    
    // ─── Parse the leading cluster - this one has names ──────────────────
    std::vector<ParamAST*> leadingParams;
    
    while (stream.check(TokenType::LPAREN)) {
        std::vector<ParamAST*> groupParams = parseParamList(stream, ctx, true);  // allowNames = true
        for (auto* p : groupParams) {
            leadingParams.push_back(p);
        }
        if (stream.check(TokenType::ARROW)) {
            break;
        }
        // No '->' means more adjacent groups
    }
    
    // ─── Parse the rest of the function type ──────────────────────────────
    TypeAST* restType = nullptr;
    
    if (stream.check(TokenType::ARROW)) {
        stream.consume(); // Consume '->'
        restType = parseType(stream, ctx);  // This could be another FuncTypeAST
        if (!restType) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "expected return type");
            return nullptr;
        }
    }
    
    // ─── Build the FuncTypeAST ─────────────────────────────────────────────
    auto* funcType = ctx.arena.make<FuncTypeAST>();
    funcType->loc = funcTypeLoc;
    
    auto paramBuilder = ctx.arena.makeBuilder<ParamAST*>();
    for (auto* p : leadingParams) {
        paramBuilder.push_back(p);
    }
    funcType->params = paramBuilder.build();
    funcType->returnType = restType;
    funcType->hasArrow = (restType != nullptr);
    
    // ─── Parse the body ────────────────────────────────────────────────────
    if (!stream.check(TokenType::LBRACE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '{', got '", stream.peekValue(), "'");
        return nullptr;
    }

    ScopedContext bodyGuard(ctx, SyntacticContext::FuncBody, stream.currentLoc());
    
    StmtAST* body = parseBlock(stream, ctx);
    if (!body) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedBlock, stream.currentLoc(),
                                "expected block body");
        return nullptr;
    }
    
    auto* anonFunc = ctx.arena.make<AnonFuncExprAST>(funcType, body);
    anonFunc->loc = funcTypeLoc;
    
    return anonFunc;
}

// =============================================================================
// Postfix Expressions
// =============================================================================

ExprAST* parsePostfixExpr(TokenStream& stream, ParserContext& ctx, ExprAST* lhs) {
    
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
        return nullptr;
    }
    
    // ─── Parse argument list ──────────────────────────────────────────────
    ArenaSpan<ExprAST*> args = parseArgList(stream, ctx);
    
    // ─── Check for '!' (argument pack) ────────────────────────────────────
    // '!' is ONLY valid after an argument list in pipeline context.
    // But parseCallExpr doesn't know if it's in a pipeline context.
    // We still parse it and set hasArgPack, then let the caller validate.
    bool hasArgPack = stream.match(TokenType::BANG);
    
    auto* call = ctx.arena.make<CallExprAST>(hasArgPack);
    call->loc = loc;
    call->callee = callee;
    call->genericArgs = genericArgs;
    call->args = args;

    return call;
}

IntrinsicCallExprAST* parseIntrinsicCallExpr(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.check(TokenType::HASH)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected '#', got '", stream.peekValue(), "'");
        return nullptr;
    }
    stream.consume();
    
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, loc,
                                "expected intrinsic name, got '", stream.peekValue(), "'");
        return nullptr;
    }
    Token nameTok = stream.consume();
    InternedString intrinsicName = ctx.pool.intern(nameTok.value);
    
    ArenaSpan<ExprAST*> args = parseArgList(stream, ctx);
    
    auto* intrinsic = ctx.arena.make<IntrinsicCallExprAST>(intrinsicName);
    intrinsic->loc = loc;
    intrinsic->args = args;
    
    return intrinsic;
}

IndexExprAST* parseIndexExpr(TokenStream& stream, ParserContext& ctx, ExprAST* target) {
    SourceLocation loc = stream.currentLoc();
    
    if (!target) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "expected target for index");
        return nullptr;
    }
    
    if (!stream.check(TokenType::LBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected '[' for index expression, got '", stream.peekValue(), "'");
        return nullptr;
    }
    stream.consume();
    
    if (stream.check(TokenType::RBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "empty index expression '[]', please fill a value '[ <?> ]'");
        stream.consume(); // Consume ']'
        return nullptr;
    }
    
    ExprAST* index = parseExpr(stream, ctx);
    if (!index) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected index expression, got '", stream.peekValue(),  "'");
        return nullptr;
    }
    
    if (!stream.check(TokenType::RBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ']' to close index expression, got '", stream.peekValue(), "'");
        return nullptr;
    }
    stream.consume(); // Consume ']'
    
    auto* indexExpr = ctx.arena.make<IndexExprAST>(target, index);
    indexExpr->loc = loc;
    
    return indexExpr;
}

SliceExprAST* parseSliceExpr(TokenStream& stream, ParserContext& ctx, ExprAST* target) {
    SourceLocation loc = stream.currentLoc();
    
    if (!target) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "expected target for slice");
        return nullptr;
    }
    
    if (!stream.check(TokenType::LBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected '[' for slice expression, got '", stream.peekValue(), "'");
        return nullptr;
    }
    stream.consume();
    
    // ─── Check for empty slice ──────────────────────────────────────────────
    if (stream.check(TokenType::RBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "empty slice expression '[]', expected 'low..high' or '..high'");
        stream.consume(); // Consume ']'
        return nullptr;
    }
    
    ExprAST* start = nullptr;
    ExprAST* end = nullptr;
    bool isExclusive = false;
    
    // ─── Parse start expression ──────────────────────────────────────────
    // Check if we have a range operator first (meaning start is omitted)
    if (stream.check(TokenType::RANGE) || stream.check(TokenType::RANGE_EXCLUSIVE)) {
        // Start is omitted (e.g., [..end] or [..<end])
        start = nullptr;
    } else {
        // Parse start expression
        start = parseExpr(stream, ctx);
        if (!start) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected slice start expression or '..'");
            return nullptr;
        }
    }
    
    // ─── Parse range operator ──────────────────────────────────────────────
    if (stream.check(TokenType::RANGE) || stream.check(TokenType::RANGE_EXCLUSIVE)) {
        isExclusive = stream.match(TokenType::RANGE_EXCLUSIVE);
        if (!isExclusive) {
            stream.match(TokenType::RANGE);
        }
    } else {
        // No range operator - this is an error
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '..' or '..<', got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    // ─── Parse end expression ──────────────────────────────────────────────
    if (stream.check(TokenType::RBRACKET)) {
        // End is omitted (e.g., [start..] or [..])
        end = nullptr;
    } else {
        end = parseExpr(stream, ctx);
        if (!end) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected slice end expression");
            return nullptr;
        }
    }
    
    // ─── Parse closing bracket ──────────────────────────────────────────────
    if (!stream.check(TokenType::RBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ']', got '", stream.peekValue(), "'");
        return nullptr;
    }
    stream.consume(); // Consume ']'
    
    // ─── Build and return the slice ──────────────────────────────────────
    auto* slice = ctx.arena.make<SliceExprAST>(target, start, end, isExclusive);
    slice->loc = loc;

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
    
    if (!lhs) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "expected object for field access");
        return nullptr;
    }
    
    // ─── 1. Consume '.' ──────────────────────────────────────────────────
    if (!stream.match(TokenType::DOT)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected '.', got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    // ─── 2. Parse field name ──────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected field name, got '", stream.peekValue(), "'");
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
        
        // ─── Error Recovery: Consume the ':' and skip to the next statement ──
        stream.consume(); // Consume ':'
        
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
    
    // ─── 1. Parse module name ─────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected module name, got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    Token moduleTok = stream.consume();
    InternedString moduleName = ctx.pool.intern(moduleTok.value);
    
    // ─── 2. Expect ':' ────────────────────────────────────────────────────
    if (!stream.match(TokenType::COLON)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ':', got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    // ─── 3. Parse member name ─────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected member name, got '", stream.peekValue(), "'");
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
    
    return moduleAccess;
}

// =============================================================================
// Pipeline & Composition
// =============================================================================

ExprAST* parsePipelineExpr(TokenStream& stream, ParserContext& ctx, ExprAST* seed) {
    SourceLocation loc = stream.currentLoc();
    
    if (!seed) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "expected pipeline seed");
        return nullptr;
    }
    
    std::vector<PipelineStepAST*> steps;
    
    while (!stream.isAtEnd() && stream.check(TokenType::PIPELINE)) {
        int count = stream.consumeTrailing(TokenType::PIPELINE);
        if (count == 0) {
             ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected pipeline step after '|>'");
        } else if (count == 2) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.previousLoc(),
                                    "expected step in pipeline");
        } else if (count > 3) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                    "unexpected consecutive '|>' operators");
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
        return nullptr;
    }

    auto builder = ctx.arena.makeBuilder<PipelineStepAST*>();
    for (auto* s : steps) {
        builder.push_back(s);
    }

    auto* pipeline = ctx.arena.make<PipelineExprAST>(seed, builder.build());
    pipeline->loc = loc;
    
    return pipeline;
}

PipelineStepAST* parsePipelineStep(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "expected pipeline step");
        return nullptr;
    }
    
    // ─── Anonymous function step ──────────────────────────────────────────
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
    
    // ─── Parse the expression ──────────────────────────────────────────────
    ExprAST* expr = parseExpr(stream, ctx);
    if (!expr) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected pipeline step expression");
        return nullptr;
    }
    
    // ─── REJECT: Intrinsic calls are not allowed in pipeline steps ────────
    // IntrinsicCallExprAST represents a complete call, not a function reference.
    // The user should use a wrapper function instead:
    //   const sqrtWrapper (x float) -> float = { return #sqrt(x) }
    if (expr->isa<IntrinsicCallExprAST>()) {
        IntrinsicCallExprAST* intrinsic = expr->as<IntrinsicCallExprAST>();
        ctx.diagnostics.errorAt(DiagCode::Sem_PipelineMismatch, intrinsic->loc,
                                "intrinsic call cannot be used as a pipeline step");
        ctx.diagnostics.noteAt(intrinsic->loc,
                               "Intrinsic '#", ctx.pool.lookup(intrinsic->intrinsicName),
                               "' is a complete call, not a function reference");
        ctx.diagnostics.noteAt(intrinsic->loc,
                               "Wrap it in a function: 'const wrapper (x T) -> R = { return #",
                               ctx.pool.lookup(intrinsic->intrinsicName), "(x) }'");
        return nullptr;
    }
    
    // ─── Check for stray '!' after generic args ──────────────────────────
    if (stream.check(TokenType::BANG)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "unexpected '!' - argument pack '!' must follow an argument list '(args)'");
        ctx.diagnostics.noteAt(expr->loc,
                               "Use '", stream.peekValue(), "(args)!' to create an argument pack");
        stream.consume();
        return nullptr;
    }
    
    // ─── Extract callee and pack args ──────────────────────────────────
    ArenaSpan<ExprAST*> packArgs = ctx.arena.makeBuilder<ExprAST*>().build();
    ExprAST* callee = expr;
    
    // ─── CallExprAST (fn(args) or fn(args)!) ─────────────────────
    if (expr->isa<CallExprAST>()) {
        CallExprAST* call = expr->as<CallExprAST>();
        
        // ─── Check: fn(args) without '!' is SYNTAX ERROR ────────────────
        if (!call->hasArgPack) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, call->loc,
                                    "expected '!' after argument list in pipeline step");
            ctx.diagnostics.noteAt(call->callee->loc,
                                   "In a pipeline step, use 'fn(args)!' to inject upstream values");
            return nullptr;
        }
        
        // Valid: fn(args)! - extract callee and args
        callee = call->callee;
        packArgs = call->args;
    }
    
    // ─── Build the pipeline step ──────────────────────────────────────────
    auto* step = ctx.arena.make<PipelineStepAST>(callee, packArgs);
    step->loc = loc;
    
    return step;
}

ExprAST* parseComposeExpr(TokenStream& stream, ParserContext& ctx, ExprAST* lhs) {
    SourceLocation loc = stream.currentLoc();
    
    if (!lhs) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "expected left-hand side");
        return nullptr;
    }
    
    if (!stream.check(TokenType::COMPOSE)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                                "expected '+>', got '", stream.peekValue(), "'");
        return nullptr;
    }
    stream.consume(); // Consume '+>'
    
    std::vector<ComposeOperandAST*> operands;
    
    // ─── Parse first operand ──────────────────────────────────────────────
    ComposeOperandAST* operand = parseComposeOperand(stream, ctx);
    if (!operand) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected composition operand");
        return nullptr;
    }
    operands.push_back(operand);
    
    // ─── Parse additional operands ────────────────────────────────────────
    while (!stream.isAtEnd() && stream.check(TokenType::COMPOSE)) {
        int count = stream.consumeTrailing(TokenType::COMPOSE);
        if (count == 0) {
             ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected '+>' to separate operand");
        } else if (count == 2) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.previousLoc(),
                                    "expected operand after '+>'");
        } else if (count > 3) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                    "unexpected consecutive '+>'");
        }
        
        operand = parseComposeOperand(stream, ctx);
        if (!operand) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected composition operand");
            return nullptr;
        }
        operands.push_back(operand);
    }

    auto builder = ctx.arena.makeBuilder<ComposeOperandAST*>();
    for (auto* op : operands) {
        builder.push_back(op);
    }
    
    auto* compose = ctx.arena.make<ComposeExprAST>(lhs, builder.build());
    compose->loc = loc;
    
    return compose;
}

ComposeOperandAST* parseComposeOperand(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    ExprAST* callable = parseExpr(stream, ctx);
    if (!callable) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected composition operand");
        return nullptr;
    }
    
    // ─── Extract generic arguments if present ─────────────────────────────
    // FieldAccessExprAST intentionally omitted - no genericArgs field
    ArenaSpan<TypeAST*> genericArgs;
    
    if (callable->isa<IdentifierExprAST>()) {
        auto* idExpr = callable->as<IdentifierExprAST>();
        genericArgs = idExpr->genericArgs;
    } else if (callable->isa<ModuleAccessExprAST>()) {
        auto* moduleAccess = callable->as<ModuleAccessExprAST>();
        genericArgs = moduleAccess->genericArgs;
    }
    
    auto* operand = ctx.arena.make<ComposeOperandAST>(callable, genericArgs);
    operand->loc = loc;

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
    
    if (!lhs) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "expected left-hand side");
        return nullptr;
    }
    
    ExprAST* rhs = parsePrattExpr(stream, ctx, infixPrec(opTok));
    if (!rhs) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected right-hand side");
        return nullptr;
    }
    
    AssignOp op = tokenToAssignOp(opTok);
    
    auto* assign = ctx.arena.make<AssignExprAST>(op);
    assign->loc = loc;
    assign->lhs = lhs;
    assign->rhs = rhs;
    
    return assign;
}

ExprAST* parseInfixNullCoalesce(TokenStream& stream, ParserContext& ctx, ExprAST* lhs) {
    SourceLocation loc = stream.currentLoc();
    
    if (!lhs) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                                "expected left-hand side");
        return nullptr;
    }
    
    ExprAST* rhs = parsePrattExpr(stream, ctx, infixPrec(TokenType::QUESTION_QUESTION));
    if (!rhs) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                "expected right-hand side");
        return nullptr;
    }
    
    auto* coalesce = ctx.arena.make<NullCoalesceExprAST>(lhs, rhs);
    coalesce->loc = loc;

    return coalesce;
}

ExprAST* parseInfixBinary(TokenStream& stream, ParserContext& ctx, ExprAST* lhs, TokenType opTok, int prec) {
    SourceLocation loc = stream.currentLoc();
    
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
    
    return binary;
}

} // namespace parser