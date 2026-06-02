#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// If Expression (Expression Form)
// ============================================================================
// 
// parseIfExpr() parses the expression form of 'if', which produces a value.
// 
// Grammar: 'if' expr '??' expr 'else' expr
// 
// Example: let grade = if score >= 60 ?? "pass" else "fail"
// 
// ─── Comparison with IfStmtAST ────────────────────────────────────────────
//   IfExprAST (this)              | IfStmtAST (in ParserStmt.cpp)
//   ------------------------------|------------------------------------------
//   Expression context            | Statement context
//   'else' required               | 'else' optional
//   Produces a value              | No value produced
//   Uses '??' separator           | No '??' separator
// 
// ─── Operator Precedence ──────────────────────────────────────────────────
// The '??' here is a syntactic separator (not the null‑coalescing operator).
// The condition is parsed with PREC_NULLCOAL to stop at the first '??'.
// The expression is right‑associative: a ?? b else c ?? d else e.
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at 'if' keyword
// On exit:  positioned after the else branch expression
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
// - Missing condition after 'if' → returns UnknownExprAST
// - Missing '??' after condition → reports error, returns UnknownExprAST
// - Missing 'else' keyword → reports error, returns UnknownExprAST
// ============================================================================

ExprPtr Parser::parseIfExpr(bool allowStructLiteral) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::IF, "expected 'if'");

    ExprPtr condition = parsePrattExpr(PREC_NULLCOAL, allowStructLiteral);
    if (!condition) {
        errorAt(DiagCode::E1008, "expected condition after 'if'");
        return arena_.make<UnknownExprAST>();
    }

    if (!ts_.match(TokenType::QUESTION_QUESTION)) {
        errorAt(DiagCode::E1001, "expected '\?\?' after if condition in expression form");
        return arena_.make<UnknownExprAST>();
    }

    ExprPtr thenBranch = parseExpr();
    if (!thenBranch) {
        errorAt(DiagCode::E1008, "expected expression after '\?\?'");
    }

    if (!ts_.match(TokenType::ELSE)) {
        errorAt(DiagCode::E1006, "expression-form 'if' requires an 'else' branch");
        return arena_.make<UnknownExprAST>();
    }

    ExprPtr elseBranch = parseExpr();
    if (!elseBranch) {
        errorAt(DiagCode::E1008, "expected expression after 'else'");
    }

    auto node = arena_.make<IfExprAST>();
    node->loc = loc;
    node->condition = std::move(condition);
    node->thenBranch = std::move(thenBranch);
    node->elseBranch = std::move(elseBranch);
    return node;
}