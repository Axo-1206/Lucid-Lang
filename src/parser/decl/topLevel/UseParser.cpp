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
 * ─── Note on Metadata ─────────────────────────────────────────────────────
 * Doc comments and attributes are handled by the dispatcher (parseDeclaration).
 * This function should NOT call harvestDocComment() or parseAttributes().
 * 
 * ─── Module Path Format ────────────────────────────────────────────────────
 *   - Dotted identifiers: `std.io`, `renderer.core.math`
 *   - Minimum one identifier
 * 
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 * Uses saved position pattern when parsing path segments. If a segment parse
 * makes no progress, consumes one token and continues (prevents infinite loop).
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing 'use' keyword: returns nullptr (caller error)
 * - Missing module path: reports error, returns nullptr
 * - Missing identifier after '.': reports error, aborts path parsing
 * - Missing alias after 'as': reports error, continues (returns node without alias)
 * 
 * @return UseDeclAST* on success, nullptr on error
 */
UseDeclPtr Parser::parseUseDecl(Visibility vis) {
    LOG_DECL_VERBOSE("parseUseDecl: entering");
    
    SourceLocation loc = ts_.currentLoc();
    
    // Check for 'use' keyword (should be present if called correctly)
    if (!ts_.check(TokenType::USE)) {
        LOG_DECL("parseUseDecl: ERROR - expected 'use' keyword");
        errorAt(DiagCode::E1001, "use", ts_.peek().value);
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
    int consecutiveFailures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 5;
    
    // Parse first segment
    path.push_back(pool_.intern(ts_.advance().value));
    LOG_DECL_EXTREME("parseUseDecl: path segment 1 = " << pool_.lookup(path.back()));
    
    // Parse additional segments after dots with loop safety
    while (ts_.match(TokenType::DOT) && !pathError) {
        size_t savedPos = ts_.getPos();
        
        if (!ts_.check(TokenType::IDENTIFIER)) {
            LOG_DECL("parseUseDecl: ERROR - expected identifier after '.'");
            errorAt(DiagCode::E1002, "path name", ts_.peek().value);
            pathError = true;
            break;
        }
        
        path.push_back(pool_.intern(ts_.advance().value));
        LOG_DECL_EXTREME("parseUseDecl: path segment " << path.size() 
                         << " = " << pool_.lookup(path.back()));
        consecutiveFailures = 0;
        
        // Check for progress
        if (ts_.getPos() == savedPos && !ts_.isAtEnd()) {
            consecutiveFailures++;
            LOG_DECL("parseUseDecl: no progress parsing path segment (attempt " 
                     << consecutiveFailures << ")");
            
            // Force consumption to make progress
            if (consecutiveFailures > 3) {
                LOG_DECL("parseUseDecl: forcing token consumption to break stalemate");
                ts_.advance();
            }
        }
        
        // Safety: prevent infinite loops by aborting path parsing
        if (consecutiveFailures > MAX_CONSECUTIVE_FAILURES) {
            LOG_DECL("parseUseDecl: too many consecutive failures, aborting path parsing");
            pathError = true;
            break;
        }
    }
    
    // If there was an error in path parsing, abort
    if (pathError) {
        LOG_DECL_VERBOSE("parseUseDecl: aborted due to path error");
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
    
    // Parse optional alias with loop safety
    if (ts_.match(TokenType::AS)) {
        LOG_DECL_EXTREME("parseUseDecl: parsing alias");
        size_t savedPos = ts_.getPos();
        
        if (!ts_.check(TokenType::IDENTIFIER)) {
            LOG_DECL("parseUseDecl: ERROR - expected alias name after 'as'");
            errorAt(DiagCode::E1002, "alias name", ts_.peek().value);
            // Continue without alias (error already reported)
        } else {
            node->alias = pool_.intern(ts_.advance().value);
            LOG_DECL_EXTREME("parseUseDecl: alias = " << pool_.lookup(node->alias.value()));
        }
        
        // Check for progress
        if (ts_.getPos() == savedPos && !ts_.isAtEnd()) {
            LOG_DECL("parseUseDecl: no progress parsing alias, forcing token consumption");
            ts_.advance();
        }
    }
    
    LOG_DECL_VERBOSE("parseUseDecl: success");
    return node;
}