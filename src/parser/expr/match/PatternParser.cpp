#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Pattern Dispatcher
// ============================================================================

ASTPtr<PatternAST> Parser::parsePattern() {
    // Wildcard
    if (ts_.check(TokenType::WILDCARD)) {
        return parseWildcardPattern();
    }

    // Literal patterns (and ranges)
    switch (ts_.peekType()) {
        case TokenType::INT_LITERAL:
        case TokenType::FLOAT_LITERAL:
        case TokenType::STRING_LITERAL:
        case TokenType::RAW_STRING_LITERAL:
        case TokenType::CHAR_LITERAL:
        case TokenType::HEX_LITERAL:
        case TokenType::BINARY_LITERAL:
        case TokenType::TRUE:
        case TokenType::FALSE:
        case TokenType::NIL:
        case TokenType::MINUS:
            return parseLiteralOrRangePattern();
        default:
            break;
    }

    // IDENTIFIER-based patterns
    if (ts_.check(TokenType::IDENTIFIER)) {
        std::string name = ts_.peek().value;

        // Type pattern: IDENTIFIER 'is' type
        if (ts_.peekNextType() == TokenType::IS) {
            ts_.advance(); // consume IDENTIFIER
            return parseTypePattern(pool_.intern(name));
        }

        // Struct pattern: IDENTIFIER '{'
        if (ts_.peekNextType() == TokenType::LBRACE) {
            ts_.advance(); // consume IDENTIFIER
            return parseStructPattern(pool_.intern(name));
        }

        // Qualified constant pattern: IDENTIFIER '.' ...
        if (ts_.peekNextType() == TokenType::DOT) {
            ExprPtr expr = parseExpr();
            if (!expr) {
                errorAt(DiagCode::E1002, "expected expression after '.' in pattern");
                return nullptr;
            }
            return arena_.make<PatternExprAST>(std::move(expr));
        }

        // Simple bind pattern
        ts_.advance(); // consume IDENTIFIER
        if (ts_.check(TokenType::RANGE)) {
            errorAt(DiagCode::E1002, "bind patterns cannot be used as range bounds");
            ts_.advance(); // consume '..'
            parseLiteralOrRangePattern(); // recover
        }
        return parseBindPattern(pool_.intern(name));
    }

    errorAt(DiagCode::E1002, "expected pattern");
    return nullptr;
}

// ============================================================================
// Literal or Range Pattern
// ============================================================================

ASTPtr<PatternAST> Parser::parseLiteralOrRangePattern() {
    SourceLocation loc = ts_.currentLoc();

    // Handle unary minus for negative literals
    bool negative = false;
    if (ts_.check(TokenType::MINUS)) {
        negative = true;
        ts_.advance();
    }

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
            errorAt(DiagCode::E1007, "expected literal value in pattern");
            return nullptr;
    }

    std::string rawValue = negative ? ("-" + tok.value) : tok.value;
    InternedString internedValue = pool_.intern(rawValue);

    // Check for range: lo '..' [ '<' ] hi
    if (ts_.check(TokenType::RANGE)) {
        ts_.advance(); // consume '..'
        bool isExclusive = ts_.match(TokenType::LESS);

        bool negHi = false;
        if (ts_.check(TokenType::MINUS)) {
            negHi = true;
            ts_.advance();
        }

        if (!ts_.checkAny({TokenType::INT_LITERAL, TokenType::HEX_LITERAL, TokenType::FLOAT_LITERAL})) {
            errorAt(DiagCode::E1007, "expected literal after '..' in range pattern");
            return nullptr;
        }
        Token hiTok = ts_.advance();
        std::string hiRaw = negHi ? ("-" + hiTok.value) : hiTok.value;
        InternedString hiInterned = pool_.intern(hiRaw);

        LiteralKind hiKind;
        switch (hiTok.type) {
            case TokenType::INT_LITERAL: hiKind = LiteralKind::Int; break;
            case TokenType::HEX_LITERAL: hiKind = LiteralKind::Hex; break;
            default: hiKind = LiteralKind::Float; break;
        }

        auto loExpr = arena_.make<LiteralExprAST>(kind, internedValue);
        loExpr->loc = loc;
        auto hiExpr = arena_.make<LiteralExprAST>(hiKind, hiInterned);
        hiExpr->loc = ts_.locOf(hiTok);

        auto range = arena_.make<RangeExprAST>();
        range->loc = loc;
        range->lo = std::move(loExpr);
        range->hi = std::move(hiExpr);
        range->isExclusive = isExclusive;

        return arena_.make<PatternExprAST>(std::move(range));
    }

    // Simple literal pattern
    auto lit = arena_.make<LiteralExprAST>(kind, internedValue);
    lit->loc = loc;
    return arena_.make<PatternExprAST>(std::move(lit));
}

// ============================================================================
// Bind Pattern
// ============================================================================

ASTPtr<BindPatternAST> Parser::parseBindPattern(InternedString name) {
    SourceLocation loc = ts_.currentLoc();
    auto pat = arena_.make<BindPatternAST>(name);
    pat->loc = loc;
    return pat;
}

// ============================================================================
// Type Pattern
// ============================================================================

ASTPtr<TypePatternAST> Parser::parseTypePattern(InternedString bindName) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::IS, "expected 'is' in type pattern");

    TypePtr checkType = parseType();
    if (!checkType) {
        errorAt(DiagCode::E1005, "expected type after 'is' in type pattern");
        return nullptr;
    }

    auto pat = arena_.make<TypePatternAST>();
    pat->loc = loc;
    pat->bindName = bindName;
    pat->checkType = std::move(checkType);
    return pat;
}

// ============================================================================
// Wildcard Pattern
// ============================================================================

ASTPtr<WildcardPatternAST> Parser::parseWildcardPattern() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::WILDCARD, "expected '_'");
    auto pat = arena_.make<WildcardPatternAST>();
    pat->loc = loc;
    return pat;
}

// ============================================================================
// Struct Pattern
// ============================================================================

ASTPtr<StructPatternAST> Parser::parseStructPattern(InternedString typeName) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LBRACE, "expected '{' in struct pattern");

    auto pat = arena_.make<StructPatternAST>();
    pat->loc = loc;
    pat->typeName = typeName;

    std::vector<FieldPatternPtr> fields;

    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t savedPos = ts_.getPos();
        FieldPatternPtr fp = parseFieldPattern();
        if (fp) {
            fields.push_back(std::move(fp));
        } else {
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) {
                errorAt(DiagCode::E1003, "expected field name in struct pattern");
                ts_.advance();
            }
        }
    }

    auto builder = arena_.makeBuilder<FieldPatternPtr>();
    for (auto& f : fields) builder.push_back(std::move(f));
    pat->fields = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close struct pattern");
    return pat;
}

// ============================================================================
// Field Pattern
// ============================================================================

FieldPatternPtr Parser::parseFieldPattern() {
    SourceLocation loc = ts_.currentLoc();

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E1003, "expected field name in struct pattern");
        return nullptr;
    }
    InternedString fieldName = pool_.intern(ts_.advance().value);

    auto fp = arena_.make<FieldPatternAST>();
    fp->loc = loc;
    fp->field = fieldName;

    // Full form: 'fieldName : sub_pattern'
    if (ts_.check(TokenType::COLON)) {
        ts_.advance(); // consume ':'
        fp->subPattern = parsePattern();
        if (!fp->subPattern) {
            errorAt(DiagCode::E1002, "expected sub-pattern after ':' in field pattern");
        }
    }
    // else: shorthand — subPattern is nullptr, bind by field name

    return fp;
}