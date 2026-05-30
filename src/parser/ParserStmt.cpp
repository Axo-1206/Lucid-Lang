/**
 * @file ParserStmt.cpp
 * @brief Parses all statements: control flow, blocks, assignments, and jumps.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements the statement parsers for the Luc language.
 * 
 * ## Statements Covered
 * 
 *   - Block statements      : `{ stmt* }`
 *   - Variable declarations : `let x int = 5`, `const MAX int = 100`
 *   - Multi‑variable declaration : `let x int, y int = f()`
 *   - Multi‑assignment (reassignment) : `x, y = g()`
 *   - If statements         : `if cond { ... } else { ... }`
 *   - Switch statements     : `switch expr { case val: ... default: ... }`
 *   - For loops             : `for i in 0..10 { ... }`
 *   - While loops           : `while cond { ... }`
 *   - Do‑while loops        : `do { ... } while cond`
 *   - Return statements     : `return expr`
 *   - Break/Continue        : `break`, `continue`
 *   - Expression statements : `func();`
 * 
 * ## Statement Dispatch Priority (parseStmt)
 * 
 *   1. Multi‑assignment (reassignment)      → `parseMultiAssignStmt()`
 *   2. Multi‑variable declaration (let/const with commas) → `parseMultiVarDecl()`
 *   3. Local declarations (type, struct, etc.) → `parseDeclaration(Local)`
 *   4. 'pub' inside block (error)           → report, then continue
 *   5. Control flow keywords                → respective parsers
 *   6. Expression statement                 → `parseExpr()` then wrap in `ExprStmtAST`
 * 
 * ## Context Flags
 * 
 *   - `loopDepth_`     : Incremented when entering a loop body. Used by
 *                        `parseBreakStmt()` and `parseContinueStmt()` to
 *                        validate that `break`/`continue` are inside a loop.
 *   - `parallelDepth_` : Incremented in `~parallel` body. Used to reject
 *                        `await`, `return`, `break`, `continue` in parallel
 *                        contexts.
 * 
 * ## Loop Safety
 * 
 * Functions that parse sequences of statements (e.g., `parseBlock()`) use
 * the saved position pattern to prevent infinite loops:
 * 
 *   size_t savedPos = ts_.getPos();
 *   while (!ts_.check(endToken)) {
 *       parseItem();
 *       if (ts_.getPos() == savedPos) {
 *           if (!ts_.isAtEnd()) ts_.advance();
 *           break;
 *       }
 *       savedPos = ts_.getPos();
 *   }
 * 
 * @see ParserDecl.cpp for local declarations inside blocks
 * @see ParserExpr.cpp for expressions used in statements
 * @see LUC_GRAMMAR.md for statement grammar rules
 */

#include "Parser.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

#include <cassert>
#include <algorithm>
#include <iostream>

// ============================================================================
// Statement Dispatcher
// ============================================================================
// 
// parseStmt() is the root entry point for parsing statements.
// 
// Dispatch Priority:
//   1. Multi‑assignment (reassignment) – lookahead detects IDENTIFIER followed
//      by comma and eventually '='. Examples: `a, b = f()`, `arr[i], x = g()`
// 
//   2. Multi‑variable declaration – when 'let' or 'const' is followed by
//      identifier, type, comma. Example: `let x int, y int = f()`
// 
//   3. Local declarations – `type`, `struct`, `enum`, `impl`, `trait`, `from`,
//      `let`, `const`, `@`, `use`. Calls `parseDeclaration(Local)`.
// 
//   4. 'pub' inside block – error (E2014), then try to parse as local decl.
// 
//   5. Control flow – `if`, `switch`, `for`, `while`, `do`, `return`, `break`,
//      `continue`.
// 
//   6. Expression statement – fallback. Parses an expression, wraps in
//      `ExprStmtAST`. The expression's result is discarded.
// 
// ─── Error Recovery ─────────────────────────────────────────────────────────
//   - Invalid variable without let/const: detects `ident type = expr`, reports
//     error, skips to semicolon or closing brace.
//   - Expression statement failure: reports error, skips to statement boundary.
//   - Unknown token: reports error, consumes one token, returns UnknownStmtAST.
// 
// ─── Loop Safety ────────────────────────────────────────────────────────────
//   - Expression statement fallback includes a skip loop that guarantees
//     forward progress.
//   - The multi‑var detection lookahead uses saved position and restores on
//     failure (no tokens consumed on mismatch).
// ============================================================================

StmtPtr Parser::parseStmt() {
    // Multi-assignment (reassignment) - safe lookahead
    if (looksLikeMultiAssignStart()) {
        return parseMultiAssignStmt();
    }

    // Multi-variable declaration (let/const with commas)
    if (ts_.checkAny({TokenType::LET, TokenType::CONST})) {
        size_t savedPos = ts_.getPos();
        ts_.advance(); // consume keyword
        
        bool hasIdentifier = ts_.check(TokenType::IDENTIFIER);
        if (hasIdentifier) ts_.advance();
        
        bool hasType = false;
        if (looksLikeType()) {
            size_t typePos = ts_.getPos();
            // Need a way to check if type is valid without consuming - simplified
            hasType = true;
            if (hasIdentifier && hasType && ts_.check(TokenType::COMMA)) {
                ts_.setPos(savedPos); // Would need setter - simplified approach
                return parseMultiVarDecl();
            }
        }
        // Restore position and fall through to single declaration
        ts_.setPos(savedPos);
    }

    // Local declarations
    if (ts_.checkAny({TokenType::TYPE, TokenType::STRUCT, TokenType::ENUM,
                      TokenType::IMPL, TokenType::TRAIT, TokenType::FROM,
                      TokenType::LET, TokenType::CONST, TokenType::AT_SIGN,
                      TokenType::USE})) {
        DeclPtr decl = parseDeclaration(DeclContext::Local);
        if (!decl) return nullptr;
        
        auto ds = arena_.make<DeclStmtAST>(std::move(decl));
        ds->loc = ts_.currentLoc();
        return ds;
    }

    // 'pub' inside a block - error
    if (ts_.check(TokenType::PUB)) {
        errorAt(DiagCode::E2014, "'pub' is not valid inside a block");
        ts_.advance();
        if (ts_.checkAny({TokenType::LET, TokenType::CONST, TokenType::TYPE,
                          TokenType::STRUCT, TokenType::ENUM, TokenType::IMPL,
                          TokenType::FROM})) {
            DeclPtr decl = parseDeclaration(DeclContext::Local);
            if (decl) {
                auto ds = arena_.make<DeclStmtAST>(std::move(decl));
                ds->loc = ts_.currentLoc();
                return ds;
            }
        }
        auto unknown = arena_.make<UnknownStmtAST>();
        unknown->loc = ts_.currentLoc();
        return unknown;
    }

    // Control flow keywords
    if (ts_.check(TokenType::IF))       return parseIfStmt();
    if (ts_.check(TokenType::SWITCH))   return parseSwitchStmt();
    if (ts_.check(TokenType::FOR))      return parseForStmt();
    if (ts_.check(TokenType::WHILE))    return parseWhileStmt();
    if (ts_.check(TokenType::DO))       return parseDoWhileStmt();
    if (ts_.check(TokenType::RETURN))   return parseReturnStmt();
    if (ts_.check(TokenType::BREAK))    return parseBreakStmt();
    if (ts_.check(TokenType::CONTINUE)) return parseContinueStmt();

    // Detect invalid variable declaration missing let/const
    if (ts_.check(TokenType::IDENTIFIER)) {
        size_t savedPos = ts_.getPos();
        ts_.advance();
        if (looksLikeType() && ts_.check(TokenType::ASSIGN)) {
            errorAt(DiagCode::E2002, "variable declaration requires 'let' or 'const'");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::SEMICOLON) && !ts_.check(TokenType::RBRACE)) {
                ts_.advance();
            }
            if (ts_.check(TokenType::SEMICOLON)) ts_.advance();
            auto unknown = arena_.make<UnknownStmtAST>();
            unknown->loc = ts_.currentLoc();
            return unknown;
        }
        ts_.setPos(savedPos);
    }

    // Expression statement
    if (!looksLikeStmtStart()) {
        errorAt(DiagCode::E2002, "unexpected token '" + ts_.peek().value + "'");
        auto unknown = arena_.make<UnknownStmtAST>();
        unknown->loc = ts_.currentLoc();
        if (!ts_.isAtEnd()) ts_.advance();
        return unknown;
    }

    SourceLocation loc = ts_.currentLoc();
    ExprPtr expr = parseExpr();
    if (!expr) {
        errorAt(DiagCode::E2008, "expected expression statement");
        auto unknown = arena_.make<UnknownStmtAST>();
        unknown->loc = ts_.currentLoc();
        while (!ts_.isAtEnd() && !ts_.check(TokenType::SEMICOLON) && !ts_.check(TokenType::RBRACE)) {
            ts_.advance();
        }
        if (ts_.check(TokenType::SEMICOLON)) ts_.advance();
        return unknown;
    }

    auto stmt = arena_.make<ExprStmtAST>(std::move(expr));
    stmt->loc = loc;
    return stmt;
}

// ============================================================================
// Multi‑Variable Declaration
// ============================================================================
// 
// parseMultiVarDecl() parses `let` or `const` declarations with multiple
// variables.
// 
// Grammar: `let` IDENTIFIER type `,` IDENTIFIER type `=` expr
// 
// Example: `let q int, r int = divmod(10, 3)`
// 
// ─── Important Rules ────────────────────────────────────────────────────────
//   - Each variable has its own explicit type annotation (no type inference)
//   - The RHS must be a single expression returning N values (N = variable count)
//   - `const` multi‑declaration requires compile‑time constant RHS
//   - Attributes are NOT allowed (E2027)
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at 'let' or 'const' keyword
// On exit:  positioned after the RHS expression
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
//   - Missing '=': reports error, returns nullptr
//   - Missing RHS: reports error, returns nullptr
//   - Invalid variable spec: breaks loop, continues with already parsed vars
// ============================================================================

ASTPtr<MultiVarDeclAST> Parser::parseMultiVarDecl(std::vector<AttributePtr> attrs) {

    if (!attrs.empty()) {
        error(attrs[0]->loc, DiagCode::E2027, "attributes cannot be used on multi-variable declarations");
    }

    SourceLocation loc = ts_.currentLoc();

    if (!ts_.checkAny({TokenType::LET, TokenType::CONST})) {
        errorAt(DiagCode::E2002, "expected 'let' or 'const'");
        return nullptr;
    }
    Token kwTok = ts_.advance();
    DeclKeyword kw = (kwTok.type == TokenType::LET) ? DeclKeyword::Let : DeclKeyword::Const;

    std::vector<std::pair<InternedString, TypePtr>> vars;

    // Parse first variable
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected variable name");
        return nullptr;
    }
    std::string firstName = ts_.advance().value;
    if (!looksLikeType()) {
        errorAt(DiagCode::E2005, "expected type annotation for '" + firstName + "'");
        return nullptr;
    }
    TypePtr firstType = parseType();
    if (!firstType) return nullptr;
    vars.emplace_back(pool_.intern(firstName), std::move(firstType));

    // Parse additional variables
    while (ts_.check(TokenType::COMMA)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected variable name after comma");
            break;
        }
        std::string name = ts_.advance().value;
        if (!looksLikeType()) {
            errorAt(DiagCode::E2005, "expected type annotation for '" + name + "'");
            break;
        }
        TypePtr type = parseType();
        if (!type) break;
        vars.emplace_back(pool_.intern(name), std::move(type));
    }

    ts_.consume(TokenType::ASSIGN, DiagCode::E2001, "expected '=' in multi-assignment");
    ExprPtr rhs = parseExpr();
    if (!rhs) {
        errorAt(DiagCode::E2008, "expected expression after '='");
        return nullptr;
    }

    auto node = arena_.make<MultiVarDeclAST>();
    node->loc = loc;
    node->keyword = kw;
    
    // Build vars span
    auto builder = arena_.makeBuilder<std::pair<InternedString, TypePtr>>();
    for (auto& v : vars) builder.push_back(std::move(v));
    node->vars = builder.build();
    
    node->rhs = std::move(rhs);
    
    return node;
}

// ============================================================================
// Multi‑Assignment (Reassignment)
// ============================================================================
// 
// parseMultiAssignStmt() parses assignment to multiple existing variables.
// 
// Grammar: lvalue `,` lvalue `=` expr
// 
// Example: `a, b = f()`, `arr[i], obj.field = g()`
// 
// ─── Lvalue Rules ──────────────────────────────────────────────────────────
//   - Each lvalue must be assignable: variable, field access, or array index
//   - Function calls, literals, and method calls are NOT lvalues
//   - Assignment to `const` variables is a semantic error
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at first identifier (detected by lookahead)
// On exit:  positioned after the RHS expression
// 
// ─── Loop Safety ──────────────────────────────────────────────────────────
// Uses saved position pattern when parsing additional lvalues after commas.
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
//   - Missing lvalue after comma: breaks loop, returns nullptr
//   - Missing '=': skips tokens until semicolon/brace, returns nullptr
//   - Missing RHS: reports error, returns nullptr
// ============================================================================

ASTPtr<MultiAssignStmtAST> Parser::parseMultiAssignStmt() {
    SourceLocation loc = ts_.currentLoc();
    std::vector<ExprPtr> lhs;

    ExprPtr first = parseLvalue();
    if (!first) {
        errorAt(DiagCode::E2008, "expected left-hand side expression");
        return nullptr;
    }
    lhs.push_back(std::move(first));

    while (ts_.check(TokenType::COMMA)) {
        ts_.advance();
        ExprPtr next = parseLvalue();
        if (!next) {
            errorAt(DiagCode::E2008, "expected left-hand side expression after comma");
            break;
        }
        lhs.push_back(std::move(next));
    }

    if (!ts_.check(TokenType::ASSIGN)) {
        errorAt(DiagCode::E2001, "expected '=' in multiple assignment");
        while (!ts_.isAtEnd() && !ts_.check(TokenType::SEMICOLON) && !ts_.check(TokenType::RBRACE)) {
            ts_.advance();
        }
        return nullptr;
    }
    ts_.advance();

    ExprPtr rhs = parseExpr(true);
    if (!rhs) {
        errorAt(DiagCode::E2008, "expected expression after '='");
        return nullptr;
    }

    auto node = arena_.make<MultiAssignStmtAST>();
    node->loc = loc;
    
    // Build lhs span
    auto builder = arena_.makeBuilder<ExprPtr>();
    for (auto& e : lhs) builder.push_back(std::move(e));
    node->lhs = builder.build();
    
    node->rhs = std::move(rhs);
    return node;
}

// ============================================================================
// Block Statement
// ============================================================================
// 
// parseBlock() parses a brace‑delimited sequence of statements.
// 
// Grammar: `{` stmt* `}`
// 
// Example:
//   {
//       let x int = 5
//       io.printl(x)
//   }
// 
// ─── Scoping ────────────────────────────────────────────────────────────────
//   - Every block opens a new lexical scope (semantic pass)
//   - Names declared inside are not visible outside
//   - Function bodies, if/else branches, and loop bodies are blocks
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at '{'
// On exit:  positioned after the closing '}'
// 
// ─── Loop Safety ──────────────────────────────────────────────────────────
// Uses saved position pattern. If parseStmt() makes no progress:
//   - Consumes one token (advance)
//   - Skips any following semicolons
//   - Continues (does NOT push a statement)
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
//   - Missing '{': reports error, returns empty block (caller handles)
//   - Missing '}': consume() reports error and recovers
// ============================================================================

ASTPtr<BlockStmtAST> Parser::parseBlock() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LBRACE, "expected '{'");

    std::vector<StmtPtr> stmts;
    
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        if (ts_.match(TokenType::SEMICOLON)) continue;

        size_t savedPos = ts_.getPos();
        StmtPtr stmt = parseStmt();

        if (ts_.getPos() == savedPos) {
            if (!ts_.isAtEnd()) ts_.advance();
            while (ts_.match(TokenType::SEMICOLON)) {}
            continue;
        }

        if (stmt) stmts.push_back(std::move(stmt));
    }

    ts_.consume(TokenType::RBRACE, "expected '}' to close block");
    
    auto block = arena_.make<BlockStmtAST>();
    block->loc = loc;
    
    auto builder = arena_.makeBuilder<StmtPtr>();
    for (auto& s : stmts) builder.push_back(std::move(s));
    block->stmts = builder.build();
    
    return block;
}

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
        errorAt(DiagCode::E2008, "expected condition after 'if'");
        auto node = arena_.make<IfStmtAST>();
        node->loc = loc;
        return node;
    }

    if (!ts_.check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' after if condition");
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
            errorAt(DiagCode::E2001, "expected 'if' or '{' after 'else'");
        }
    }

    return node;
}

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
        errorAt(DiagCode::E2008, "expected expression after 'switch'");
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
                errorAt(DiagCode::E2007, "duplicate 'default' clause");
            }
            node->defaultLoc = ts_.currentLoc();
            ts_.advance();
            ts_.consume(TokenType::COLON, "expected ':' after 'default'");
            if (!ts_.check(TokenType::LBRACE)) {
                errorAt(DiagCode::E2001, "expected '{' to start default body");
            } else {
                node->defaultBody = parseBlock();
            }
            hasDefault = true;
            continue;
        }

        errorAt(DiagCode::E2002, "expected 'case' or 'default' inside switch");
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
        errorAt(DiagCode::E2001, "expected case value before ':'");
    } else {
        size_t savedPos = ts_.getPos();
        ExprPtr val = parsePrattExpr(0, false);
        if (ts_.getPos() == savedPos) {
            errorAt(DiagCode::E2002, "expected case value");
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
            errorAt(DiagCode::E2002, "expected case value after comma");
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
        errorAt(DiagCode::E2002, "too many errors in case values; skipping to ':'");
        while (!ts_.isAtEnd() && !ts_.check(TokenType::COLON)) ts_.advance();
    }

    ts_.consume(TokenType::COLON, "expected ':' after case values");

    // Build values span
    auto builder = arena_.makeBuilder<ExprPtr>();
    for (auto& v : values) builder.push_back(std::move(v));
    sc->values = builder.build();

    if (!ts_.check(TokenType::LBRACE)) {
        errorAt(DiagCode::E2001, "expected '{' to start case body");
    } else {
        sc->body = parseBlock();
    }

    return sc;
}

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
        error(loc, DiagCode::E2006, "'return' is not valid inside a 'parallel' body");
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
                    errorAt(DiagCode::E2008, "empty expression in return list");
                    ts_.advance();
                    continue;
                }
                if (ts_.check(TokenType::RBRACE) || ts_.check(TokenType::SEMICOLON) || ts_.isAtEnd()) {
                    errorAt(DiagCode::E2001, "trailing comma in return list");
                    break;
                }
            }
            first = false;

            size_t savedPos = ts_.getPos();
            ExprPtr expr = parseExpr();
            if (ts_.getPos() == savedPos) {
                errorAt(DiagCode::E2008, "expected expression after 'return'");
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
//   - Reports error with diagnostic code E3031 (outside loop) or E2006
//     (inside parallel). Node is still created to avoid nullptr.
// ============================================================================

ASTPtr<BreakStmtAST> Parser::parseBreakStmt() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::BREAK, "expected 'break'");

    if (loopDepth_ == 0) {
        error(loc, DiagCode::E2006, "'break' is only valid inside a loop body");
    }
    if (parallelDepth_ > 0) {
        error(loc, DiagCode::E2006, "'break' is not valid inside a 'parallel' body");
    }

    auto node = arena_.make<BreakStmtAST>();
    node->loc = loc;
    return node;
}

ASTPtr<ContinueStmtAST> Parser::parseContinueStmt() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::CONTINUE, "expected 'continue'");

    if (loopDepth_ == 0) {
        error(loc, DiagCode::E2006, "'continue' is only valid inside a loop body");
    }
    if (parallelDepth_ > 0) {
        error(loc, DiagCode::E2006, "'continue' is not valid inside a 'parallel' body");
    }

    auto node = arena_.make<ContinueStmtAST>();
    node->loc = loc;
    return node;
}