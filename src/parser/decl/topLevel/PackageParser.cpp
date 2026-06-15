#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

/**
 * @brief Parses `package name` declaration.
 * 
 * Must be the first non‑comment line of every .luc file.
 * 
 * Grammar: `package` IDENTIFIER
 * 
 * Example: `package math`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at any token (checks for 'package' keyword)
 * On exit:  positioned after the package name (if successful)
 *           OR at recovery position (if error)
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing 'package' keyword: reports error, returns nullptr
 * - Missing package name: reports error, returns nullptr
 * - On any error: does NOT consume tokens beyond the error point
 * 
 * @return PackageDeclAST* on success, nullptr on error
 */
PackageDeclPtr Parser::parsePackageDecl() {
    LUC_LOG_DECL_VERBOSE("parsePackageDecl: entering");

    // Harvest doc comments attached to this package declaration
    auto doc = harvestDocComment();

    SourceLocation loc = ts_.currentLoc();
    
    // Check for 'package' keyword
    if (!ts_.check(TokenType::PACKAGE)) {
        LUC_LOG_DECL("parsePackageDecl: ERROR - expected 'package' keyword");
        errorAt(DiagCode::E1001, "package", ts_.peek().value);
        return nullptr;
    }
    
    // Consume 'package' keyword
    ts_.advance();

    // Parse package name (required)
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_DECL("parsePackageDecl: ERROR - expected package name");
        errorAt(DiagCode::E1003, "expected package name");
        return nullptr;
    }
    
    InternedString name = pool_.intern(ts_.advance().value);
    LUC_LOG_DECL_VERBOSE("parsePackageDecl: package name = " << pool_.lookup(name));
    
    auto* node = arena_.make<PackageDeclAST>(name);
    node->loc = loc;

        
    // Attach doc comment if found
    if (doc) {
        node->doc = std::move(doc);
        LUC_LOG_DECL_EXTREME("parsePackageDecl: attached doc comment to package");
    }
    
    return node;
}