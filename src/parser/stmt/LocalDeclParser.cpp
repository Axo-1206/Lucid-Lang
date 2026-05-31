/**
 * @file LocalDeclParser.cpp
 * @brief Local declarations and assignments inside blocks.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements parsing of declarations that appear inside block scopes
 * (not at top level). These include:
 *   - Multi‑variable declarations (`let a int, b string = f()`)
 *   - Multi‑assignments (`a, b = g()`)
 *   - Lvalue parsing for assignable expressions
 * 
 * ## Relationship with parseDeclaration
 * 
 * Local declarations are parsed by calling `parseDeclaration(DeclContext::Local)`
 * from `parseStmt`. That function handles visibility enforcement (pub/export
 * are rejected) and dispatches to specific declaration parsers (struct, enum,
 * type, impl, from, var, func). The result is wrapped in `DeclStmtAST`.
 * 
 * This file contains the remaining local constructs that are not handled by
 * the generic `parseDeclaration` dispatch:
 *   - Multi‑variable declarations (multiple `let`/`const` in one statement)
 *   - Multi‑assignments (reassignment to multiple existing variables)
 *   - Lvalue parsing (shared by both)
 * 
 * @see Parser.cpp::parseDeclaration() for generic declaration dispatch
 * @see ParserStmt.cpp::parseStmt() for statement dispatch
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// 1. LVALUE PARSING
// ============================================================================
//
// parseLvalue() parses an assignable left‑hand side expression for multi‑assignment
// and compound assignment operators.
//
// Grammar: IDENTIFIER { ( '.' IDENTIFIER ) | ( '[' expr ']' ) }
//
// Examples: x, point.x, arr[i], matrix[row][col]
//
// This is distinct from parseExpr() because it:
//   - Stops before operators like '=' (no operator parsing)
//   - Does not allow behavior access (':') or function calls
//   - Only allows valid lvalue forms
// ============================================================================

/**
 * @brief Parses an assignable left‑hand side expression (lvalue).
 *
 * Used for multi-assignment and compound assignment operators.
 *
 * @return ExprPtr – the parsed lvalue expression, or nullptr on error.
 *
 * ─── Valid Lvalue Forms ────────────────────────────────────────────────────
 *   - Plain identifier: `x`
 *   - Field access:     `point.x`
 *   - Array element:    `arr[i]`
 *   - Nested chains:    `matrix[row][col]`, `obj.field[index]`
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at first identifier
 * On exit:  positioned after the complete lvalue (before '=')
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing identifier after '.': reports error, returns partially parsed expr
 * - Missing index expression: reports error, returns partially parsed expr
 * - Missing ']' after index: consume() reports error
 */
ExprPtr Parser::parseLvalue() {
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected identifier for lvalue");
        return nullptr;
    }
    std::string name = ts_.advance().value;
    ExprPtr expr = arena_.make<IdentifierExprAST>(pool_.intern(name));
    expr->loc = ts_.currentLoc();

    while (true) {
        // Field access: .field
        if (ts_.check(TokenType::DOT)) {
            ts_.advance();
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected field name after '.'");
                return expr;
            }
            std::string field = ts_.advance().value;
            auto node = arena_.make<FieldAccessExprAST>();
            node->loc = expr->loc;
            node->object = std::move(expr);
            node->field = pool_.intern(field);
            expr = std::move(node);
        }
        // Array index: [index]
        else if (ts_.check(TokenType::LBRACKET)) {
            ts_.advance();
            ExprPtr index = parseExpr();
            if (!index) {
                errorAt(DiagCode::E2008, "expected index expression");
                return expr;
            }
            ts_.consume(TokenType::RBRACKET, "expected ']' after index");
            auto node = arena_.make<IndexExprAST>();
            node->loc = expr->loc;
            node->target = std::move(expr);
            node->index = std::move(index);
            node->kind = IndexKind::Element;
            expr = std::move(node);
        }
        // Method call ':' – not an lvalue, stop parsing
        else if (ts_.check(TokenType::COLON)) {
            break;
        }
        else {
            break;
        }
    }
    return expr;
}

// ============================================================================
// 2. MULTI‑VARIABLE DECLARATION
// ============================================================================
//
// parseMultiVarDecl() parses `let` or `const` declarations with multiple
// variables in a single statement.
//
// Grammar: 'let' IDENTIFIER type { ',' IDENTIFIER type } '=' expr
//
// Example: `let q int, r int = divmod(10, 3)`
//          `const w int, h int = getScreenSize()`
//
// This form is distinct from single variable declarations (parseVarDecl)
// which are handled by the generic parseDeclaration dispatch.
// ============================================================================

/**
 * @brief Parses a multi‑variable declaration (`let` or `const` with multiple vars).
 *
 * Example: `let x int, y string = f()`
 *
 * @param attrs Attributes (NOT allowed – emits E2027 error).
 * @return ASTPtr<MultiVarDeclAST> – parsed node, or nullptr on error.
 *
 * ─── Important Rules ────────────────────────────────────────────────────────
 *   - Each variable has its own explicit type annotation (no type inference)
 *   - The RHS must be a single expression returning N values (N = variable count)
 *   - `const` multi‑declaration requires compile‑time constant RHS
 *   - Attributes are NOT allowed (E2027)
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'let' or 'const' keyword
 * On exit:  positioned after the RHS expression
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - Missing '=': reports error, returns nullptr
 *   - Missing RHS: reports error, returns nullptr
 *   - Invalid variable spec: breaks loop, continues with already parsed vars
 */
ASTPtr<MultiVarDeclAST> Parser::parseMultiVarDecl(std::vector<AttributePtr> attrs) {
    // Attributes are not allowed on multi-variable declarations
    if (!attrs.empty()) {
        error(attrs[0]->loc, DiagCode::E2027, 
              "attributes cannot be used on multi-variable declarations");
    }

    SourceLocation loc = ts_.currentLoc();

    if (!ts_.checkAny({TokenType::LET, TokenType::CONST})) {
        errorAt(DiagCode::E2002, "expected 'let' or 'const'");
        return nullptr;
    }
    Token kwTok = ts_.advance();
    DeclKeyword kw = (kwTok.type == TokenType::LET) ? DeclKeyword::Let : DeclKeyword::Const;

    std::vector<std::pair<InternedString, TypePtr>> vars;

    // Parse first variable: IDENTIFIER type
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

    // Parse additional variables: , IDENTIFIER type
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

    // Parse '=' and RHS
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
// 3. MULTI‑ASSIGNMENT (REASSIGNMENT)
// ============================================================================
//
// parseMultiAssignStmt() parses assignment to multiple existing variables.
//
// Grammar: lvalue { ',' lvalue } '=' expr
//
// Example: `a, b = f()`, `arr[i], obj.field = g()`
//
// This is distinct from multi‑variable declaration because there is no
// `let`/`const` keyword – the variables already exist.
// ============================================================================

/**
 * @brief Parses a multi‑assignment statement (reassignment to existing variables).
 *
 * Example: `a, b = f()`, `arr[i], obj.field = g()`
 *
 * @return ASTPtr<MultiAssignStmtAST> – parsed node, or nullptr on error.
 *
 * ─── Lvalue Rules ──────────────────────────────────────────────────────────
 *   - Each lvalue must be assignable: variable, field access, or array index
 *   - Function calls, literals, and method calls are NOT lvalues
 *   - Assignment to `const` variables is a semantic error (not parser)
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at first identifier (detected by lookahead)
 * On exit:  positioned after the RHS expression
 *
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 * Uses saved position pattern when parsing additional lvalues after commas.
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - Missing lvalue after comma: breaks loop, returns nullptr
 *   - Missing '=': skips tokens until semicolon/brace, returns nullptr
 *   - Missing RHS: reports error, returns nullptr
 */
ASTPtr<MultiAssignStmtAST> Parser::parseMultiAssignStmt() {
    SourceLocation loc = ts_.currentLoc();
    std::vector<ExprPtr> lhs;

    // Parse first lvalue
    ExprPtr first = parseLvalue();
    if (!first) {
        errorAt(DiagCode::E2008, "expected left-hand side expression");
        return nullptr;
    }
    lhs.push_back(std::move(first));

    // Parse additional lvalues after commas
    while (ts_.check(TokenType::COMMA)) {
        ts_.advance();
        ExprPtr next = parseLvalue();
        if (!next) {
            errorAt(DiagCode::E2008, "expected left-hand side expression after comma");
            break;
        }
        lhs.push_back(std::move(next));
    }

    // Expect '='
    if (!ts_.check(TokenType::ASSIGN)) {
        errorAt(DiagCode::E2001, "expected '=' in multiple assignment");
        // Skip to recovery point
        while (!ts_.isAtEnd() && !ts_.check(TokenType::SEMICOLON) && !ts_.check(TokenType::RBRACE)) {
            ts_.advance();
        }
        return nullptr;
    }
    ts_.advance();

    // Parse RHS
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