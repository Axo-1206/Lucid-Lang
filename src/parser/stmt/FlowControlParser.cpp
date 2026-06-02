#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

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

ASTPtr<IfStmtAST> Parser::parseIfStmt() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::IF, "expected 'if'");

    ExprPtr condition = parseExpr(false);
    if (!condition) {
        errorAt(DiagCode::E1008, "expected condition after 'if'");
        auto node = arena_.make<IfStmtAST>();
        node->loc = loc;
        return node;
    }

    if (!ts_.check(TokenType::LBRACE)) {
        errorAt(DiagCode::E1001, "expected '{' after if condition");
        auto node = arena_.make<IfStmtAST>();
        node->loc = loc;
        node->condition = std::move(condition);
        return node;
    }
    StmtPtr thenBranch = parseBlock();

    auto node = arena_.make<IfStmtAST>();
    node->loc = loc;
    node->condition = std::move(condition);
    node->thenBranch = std::move(thenBranch);

    if (ts_.match(TokenType::ELSE)) {
        if (ts_.check(TokenType::IF)) {
            node->elseBranch = parseIfStmt();
        } else if (ts_.check(TokenType::LBRACE)) {
            node->elseBranch = parseBlock();
        } else {
            errorAt(DiagCode::E1001, "expected 'if' or '{' after 'else'");
        }
    }

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

ASTPtr<ReturnStmtAST> Parser::parseReturnStmt() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::RETURN, "expected 'return'");

    if (parallelDepth_ > 0) {
        error(loc, DiagCode::E1006, "'return' is not valid inside a 'parallel' body");
    }

    auto node = arena_.make<ReturnStmtAST>();
    node->loc = loc;

    if (!ts_.check(TokenType::RBRACE) && !ts_.check(TokenType::SEMICOLON) && !ts_.isAtEnd()) {
        std::vector<ExprPtr> values;
        bool first = true;
        
        while (!ts_.check(TokenType::RBRACE) && !ts_.check(TokenType::SEMICOLON) && !ts_.isAtEnd()) {
            if (!first) {
                if (!ts_.match(TokenType::COMMA)) break;
                if (ts_.check(TokenType::COMMA)) {
                    errorAt(DiagCode::E1008, "empty expression in return list");
                    ts_.advance();
                    continue;
                }
                if (ts_.check(TokenType::RBRACE) || ts_.check(TokenType::SEMICOLON) || ts_.isAtEnd()) {
                    errorAt(DiagCode::E1001, "trailing comma in return list");
                    break;
                }
            }
            first = false;

            size_t savedPos = ts_.getPos();
            ExprPtr expr = parseExpr();
            if (ts_.getPos() == savedPos) {
                errorAt(DiagCode::E1008, "expected expression after 'return'");
                if (!ts_.isAtEnd()) ts_.advance();
                break;
            }
            values.push_back(std::move(expr));
        }
        
        auto builder = arena_.makeBuilder<ExprPtr>();
        for (auto& v : values) builder.push_back(std::move(v));
        node->values = builder.build();
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

ASTPtr<BreakStmtAST> Parser::parseBreakStmt() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::BREAK, "expected 'break'");

    if (loopDepth_ == 0) {
        error(loc, DiagCode::E1006, "'break' is only valid inside a loop body");
    }
    if (parallelDepth_ > 0) {
        error(loc, DiagCode::E1006, "'break' is not valid inside a 'parallel' body");
    }

    auto node = arena_.make<BreakStmtAST>();
    node->loc = loc;
    return node;
}

ASTPtr<ContinueStmtAST> Parser::parseContinueStmt() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::CONTINUE, "expected 'continue'");

    if (loopDepth_ == 0) {
        error(loc, DiagCode::E1006, "'continue' is only valid inside a loop body");
    }
    if (parallelDepth_ > 0) {
        error(loc, DiagCode::E1006, "'continue' is not valid inside a 'parallel' body");
    }

    auto node = arena_.make<ContinueStmtAST>();
    node->loc = loc;
    return node;
}