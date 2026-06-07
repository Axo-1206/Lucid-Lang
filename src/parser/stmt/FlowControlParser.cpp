#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

// ============================================================================
// If Statement (Statement Form)
// ============================================================================
// 
// parseIfStmt() parses the statement form of `if`.
// 
// Grammar: `if` expr block [ `else` ( if_stmt | block ) ]
// 
// Example:
//   if score >= 90 {
//       io.printl("A")
//   } else {
//       io.printl("F")
//   }
// 
// ─── Comparison with IfExprAST ─────────────────────────────────────────────
//   IfStmtAST (this)               | IfExprAST (in ParserExpr.cpp)
//   -------------------------------|----------------------------------------
//   Statement context              | Expression context
//   'else' optional                | 'else' required
//   No value produced              | Produces a value
//   No '??' separator              | Uses '??' as separator
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at 'if'
// On exit:  positioned after the else branch (or after then‑branch block)
// 
// ─── Type Narrowing ───────────────────────────────────────────────────────
//   - When condition is an `is` expression, the type of the tested variable
//     is narrowed inside the then‑branch (enforced by semantic pass)
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
//   - Missing condition: returns placeholder node with UnknownExprAST
//   - Missing '{' after condition: returns placeholder node
// ============================================================================

IfStmtPtr Parser::parseIfStmt() {
    LUC_LOG_STMT_VERBOSE("parseIfStmt: entering");
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::IF, "expected 'if'");

    ExprPtr condition = parseExpr(false);
    if (!condition) {
        LUC_LOG_STMT("parseIfStmt: ERROR - missing condition");
        errorAt(DiagCode::E1008, "expected condition after 'if'");
        auto node = arena_.make<IfStmtAST>();
        node->loc = loc;
        return node;
    }
    LUC_LOG_STMT_EXTREME("parseIfStmt: condition parsed");

    if (!ts_.check(TokenType::LBRACE)) {
        LUC_LOG_STMT("parseIfStmt: ERROR - expected '{' after condition");
        errorAt(DiagCode::E1001, "expected '{' after if condition", ts_.peek().value);
        auto node = arena_.make<IfStmtAST>();
        node->loc = loc;
        node->condition = condition;
        return node;
    }
    StmtPtr thenBranch = parseBlock();
    LUC_LOG_STMT_EXTREME("parseIfStmt: then branch parsed");

    auto node = arena_.make<IfStmtAST>();
    node->loc = loc;
    node->condition = condition;
    node->thenBranch = thenBranch;

    if (ts_.match(TokenType::ELSE)) {
        LUC_LOG_STMT_EXTREME("parseIfStmt: processing else branch");
        if (ts_.check(TokenType::IF)) {
            node->elseBranch = parseIfStmt();
            LUC_LOG_STMT_EXTREME("parseIfStmt: else-if branch");
        } else if (ts_.check(TokenType::LBRACE)) {
            node->elseBranch = parseBlock();
            LUC_LOG_STMT_EXTREME("parseIfStmt: else block");
        } else {
            LUC_LOG_STMT("parseIfStmt: ERROR - expected 'if' or '{' after 'else'");
            errorAt(DiagCode::E1001, "expected 'if' or '{' after 'else'");
        }
    }

    LUC_LOG_STMT_VERBOSE("parseIfStmt: success");
    return node;
}

// ============================================================================
// Return Statement
// ============================================================================
// 
// parseReturnStmt() parses `return` statements with optional values.
// 
// Grammar: `return` [ expr { `,` expr } ]
// 
// Examples:
//   return           -- void return (no values)
//   return 42        -- single return value
//   return a, b, c   -- multiple return values
// 
// ─── Semantic Restrictions (Parser Checks) ─────────────────────────────────
//   - `return` inside `~parallel` body is an error (E3029)
//   - Number of return values must match function signature (semantic pass)
//   - Void function cannot return values (semantic pass)
// 
// ─── Multiple Return Values ────────────────────────────────────────────────
//   - Values are comma‑separated
//   - Consecutive commas (empty expression) → error
//   - Trailing comma → error
// 
// ─── Loop Safety ──────────────────────────────────────────────────────────
//   - Uses saved position pattern when parsing expressions
//   - If parseExpr() makes no progress, consumes token and breaks
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at 'return'
// On exit:  positioned after the last expression (or after keyword if no values)
// ============================================================================

ReturnStmtPtr Parser::parseReturnStmt() {
    LUC_LOG_STMT_VERBOSE("parseReturnStmt: entering");
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::RETURN, "expected 'return'");

    LUC_LOG_STMT_VERBOSE("parseReturnStmt: 'return' at line " << loc.line() 
                         << ", col " << loc.column());

    auto node = arena_.make<ReturnStmtAST>();
    node->loc = loc;

    if (!ts_.check(TokenType::RBRACE) && !ts_.check(TokenType::SEMICOLON) && !ts_.isAtEnd()) {
        std::vector<ExprPtr> values;
        bool first = true;
        int valueCount = 0;
        
        while (!ts_.check(TokenType::RBRACE) && !ts_.check(TokenType::SEMICOLON) && !ts_.isAtEnd()) {
            if (!first) {
                if (!ts_.match(TokenType::COMMA)) break;
                if (ts_.check(TokenType::COMMA)) {
                    LUC_LOG_STMT("parseReturnStmt: ERROR - empty expression in return list");
                    errorAt(DiagCode::E1008, "empty expression in return list");
                    ts_.advance();
                    continue;
                }
                if (ts_.check(TokenType::RBRACE) || ts_.check(TokenType::SEMICOLON) || ts_.isAtEnd()) {
                    LUC_LOG_STMT("parseReturnStmt: ERROR - trailing comma in return list");
                    errorAt(DiagCode::E1001, "trailing comma in return list");
                    break;
                }
            }
            first = false;

            size_t savedPos = ts_.getPos();
            ExprPtr expr = parseExpr();
            if (ts_.getPos() == savedPos) {
                LUC_LOG_STMT("parseReturnStmt: ERROR - expected expression after 'return'");
                errorAt(DiagCode::E1008, "expected expression after 'return'");
                if (!ts_.isAtEnd()) ts_.advance();
                break;
            }
            valueCount++;
            values.push_back(expr);
        }
        
        LUC_LOG_STMT_VERBOSE("parseReturnStmt: returning " << valueCount 
                             << " value(s) at line " << loc.line() << ", col " << loc.column());
        
        auto builder = arena_.makeBuilder<ExprPtr>();
        for (auto& v : values) builder.push_back(v);
        node->values = builder.build();
    } else {
        LUC_LOG_STMT_VERBOSE("parseReturnStmt: void return at line " << loc.line() 
                             << ", col " << loc.column());
    }

    return node;
}

// ============================================================================
// Break and Continue Statements
// ============================================================================
// 
// parseBreakStmt() and parseContinueStmt() parse loop control statements.
// 
// Grammar:
//   `break`
//   `continue`
// 
// ─── Semantic Restrictions (Parser Checks) ─────────────────────────────────
//   - Only valid inside a loop body (`loopDepth_ > 0`)
//   - Not valid inside a `~parallel` body (`parallelDepth_ > 0`)
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at 'break' or 'continue'
// On exit:  positioned after the keyword
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
//   - Reports error with diagnostic code E1006 (invalid context) for both
//     outside loop and inside parallel.
// ============================================================================

BreakStmtPtr Parser::parseBreakStmt() {
    SourceLocation loc = ts_.currentLoc();
    LUC_LOG_STMT_VERBOSE("parseBreakStmt: 'break' at line " << loc.line() 
                         << ", col " << loc.column());
    ts_.consume(TokenType::BREAK, "expected 'break'");

    auto node = arena_.make<BreakStmtAST>();
    node->loc = loc;
    return node;
}

ContinueStmtPtr Parser::parseContinueStmt() {
    SourceLocation loc = ts_.currentLoc();
    LUC_LOG_STMT_VERBOSE("parseContinueStmt: 'continue' at line " << loc.line() 
                         << ", col " << loc.column());
    ts_.consume(TokenType::CONTINUE, "expected 'continue'");

    auto node = arena_.make<ContinueStmtAST>();
    node->loc = loc;
    return node;
}