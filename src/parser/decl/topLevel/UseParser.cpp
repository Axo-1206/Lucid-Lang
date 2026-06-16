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
 * ─── Precondition ─────────────────────────────────────────────────────────
 * Caller MUST have already verified that the current token is USE.
 * This function assumes it is positioned at the 'use' keyword.
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
 * - Missing 'use' keyword: returns nullptr (caller error)
 * - Missing module path: reports error, returns nullptr
 * - Missing alias after 'as': reports error, continues (returns node without alias)
 * 
 * @return UseDeclAST* on success, nullptr on error
 */
UseDeclPtr Parser::parseUseDecl(Visibility vis) {
    LOG_DECL_VERBOSE("parseUseDecl: entering");
    
    // Harvest doc comments attached to this use declaration
    auto doc = harvestDocComment();
    
    SourceLocation loc = ts_.currentLoc();
    
    // Check for 'use' keyword (should be present if called correctly)
    if (!ts_.check(TokenType::USE)) {
        LOG_DECL("parseUseDecl: ERROR - expected 'use' keyword");
        errorAt(DiagCode::E1001, "use", ts_.peek().value); // Expected keyword
        return nullptr;
    }
    ts_.advance(); // Consume 'use' keyword

    // Parse module path (dotted identifiers)
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LOG_DECL("parseUseDecl: ERROR - expected module path after 'use'");
        errorAt(DiagCode::E1102, ts_.peek().value);
        return nullptr;
    }
    
    // Parse use path (dotted identifiers)
    std::vector<InternedString> path;
    bool pathError = false;
    
    // Parse first segment
    path.push_back(pool_.intern(ts_.advance().value));
    LOG_DECL_EXTREME("parseUseDecl: path segment 1 = " << pool_.lookup(path.back()));
    
    // Parse additional segments after dots
    while (ts_.match(TokenType::DOT)) {
        if (!ts_.check(TokenType::IDENTIFIER)) {
            LOG_DECL("parseUseDecl: ERROR - expected identifier after '.'");
            errorAt(DiagCode::E1002);
            pathError = true;
            break;
        }
        path.push_back(pool_.intern(ts_.advance().value));
        LOG_DECL_EXTREME("parseUseDecl: path segment " << path.size() 
                             << " = " << pool_.lookup(path.back()));
    }
    
    // If there was an error in path parsing, abort
    if (pathError) {
        LOG_DECL_VERBOSE("parseUseDecl: aborted");
        return nullptr;
    }
    
    LOG_DECL_EXTREME("parseUseDecl: path has " << path.size() << " segment(s)");
    
    // Create the AST node
    auto* node = arena_.make<UseDeclAST>();
    node->loc = loc;
    node->visibility = vis;
    
    // Build the path span
    auto builder = arena_.makeBuilder<InternedString>();
    for (auto& p : path) builder.push_back(p);
    node->path = builder.build();
    
    // Parse optional alias
    if (ts_.match(TokenType::AS)) {
        LOG_DECL_EXTREME("parseUseDecl: parsing alias");
        if (!ts_.check(TokenType::IDENTIFIER)) {
            LOG_DECL("parseUseDecl: ERROR - expected alias name after 'as'");
            errorAt(DiagCode::E1103, ts_.peek().value);
            // Continue without alias (error already reported)
        } else {
            node->alias = pool_.intern(ts_.advance().value);
            LOG_DECL_EXTREME("parseUseDecl: alias = " << pool_.lookup(node->alias.value()));
        }
    }
    
    // Attach doc comment if found
    if (doc) {
        node->doc = std::move(doc);
        LOG_DECL_EXTREME("parseUseDecl: attached doc comment");
    }
    
    LOG_DECL_VERBOSE("parseUseDecl: success");
    return node;
}