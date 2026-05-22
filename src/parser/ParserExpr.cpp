/**
 * @file ParserExpr.cpp
 * 
 * Expression parsing using Pratt parser.
 * Handles literals, operators, calls, indexing, pipelines, composition,
 * and all expression-specific grammar rules.
 */

#include "Parser.hpp"
#include "ast/ExprAST.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

#include <cassert>
#include <string>

namespace {
    // Precedence levels
    constexpr int PREC_NONE = 0;
    constexpr int PREC_ASSIGN = 1;
    constexpr int PREC_COMPOSE = 2;
    constexpr int PREC_PIPE = 3;
    constexpr int PREC_NULLCOAL = 4;
    constexpr int PREC_OR = 5;
    constexpr int PREC_AND = 6;
    constexpr int PREC_CMP = 7;
    constexpr int PREC_BITWISE = 8;
    constexpr int PREC_ADD = 10;
    constexpr int PREC_MUL = 11;
    constexpr int PREC_POW = 12;
}

// -----------------------------------------------------------------------------
// Precedence helpers
// -----------------------------------------------------------------------------

int Parser::infixPrec(TokenType t) const {
    switch (t) {
        case TokenType::ASSIGN:
        case TokenType::PLUS_ASSIGN:
        case TokenType::MINUS_ASSIGN:
        case TokenType::MUL_ASSIGN:
        case TokenType::DIV_ASSIGN:
        case TokenType::POW_ASSIGN:
        case TokenType::MOD_ASSIGN:
        case TokenType::BIT_AND_ASSIGN:
        case TokenType::BIT_OR_ASSIGN:
        case TokenType::BIT_XOR_ASSIGN:
        case TokenType::SHL_ASSIGN:
        case TokenType::SHR_ASSIGN:
            return PREC_ASSIGN;
        case TokenType::COMPOSE:            return PREC_COMPOSE;
        case TokenType::PIPELINE:           return PREC_PIPE;
        case TokenType::QUESTION_QUESTION:  return PREC_NULLCOAL;
        case TokenType::OR:                 return PREC_OR;
        case TokenType::AND:                return PREC_AND;
        case TokenType::EQUAL_EQUAL:
        case TokenType::EQUAL_EQUAL_EQUAL:
        case TokenType::NOT_EQUAL:
        case TokenType::LESS:
        case TokenType::GREATER:
        case TokenType::LESS_EQUAL:
        case TokenType::GREATER_EQUAL:
        case TokenType::IS:
            return PREC_CMP;
        case TokenType::BIT_AND:
        case TokenType::BIT_OR:
        case TokenType::BIT_XOR:
        case TokenType::SHL:
        case TokenType::SHR:
            return PREC_BITWISE;
        case TokenType::PLUS:
        case TokenType::MINUS:
            return PREC_ADD;
        case TokenType::MUL:
        case TokenType::DIV:
        case TokenType::MOD:
            return PREC_MUL;
        case TokenType::POW:
            return PREC_POW;
        default:
            return PREC_NONE;
    }
}

BinaryOp Parser::tokenToBinaryOp(TokenType t) const {
    switch (t) {
        case TokenType::PLUS:                return BinaryOp::Add;
        case TokenType::MINUS:               return BinaryOp::Sub;
        case TokenType::MUL:                 return BinaryOp::Mul;
        case TokenType::DIV:                 return BinaryOp::Div;
        case TokenType::POW:                 return BinaryOp::Pow;
        case TokenType::MOD:                 return BinaryOp::Mod;
        case TokenType::EQUAL_EQUAL:         return BinaryOp::Eq;
        case TokenType::EQUAL_EQUAL_EQUAL:   return BinaryOp::RefEq;
        case TokenType::NOT_EQUAL:           return BinaryOp::Ne;
        case TokenType::LESS:                return BinaryOp::Lt;
        case TokenType::GREATER:             return BinaryOp::Gt;
        case TokenType::LESS_EQUAL:          return BinaryOp::Le;
        case TokenType::GREATER_EQUAL:       return BinaryOp::Ge;
        case TokenType::AND:                 return BinaryOp::And;
        case TokenType::OR:                  return BinaryOp::Or;
        case TokenType::BIT_AND:             return BinaryOp::BitAnd;
        case TokenType::BIT_OR:              return BinaryOp::BitOr;
        case TokenType::BIT_XOR:             return BinaryOp::BitXor;
        case TokenType::SHL:                 return BinaryOp::Shl;
        case TokenType::SHR:                 return BinaryOp::Shr;
        default:                             return BinaryOp::Add;
    }
}

AssignOp Parser::tokenToAssignOp(TokenType t) const {
    switch (t) {
        case TokenType::ASSIGN:          return AssignOp::Assign;
        case TokenType::PLUS_ASSIGN:     return AssignOp::AddAssign;
        case TokenType::MINUS_ASSIGN:    return AssignOp::SubAssign;
        case TokenType::MUL_ASSIGN:      return AssignOp::MulAssign;
        case TokenType::DIV_ASSIGN:      return AssignOp::DivAssign;
        case TokenType::POW_ASSIGN:      return AssignOp::PowAssign;
        case TokenType::MOD_ASSIGN:      return AssignOp::ModAssign;
        case TokenType::BIT_AND_ASSIGN:  return AssignOp::BitAndAssign;
        case TokenType::BIT_OR_ASSIGN:   return AssignOp::BitOrAssign;
        case TokenType::BIT_XOR_ASSIGN:  return AssignOp::BitXorAssign;
        case TokenType::SHL_ASSIGN:      return AssignOp::ShlAssign;
        case TokenType::SHR_ASSIGN:      return AssignOp::ShrAssign;
        default:                         return AssignOp::Assign;
    }
}

bool Parser::isAssignOp(TokenType t) const {
    switch (t) {
        case TokenType::ASSIGN:
        case TokenType::PLUS_ASSIGN:
        case TokenType::MINUS_ASSIGN:
        case TokenType::MUL_ASSIGN:
        case TokenType::DIV_ASSIGN:
        case TokenType::POW_ASSIGN:
        case TokenType::MOD_ASSIGN:
        case TokenType::BIT_AND_ASSIGN:
        case TokenType::BIT_OR_ASSIGN:
        case TokenType::BIT_XOR_ASSIGN:
        case TokenType::SHL_ASSIGN:
        case TokenType::SHR_ASSIGN:
            return true;
        default:
            return false;
    }
}

// -----------------------------------------------------------------------------
// Pratt parser core
// -----------------------------------------------------------------------------

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

// -----------------------------------------------------------------------------
// Infix operator handlers
// -----------------------------------------------------------------------------

ExprPtr Parser::parseInfixAssign(ExprPtr lhs, bool allowStructLiteral) {
    TokenType opTok = ts_.advance().type;
    AssignOp op = tokenToAssignOp(opTok);
    
    ExprPtr rhs = parsePrattExpr(PREC_ASSIGN - 1, allowStructLiteral);
    if (!rhs) {
        errorAt(DiagCode::E2008, "expected expression after assignment operator");
        return lhs;
    }

    auto node = arena_.make<AssignExprAST>();
    node->loc = lhs->loc;
    node->op = op;
    node->lhs = std::move(lhs);
    node->rhs = std::move(rhs);
    return node;
}

ExprPtr Parser::parseInfixIs(ExprPtr lhs) {
    ts_.advance();
    TypePtr checkType = parseType();
    if (!checkType) {
        errorAt(DiagCode::E2005, "expected type after 'is'");
        return lhs;
    }

    auto node = arena_.make<IsExprAST>();
    node->loc = lhs->loc;
    node->expr = std::move(lhs);
    node->checkType = std::move(checkType);
    return node;
}

ExprPtr Parser::parseInfixNullCoalesce(ExprPtr lhs, bool allowStructLiteral) {
    ts_.advance();
    
    ExprPtr fallback = parsePrattExpr(PREC_NULLCOAL - 1, allowStructLiteral);
    if (!fallback) {
        errorAt(DiagCode::E2008, "expected expression after '\?\?'");
        return lhs;
    }

    auto node = arena_.make<NullCoalesceExprAST>();
    node->loc = lhs->loc;
    node->value = std::move(lhs);
    node->fallback = std::move(fallback);
    return node;
}

ExprPtr Parser::parseInfixBinary(ExprPtr lhs, TokenType opTok, int prec, bool allowStructLiteral) {
    ts_.advance();
    
    int nextPrec = (opTok == TokenType::POW) ? prec - 1 : prec;
    ExprPtr rhs = parsePrattExpr(nextPrec, allowStructLiteral);
    if (!rhs) {
        errorAt(DiagCode::E2008, "expected right-hand side of binary expression");
        return lhs;
    }

    // Chained comparison detection
    auto isComparisonOp = [](TokenType tt) {
        switch (tt) {
            case TokenType::EQUAL_EQUAL:
            case TokenType::EQUAL_EQUAL_EQUAL:
            case TokenType::NOT_EQUAL:
            case TokenType::LESS:
            case TokenType::GREATER:
            case TokenType::LESS_EQUAL:
            case TokenType::GREATER_EQUAL:
            case TokenType::IS:
                return true;
            default:
                return false;
        }
    };

    if (isComparisonOp(opTok) && isComparisonOp(ts_.peekType())) {
        errorAt(DiagCode::E3014, "chained comparisons not allowed; use 'and' explicitly");
    }

    auto node = arena_.make<BinaryExprAST>();
    node->loc = lhs->loc;
    node->op = tokenToBinaryOp(opTok);
    node->left = std::move(lhs);
    node->right = std::move(rhs);
    return node;
}

// -----------------------------------------------------------------------------
// Prefix & primary parsers
// -----------------------------------------------------------------------------

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

ExprPtr Parser::parsePrimaryExpr(bool allowStructLiteral) {
    SourceLocation loc = ts_.currentLoc();

    // match expression
    if (ts_.check(TokenType::MATCH)) {
        return parseMatchExpr();
    }

    // if expression
    if (ts_.check(TokenType::IF)) {
        return parseIfExpr();
    }

    // #intrinsic call
    if (ts_.check(TokenType::HASH)) {
        return parseIntrinsicCallExpr();
    }

    // await
    if (ts_.check(TokenType::AWAIT)) {
        return parseAwaitExpr();
    }

    // array literal
    if (ts_.check(TokenType::LBRACKET)) {
        return parseArrayLiteralExpr();
    }

    // bare '{' error
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

    // anonymous function
    if (looksLikeAnonFunc()) {
        return parseAnonFuncExpr();
    }

    // grouped expression
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

    // '*' unsafe cast
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

    // identifier
    if (ts_.check(TokenType::IDENTIFIER)) {
        std::string name = ts_.peek().value;

        // struct literal
        if (allowStructLiteral && looksLikeStructLiteral()) {
            ts_.advance();
            ArenaSpan<TypePtr> genericArgs;
            if (ts_.check(TokenType::LESS)) {
                genericArgs = parseGenericArgs();
            }
            return parseStructLiteralExpr(name, genericArgs);
        }

        // behavior access
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

        // plain identifier
        ts_.advance();
        auto node = arena_.make<IdentifierExprAST>(pool_.intern(name));
        node->loc = loc;
        return node;
    }

    // primitive type cast
    if (looksLikeType() && ts_.peekNextType() == TokenType::LPAREN) {
        TypePtr targetType = parsePrimitiveType();
        if (targetType && ts_.check(TokenType::LPAREN)) {
            return parseTypeConvExpr(false, std::move(targetType));
        }
    }

    // literal
    return parseLiteralExpr();
}

ExprPtr Parser::parsePostfixExpr(ExprPtr lhs) {
    while (true) {
        if (ts_.check(TokenType::RPAREN)) break;
        if (ts_.check(TokenType::PIPELINE) || ts_.check(TokenType::COMPOSE)) break;

        // function call
        if (ts_.check(TokenType::LPAREN)) {
            lhs = parseCallExpr(std::move(lhs), ArenaSpan<TypePtr>());
            continue;
        }

        // generic call
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

        // index/slice
        if (ts_.check(TokenType::LBRACKET)) {
            lhs = parseIndexExpr(std::move(lhs));
            continue;
        }

        // field access
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

        // nullable chain
        if (ts_.check(TokenType::QUESTION_DOT)) {
            // Collect all consecutive '?.' steps in one go
            std::vector<InternedString> steps;
            ExprPtr object = std::move(lhs);
            
            while (ts_.check(TokenType::QUESTION_DOT)) {
                ts_.advance(); // consume '?.'
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

// -----------------------------------------------------------------------------
// Literal and value parsers
// -----------------------------------------------------------------------------

ExprPtr Parser::parseLiteralExpr() {
    SourceLocation loc = ts_.currentLoc();
    Token tok = ts_.advance();

    LiteralKind kind;
    switch (tok.type) {
        case TokenType::INT_LITERAL:        kind = LiteralKind::Int; break;
        case TokenType::FLOAT_LITERAL:      kind = LiteralKind::Float; break;
        case TokenType::STRING_LITERAL:     kind = LiteralKind::String; break;
        case TokenType::RAW_STRING_LITERAL: kind = LiteralKind::RawString; break;
        case TokenType::CHAR_LITERAL:       kind = LiteralKind::Char; break;
        case TokenType::HEX_LITERAL:        kind = LiteralKind::Hex; break;
        case TokenType::BINARY_LITERAL:     kind = LiteralKind::Binary; break;
        case TokenType::TRUE:               kind = LiteralKind::True; break;
        case TokenType::FALSE:              kind = LiteralKind::False; break;
        case TokenType::NIL:                kind = LiteralKind::Nil; break;
        default:
            errorAt(DiagCode::E2002, "internal error: parseLiteralExpr on non-literal token");
            return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<LiteralExprAST>(kind, pool_.intern(tok.value));
    node->loc = loc;
    return node;
}

ExprPtr Parser::parseArrayLiteralExpr() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LBRACKET, "expected '['");

    std::vector<ExprPtr> elements;  // temporary

    while (!ts_.check(TokenType::RBRACKET) && !ts_.isAtEnd()) {
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACKET)) break;

        size_t beforePos = ts_.getPos();
        ExprPtr elem = parseExpr();
        if (ts_.getPos() == beforePos) {
            errorAt(DiagCode::E2008, "expected expression inside array literal");
            if (!ts_.isAtEnd()) ts_.advance();
            break;
        }
        elements.push_back(std::move(elem));
    }

    ts_.consume(TokenType::RBRACKET, "expected ']' to close array literal");

    auto node = arena_.make<ArrayLiteralExprAST>();
    node->loc = loc;

    auto builder = arena_.makeBuilder<ExprPtr>();
    for (auto& e : elements) builder.push_back(std::move(e));
    node->elements = builder.build();

    return node;
}

ExprPtr Parser::parseStructLiteralExpr(std::string typeName, ArenaSpan<TypePtr> genericArgs) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LBRACE, "expected '{' to open struct literal");

    auto node = arena_.make<StructLiteralExprAST>();
    node->loc = loc;
    node->typeName = pool_.intern(typeName);
    node->genericArgs = genericArgs;

    std::vector<FieldInitPtr> inits;
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::COMMA);
        ts_.match(TokenType::SEMICOLON);
        if (ts_.check(TokenType::RBRACE)) break;

        SourceLocation fieldLoc = ts_.currentLoc();

        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected field name in struct literal");
            ts_.advance();
            continue;
        }
        std::string fieldName = ts_.advance().value;

        ts_.consume(TokenType::ASSIGN, "expected '=' after field name");
        ExprPtr val = parseExpr();
        if (!val) {
            errorAt(DiagCode::E2008, "expected expression for field");
            continue;
        }

        auto init = arena_.make<FieldInitAST>(pool_.intern(fieldName), std::move(val));
        init->loc = fieldLoc;
        inits.push_back(std::move(init));
    }

    auto builder = arena_.makeBuilder<FieldInitPtr>();
    for (auto& i : inits) builder.push_back(std::move(i));
    node->inits = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close struct literal");
    return node;
}

ExprPtr Parser::parseAnonFuncExpr() {
    SourceLocation loc = ts_.currentLoc();
    
    auto node = arena_.make<AnonFuncExprAST>();
    node->loc = loc;
    
    if (ts_.check(TokenType::TILDE)) {
        errorAt(DiagCode::E2015, "anonymous function cannot have qualifiers");
        while (ts_.check(TokenType::TILDE)) {
            ts_.advance();
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected qualifier name after '~'");
                break;
            }
            ts_.advance();
        }
    }
    
    if (!ts_.check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' to start anonymous function parameters");
        return arena_.make<UnknownExprAST>();
    }
    
    // Parameter groups: flat accumulation
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    while (ts_.check(TokenType::LPAREN)) {
        ParamGroup group = parseParamGroup();
        groupSizes.push_back(group.size());
        for (size_t i = 0; i < group.size(); ++i) {
            allParams.push_back(group[i]);
        }
    }
    auto paramsBuilder = arena_.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramsBuilder.push_back(std::move(p));
    node->sig.allParams = paramsBuilder.build();
    
    auto gsBuilder = arena_.makeBuilder<size_t>();
    for (auto& sz : groupSizes) gsBuilder.push_back(sz);
    node->sig.groupSizes = gsBuilder.build();
    
    if (ts_.check(TokenType::ARROW)) {
        ts_.advance();
        node->sig.returnTypes = parseReturnList();
        if (node->sig.returnTypes.empty() && !ts_.check(TokenType::LBRACE)) {
            errorAt(DiagCode::E2005, "expected return type after '->' in anonymous function");
        }
    }
    
    if (!ts_.check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start anonymous function body");
        return arena_.make<UnknownExprAST>();
    }
    node->body = parseBlock();
    
    return node;
}

ExprPtr Parser::parseAwaitExpr(bool allowStructLiteral) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::AWAIT, "expected 'await'");

    if (parallelDepth_ > 0) {
        error(loc, DiagCode::E2006, "'await' is not valid inside a 'parallel' block");
    }

    ExprPtr inner = parsePrattExpr(PREC_NONE, allowStructLiteral);
    if (!inner) {
        errorAt(DiagCode::E2008, "expected expression after 'await'");
        return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<AwaitExprAST>(std::move(inner));
    node->loc = loc;
    return node;
}

ExprPtr Parser::parseIfExpr(bool allowStructLiteral) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::IF, "expected 'if'");

    ExprPtr condition = parsePrattExpr(PREC_NULLCOAL, allowStructLiteral);
    if (!condition) {
        errorAt(DiagCode::E2008, "expected condition after 'if'");
        return arena_.make<UnknownExprAST>();
    }

    if (!ts_.match(TokenType::QUESTION_QUESTION)) {
        errorAt(DiagCode::E2001, "expected '\?\?' after if condition in expression form");
        return arena_.make<UnknownExprAST>();
    }

    ExprPtr thenBranch = parseExpr();
    if (!thenBranch) {
        errorAt(DiagCode::E2008, "expected expression after '\?\?'");
    }

    if (!ts_.match(TokenType::ELSE)) {
        errorAt(DiagCode::E2006, "expression-form 'if' requires an 'else' branch");
        return arena_.make<UnknownExprAST>();
    }

    ExprPtr elseBranch = parseExpr();
    if (!elseBranch) {
        errorAt(DiagCode::E2008, "expected expression after 'else'");
    }

    auto node = arena_.make<IfExprAST>();
    node->loc = loc;
    node->condition = std::move(condition);
    node->thenBranch = std::move(thenBranch);
    node->elseBranch = std::move(elseBranch);
    return node;
}

ExprPtr Parser::parseTypeConvExpr(bool isUnsafe, TypePtr targetType) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LPAREN, "expected '(' for explicit type cast");

    ExprPtr expr = parseExpr();
    if (!expr) {
        errorAt(DiagCode::E2008, "expected expression inside explicit type cast");
        return arena_.make<UnknownExprAST>();
    }

    ts_.consume(TokenType::RPAREN, "expected ')' to close explicit type cast");

    auto node = arena_.make<TypeConvExprAST>(std::move(targetType), std::move(expr), isUnsafe);
    node->loc = loc;
    return node;
}

ExprPtr Parser::parseRangeExpr(ExprPtr lo, bool allowStructLiteral) {
    SourceLocation loc = lo->loc;
    ts_.consume(TokenType::RANGE, "expected '..'");

    bool isExclusive = ts_.match(TokenType::LESS);
    ExprPtr hi = parsePrattExpr(PREC_ADD, allowStructLiteral);
    if (!hi) {
        errorAt(DiagCode::E2008, "expected upper bound after '..'");
        return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<RangeExprAST>();
    node->loc = loc;
    node->lo = std::move(lo);
    node->hi = std::move(hi);
    node->isExclusive = isExclusive;
    return node;
}

// -----------------------------------------------------------------------------
// Intrinsic call
// -----------------------------------------------------------------------------

ExprPtr Parser::parseIntrinsicCallExpr() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::HASH, "expected '#'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected intrinsic name after '#'");
        return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<IntrinsicCallExprAST>();
    node->loc = loc;
    node->intrinsicName = pool_.intern(ts_.advance().value);

    if (!ts_.check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' after intrinsic name");
        return arena_.make<UnknownExprAST>();
    }
    ts_.advance();

    std::string intrinsicStr = std::string(pool_.lookup(node->intrinsicName));
    bool isTypeIntrinsic = (intrinsicStr == "sizeof" || intrinsicStr == "alignof");

    if (isTypeIntrinsic) {
        if (ts_.check(TokenType::RPAREN)) {
            errorAt(DiagCode::E2005, "expected type argument");
        } else {
            TypePtr typeArg = parseType();
            if (!typeArg) errorAt(DiagCode::E2005, "invalid type argument");
            else node->typeArg = std::move(typeArg);
        }
        ts_.consume(TokenType::RPAREN, "expected ')' after type argument");
    } else {
        std::vector<ExprPtr> args;
        while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
            size_t savedPos = ts_.getPos();
            ExprPtr arg = parseExpr();
            if (ts_.getPos() == savedPos) {
                errorAt(DiagCode::E2008, "expected argument expression");
                if (!ts_.isAtEnd()) ts_.advance();
                break;
            }
            args.push_back(std::move(arg));
            if (ts_.check(TokenType::RPAREN)) break;
            if (!ts_.match(TokenType::COMMA)) {
                errorAt(DiagCode::E2001, "expected ',' or ')' in intrinsic argument list");
                break;
            }
        }
        auto builder = arena_.makeBuilder<ExprPtr>();
        for (auto& a : args) builder.push_back(std::move(a));
        node->args = builder.build();
        ts_.consume(TokenType::RPAREN, "expected ')' to close intrinsic call");
    }

    return node;
}

// -----------------------------------------------------------------------------
// Call and index parsers
// -----------------------------------------------------------------------------

ExprPtr Parser::parseCallExpr(ExprPtr callee, ArenaSpan<TypePtr> genericArgs) {
    SourceLocation loc = callee->loc;
    ts_.consume(TokenType::LPAREN, "expected '('");

    auto node = arena_.make<CallExprAST>();
    node->loc = loc;
    node->callee = std::move(callee);
    node->genericArgs = genericArgs;

    if (!ts_.check(TokenType::RPAREN)) {
        node->args = parseArgList();
    }

    ts_.consume(TokenType::RPAREN, "expected ')' to close argument list");
    node->isArgPack = ts_.match(TokenType::BANG);

    return node;
}

ExprPtr Parser::parseIndexExpr(ExprPtr target) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LBRACKET, "expected '['");

    ExprPtr startExpr = parseExpr();
    if (!startExpr) {
        errorAt(DiagCode::E2008, "expected index expression");
        return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<IndexExprAST>();
    node->loc = loc;
    node->target = std::move(target);

    if (ts_.check(TokenType::RANGE)) {
        ts_.advance();
        bool isExclusive = ts_.match(TokenType::LESS);
        ExprPtr endExpr = parseExpr();
        if (!endExpr) {
            errorAt(DiagCode::E2008, "expected end of slice range after '..'");
            return arena_.make<UnknownExprAST>();
        }
        node->index = std::move(startExpr);
        node->sliceEnd = std::move(endExpr);
        node->kind = IndexKind::Slice;
        node->isExclusive = isExclusive;
    } else {
        node->index = std::move(startExpr);
        node->kind = IndexKind::Element;
    }

    ts_.consume(TokenType::RBRACKET, "expected ']' to close index expression");
    return node;
}

ArenaSpan<ExprPtr> Parser::parseArgList() {
    std::vector<ExprPtr> args;
    int consecutiveErrors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 5;

    while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
        if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
            errorAt(DiagCode::E2002, "too many consecutive errors in argument list; skipping to ')'");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RPAREN)) ts_.advance();
            break;
        }

        size_t savedPos = ts_.getPos();
        ExprPtr arg = parseExpr();

        if (ts_.getPos() == savedPos) {
            errorAt(DiagCode::E2008, "expected argument expression");
            if (!ts_.isAtEnd()) ts_.advance();
            consecutiveErrors++;
            if (ts_.check(TokenType::COMMA)) ts_.advance();
            continue;
        }

        consecutiveErrors = 0;
        args.push_back(std::move(arg));

        if (ts_.check(TokenType::RPAREN)) break;
        if (!ts_.match(TokenType::COMMA)) {
            errorAt(DiagCode::E2001, "expected ',' after argument");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::COMMA) && !ts_.check(TokenType::RPAREN)) {
                ts_.advance();
            }
            if (ts_.check(TokenType::COMMA)) ts_.advance();
            break;
        }
    }
    
    auto builder = arena_.makeBuilder<ExprPtr>();
    for (auto& a : args) builder.push_back(std::move(a));
    return builder.build();
}

// -----------------------------------------------------------------------------
// Pipeline and composition
// -----------------------------------------------------------------------------

ExprPtr Parser::parsePipelineExpr(ExprPtr seed) {
    if (!seed) {
        errorAt(DiagCode::E2008, "expected pipeline seed before '|>'");
        return arena_.make<UnknownExprAST>();
    }

    std::vector<PipelineStepPtr> steps;  // temporary

    while (ts_.check(TokenType::PIPELINE)) {
        ts_.advance();
        PipelineStepPtr step = parsePipelineStep();
        if (step) {
            steps.push_back(std::move(step));
        } else {
            break;
        }
    }

    if (steps.empty()) {
        errorAt(DiagCode::E2006, "pipeline '|>' requires at least one step");
        return seed;
    }

    auto node = arena_.make<PipelineExprAST>();
    node->loc = seed->loc;
    node->seed = std::move(seed);
    
    auto builder = arena_.makeBuilder<PipelineStepPtr>();
    for (auto& s : steps) builder.push_back(std::move(s));
    node->steps = builder.build();

    return node;
}

ExprPtr Parser::parseComposeExpr(ExprPtr lhs) {
    auto node = arena_.make<ComposeExprAST>();
    node->loc = lhs->loc;
    node->left = std::move(lhs);

    std::vector<ComposeOperandPtr> operands;

    while (ts_.check(TokenType::COMPOSE)) {
        ts_.advance();
        ComposeOperandPtr op = parseComposeOperand();
        if (!op) {
            errorAt(DiagCode::E2002, "expected function name after '+>'");
            break;
        }
        operands.push_back(std::move(op));
    }

    if (operands.empty()) {
        return std::move(node->left);
    }

    // Build ArenaSpan
    auto builder = arena_.makeBuilder<ComposeOperandPtr>();
    for (auto& op : operands) builder.push_back(std::move(op));
    node->operands = builder.build();

    return node;
}

PipelineStepPtr Parser::parsePipelineStep() {
    if (looksLikeAnonFunc()) {
        return parseAnonFuncPipelineStep();
    }

    bool isPrimitiveType = isPrimitiveTypeToken(ts_.peekType());
    if (!ts_.check(TokenType::IDENTIFIER) && !isPrimitiveType) {
        errorAt(DiagCode::E2002, "expected function name, method reference, or anonymous function");
        auto step = arena_.make<PipelineStepAST>();
        step->kind = PipelineStepKind::Ident;
        step->ident = pool_.intern("<error>");
        ts_.advance();
        return step;
    }

    std::string name = ts_.advance().value;
    ArenaSpan<TypePtr> genericArgs;
    if (ts_.check(TokenType::LESS)) {
        genericArgs = parseGenericArgs();
    }

    if (ts_.check(TokenType::COLON)) {
        return parseBehaviorPipelineStep(name, genericArgs);
    }
    
    if (ts_.check(TokenType::DOT)) {
        return parseFieldPipelineStep(name, genericArgs);
    }

    if (ts_.check(TokenType::LBRACKET)) {
        return parseIndexPipelineStep(name, genericArgs);
    }

    if (ts_.check(TokenType::LPAREN)) {
        return parseArgPackPipelineStep(name, genericArgs);
    }

    auto step = arena_.make<PipelineStepAST>();
    step->kind = PipelineStepKind::Ident;
    step->ident = pool_.intern(name);
    step->genericArgs = genericArgs;
    return step;
}

PipelineStepPtr Parser::parseAnonFuncPipelineStep() {
    auto step = arena_.make<PipelineStepAST>();
    step->kind = PipelineStepKind::AnonFunc;
    step->anonFunc = parseAnonFuncExpr();
    return step;
}

PipelineStepPtr Parser::parseBehaviorPipelineStep(const std::string& typeName, ArenaSpan<TypePtr> genericArgs) {
    auto step = arena_.make<PipelineStepAST>();
    step->kind = PipelineStepKind::BehaviorRef;
    step->typeName = pool_.intern(typeName);
    step->genericArgs = genericArgs;

    ts_.consume(TokenType::COLON, "expected ':'");
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected method name after ':'");
        step->method = pool_.intern("<error>");
        return step;
    }
    step->method = pool_.intern(ts_.advance().value);

    if (ts_.check(TokenType::LPAREN)) {
        ts_.advance();
        std::vector<ExprPtr> packArgs;
        if (!ts_.check(TokenType::RPAREN)) packArgs = parseExprList(TokenType::RPAREN);
        ts_.consume(TokenType::RPAREN, "expected ')'");
        if (!ts_.match(TokenType::BANG)) {
            errorAt(DiagCode::E2001, "expected '!' after arguments for method argument pack");
            return step;
        }
        step->kind = PipelineStepKind::BehaviorArgPack;
        auto builder = arena_.makeBuilder<ExprPtr>();
        for (auto& a : packArgs) builder.push_back(std::move(a));
        step->packArgs = builder.build();
    }

    return step;
}

PipelineStepPtr Parser::parseFieldPipelineStep(const std::string& ident, ArenaSpan<TypePtr> genericArgs) {
    auto step = arena_.make<PipelineStepAST>();
    step->kind = PipelineStepKind::FieldRef;
    step->ident = pool_.intern(ident);
    step->genericArgs = genericArgs;

    ts_.consume(TokenType::DOT, "expected '.'");
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected field name after '.'");
        step->field = pool_.intern("<error>");
        return step;
    }
    step->field = pool_.intern(ts_.advance().value);

    if (ts_.check(TokenType::LPAREN)) {
        ts_.advance();
        std::vector<ExprPtr> packArgs;
        if (!ts_.check(TokenType::RPAREN)) packArgs = parseExprList(TokenType::RPAREN);
        ts_.consume(TokenType::RPAREN, "expected ')'");
        if (!ts_.match(TokenType::BANG)) {
            errorAt(DiagCode::E2001, "expected '!' after arguments for field argument pack");
            return step;
        }
        step->kind = PipelineStepKind::FieldArgPack;
        auto builder = arena_.makeBuilder<ExprPtr>();
        for (auto& a : packArgs) builder.push_back(std::move(a));
        step->packArgs = builder.build();
    }

    return step;
}

PipelineStepPtr Parser::parseIndexPipelineStep(const std::string& ident, ArenaSpan<TypePtr> genericArgs) {
    auto step = arena_.make<PipelineStepAST>();
    step->kind = PipelineStepKind::IndexRef;
    step->ident = pool_.intern(ident);
    step->genericArgs = genericArgs;

    auto addIndex = [&](ExprPtr target, ExprPtr idx) -> ExprPtr {
        auto node = arena_.make<IndexExprAST>();
        node->target = std::move(target);
        node->index = std::move(idx);
        node->kind = IndexKind::Element;
        return node;
    };

    ts_.consume(TokenType::LBRACKET, "expected '['");
    ExprPtr idx = parseExpr();
    if (!idx) {
        errorAt(DiagCode::E2008, "expected index expression");
        int bracketDepth = 1;
        while (!ts_.isAtEnd() && bracketDepth > 0) {
            if (ts_.check(TokenType::LBRACKET)) ++bracketDepth;
            else if (ts_.check(TokenType::RBRACKET)) --bracketDepth;
            ts_.advance();
        }
        step->index = arena_.make<UnknownExprAST>();
        return step;
    }
    ts_.consume(TokenType::RBRACKET, "expected ']' after index");
    
    auto baseIdent = arena_.make<IdentifierExprAST>(pool_.intern(ident));
    baseIdent->loc = ts_.currentLoc();
    ExprPtr indexChain = addIndex(std::move(baseIdent), std::move(idx));

    while (ts_.check(TokenType::LBRACKET)) {
        ts_.advance();
        ExprPtr nextIdx = parseExpr();
        if (!nextIdx) {
            errorAt(DiagCode::E2008, "expected index expression");
            int bracketDepth = 1;
            while (!ts_.isAtEnd() && bracketDepth > 0) {
                if (ts_.check(TokenType::LBRACKET)) ++bracketDepth;
                else if (ts_.check(TokenType::RBRACKET)) --bracketDepth;
                ts_.advance();
            }
            break;
        }
        ts_.consume(TokenType::RBRACKET, "expected ']' after index");
        indexChain = addIndex(std::move(indexChain), std::move(nextIdx));
    }

    step->index = std::move(indexChain);

    if (ts_.check(TokenType::LPAREN)) {
        ts_.advance();
        std::vector<ExprPtr> packArgs;
        if (!ts_.check(TokenType::RPAREN)) packArgs = parseExprList(TokenType::RPAREN);
        ts_.consume(TokenType::RPAREN, "expected ')'");
        if (!ts_.match(TokenType::BANG)) {
            errorAt(DiagCode::E2001, "expected '!' after arguments for array index argument pack");
            return step;
        }
        step->kind = PipelineStepKind::IndexArgPack;
        auto builder = arena_.makeBuilder<ExprPtr>();
        for (auto& a : packArgs) builder.push_back(std::move(a));
        step->packArgs = builder.build();
    }

    return step;
}

PipelineStepPtr Parser::parseArgPackPipelineStep(const std::string& ident, ArenaSpan<TypePtr> genericArgs) {
    auto step = arena_.make<PipelineStepAST>();
    step->kind = PipelineStepKind::ArgPack;
    step->ident = pool_.intern(ident);
    step->genericArgs = genericArgs;

    ts_.consume(TokenType::LPAREN, "expected '('");
    std::vector<ExprPtr> packArgs;
    if (!ts_.check(TokenType::RPAREN)) packArgs = parseExprList(TokenType::RPAREN);
    ts_.consume(TokenType::RPAREN, "expected ')'");
    
    if (!ts_.match(TokenType::BANG)) {
        errorAt(DiagCode::E2001, "expected '!' after arguments for function argument pack");
        step->kind = PipelineStepKind::Ident;
        step->ident = pool_.intern("<error>");
        return step;
    }
    
    auto builder = arena_.makeBuilder<ExprPtr>();
    for (auto& a : packArgs) builder.push_back(std::move(a));
    step->packArgs = builder.build();
    return step;
}

ComposeOperandPtr Parser::parseComposeOperand() {
    auto op = arena_.make<ComposeOperandAST>();

    bool isPrimitiveType = isPrimitiveTypeToken(ts_.peekType());
    if (!ts_.check(TokenType::IDENTIFIER) && !isPrimitiveType) {
        errorAt(DiagCode::E2002, "expected function name or method reference");
        return nullptr;
    }

    std::string name = ts_.advance().value;

    if (ts_.check(TokenType::COLON) && ts_.peekNextType() == TokenType::IDENTIFIER) {
        ts_.advance();
        std::string method = ts_.advance().value;
        op->kind = ComposeOperandKind::BehaviorRef;
        op->typeName = pool_.intern(name);
        op->method = pool_.intern(method);
    } else if (ts_.check(TokenType::DOT) && ts_.peekNextType() == TokenType::IDENTIFIER) {
        ts_.advance();
        std::string field = ts_.advance().value;
        op->kind = ComposeOperandKind::FieldRef;
        op->ident = pool_.intern(name);
        op->field = pool_.intern(field);
    } else {
        op->kind = ComposeOperandKind::Ident;
        op->ident = pool_.intern(name);
    }

    return op;
}

// -----------------------------------------------------------------------------
// Match expression (stubs - patterns in ParserPattern.cpp)
// -----------------------------------------------------------------------------

ExprPtr Parser::parseMatchExpr() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::MATCH, "expected 'match'");

    ExprPtr subject = parseExpr(false);
    if (!subject) {
        errorAt(DiagCode::E2008, "expected expression after 'match'");
        return arena_.make<UnknownExprAST>();
    }

    ts_.consume(TokenType::LBRACE, "expected '{' after match subject");

    auto node = arena_.make<MatchExprAST>();
    node->loc = loc;
    node->subject = std::move(subject);

    std::vector<MatchArmPtr> arms;  // temporary
    bool hasDefault = false;

    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        if (ts_.check(TokenType::DEFAULT)) {
            if (hasDefault) {
                errorAt(DiagCode::E2007, "duplicate 'default' arm in match expression");
            }
            node->defaultBody = parseDefaultArm();
            if (node->defaultBody) {
                node->defaultLoc = node->defaultBody->loc;
            }
            hasDefault = true;
            break;
        }

        size_t beforePos = ts_.getPos();
        MatchArmPtr arm = parseMatchArm();
        if (ts_.getPos() == beforePos) {
            errorAt(DiagCode::E2007, "failed to parse match arm, skipping");
            ts_.advance();
            continue;
        }
        if (!arm) {
            errorAt(DiagCode::E2007, "invalid match arm, skipping");
            continue;
        }
        arms.push_back(std::move(arm));
    }

    ts_.consume(TokenType::RBRACE, "expected '}' to close match expression");

    if (!hasDefault) {
        error(loc, DiagCode::E2006, "match expression must have a 'default' arm");
    }

    auto armsBuilder = arena_.makeBuilder<MatchArmPtr>();
    for (auto& a : arms) armsBuilder.push_back(std::move(a));
    node->arms = armsBuilder.build();

    return node;
}

// -----------------------------------------------------------------------------
// Lvalue parsing (for assignments)
// -----------------------------------------------------------------------------

ExprPtr Parser::parseLvalue() {
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected identifier for lvalue");
        return nullptr;
    }
    std::string name = ts_.advance().value;
    ExprPtr expr = arena_.make<IdentifierExprAST>(pool_.intern(name));
    expr->loc = ts_.currentLoc();

    while (true) {
        if (ts_.check(TokenType::DOT)) {
            ts_.advance();
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected field name after '.'");
                return expr;
            }
            std::string field = ts_.advance().value;
            auto node = arena_.make<FieldAccessExprAST>();
            node->loc = expr->loc;
            node->object = std::move(expr);
            node->field = pool_.intern(field);
            expr = std::move(node);
        } 
        else if (ts_.check(TokenType::LBRACKET)) {
            ts_.advance();
            ExprPtr index = parseExpr();
            if (!index) {
                errorAt(DiagCode::E2008, "expected index expression");
                return expr;
            }
            ts_.consume(TokenType::RBRACKET, "expected ']' after index");
            auto node = arena_.make<IndexExprAST>();
            node->loc = expr->loc;
            node->target = std::move(expr);
            node->index = std::move(index);
            node->kind = IndexKind::Element;
            expr = std::move(node);
        }
        else if (ts_.check(TokenType::COLON)) {
            break;
        }
        else {
            break;
        }
    }
    return expr;
}