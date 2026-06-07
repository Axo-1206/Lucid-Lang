#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

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

BlockStmtPtr Parser::parseBlock() {
    LUC_LOG_STMT_VERBOSE("parseBlock: entering");
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LBRACE, "expected '{'");

    std::vector<StmtPtr> stmts;
    int stmtCount = 0;
    
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        if (ts_.match(TokenType::SEMICOLON)) continue;

        size_t savedPos = ts_.getPos();
        StmtPtr stmt = parseStmt();

        if (ts_.getPos() == savedPos) {
            LUC_LOG_STMT("parseBlock: WARNING - no progress in parseStmt, advancing");
            if (!ts_.isAtEnd()) ts_.advance();
            while (ts_.match(TokenType::SEMICOLON)) {}
            continue;
        }

        if (stmt) {
            stmtCount++;
            LUC_LOG_STMT_EXTREME("parseBlock: parsed statement #" << stmtCount 
                                 << " (" << LucDebug::kindToString(stmt->kind) << ")");
            stmts.push_back(stmt);
        }
    }

    ts_.consume(TokenType::RBRACE, "expected '}' to close block");
    
    auto block = arena_.make<BlockStmtAST>();
    block->loc = loc;
    
    auto builder = arena_.makeBuilder<StmtPtr>();
    for (auto& s : stmts) builder.push_back(s);
    block->stmts = builder.build();
    
    LUC_LOG_STMT_VERBOSE("parseBlock: parsed block with " << stmtCount << " statements");
    return block;
}