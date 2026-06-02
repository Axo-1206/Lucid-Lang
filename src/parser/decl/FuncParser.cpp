#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

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
 * ─── Parameter Groups ──────────────────────────────────────────────────────
 *   - Function may have multiple `(params)` groups (currying)
 *   - Parameters are flattened into `allParams` with `groupSizes` tracking
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing '(' after name: reports error, returns nullptr
 * - Missing body after '=': reports error, returns nullptr
 * - Invalid parameter group: skip group, continue parsing
 */
ASTPtr<FuncDeclAST> Parser::parseFuncDecl(DeclKeyword kw, Visibility vis) {
    SourceLocation loc = ts_.currentLoc();

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E1003, "expected function name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    
    auto node = arena_.make<FuncDeclAST>();
    node->loc = loc;
    node->keyword = kw;
    node->name = name;
    node->visibility = vis;

    if (ts_.check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    // Create a FuncTypeAST to hold signature and qualifiers
    auto funcType = arena_.make<FuncTypeAST>();
    funcType->loc = loc;

    // Parse raw qualifiers and build bitmask
    std::vector<InternedString> rawQuals;
    uint32_t qualMask = 0;
    while (ts_.check(TokenType::TILDE)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E1003, "expected qualifier name after '~'");
            break;
        }
        InternedString q = pool_.intern(ts_.advance().value);
        rawQuals.push_back(q);
        // Set bit based on qualifier name (semantic pass will validate)
        if (pool_.lookup(q) == "async") qualMask |= QualifierBits::Async;
        else if (pool_.lookup(q) == "nullable") qualMask |= QualifierBits::Nullable;
        else if (pool_.lookup(q) == "parallel") qualMask |= QualifierBits::Parallel;
        // Other qualifiers are ignored here; semantic pass reports errors
    }
    auto qBuilder = arena_.makeBuilder<InternedString>();
    for (auto& q : rawQuals) qBuilder.push_back(std::move(q));
    funcType->rawQualifiers = qBuilder.build();
    funcType->qualifiers = qualMask;

    // Parameter groups: accumulate flat
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    if (!ts_.check(TokenType::LPAREN)) {
        errorAt(DiagCode::E1001, "expected '(' to start parameter list for function '" + std::string(pool_.lookup(name)) + "'");
        return nullptr;
    }
    while (ts_.check(TokenType::LPAREN)) {
        std::vector<ParamPtr> group = parseParamGroup();
        groupSizes.push_back(group.size());
        for (auto& p : group) {
            allParams.push_back(std::move(p));
        }
    }
    auto paramsBuilder = arena_.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramsBuilder.push_back(std::move(p));
    funcType->sig.allParams = paramsBuilder.build();

    auto gsBuilder = arena_.makeBuilder<size_t>();
    for (auto& sz : groupSizes) gsBuilder.push_back(sz);
    funcType->sig.groupSizes = gsBuilder.build();

    // Return types
    if (ts_.match(TokenType::ARROW)) {
        funcType->sig.returnTypes = parseReturnList();
    }

    // Store the complete function type in the declaration
    node->funcType = std::move(funcType);

    // Body (unchanged)
    if (!ts_.check(TokenType::ASSIGN)) {
        ts_.match(TokenType::SEMICOLON);
        return node;
    }

    ts_.advance();

    if (ts_.check(TokenType::LBRACE)) {
        node->body = parseBlock();
    } else if (ts_.check(TokenType::LPAREN)) {
        if (!ts_.check(TokenType::LBRACE)) {
            errorAt(DiagCode::E1001, "expected '{' to start function body");
            return nullptr;
        }
        node->body = parseBlock();
    } else {
        SourceLocation bodyLoc = ts_.currentLoc();
        ExprPtr expr = parseExpr();
        if (!expr) {
            errorAt(DiagCode::E1008, "expected expression after '='");
            return nullptr;
        }

        auto ret = arena_.make<ReturnStmtAST>();
        ret->loc = bodyLoc;
        std::vector<ExprPtr> vals;
        vals.push_back(std::move(expr));
        auto valsBuilder = arena_.makeBuilder<ExprPtr>();
        for (auto& v : vals) valsBuilder.push_back(std::move(v));
        ret->values = valsBuilder.build();

        auto block = arena_.make<BlockStmtAST>();
        block->loc = bodyLoc;
        std::vector<StmtPtr> stmts;
        stmts.push_back(std::move(ret));
        auto stmtsBuilder = arena_.makeBuilder<StmtPtr>();
        for (auto& s : stmts) stmtsBuilder.push_back(std::move(s));
        block->stmts = stmtsBuilder.build();

        node->body = std::move(block);
    }
    
    ts_.match(TokenType::SEMICOLON);
    return node;
}