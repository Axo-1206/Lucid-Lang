#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

/**
 * @brief Parses a function declaration.
 * 
 * Grammar:
 *   `let`/`const` IDENTIFIER [ `<` generic_params `>` ]
 *   [ `~async` | `~nullable` | `~parallel` ]*
 *   param_group+ [ `->` return_list ] [ `=` body ]
 * 
 * Example: `let add ~async (a int)(b int) -> int = { return a + b }`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at function name (keyword already consumed)
 * On exit:  positioned after the body (or after the signature if no body)
 * 
 * ─── Body Parsing Variants ─────────────────────────────────────────────────
 *   1. Block body      : `= { ... }`
 *   2. Verbose anon-func: `= (params) -> ret { ... }`
 *   3. Expression body  : `= expr` (wrapped in ReturnStmt + BlockStmt)
 *   4. No body          : (valid for `@extern` declarations)
 * 
 * ─── Parameter Groups (Currying via Recursion) ────────────────────────────
 *   - Multiple parameter groups desugar to nested FuncTypeAST
 *   - Example: `(a int)(b int) -> int` becomes:
 *       FuncTypeAST(params=[a]) -> FuncTypeAST(params=[b]) -> returnTypes=[int]
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing '(' after name: reports error, returns nullptr
 * - Missing body after '=': reports error, returns nullptr
 * - Invalid parameter group: skip group, continue parsing
 */
FuncDeclPtr Parser::parseFuncDecl(DeclKeyword kw, Visibility vis) {
    LUC_LOG_DECL_VERBOSE("parseFuncDecl: entering, kw=" << (kw == DeclKeyword::Let ? "let" : "const"));
    LUC_LOG_DECL("parseFuncDecl: current token at entry = '" << ts_.peek().value 
                 << "' (type=" << static_cast<int>(ts_.peek().type) 
                 << ") at line " << ts_.peek().line << ", col " << ts_.peek().column);
    
    SourceLocation loc = ts_.currentLoc();

    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_DECL("parseFuncDecl: ERROR - expected function name");
        errorAt(DiagCode::E1003, "expected function name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    LUC_LOG_DECL_EXTREME("parseFuncDecl: function name = " << pool_.lookup(name));
    
    auto node = arena_.make<FuncDeclAST>();
    node->loc = loc;
    node->keyword = kw;
    node->name = name;
    node->visibility = vis;

    // Parse generic parameters (on the function itself, not on the type)
    if (ts_.check(TokenType::LESS)) {
        LUC_LOG_DECL_EXTREME("parseFuncDecl: parsing generic parameters");
        node->genericParams = parseGenericParams();
        LUC_LOG_DECL_EXTREME("parseFuncDecl: " << node->genericParams.size() << " generic parameter(s)");
    }

    // Parse the function type (qualifiers + parameter groups + return types)
    TypePtr funcType = parseFuncType();
    if (!funcType || funcType->isa<UnknownTypeAST>()) {
        LUC_LOG_DECL("parseFuncDecl: ERROR - invalid function signature");
        errorAt(DiagCode::E1005, "invalid function signature");
        return nullptr;
    }
    node->funcType = funcType->as<FuncTypeAST>();

    // Body
    if (!ts_.check(TokenType::ASSIGN)) {
        LUC_LOG_DECL_EXTREME("parseFuncDecl: no body (extern or forward declaration)");
        ts_.match(TokenType::SEMICOLON);
        return node;
    }

    LUC_LOG_DECL_EXTREME("parseFuncDecl: parsing body");
    ts_.advance(); // consume '='

    if (ts_.check(TokenType::LBRACE)) {
        LUC_LOG_DECL_EXTREME("parseFuncDecl: block body");
        node->body = parseBlock();
    } else if (ts_.check(TokenType::LPAREN)) {
        LUC_LOG_DECL_EXTREME("parseFuncDecl: verbose anon-func body");
        if (!ts_.check(TokenType::LBRACE)) {
            LUC_LOG_DECL("parseFuncDecl: ERROR - expected '{' to start function body");
            errorAt(DiagCode::E1001, "expected '{' to start function body");
            return nullptr;
        }
        node->body = parseBlock();
    } else {
        LUC_LOG_DECL_EXTREME("parseFuncDecl: expression body (will be wrapped)");
        SourceLocation bodyLoc = ts_.currentLoc();
        ExprPtr expr = parseExpr();
        if (!expr) {
            LUC_LOG_DECL("parseFuncDecl: ERROR - expected expression after '='");
            errorAt(DiagCode::E1008, "expected expression after '='");
            return nullptr;
        }

        auto ret = arena_.make<ReturnStmtAST>();
        ret->loc = bodyLoc;
        
        auto valsBuilder = arena_.makeBuilder<ExprPtr>();
        valsBuilder.push_back(expr);
        ret->values = valsBuilder.build();

        auto block = arena_.make<BlockStmtAST>();
        block->loc = bodyLoc;
        
        auto stmtsBuilder = arena_.makeBuilder<StmtPtr>();
        stmtsBuilder.push_back(ret);
        block->stmts = stmtsBuilder.build();

        node->body = block;
    }
    
    ts_.match(TokenType::SEMICOLON);
    LUC_LOG_DECL_VERBOSE("parseFuncDecl: success");
    return node;
}