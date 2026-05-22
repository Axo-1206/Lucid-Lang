/**
 * @file ParserPattern.cpp
 * 
 * Pattern parsing for match expressions.
 * Handles literal patterns, bind patterns, wildcard patterns,
 * type patterns, struct patterns, and match arms.
 */

#include "Parser.hpp"
#include "ast/ExprAST.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// -----------------------------------------------------------------------------
// parseMatchArm
// -----------------------------------------------------------------------------

MatchArmPtr Parser::parseMatchArm() {
    SourceLocation loc = ts_.currentLoc();
    auto arm = arena_.make<MatchArmAST>();
    arm->loc = loc;

    // Parse patterns (at least one)
    std::vector<ASTPtr<PatternAST>> patterns;
    ASTPtr<PatternAST> pat = parsePattern();
    if (!pat) return nullptr;
    patterns.push_back(std::move(pat));

    while (ts_.check(TokenType::COMMA)) {
        // Peek ahead - is the next token a valid pattern start?
        TokenType nextType = ts_.peekNextType();
        bool isPatternStart = (nextType == TokenType::WILDCARD ||
                               nextType == TokenType::INT_LITERAL ||
                               nextType == TokenType::FLOAT_LITERAL ||
                               nextType == TokenType::STRING_LITERAL ||
                               nextType == TokenType::RAW_STRING_LITERAL ||
                               nextType == TokenType::CHAR_LITERAL ||
                               nextType == TokenType::HEX_LITERAL ||
                               nextType == TokenType::BINARY_LITERAL ||
                               nextType == TokenType::TRUE ||
                               nextType == TokenType::FALSE ||
                               nextType == TokenType::NIL ||
                               nextType == TokenType::MINUS ||
                               nextType == TokenType::IDENTIFIER);
        if (!isPatternStart) {
            errorAt(DiagCode::E2007, "expected pattern after ',' in match arm");
            break;
        }
        ts_.advance(); // consume comma
        pat = parsePattern();
        if (!pat) break;
        patterns.push_back(std::move(pat));
    }

    // Build patterns span
    auto patternsBuilder = arena_.makeBuilder<ASTPtr<PatternAST>>();
    for (auto& p : patterns) patternsBuilder.push_back(std::move(p));
    arm->patterns = patternsBuilder.build();

    // Optional guard: 'if' expr
    if (ts_.check(TokenType::IF)) {
        ts_.advance();
        size_t savedPos = ts_.getPos();
        ExprPtr guard = parseExpr();
        if (ts_.getPos() == savedPos) {
            errorAt(DiagCode::E2008, "expected guard expression after 'if' in match arm");
        } else {
            arm->guard = std::move(guard);
        }
    }

    ts_.consume(TokenType::FAT_ARROW, "expected '=>' after match pattern");

    // Parse result expressions: at least one, at most two
    std::vector<ExprPtr> exprs;
    size_t beforePos = ts_.getPos();
    ExprPtr first = parseExpr();
    if (ts_.getPos() == beforePos || !first) {
        errorAt(DiagCode::E2008, "expected result expression after '=>' in match arm");
    } else {
        exprs.push_back(std::move(first));
    }

    // Optional second expression after comma
    if (ts_.match(TokenType::COMMA)) {
        if (ts_.check(TokenType::COMMA) || ts_.check(TokenType::RBRACE) || 
            ts_.check(TokenType::FAT_ARROW) || ts_.isAtEnd()) {
            errorAt(DiagCode::E2001, "expected expression after ',' in match arm");
        } else {
            size_t beforePos2 = ts_.getPos();
            ExprPtr second = parseExpr();
            if (ts_.getPos() == beforePos2 || !second) {
                errorAt(DiagCode::E2008, "expected second result expression after ',' in match arm");
            } else {
                exprs.push_back(std::move(second));
            }
        }
        // No more commas allowed
        if (ts_.match(TokenType::COMMA)) {
            errorAt(DiagCode::E2001, "match arm cannot have more than two expressions");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && !ts_.check(TokenType::DEFAULT)) {
                TokenType t = ts_.peekType();
                if (t == TokenType::WILDCARD || t == TokenType::IDENTIFIER ||
                    t == TokenType::INT_LITERAL || t == TokenType::FLOAT_LITERAL ||
                    t == TokenType::STRING_LITERAL || t == TokenType::CHAR_LITERAL ||
                    t == TokenType::HEX_LITERAL || t == TokenType::BINARY_LITERAL ||
                    t == TokenType::TRUE || t == TokenType::FALSE || t == TokenType::NIL ||
                    t == TokenType::MINUS) {
                    break;
                }
                ts_.advance();
            }
        }
    }

    auto exprsBuilder = arena_.makeBuilder<ExprPtr>();
    for (auto& e : exprs) exprsBuilder.push_back(std::move(e));
    arm->exprs = exprsBuilder.build();

    return arm;
}

// -----------------------------------------------------------------------------
// parseDefaultArm
// -----------------------------------------------------------------------------

ASTPtr<DefaultArmAST> Parser::parseDefaultArm() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::DEFAULT, "expected 'default'");
    ts_.consume(TokenType::FAT_ARROW, "expected '=>' after 'default'");

    auto arm = arena_.make<DefaultArmAST>();
    arm->loc = loc;

    std::vector<ExprPtr> exprs;

    // First expression (required)
    size_t savedPos = ts_.getPos();
    ExprPtr first = parseExpr();
    if (ts_.getPos() == savedPos || !first) {
        errorAt(DiagCode::E2008, "expected expression after '=>' in default arm");
    } else {
        exprs.push_back(std::move(first));
    }

    // Optional second expression after comma
    if (ts_.match(TokenType::COMMA)) {
        if (ts_.check(TokenType::COMMA) || ts_.check(TokenType::RBRACE) || 
            ts_.check(TokenType::FAT_ARROW) || ts_.isAtEnd()) {
            errorAt(DiagCode::E2001, "expected expression after ',' in default arm");
        } else {
            size_t savedPos2 = ts_.getPos();
            ExprPtr second = parseExpr();
            if (ts_.getPos() == savedPos2 || !second) {
                errorAt(DiagCode::E2008, "expected second expression after ',' in default arm");
            } else {
                exprs.push_back(std::move(second));
            }
        }
        // No more commas allowed
        if (ts_.match(TokenType::COMMA)) {
            errorAt(DiagCode::E2001, "default arm cannot have more than two expressions");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && !ts_.check(TokenType::DEFAULT)) {
                ts_.advance();
            }
        }
    }

    auto builder = arena_.makeBuilder<ExprPtr>();
    for (auto& e : exprs) builder.push_back(std::move(e));
    arm->exprs = builder.build();

    return arm;
}

// -----------------------------------------------------------------------------
// parsePattern
// -----------------------------------------------------------------------------

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
                errorAt(DiagCode::E2007, "expected expression after '.' in pattern");
                return nullptr;
            }
            return arena_.make<PatternExprAST>(std::move(expr));
        }

        // Simple bind pattern
        ts_.advance(); // consume IDENTIFIER
        if (ts_.check(TokenType::RANGE)) {
            errorAt(DiagCode::E2007, "bind patterns cannot be used as range bounds");
            ts_.advance(); // consume '..'
            parseLiteralOrRangePattern(); // recover
        }
        return parseBindPattern(pool_.intern(name));
    }

    errorAt(DiagCode::E2007, "expected pattern");
    return nullptr;
}

// -----------------------------------------------------------------------------
// parseLiteralOrRangePattern
// -----------------------------------------------------------------------------

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
            errorAt(DiagCode::E2009, "expected literal value in pattern");
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
            errorAt(DiagCode::E2009, "expected literal after '..' in range pattern");
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

// -----------------------------------------------------------------------------
// parseBindPattern
// -----------------------------------------------------------------------------

ASTPtr<BindPatternAST> Parser::parseBindPattern(InternedString name) {
    SourceLocation loc = ts_.currentLoc();
    auto pat = arena_.make<BindPatternAST>(name);
    pat->loc = loc;
    return pat;
}

// -----------------------------------------------------------------------------
// parseTypePattern
// -----------------------------------------------------------------------------

ASTPtr<TypePatternAST> Parser::parseTypePattern(InternedString bindName) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::IS, "expected 'is' in type pattern");

    TypePtr checkType = parseType();
    if (!checkType) {
        errorAt(DiagCode::E2005, "expected type after 'is' in type pattern");
        return nullptr;
    }

    auto pat = arena_.make<TypePatternAST>();
    pat->loc = loc;
    pat->bindName = bindName;
    pat->checkType = std::move(checkType);
    return pat;
}

// -----------------------------------------------------------------------------
// parseWildcardPattern
// -----------------------------------------------------------------------------

ASTPtr<WildcardPatternAST> Parser::parseWildcardPattern() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::WILDCARD, "expected '_'");
    auto pat = arena_.make<WildcardPatternAST>();
    pat->loc = loc;
    return pat;
}

// -----------------------------------------------------------------------------
// parseStructPattern
// -----------------------------------------------------------------------------

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
                errorAt(DiagCode::E2003, "expected field name in struct pattern");
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

// -----------------------------------------------------------------------------
// parseFieldPattern
// -----------------------------------------------------------------------------

FieldPatternPtr Parser::parseFieldPattern() {
    SourceLocation loc = ts_.currentLoc();

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected field name in struct pattern");
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
            errorAt(DiagCode::E2007, "expected sub-pattern after ':' in field pattern");
        }
    }
    // else: shorthand — subPattern is nullptr, bind by field name

    return fp;
}