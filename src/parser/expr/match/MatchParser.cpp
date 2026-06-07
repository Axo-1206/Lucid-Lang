#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

// ============================================================================
// Match Expression
// ============================================================================

/**
 * @brief Parses a match expression (pattern matching).
 * 
 * Grammar:
 *   match_expr := 'match' expr '{' { match_arm } default_arm '}'
 * 
 * Example:
 *   match status {
 *       200      => "ok"
 *       404      => "not found"
 *       default  => "unknown"
 *   }
 * 
 * @return ExprPtr – MatchExprAST node, or UnknownExprAST on error.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'match' keyword
 * On exit:  positioned after the closing '}'
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing subject expression: reports error, returns UnknownExprAST
 * - Missing '{' after subject: consume() reports error
 * - Missing '}' at end: consume() reports error
 * - Duplicate default arm: reports error
 * - Missing default arm: reports error (E1006)
 */
ExprPtr Parser::parseMatchExpr() {
    LUC_LOG_EXPR_VERBOSE("parseMatchExpr: entering");
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::MATCH, "expected 'match'");

    ExprPtr subject = parseExpr(false);
    if (!subject) {
        LUC_LOG_EXPR("parseMatchExpr: ERROR - expected expression after 'match'");
        errorAt(DiagCode::E1008, "expected expression after 'match'");
        return arena_.make<UnknownExprAST>();
    }
    LUC_LOG_EXPR_EXTREME("parseMatchExpr: subject parsed");

    ts_.consume(TokenType::LBRACE, "expected '{' after match subject");

    auto node = arena_.make<MatchExprAST>();
    node->loc = loc;
    node->subject = subject;

    std::vector<MatchArmPtr> arms;
    bool hasDefault = false;
    int armCount = 0;

    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        if (ts_.check(TokenType::DEFAULT)) {
            if (hasDefault) {
                LUC_LOG_EXPR("parseMatchExpr: ERROR - duplicate default arm");
                errorAt(DiagCode::E1002, "duplicate 'default' arm in match expression");
            }
            LUC_LOG_EXPR_EXTREME("parseMatchExpr: parsing default arm");
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
            LUC_LOG_EXPR("parseMatchExpr: ERROR - failed to parse match arm, skipping");
            errorAt(DiagCode::E1002, "failed to parse match arm, skipping");
            ts_.advance();
            continue;
        }
        if (!arm) {
            LUC_LOG_EXPR("parseMatchExpr: ERROR - invalid match arm, skipping");
            errorAt(DiagCode::E1002, "invalid match arm, skipping");
            continue;
        }
        armCount++;
        LUC_LOG_EXPR_EXTREME("parseMatchExpr: parsed arm #" << armCount);
        arms.push_back(arm);
    }

    ts_.consume(TokenType::RBRACE, "expected '}' to close match expression");

    if (!hasDefault) {
        LUC_LOG_EXPR("parseMatchExpr: ERROR - match expression must have default arm");
        error(loc, DiagCode::E1006, "match expression must have a 'default' arm");
    }

    auto armsBuilder = arena_.makeBuilder<MatchArmPtr>();
    for (auto& a : arms) armsBuilder.push_back(a);
    node->arms = armsBuilder.build();

    LUC_LOG_EXPR_VERBOSE("parseMatchExpr: " << armCount << " arm(s), default=" << hasDefault);
    return node;
}

// ============================================================================
// Match Arm
// ============================================================================

/**
 * @brief Parses a single match arm (non‑default).
 * 
 * Grammar:
 *   match_arm := pattern { ',' pattern } [ 'if' expr ] '=>' expr [ ',' expr ]
 * 
 * Example patterns:
 *   200           => "ok"
 *   200, 201, 202 => "success"
 *   1..10         => "light"
 *   n if n < 0    => "invalid: " + string(n)
 *   s is Circle   => s.radius * s.radius * 3.14159
 * 
 * @return MatchArmPtr – MatchArmAST node, or nullptr on error.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at first pattern (or ',' after previous arm)
 * On exit:  positioned after the last expression (or after the arm)
 * 
 * ─── Expression Count ──────────────────────────────────────────────────────
 * - At least one expression is required
 * - A second expression is allowed after a comma (multi‑value return)
 * - More than two expressions is an error
 * 
 * ─── Guard Clause ─────────────────────────────────────────────────────────
 * - Optional 'if' followed by an expression
 * - Only valid after bind/wildcard patterns
 */
MatchArmPtr Parser::parseMatchArm() {
    LUC_LOG_EXPR_EXTREME("parseMatchArm: entering");
    SourceLocation loc = ts_.currentLoc();
    auto arm = arena_.make<MatchArmAST>();
    arm->loc = loc;

    // Parse patterns (at least one)
    std::vector<PatternPtr> patterns;
    PatternPtr pat = parsePattern();
    if (!pat) return nullptr;
    patterns.push_back(pat);

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
            LUC_LOG_EXPR("parseMatchArm: ERROR - expected pattern after ','");
            errorAt(DiagCode::E1002, "expected pattern after ',' in match arm");
            break;
        }
        ts_.advance(); // consume comma
        pat = parsePattern();
        if (!pat) break;
        patterns.push_back(pat);
    }

    LUC_LOG_EXPR_EXTREME("parseMatchArm: " << patterns.size() << " pattern(s)");

    // Build patterns span
    auto patternsBuilder = arena_.makeBuilder<PatternPtr>();
    for (auto& p : patterns) patternsBuilder.push_back(p);
    arm->patterns = patternsBuilder.build();

    // Optional guard: 'if' expr
    if (ts_.check(TokenType::IF)) {
        ts_.advance();
        LUC_LOG_EXPR_EXTREME("parseMatchArm: parsing guard expression");
        size_t savedPos = ts_.getPos();
        ExprPtr guard = parseExpr();
        if (ts_.getPos() == savedPos) {
            LUC_LOG_EXPR("parseMatchArm: ERROR - expected guard expression");
            errorAt(DiagCode::E1008, "expected guard expression after 'if' in match arm");
        } else {
            arm->guard = guard;
        }
    }

    ts_.consume(TokenType::FAT_ARROW, "expected '=>' after match pattern");

    // Parse result expressions: at least one, at most two
    std::vector<ExprPtr> exprs;
    size_t beforePos = ts_.getPos();
    ExprPtr first = parseExpr();
    if (ts_.getPos() == beforePos || !first) {
        LUC_LOG_EXPR("parseMatchArm: ERROR - expected result expression");
        errorAt(DiagCode::E1008, "expected result expression after '=>' in match arm");
    } else {
        exprs.push_back(first);
    }

    // Optional second expression after comma
    if (ts_.match(TokenType::COMMA)) {
        if (ts_.check(TokenType::COMMA) || ts_.check(TokenType::RBRACE) || 
            ts_.check(TokenType::FAT_ARROW) || ts_.isAtEnd()) {
            LUC_LOG_EXPR("parseMatchArm: ERROR - expected expression after ','");
            errorAt(DiagCode::E1001, "expected expression after ',' in match arm");
        } else {
            size_t beforePos2 = ts_.getPos();
            ExprPtr second = parseExpr();
            if (ts_.getPos() == beforePos2 || !second) {
                LUC_LOG_EXPR("parseMatchArm: ERROR - expected second expression");
                errorAt(DiagCode::E1008, "expected second result expression after ',' in match arm");
            } else {
                exprs.push_back(second);
            }
        }
        // No more commas allowed
        if (ts_.match(TokenType::COMMA)) {
            LUC_LOG_EXPR("parseMatchArm: ERROR - too many expressions");
            errorAt(DiagCode::E1001, "match arm cannot have more than two expressions");
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
    for (auto& e : exprs) exprsBuilder.push_back(e);
    arm->exprs = exprsBuilder.build();
    
    LUC_LOG_EXPR_EXTREME("parseMatchArm: " << exprs.size() << " result expression(s)");

    return arm;
}

// ============================================================================
// Default Arm
// ============================================================================

/**
 * @brief Parses the default arm of a match expression.
 * 
 * Grammar:
 *   default_arm := 'default' '=>' expr [ ',' expr ]
 * 
 * @return DefaultArmPtr – DefaultArmAST node, or nullptr on error.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'default' keyword
 * On exit:  positioned after the last expression
 * 
 * ─── Expression Count ──────────────────────────────────────────────────────
 * - At least one expression is required
 * - A second expression is allowed after a comma (multi‑value return)
 * - More than two expressions is an error
 */
DefaultArmPtr Parser::parseDefaultArm() {
    LUC_LOG_EXPR_EXTREME("parseDefaultArm: entering");
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
        LUC_LOG_EXPR("parseDefaultArm: ERROR - expected expression after '=>'");
        errorAt(DiagCode::E1008, "expected expression after '=>' in default arm");
    } else {
        exprs.push_back(first);
    }

    // Optional second expression after comma
    if (ts_.match(TokenType::COMMA)) {
        if (ts_.check(TokenType::COMMA) || ts_.check(TokenType::RBRACE) || 
            ts_.check(TokenType::FAT_ARROW) || ts_.isAtEnd()) {
            LUC_LOG_EXPR("parseDefaultArm: ERROR - expected expression after ','");
            errorAt(DiagCode::E1001, "expected expression after ',' in default arm");
        } else {
            size_t savedPos2 = ts_.getPos();
            ExprPtr second = parseExpr();
            if (ts_.getPos() == savedPos2 || !second) {
                LUC_LOG_EXPR("parseDefaultArm: ERROR - expected second expression");
                errorAt(DiagCode::E1008, "expected second expression after ',' in default arm");
            } else {
                exprs.push_back(second);
            }
        }
        // No more commas allowed
        if (ts_.match(TokenType::COMMA)) {
            LUC_LOG_EXPR("parseDefaultArm: ERROR - too many expressions");
            errorAt(DiagCode::E1001, "default arm cannot have more than two expressions");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && !ts_.check(TokenType::DEFAULT)) {
                ts_.advance();
            }
        }
    }

    auto builder = arena_.makeBuilder<ExprPtr>();
    for (auto& e : exprs) builder.push_back(e);
    arm->exprs = builder.build();
    
    LUC_LOG_EXPR_EXTREME("parseDefaultArm: " << exprs.size() << " expression(s)");

    return arm;
}