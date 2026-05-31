#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

/**
 * @brief Parses `use` import declaration.
 * 
 * Grammar: `use` module_path [ `as` IDENTIFIER ]
 * 
 * Example: `use math.vec2 as v`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'use' keyword
 * On exit:  positioned after the alias (or after the last path segment)
 * 
 * ─── Module Path Format ────────────────────────────────────────────────────
 *   - Dotted identifiers: `std.io`, `renderer.core.math`
 *   - Minimum one identifier
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing module path: returns node with empty path
 * - Missing alias after 'as': reports error, continues
 */
ASTPtr<UseDeclAST> Parser::parseUseDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::USE, "expected 'use'");

    auto node = arena_.make<UseDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected module path after 'use'");
        return node;
    }

    std::vector<InternedString> path;
    path.push_back(pool_.intern(ts_.advance().value));

    while (ts_.check(TokenType::DOT) && ts_.peekNextType() == TokenType::IDENTIFIER) {
        ts_.advance();
        path.push_back(pool_.intern(ts_.advance().value));
    }

    auto builder = arena_.makeBuilder<InternedString>();
    for (auto& p : path) builder.push_back(std::move(p));
    node->path = builder.build();

    if (ts_.match(TokenType::AS)) {
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected alias name after 'as'");
        } else {
            node->alias = pool_.intern(ts_.advance().value);
        }
    }

    return node;
}