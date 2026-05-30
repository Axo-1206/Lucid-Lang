#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

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
ASTPtr<PackageDeclAST> Parser::parsePackageDecl() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::PACKAGE, "expected 'package'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected package name");
        auto node = arena_.make<PackageDeclAST>(pool_.intern("<error>"));
        node->loc = loc;
        return node;
    }
    
    InternedString name = pool_.intern(ts_.advance().value);
    auto node = arena_.make<PackageDeclAST>(name);
    node->loc = loc;
    return node;
}