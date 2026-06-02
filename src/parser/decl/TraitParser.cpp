#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

/**
 * @brief Parses a trait declaration (method contract, no implementations).
 * 
 * Grammar: `trait` IDENTIFIER [ `<` generic_params `>` ] `{` method* `}`
 * 
 * Example: `pub trait Drawable { draw (), bounds () -> Rect }`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'trait' keyword
 * On exit:  positioned after the closing '}'
 * 
 * ─── Important Notes ───────────────────────────────────────────────────────
 *   - Trait methods are signatures only (no body, no '=')
 *   - Traits are top‑level only (semantic pass rejects local traits)
 *   - The semantic pass verifies impl blocks provide all methods
 * 
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 * Uses saved position pattern with parseTraitMethod()
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing trait name: returns nullptr
 * - Missing '{' after name: reports error, returns nullptr
 * - Invalid method: skips method, continues
 * - Missing '}': consume() reports error
 */
ASTPtr<TraitDeclAST> Parser::parseTraitDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::TRAIT, "expected 'trait'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E1003, "expected trait name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    auto node = arena_.make<TraitDeclAST>();
    node->loc = loc;
    node->name = name;
    node->visibility = vis;

    if (ts_.check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    ts_.consume(TokenType::LBRACE, "expected '{' to open trait body");

    std::vector<TraitMethodPtr> methods;
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t savedPos = ts_.getPos();
        TraitMethodPtr method = parseTraitMethod();
        if (method) {
            methods.push_back(std::move(method));
        } else {
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) ts_.advance();
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::IDENTIFIER)) ts_.advance();
        }
    }

    auto builder = arena_.makeBuilder<TraitMethodPtr>();
    for (auto& m : methods) builder.push_back(std::move(m));
    node->methods = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close trait body");
    return node;
}

/**
 * @brief Parses a method signature inside a trait.
 * 
 * Grammar: IDENTIFIER [ `~async` | `~nullable` | `~parallel` ]*
 *          param_group+ [ `->` return_list ]
 * 
 * Example: `fetch ~async (url string) -> string`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at method name
 * On exit:  positioned after the return list (or after last param group)
 * 
 * ─── Important Notes ───────────────────────────────────────────────────────
 *   - No body, no '=' token
 *   - Qualifiers are stored raw; semantic phase resolves them
 *   - Supports curried methods (multiple parameter groups)
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing method name: returns nullptr
 * - Missing '(' after name/qualifiers: reports error, returns nullptr
 */
TraitMethodPtr Parser::parseTraitMethod() {
    SourceLocation loc = ts_.currentLoc();
    
    auto method = arena_.make<TraitMethodAST>();
    method->loc = loc;

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E1003, "expected trait method name");
        return nullptr;
    }
    method->name = pool_.intern(ts_.advance().value);
    
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
        if (pool_.lookup(q) == "async") qualMask |= QualifierBits::Async;
        else if (pool_.lookup(q) == "nullable") qualMask |= QualifierBits::Nullable;
        else if (pool_.lookup(q) == "parallel") qualMask |= QualifierBits::Parallel;
    }
    auto qBuilder = arena_.makeBuilder<InternedString>();
    for (auto& q : rawQuals) qBuilder.push_back(std::move(q));
    funcType->rawQualifiers = qBuilder.build();
    funcType->qualifiers = qualMask;

    // Parameter groups: flat accumulation
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    if (!ts_.check(TokenType::LPAREN)) {
        errorAt(DiagCode::E1001, "expected '(' for trait method parameters");
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

    method->funcType = std::move(funcType);
    return method;
}

/**
 * @brief Parses a trait reference in an impl conformance declaration.
 * 
 * Grammar: `:` IDENTIFIER [ `<` type_args `>` ]
 * 
 * Example: `: Drawable` or `: Comparable<int>`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at ':' token (already consumed? check caller)
 * On exit:  positioned after generic arguments (or after trait name)
 * 
 * ─── Important Notes ───────────────────────────────────────────────────────
 *   - Called after the ':' is already consumed
 *   - Generic arguments are optional
 *   - Multiple trait bounds (e.g., `: Drawable + Serializable`) NOT supported
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing trait name after ':': returns nullptr
 */
TraitRefPtr Parser::parseTraitRef() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::COLON, "expected ':' before trait name");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E1003, "expected trait name after ':'");
        return nullptr;
    }

    auto ref = arena_.make<TraitRefAST>();
    ref->loc = loc;
    ref->name = pool_.intern(ts_.advance().value);

    if (ts_.check(TokenType::LESS)) {
        ref->genericArgs = parseGenericArgs();
    }

    return ref;
}