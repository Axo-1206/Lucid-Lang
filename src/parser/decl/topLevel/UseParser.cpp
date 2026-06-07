#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

/**
 * @brief Parses `use` import declaration.
 * 
 * Grammar: `use` use_path [ `as` IDENTIFIER ]
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
UseDeclPtr Parser::parseUseDecl(Visibility vis) {
    LUC_LOG_DECL_VERBOSE("parseUseDecl: entering");
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::USE, "expected 'use'");

    auto node = arena_.make<UseDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_DECL("parseUseDecl: ERROR - expected module path after 'use'");
        errorAt(DiagCode::E1003, "expected module path after 'use'");
        return node;
    }

    // Parse use path (dotted identifiers) - inline implementation
    std::vector<InternedString> path;
    
    path.push_back(pool_.intern(ts_.advance().value));
    LUC_LOG_DECL_EXTREME("parseUseDecl: path segment 1 = " << pool_.lookup(path.back()));
    
    while (ts_.match(TokenType::DOT)) {
        if (!ts_.check(TokenType::IDENTIFIER)) {
            LUC_LOG_DECL("parseUseDecl: ERROR - expected identifier after '.'");
            errorAt(DiagCode::E1003, "expected identifier after '.'");
            break;
        }
        path.push_back(pool_.intern(ts_.advance().value));
        LUC_LOG_DECL_EXTREME("parseUseDecl: path segment " << path.size() 
                             << " = " << pool_.lookup(path.back()));
    }
    
    auto builder = arena_.makeBuilder<InternedString>();
    for (auto& p : path) builder.push_back(p);
    node->path = builder.build();
    
    LUC_LOG_DECL_EXTREME("parseUseDecl: path has " << path.size() << " segment(s)");

    if (ts_.match(TokenType::AS)) {
        LUC_LOG_DECL_EXTREME("parseUseDecl: parsing alias");
        if (!ts_.check(TokenType::IDENTIFIER)) {
            LUC_LOG_DECL("parseUseDecl: ERROR - expected alias name after 'as'");
            errorAt(DiagCode::E1003, "expected alias name after 'as'");
        } else {
            node->alias = pool_.intern(ts_.advance().value);
            LUC_LOG_DECL_EXTREME("parseUseDecl: alias = " << pool_.lookup(node->alias.value()));
        }
    }

    LUC_LOG_DECL_VERBOSE("parseUseDecl: success");
    return node;
}