#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

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
 * @return ForStmtPtr – for loop node on success, nullptr on error
 */
ForStmtPtr Parser::parseForStmt() {
    LUC_LOG_STMT_VERBOSE("parseForStmt: entering");
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::FOR, "expected 'for'");

    // Parse iteration variable name (required)
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_STMT("parseForStmt: ERROR - expected iteration variable name");
        errorAt(DiagCode::E1003, "expected iteration variable name");
        return nullptr;
    }
    std::string varName = ts_.advance().value;
    LUC_LOG_STMT_EXTREME("parseForStmt: variable name = '" << varName << "'");

    // Parse type annotation (required - no type inference in Luc)
    if (!looksLikeType()) {
        LUC_LOG_STMT("parseForStmt: ERROR - expected type annotation for iteration variable");
        errorAt(DiagCode::E1005, "expected type annotation for iteration variable '" + varName + "'");
        return nullptr;
    }
    TypePtr varType = parseType();
    if (!varType) {
        LUC_LOG_STMT("parseForStmt: ERROR - invalid type for iteration variable");
        errorAt(DiagCode::E1005, "invalid type for iteration variable");
        return nullptr;
    }

    // Consume 'in' keyword
    ts_.consume(TokenType::IN, "expected 'in'");

    // Parse the iterable expression
    ExprPtr iterable = nullptr;
    ExprPtr step = nullptr;
    
    // Check if this is a range iteration (starts with a literal or expression that could be a range bound)
    // We need to peek ahead to see if there's a '..' after the first expression
    size_t savedPos = ts_.getPos();
    
    // Try to parse as range: lo '..' [ '<' ] hi [ '..' step ]
    ExprPtr lo = parseExpr(false);
    if (lo && ts_.check(TokenType::RANGE)) {
        LUC_LOG_STMT_EXTREME("parseForStmt: detected range iteration");
        // This IS a range iteration
        ts_.advance(); // consume '..'
        
        bool isExclusive = ts_.match(TokenType::LESS);
        LUC_LOG_STMT_EXTREME("parseForStmt: range is " << (isExclusive ? "exclusive" : "inclusive"));
        
        ExprPtr hi = parseExpr(false);
        if (!hi) {
            LUC_LOG_STMT("parseForStmt: ERROR - expected upper bound after '..'");
            errorAt(DiagCode::E1008, "expected upper bound after '..' in range iteration");
            return nullptr;
        }
        
        // Build the range expression
        auto range = arena_.make<RangeExprAST>();
        range->loc = lo->loc;
        range->lo = lo;
        range->hi = hi;
        range->isExclusive = isExclusive;
        iterable = range;
        
        // Check for optional step: '..' step
        if (ts_.match(TokenType::RANGE)) {
            LUC_LOG_STMT_EXTREME("parseForStmt: parsing step expression");
            step = parseExpr(false);
            if (!step) {
                LUC_LOG_STMT("parseForStmt: ERROR - expected step expression after '..'");
                errorAt(DiagCode::E1008, "expected step expression after '..'");
                return nullptr;
            }
        }
    } else {
        // Not a range iteration - treat as collection iteration
        LUC_LOG_STMT_EXTREME("parseForStmt: detected collection iteration");
        // Restore position and parse normally
        ts_.setPos(savedPos);
        iterable = parseExpr(false);
        if (!iterable) {
            LUC_LOG_STMT("parseForStmt: ERROR - expected iterable expression after 'in'");
            errorAt(DiagCode::E1008, "expected iterable expression after 'in'");
            return nullptr;
        }
    }

    // Parse loop body (must be a block)
    if (!ts_.check(TokenType::LBRACE)) {
        LUC_LOG_STMT("parseForStmt: ERROR - expected '{' to start for loop body");
        errorAt(DiagCode::E1001, "expected '{' to start for loop body");
        return nullptr;
    }

    StmtPtr body = parseBlock();

    auto node = arena_.make<ForStmtAST>();
    node->loc = loc;
    
    node->iterVar = arena_.make<ParamAST>();
    node->iterVar->name = pool_.intern(varName);
    node->iterVar->type = varType;
    node->iterVar->isVariadic = false;
    
    node->iterable = iterable;
    node->step = step;
    node->body = body;
    
    LUC_LOG_STMT_VERBOSE("parseForStmt: success");
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

WhileStmtPtr Parser::parseWhileStmt() {
    LUC_LOG_STMT_VERBOSE("parseWhileStmt: entering");
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::WHILE, "expected 'while'");

    ExprPtr condition = parseExpr(false);
    if (!condition) {
        LUC_LOG_STMT("parseWhileStmt: ERROR - expected condition after 'while'");
        errorAt(DiagCode::E1008, "expected condition after 'while'");
        return nullptr;
    }
    LUC_LOG_STMT_EXTREME("parseWhileStmt: condition parsed");

    if (!ts_.check(TokenType::LBRACE)) {
        LUC_LOG_STMT("parseWhileStmt: ERROR - expected '{' to start while loop body");
        errorAt(DiagCode::E1001, "expected '{' to start while loop body");
        return nullptr;
    }

    StmtPtr body = parseBlock();

    auto node = arena_.make<WhileStmtAST>();
    node->loc = loc;
    node->condition = condition;
    node->body = body;
    
    LUC_LOG_STMT_VERBOSE("parseWhileStmt: success");
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

DoWhileStmtPtr Parser::parseDoWhileStmt() {
    LUC_LOG_STMT_VERBOSE("parseDoWhileStmt: entering");
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::DO, "expected 'do'");

    if (!ts_.check(TokenType::LBRACE)) {
        LUC_LOG_STMT("parseDoWhileStmt: ERROR - expected '{' after 'do'");
        errorAt(DiagCode::E1001, "expected '{' after 'do'");
        return nullptr;
    }

    StmtPtr body = parseBlock();
    LUC_LOG_STMT_EXTREME("parseDoWhileStmt: body parsed");

    ts_.consume(TokenType::WHILE, "expected 'while' after do body");

    ExprPtr condition = parseExpr(false);
    if (!condition) {
        LUC_LOG_STMT("parseDoWhileStmt: ERROR - expected condition after 'while'");
        errorAt(DiagCode::E1008, "expected condition after 'while'");
        return nullptr;
    }

    auto node = arena_.make<DoWhileStmtAST>();
    node->loc = loc;
    node->body = body;
    node->condition = condition;
    
    LUC_LOG_STMT_VERBOSE("parseDoWhileStmt: success");
    return node;
}