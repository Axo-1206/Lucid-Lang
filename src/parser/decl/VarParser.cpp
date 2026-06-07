#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

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
VarDeclPtr Parser::parseVarDecl(Visibility vis) {
    const Token& kwTok = ts_.peekAt(0);
    DeclKeyword kw = (kwTok.type == TokenType::LET) ? DeclKeyword::Let : DeclKeyword::Const;
    LUC_LOG_DECL_VERBOSE("parseVarDecl: entering at line " << ts_.currentLoc().line()
                         << ", col " << ts_.currentLoc().column()
                         << ", kw=" << (kw == DeclKeyword::Let ? "let" : "const"));
    
    SourceLocation loc = ts_.currentLoc();

    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_DECL("parseVarDecl: ERROR - expected variable name");
        errorAt(DiagCode::E1003, "expected variable name");
        return nullptr;
    }
    
    Token nameToken = ts_.advance();
    InternedString name = pool_.intern(nameToken.value);
    LUC_LOG_DECL_EXTREME("parseVarDecl: variable name = '" << pool_.lookup(name) 
                         << "' at line " << nameToken.line << ", col " << nameToken.column);
    
    if (!looksLikeType()) {
        LUC_LOG_DECL("parseVarDecl: ERROR - expected type annotation");
        errorAt(DiagCode::E1005, "expected type annotation for variable '" 
                + std::string(pool_.lookup(name)) + "'");
        return nullptr;
    }

    TypePtr type = parseType();
    if (!type) {
        LUC_LOG_DECL("parseVarDecl: ERROR - expected type for variable");
        errorAt(DiagCode::E1005, "expected type for variable '" 
                + std::string(pool_.lookup(name)) + "'");
        return nullptr;
    }

    ExprPtr init = nullptr;
    if (ts_.match(TokenType::ASSIGN)) {
        LUC_LOG_DECL_EXTREME("parseVarDecl: parsing initializer");
        init = parseExpr();
        if (!init) {
            LUC_LOG_DECL("parseVarDecl: ERROR - expected expression after '='");
            errorAt(DiagCode::E1008, "expected expression after '=' in variable initializer");
            return nullptr;
        }
        LUC_LOG_DECL_EXTREME("parseVarDecl: initializer parsed");
    } else {
        LUC_LOG_DECL_EXTREME("parseVarDecl: no initializer");
    }

    auto node = arena_.make<VarDeclAST>();
    node->loc = loc;
    node->keyword = kw;
    node->name = name;
    node->type = type;
    node->init = init;
    node->visibility = vis;

    LUC_LOG_DECL_VERBOSE("parseVarDecl: success");
    return node;
}