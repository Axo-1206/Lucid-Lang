#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

/**
 * @brief Parses a variable declaration (`let` or `const`).
 * 
 * Grammar: `let` IDENTIFIER type_ann [ `=` expr ]
 * 
 * Example: `let x int = 42`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at variable name (keyword already consumed)
 * On exit:  positioned after the optional initialiser
 * 
 * ─── Important Notes ───────────────────────────────────────────────────────
 * - Type annotation is REQUIRED (no type inference)
 * - `const` must have an initialiser (enforced by semantic pass)
 * - `@extern` variables have no initialiser (semantic pass enforces)
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing name: returns nullptr
 * - Missing type annotation: returns nullptr
 * - Missing expression after '=': reports error, node has null init
 */
ASTPtr<VarDeclAST> Parser::parseVarDecl(Visibility vis) {
    const Token& kwTok = ts_.peekAt(0);
    DeclKeyword kw = (kwTok.type == TokenType::LET) ? DeclKeyword::Let : DeclKeyword::Const;

    SourceLocation loc = ts_.currentLoc();

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E1003, "expected variable name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    if (!looksLikeType()) {
        errorAt(DiagCode::E1005, "expected type annotation for '" + std::string(pool_.lookup(name)) + "'");
        return nullptr;
    }

    TypePtr type = parseType();
    if (!type) {
        errorAt(DiagCode::E1005, "expected type for variable '" + std::string(pool_.lookup(name)) + "'");
        return nullptr;
    }

    ExprPtr init;
    if (ts_.match(TokenType::ASSIGN)) {
        init = parseExpr();
        if (!init) {
            errorAt(DiagCode::E1008, "expected expression after '=' in variable declaration");
        }
    }

    auto node = arena_.make<VarDeclAST>();
    node->loc = loc;
    node->keyword = kw;
    node->name = name;
    node->type = std::move(type);
    node->init = std::move(init);
    node->visibility = vis;

    return node;
}