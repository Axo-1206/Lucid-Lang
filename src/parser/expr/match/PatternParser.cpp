#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

// ============================================================================
// Pattern Dispatcher
// ============================================================================

/**
 * @brief Parses a single pattern for use in match arms.
 * 
 * Grammar:
 *   pattern := wildcard_pattern
 *            | literal_pattern
 *            | range_pattern
 *            | bind_pattern
 *            | type_pattern
 *            | struct_pattern
 *            | qualified_constant_pattern
 * 
 * @return PatternPtr – the parsed pattern node, or nullptr on error.
 * 
 * ─── Dispatch Order ────────────────────────────────────────────────────────
 *   1. Wildcard `_`
 *   2. Literals and ranges (numeric, string, boolean, nil)
 *   3. IDENTIFIER-based patterns:
 *      - Type pattern: `ident 'is' type`
 *      - Struct pattern: `ident '{' ... '}'`
 *      - Qualified constant: `ident '.' ...`
 *      - Bind pattern: `ident` (fallback)
 */
PatternPtr Parser::parsePattern() {
    LUC_LOG_EXPR_EXTREME("parsePattern: entering, token=" << ts_.peek().value);
    
    // Wildcard
    if (ts_.check(TokenType::WILDCARD)) {
        LUC_LOG_EXPR_EXTREME("parsePattern: wildcard pattern");
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
            LUC_LOG_EXPR_EXTREME("parsePattern: literal/range pattern");
            return parseLiteralOrRangePattern();
        default:
            break;
    }

    // IDENTIFIER-based patterns
    if (ts_.check(TokenType::IDENTIFIER)) {
        std::string name = ts_.peek().value;

        // Type pattern: IDENTIFIER 'is' type
        if (ts_.peekNextType() == TokenType::IS) {
            LUC_LOG_EXPR_EXTREME("parsePattern: type pattern for '" << name << "'");
            ts_.advance(); // consume IDENTIFIER
            return parseTypePattern(pool_.intern(name));
        }

        // Struct pattern: IDENTIFIER '{'
        if (ts_.peekNextType() == TokenType::LBRACE) {
            LUC_LOG_EXPR_EXTREME("parsePattern: struct pattern for '" << name << "'");
            ts_.advance(); // consume IDENTIFIER
            return parseStructPattern(pool_.intern(name));
        }

        // Qualified constant pattern: IDENTIFIER '.' ...
        if (ts_.peekNextType() == TokenType::DOT) {
            LUC_LOG_EXPR_EXTREME("parsePattern: qualified constant pattern");
            ExprPtr expr = parseExpr();
            if (!expr) {
                LUC_LOG_EXPR("parsePattern: ERROR - expected expression after '.'");
                errorAt(DiagCode::E1002, "expected expression after '.' in pattern");
                return nullptr;
            }
            return arena_.make<PatternExprAST>(expr);
        }

        // Simple bind pattern
        LUC_LOG_EXPR_EXTREME("parsePattern: bind pattern for '" << name << "'");
        ts_.advance(); // consume IDENTIFIER
        if (ts_.check(TokenType::RANGE)) {
            LUC_LOG_EXPR("parsePattern: ERROR - bind pattern cannot be range bound");
            errorAt(DiagCode::E1002, "bind patterns cannot be used as range bounds");
            ts_.advance(); // consume '..'
            parseLiteralOrRangePattern(); // recover
        }
        return parseBindPattern(pool_.intern(name));
    }

    LUC_LOG_EXPR("parsePattern: ERROR - expected pattern, got '" << ts_.peek().value << "'");
    errorAt(DiagCode::E1002, "expected pattern");
    return nullptr;
}

// ============================================================================
// Literal or Range Pattern
// ============================================================================

/**
 * @brief Parses a literal or range pattern.
 * 
 * Grammar:
 *   literal_pattern := literal
 *   range_pattern   := literal '..' [ '<' ] literal
 * 
 * @return PatternPtr – PatternExprAST wrapping a LiteralExprAST or RangeExprAST.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at literal token (or '-')
 * On exit:  positioned after the literal/range
 * 
 * ─── Negative Numbers ──────────────────────────────────────────────────────
 * Supports unary minus for negative literals (e.g., `-5`).
 */
PatternPtr Parser::parseLiteralOrRangePattern() {
    LUC_LOG_EXPR_EXTREME("parseLiteralOrRangePattern: entering");
    SourceLocation loc = ts_.currentLoc();

    // Handle unary minus for negative literals
    bool negative = false;
    if (ts_.check(TokenType::MINUS)) {
        negative = true;
        ts_.advance();
        LUC_LOG_EXPR_EXTREME("parseLiteralOrRangePattern: negative literal");
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
            LUC_LOG_EXPR("parseLiteralOrRangePattern: ERROR - expected literal");
            errorAt(DiagCode::E1007, "expected literal value in pattern");
            return nullptr;
    }

    std::string rawValue = negative ? ("-" + tok.value) : tok.value;
    InternedString internedValue = pool_.intern(rawValue);
    LUC_LOG_EXPR_EXTREME("parseLiteralOrRangePattern: value = " << rawValue);

    // Check for range: lo '..' [ '<' ] hi
    if (ts_.check(TokenType::RANGE)) {
        LUC_LOG_EXPR_EXTREME("parseLiteralOrRangePattern: range pattern");
        ts_.advance(); // consume '..'
        bool isExclusive = ts_.match(TokenType::LESS);

        bool negHi = false;
        if (ts_.check(TokenType::MINUS)) {
            negHi = true;
            ts_.advance();
        }

        if (!ts_.checkAny({TokenType::INT_LITERAL, TokenType::HEX_LITERAL, TokenType::FLOAT_LITERAL})) {
            LUC_LOG_EXPR("parseLiteralOrRangePattern: ERROR - expected literal after '..'");
            errorAt(DiagCode::E1007, "expected literal after '..' in range pattern");
            return nullptr;
        }
        Token hiTok = ts_.advance();
        std::string hiRaw = negHi ? ("-" + hiTok.value) : hiTok.value;
        InternedString hiInterned = pool_.intern(hiRaw);
        LUC_LOG_EXPR_EXTREME("parseLiteralOrRangePattern: range hi = " << hiRaw);

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
        range->lo = loExpr;
        range->hi = hiExpr;
        range->isExclusive = isExclusive;

        return arena_.make<PatternExprAST>(range);
    }

    // Simple literal pattern
    LUC_LOG_EXPR_EXTREME("parseLiteralOrRangePattern: simple literal pattern");
    auto lit = arena_.make<LiteralExprAST>(kind, internedValue);
    lit->loc = loc;
    return arena_.make<PatternExprAST>(lit);
}

// ============================================================================
// Bind Pattern
// ============================================================================

/**
 * @brief Parses a bind pattern (matches any value and binds it to a name).
 * 
 * Grammar:
 *   bind_pattern := IDENTIFIER
 * 
 * Example: `n` (matches anything, binds to variable `n`)
 * 
 * @param name The already-consumed identifier name.
 * @return BindPatternPtr – BindPatternAST node.
 */
BindPatternPtr Parser::parseBindPattern(InternedString name) {
    LUC_LOG_EXPR_EXTREME("parseBindPattern: name = " << pool_.lookup(name));
    SourceLocation loc = ts_.currentLoc();
    auto pat = arena_.make<BindPatternAST>(name);
    pat->loc = loc;
    return pat;
}

// ============================================================================
// Type Pattern
// ============================================================================

/**
 * @brief Parses a type pattern (matches when subject is of a specific type).
 * 
 * Grammar:
 *   type_pattern := IDENTIFIER 'is' type
 * 
 * Example: `s is Circle` (matches if subject is Circle, binds as 's')
 * 
 * @param bindName The already-consumed identifier name to bind.
 * @return TypePatternPtr – TypePatternAST node, or nullptr on error.
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing 'is' keyword: consume() reports error
 * - Missing type after 'is': reports error, returns nullptr
 */
TypePatternPtr Parser::parseTypePattern(InternedString bindName) {
    LUC_LOG_EXPR_EXTREME("parseTypePattern: bindName = " << pool_.lookup(bindName));
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::IS, "expected 'is' in type pattern");

    TypePtr checkType = parseType();
    if (!checkType) {
        LUC_LOG_EXPR("parseTypePattern: ERROR - expected type after 'is'");
        errorAt(DiagCode::E1005, "expected type after 'is' in type pattern");
        return nullptr;
    }

    auto pat = arena_.make<TypePatternAST>();
    pat->loc = loc;
    pat->bindName = bindName;
    pat->checkType = checkType;
    return pat;
}

// ============================================================================
// Wildcard Pattern
// ============================================================================

/**
 * @brief Parses a wildcard pattern (matches any value, discards it).
 * 
 * Grammar:
 *   wildcard_pattern := '_'
 * 
 * @return WildcardPatternPtr – WildcardPatternAST node.
 */
WildcardPatternPtr Parser::parseWildcardPattern() {
    LUC_LOG_EXPR_EXTREME("parseWildcardPattern");
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::WILDCARD, "expected '_'");
    auto pat = arena_.make<WildcardPatternAST>();
    pat->loc = loc;
    return pat;
}

// ============================================================================
// Struct Pattern
// ============================================================================

/**
 * @brief Parses a struct pattern (matches struct fields against sub-patterns).
 * 
 * Grammar:
 *   struct_pattern := IDENTIFIER '{' [ field_pattern { ',' field_pattern } ] '}'
 *   field_pattern  := IDENTIFIER [ ':' pattern ]
 * 
 * Examples:
 *   Vec2 { x: 0.0, y: 0.0 }   – exact match on both fields
 *   Vec2 { x, y }              – shorthand: binds x and y from subject
 *   Player { health: 0 }       – only health must be 0, other fields ignored
 * 
 * @param typeName The already-consumed struct type name.
 * @return StructPatternPtr – StructPatternAST node, or nullptr on error.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned after the type name
 * On exit:  positioned after the closing '}'
 * 
 * ─── Field Patterns ───────────────────────────────────────────────────────
 * - Shorthand `field` binds the field value to a variable named `field`
 * - Full form `field: pattern` matches the field against a sub-pattern
 * - Fields not listed are ignored (match succeeds regardless)
 */
StructPatternPtr Parser::parseStructPattern(InternedString typeName) {
    LUC_LOG_EXPR_EXTREME("parseStructPattern: typeName = " << pool_.lookup(typeName));
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LBRACE, "expected '{' in struct pattern");

    auto pat = arena_.make<StructPatternAST>();
    pat->loc = loc;
    pat->typeName = typeName;

    std::vector<FieldPatternPtr> fields;
    int fieldCount = 0;

    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t savedPos = ts_.getPos();
        FieldPatternPtr fp = parseFieldPattern();
        if (fp) {
            fieldCount++;
            LUC_LOG_EXPR_EXTREME("parseStructPattern: field #" << fieldCount);
            fields.push_back(fp);
        } else {
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) {
                LUC_LOG_EXPR("parseStructPattern: ERROR - expected field name");
                errorAt(DiagCode::E1003, "expected field name in struct pattern");
                ts_.advance();
            }
        }
    }

    auto builder = arena_.makeBuilder<FieldPatternPtr>();
    for (auto& f : fields) builder.push_back(f);
    pat->fields = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close struct pattern");
    
    LUC_LOG_EXPR_EXTREME("parseStructPattern: " << fieldCount << " field(s)");
    return pat;
}

// ============================================================================
// Field Pattern
// ============================================================================

/**
 * @brief Parses a single field pattern inside a struct pattern.
 * 
 * Grammar:
 *   field_pattern := IDENTIFIER [ ':' pattern ]
 * 
 * Examples:
 *   x           – shorthand: binds field 'x' to name 'x'
 *   x: 0.0      – full form: matches field 'x' against sub‑pattern 0.0
 *   x: Vec2 {…} – nested pattern
 * 
 * @return FieldPatternPtr – FieldPatternAST node, or nullptr on error.
 * 
 * ─── Sub‑pattern ──────────────────────────────────────────────────────────
 * - If sub‑pattern is null (shorthand), the field binds by name
 * - If sub‑pattern is present (full form), the field is matched recursively
 */
FieldPatternPtr Parser::parseFieldPattern() {
    LUC_LOG_EXPR_EXTREME("parseFieldPattern: entering");
    SourceLocation loc = ts_.currentLoc();

    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_EXPR("parseFieldPattern: ERROR - expected field name");
        errorAt(DiagCode::E1003, "expected field name in struct pattern");
        return nullptr;
    }
    InternedString fieldName = pool_.intern(ts_.advance().value);
    LUC_LOG_EXPR_EXTREME("parseFieldPattern: fieldName = " << pool_.lookup(fieldName));

    auto fp = arena_.make<FieldPatternAST>();
    fp->loc = loc;
    fp->field = fieldName;

    // Full form: 'fieldName : sub_pattern'
    if (ts_.check(TokenType::COLON)) {
        ts_.advance(); // consume ':'
        LUC_LOG_EXPR_EXTREME("parseFieldPattern: with sub-pattern");
        fp->subPattern = parsePattern();
        if (!fp->subPattern) {
            LUC_LOG_EXPR("parseFieldPattern: ERROR - expected sub-pattern after ':'");
            errorAt(DiagCode::E1002, "expected sub-pattern after ':' in field pattern");
        }
    }
    // else: shorthand — subPattern is nullptr, bind by field name

    return fp;
}