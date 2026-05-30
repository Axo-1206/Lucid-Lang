#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

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