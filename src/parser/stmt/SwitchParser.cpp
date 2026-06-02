#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Switch Statement
// ============================================================================
// 
// parseSwitchStmt() parses the statement form of `switch`.
// 
// Grammar:
//   `switch` expr `{` { `case` values `:` block } [ `default` `:` block ] `}`
// 
// Example:
//   switch code {
//       case 200, 201: { io.printl("ok") }
//       case 1..10:    { io.printl("light") }
//       default:       { io.printl("unknown") }
//   }
// 
// ─── Comparison with MatchExprAST ─────────────────────────────────────────
//   SwitchStmtAST (this)           | MatchExprAST (in ParserExpr.cpp)
//   -------------------------------|----------------------------------------
//   Statement (no value produced)  | Expression (produces value)
//   'default' optional             | 'default' required
//   Body is a block (statements)   | Body is an expression
//   No pattern matching            | Full pattern matching
// 
// ─── No Fallthrough ────────────────────────────────────────────────────────
//   Each case is isolated. There is no implicit or explicit fallthrough.
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at 'switch'
// On exit:  positioned after the closing '}'
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
//   - Missing subject: returns nullptr
//   - Missing '{': returns nullptr
//   - Duplicate 'default': reports error, skips second
//   - Missing '}': consume() reports error
// ============================================================================

ASTPtr<SwitchStmtAST> Parser::parseSwitchStmt() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::SWITCH, "expected 'switch'");

    ExprPtr subject = parseExpr(false);
    if (!subject) {
        errorAt(DiagCode::E1008, "expected expression after 'switch'");
        return nullptr;
    }

    ts_.consume(TokenType::LBRACE, "expected '{' after switch subject");

    auto node = arena_.make<SwitchStmtAST>();
    node->loc = loc;
    node->subject = std::move(subject);

    std::vector<SwitchCasePtr> cases;
    bool hasDefault = false;

    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        if (ts_.check(TokenType::RBRACE)) break;

        if (ts_.check(TokenType::CASE)) {
            SwitchCasePtr sc = parseSwitchCase();
            if (sc) cases.push_back(std::move(sc));
            continue;
        }

        if (ts_.check(TokenType::DEFAULT)) {
            if (hasDefault) {
                errorAt(DiagCode::E1002, "duplicate 'default' clause");
            }
            node->defaultLoc = ts_.currentLoc();
            ts_.advance();
            ts_.consume(TokenType::COLON, "expected ':' after 'default'");
            if (!ts_.check(TokenType::LBRACE)) {
                errorAt(DiagCode::E1001, "expected '{' to start default body");
            } else {
                node->defaultBody = parseBlock();
            }
            hasDefault = true;
            continue;
        }

        errorAt(DiagCode::E1002, "expected 'case' or 'default' inside switch");
        ts_.advance();
    }

    auto builder = arena_.makeBuilder<SwitchCasePtr>();
    for (auto& c : cases) builder.push_back(std::move(c));
    node->cases = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close switch");
    return node;
}

// ============================================================================
// Switch Case
// ============================================================================
// 
// parseSwitchCase() parses a single case clause inside a switch statement.
// 
// Grammar: `case` value { `,` value } `:` block
// 
// Example: `case 200, 201, 202: { io.printl("ok") }`
// 
// ─── Range Support ─────────────────────────────────────────────────────────
//   Values can be expressions or ranges: `case 1..10:`, `case 1..<10:`
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at 'case'
// On exit:  positioned after the case body block
// 
// ─── Loop Safety ──────────────────────────────────────────────────────────
//   - Uses consecutive error counter (max 5) to prevent infinite loops
//   - Saved position pattern for each value parse
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
//   - Missing ':' after values: consume() reports error
//   - Too many consecutive errors: skips to ':' and continues
// ============================================================================

SwitchCasePtr Parser::parseSwitchCase() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::CASE, "expected 'case'");

    auto sc = arena_.make<SwitchCaseAST>();
    sc->loc = loc;

    std::vector<ExprPtr> values;
    int consecutiveErrors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 5;

    if (ts_.check(TokenType::COLON)) {
        errorAt(DiagCode::E1001, "expected case value before ':'");
    } else {
        size_t savedPos = ts_.getPos();
        ExprPtr val = parsePrattExpr(0, false);
        if (ts_.getPos() == savedPos) {
            errorAt(DiagCode::E1002, "expected case value");
            if (!ts_.isAtEnd()) ts_.advance();
            consecutiveErrors++;
        } else if (val && !val->isa<UnknownExprAST>()) {
            if (ts_.check(TokenType::RANGE)) {
                values.push_back(parseRangeExpr(std::move(val)));
            } else {
                values.push_back(std::move(val));
            }
            consecutiveErrors = 0;
        }
    }

    while (ts_.check(TokenType::COMMA) && consecutiveErrors < MAX_CONSECUTIVE_ERRORS) {
        ts_.advance();
        if (ts_.check(TokenType::COLON)) break;

        size_t savedPos = ts_.getPos();
        ExprPtr val = parsePrattExpr(0, false);
        if (ts_.getPos() == savedPos) {
            errorAt(DiagCode::E1002, "expected case value after comma");
            if (!ts_.isAtEnd()) ts_.advance();
            break;
        }
        if (val && !val->isa<UnknownExprAST>()) {
            if (ts_.check(TokenType::RANGE)) {
                values.push_back(parseRangeExpr(std::move(val)));
            } else {
                values.push_back(std::move(val));
            }
        }
    }

    if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
        errorAt(DiagCode::E1002, "too many errors in case values; skipping to ':'");
        while (!ts_.isAtEnd() && !ts_.check(TokenType::COLON)) ts_.advance();
    }

    ts_.consume(TokenType::COLON, "expected ':' after case values");

    // Build values span
    auto builder = arena_.makeBuilder<ExprPtr>();
    for (auto& v : values) builder.push_back(std::move(v));
    sc->values = builder.build();

    if (!ts_.check(TokenType::LBRACE)) {
        errorAt(DiagCode::E1001, "expected '{' to start case body");
    } else {
        sc->body = parseBlock();
    }

    return sc;
}