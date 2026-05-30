#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// NOTE for local declaration look for parseDeclaration in Parser.cpp
// the function parseDeclaration will parse on context, here is local declaration

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