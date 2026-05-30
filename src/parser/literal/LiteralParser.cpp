#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

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
        for (auto& p : group) {
            allParams.push_back(std::move(p));
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