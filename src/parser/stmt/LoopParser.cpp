#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

/**
 * @brief Parses `for` loops over ranges or collections.
 * 
 * Grammar:
 *   for_stmt := 'for' IDENTIFIER type_ann 'in' ( range_expr | expr ) [ '..' expr ] block
 * 
 *   range_expr := expr range_op expr
 *   range_op   := '..' | '..<'
 * 
 * Examples:
 *   for i int in 0..10 { ... }          -- range iteration (0 to 10 inclusive)
 *   for i int in 0..<10 { ... }         -- exclusive end (0 to 9)
 *   for i int in 0..10..2 { ... }       -- with step (0, 2, 4, 6, 8, 10)
 *   for i int in 0..<10..2 { ... }      -- exclusive range with step
 *   for item string in items { ... }    -- collection iteration
 * 
 * ─── Parsing Strategy ──────────────────────────────────────────────────────
 *   1. Parse 'for' keyword
 *   2. Parse iteration variable name and required type annotation
 *   3. Parse 'in' keyword
 *   4. Try to parse as range: lo '..' [ '<' ] hi [ '..' step ]
 *      - If '..' is found after lo, this is a range iteration
 *      - Build RangeExprAST with lo, hi, and isExclusive flag
 *      - If second '..' is found, parse step expression
 *   5. If no '..' found, parse as collection iteration (restore position)
 *   6. Parse loop body (must be a block)
 * 
 * ─── Range Iteration ───────────────────────────────────────────────────────
 *   - Lo expression: start (inclusive)
 *   - Hi expression: end (inclusive for '..', exclusive for '..<')
 *   - Optional step: third expression after second '..'
 *   - Iteration variable type is specified explicitly (no inference)
 * 
 * ─── Collection Iteration ──────────────────────────────────────────────────
 *   - Iterable must be a collection (array, slice, dynamic array)
 *   - Iteration variable type must match element type
 *   - No step allowed
 * 
 * ─── Loop Depth Tracking ───────────────────────────────────────────────────
 *   - `loopDepth_` is incremented before parsing the body
 *   - Used by `break`/`continue` to validate loop context
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'for' keyword
 * On exit:  positioned after the loop body block
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing variable name: returns nullptr
 * - Missing type annotation: reports error, returns nullptr
 * - Missing 'in': reports error, returns nullptr
 * - Missing hi after '..' in range: reports error, returns nullptr
 * - Missing expression after second '..': reports error, returns nullptr
 * - Missing iterable for collection: reports error, returns nullptr
 * - Missing '{' for body: reports error, returns nullptr
 * 
 * @return ASTPtr<ForStmtAST> – for loop node on success, nullptr on error
 */
ASTPtr<ForStmtAST> Parser::parseForStmt() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::FOR, "expected 'for'");

    // Parse iteration variable name (required)
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected iteration variable name");
        return nullptr;
    }
    std::string varName = ts_.advance().value;

    // Parse type annotation (required - no type inference in Luc)
    if (!looksLikeType()) {
        errorAt(DiagCode::E2005, "expected type annotation for iteration variable '" + varName + "'");
        return nullptr;
    }
    TypePtr varType = parseType();
    if (!varType) {
        errorAt(DiagCode::E2005, "invalid type for iteration variable");
        return nullptr;
    }

    // Consume 'in' keyword
    ts_.consume(TokenType::IN, "expected 'in'");

    // Parse the iterable expression
    ExprPtr iterable;
    ExprPtr step = nullptr;
    
    // Check if this is a range iteration (starts with a literal or expression that could be a range bound)
    // We need to peek ahead to see if there's a '..' after the first expression
    size_t savedPos = ts_.getPos();
    
    // Try to parse as range: lo '..' [ '<' ] hi [ '..' step ]
    ExprPtr lo = parseExpr(false);
    if (lo && ts_.check(TokenType::RANGE)) {
        // This IS a range iteration
        ts_.advance(); // consume '..'
        
        bool isExclusive = ts_.match(TokenType::LESS);
        
        ExprPtr hi = parseExpr(false);
        if (!hi) {
            errorAt(DiagCode::E2008, "expected upper bound after '..' in range iteration");
            return nullptr;
        }
        
        // Build the range expression
        auto range = arena_.make<RangeExprAST>();
        range->loc = lo->loc;
        range->lo = std::move(lo);
        range->hi = std::move(hi);
        range->isExclusive = isExclusive;
        iterable = std::move(range);
        
        // Check for optional step: '..' step
        if (ts_.match(TokenType::RANGE)) {
            step = parseExpr(false);
            if (!step) {
                errorAt(DiagCode::E2008, "expected step expression after '..'");
                return nullptr;
            }
        }
    } else {
        // Not a range iteration - treat as collection iteration
        // Restore position and parse normally
        ts_.setPos(savedPos);
        iterable = parseExpr(false);
        if (!iterable) {
            errorAt(DiagCode::E2008, "expected iterable expression after 'in'");
            return nullptr;
        }
    }

    // Parse loop body (must be a block)
    if (!ts_.check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start for loop body");
        return nullptr;
    }

    ++loopDepth_;
    StmtPtr body = parseBlock();
    --loopDepth_;

    auto node = arena_.make<ForStmtAST>();
    node->loc = loc;
    
    node->iterVar = arena_.make<ParamAST>();
    node->iterVar->name = pool_.intern(varName);
    node->iterVar->type = std::move(varType);
    node->iterVar->isVariadic = false;
    
    node->iterable = std::move(iterable);
    node->step = std::move(step);
    node->body = std::move(body);
    
    return node;
}

// ============================================================================
// While Loop
// ============================================================================
// 
// parseWhileStmt() parses condition‑first loops.
// 
// Grammar: `while` expr block
// 
// Example: `while n < 5 { n += 1 }`
// 
// ─── Semantics ─────────────────────────────────────────────────────────────
//   - Condition evaluated before each iteration
//   - Loop exits when condition evaluates to false
//   - Body never executes if condition is initially false
// 
// ─── Loop Depth Tracking ───────────────────────────────────────────────────
//   - `loopDepth_` is incremented before parsing the body
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at 'while'
// On exit:  positioned after the loop body
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
//   - Missing condition: returns nullptr
//   - Missing '{' after condition: reports error, returns nullptr
// ============================================================================

ASTPtr<WhileStmtAST> Parser::parseWhileStmt() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::WHILE, "expected 'while'");

    ExprPtr condition = parseExpr(false);
    if (!condition) {
        errorAt(DiagCode::E2008, "expected condition after 'while'");
        return nullptr;
    }

    if (!ts_.check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start while loop body");
        return nullptr;
    }

    ++loopDepth_;
    StmtPtr body = parseBlock();
    --loopDepth_;

    auto node = arena_.make<WhileStmtAST>();
    node->loc = loc;
    node->condition = std::move(condition);
    node->body = std::move(body);
    
    return node;
}

// ============================================================================
// Do-While Loop
// ============================================================================
// 
// parseDoWhileStmt() parses body‑first loops.
// 
// Grammar: `do` block `while` expr
// 
// Example: `do { retries += 1 } while retries < 3`
// 
// ─── Semantics ─────────────────────────────────────────────────────────────
//   - Body executes at least once (unconditionally)
//   - Condition evaluated after each iteration
//   - Loop repeats while condition is true
// 
// ─── Loop Depth Tracking ───────────────────────────────────────────────────
//   - `loopDepth_` is incremented before parsing the body
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at 'do'
// On exit:  positioned after the condition expression
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
//   - Missing '{' after 'do': returns nullptr
//   - Missing 'while' after body: reports error, returns nullptr
//   - Missing condition: reports error, returns nullptr
// ============================================================================

ASTPtr<DoWhileStmtAST> Parser::parseDoWhileStmt() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::DO, "expected 'do'");

    if (!ts_.check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' after 'do'");
        return nullptr;
    }

    ++loopDepth_;
    StmtPtr body = parseBlock();
    --loopDepth_;

    ts_.consume(TokenType::WHILE, "expected 'while' after do body");

    ExprPtr condition = parseExpr(false);
    if (!condition) {
        errorAt(DiagCode::E2008, "expected condition after 'while'");
        return nullptr;
    }

    auto node = arena_.make<DoWhileStmtAST>();
    node->loc = loc;
    node->body = std::move(body);
    node->condition = std::move(condition);
    
    return node;
}