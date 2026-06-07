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
 * On entry: positioned at 'package' keyword
 * On exit:  positioned after the package name
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing package name: returns dummy node with "<error>" name
 * - Missing 'package' keyword: handled by caller (parse())
 */
PackageDeclPtr Parser::parsePackageDecl() {
    LUC_LOG_DECL_VERBOSE("parsePackageDecl: entering");
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::PACKAGE, "expected 'package'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_DECL("parsePackageDecl: ERROR - expected package name");
        errorAt(DiagCode::E1003, "expected package name");
        auto node = arena_.make<PackageDeclAST>(pool_.intern("<error>"));
        node->loc = loc;
        return node;
    }
    
    InternedString name = pool_.intern(ts_.advance().value);
    LUC_LOG_DECL_VERBOSE("parsePackageDecl: package name = " << pool_.lookup(name));
    
    auto node = arena_.make<PackageDeclAST>(name);
    node->loc = loc;
    return node;
}