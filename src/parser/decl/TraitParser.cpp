#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

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
    LUC_LOG_DECL_VERBOSE("parseTraitDecl: entering");
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::TRAIT, "expected 'trait'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_DECL("parseTraitDecl: ERROR - expected trait name");
        errorAt(DiagCode::E1003, "expected trait name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    LUC_LOG_DECL_EXTREME("parseTraitDecl: trait name = " << pool_.lookup(name));

    auto node = arena_.make<TraitDeclAST>();
    node->loc = loc;
    node->name = name;
    node->visibility = vis;

    if (ts_.check(TokenType::LESS)) {
        LUC_LOG_DECL_EXTREME("parseTraitDecl: parsing generic parameters");
        node->genericParams = parseGenericParams();
        LUC_LOG_DECL_EXTREME("parseTraitDecl: " << node->genericParams.size() << " generic parameter(s)");
    }

    ts_.consume(TokenType::LBRACE, "expected '{' to open trait body");

    std::vector<TraitMethodPtr> methods;
    int methodCount = 0;
    
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        if (ts_.check(TokenType::RBRACE)) break;

        size_t savedPos = ts_.getPos();
        TraitMethodPtr method = parseTraitMethod();
        if (method) {
            methodCount++;
            LUC_LOG_DECL_EXTREME("parseTraitDecl: parsed method #" << methodCount);
            methods.push_back(std::move(method));
        } else {
            LUC_LOG_DECL("parseTraitDecl: ERROR - failed to parse trait method");
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) ts_.advance();
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::IDENTIFIER)) ts_.advance();
        }
    }

    auto builder = arena_.makeBuilder<TraitMethodPtr>();
    for (auto& m : methods) builder.push_back(std::move(m));
    node->methods = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close trait body");
    
    LUC_LOG_DECL_VERBOSE("parseTraitDecl: parsed " << methodCount << " method(s)");
    return node;
}

// Add logging to parseTraitMethod and parseTraitRef as well
TraitMethodPtr Parser::parseTraitMethod() {
    LUC_LOG_DECL_EXTREME("parseTraitMethod: entering");
    SourceLocation loc = ts_.currentLoc();
    
    auto method = arena_.make<TraitMethodAST>();
    method->loc = loc;

    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_DECL("parseTraitMethod: ERROR - expected trait method name");
        errorAt(DiagCode::E1003, "expected trait method name");
        return nullptr;
    }
    method->name = pool_.intern(ts_.advance().value);
    LUC_LOG_DECL_EXTREME("parseTraitMethod: method name = " << pool_.lookup(method->name));
    
    // Create a FuncTypeAST to hold signature and qualifiers
    auto funcType = arena_.make<FuncTypeAST>();
    funcType->loc = loc;

    // Parse raw qualifiers and build bitmask
    std::vector<InternedString> rawQuals;
    uint32_t qualMask = 0;
    while (ts_.check(TokenType::TILDE)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            LUC_LOG_DECL("parseTraitMethod: ERROR - expected qualifier name after '~'");
            errorAt(DiagCode::E1003, "expected qualifier name after '~'");
            break;
        }
        InternedString q = pool_.intern(ts_.advance().value);
        rawQuals.push_back(q);
        std::string_view qstr = pool_.lookup(q);
        if (qstr == "async") qualMask |= QualifierBits::Async;
        else if (qstr == "nullable") qualMask |= QualifierBits::Nullable;
        else if (qstr == "parallel") qualMask |= QualifierBits::Parallel;
    }
    auto qBuilder = arena_.makeBuilder<InternedString>();
    for (auto& q : rawQuals) qBuilder.push_back(std::move(q));
    funcType->rawQualifiers = qBuilder.build();
    funcType->qualifiers = qualMask;

    // Parameter groups: flat accumulation
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    
    if (!ts_.check(TokenType::LPAREN)) {
        LUC_LOG_DECL("parseTraitMethod: ERROR - expected '(' for trait method parameters");
        errorAt(DiagCode::E1001, "expected '(' for trait method parameters");
        return nullptr;
    }
    
    int groupCount = 0;
    while (ts_.check(TokenType::LPAREN)) {
        groupCount++;
        ParamGroup group = parseParamGroup();
        groupSizes.push_back(group.size());
        LUC_LOG_DECL_EXTREME("parseTraitMethod: group #" << groupCount 
                             << " has " << group.size() << " parameter(s)");
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
    
    LUC_LOG_DECL_EXTREME("parseTraitMethod: total " << allParams.size() << " parameters");

    // Return types
    if (ts_.match(TokenType::ARROW)) {
        LUC_LOG_DECL_EXTREME("parseTraitMethod: parsing return types");
        funcType->sig.returnTypes = parseReturnList();
        LUC_LOG_DECL_EXTREME("parseTraitMethod: " << funcType->sig.returnTypes.size() << " return type(s)");
    }

    method->funcType = std::move(funcType);
    
    LUC_LOG_DECL_EXTREME("parseTraitMethod: success");
    return method;
}

TraitRefPtr Parser::parseTraitRef() {
    LUC_LOG_DECL_EXTREME("parseTraitRef: entering at line " << ts_.currentLoc().line()
                         << ", col " << ts_.currentLoc().column());
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::COLON, "expected ':' before trait name");

    LUC_LOG_DECL("parseTraitRef: after consuming ':', token is "
                 << LucDebug::tokenToString(ts_.peek())
                 << " at line " << ts_.peek().line << ", col " << ts_.peek().column);

    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_DECL("parseTraitRef: ERROR - expected trait name after ':' at line "
                     << ts_.peek().line << ", col " << ts_.peek().column);
        errorAt(DiagCode::E1003, "expected trait name after ':'");
        return nullptr;
    }

    auto ref = arena_.make<TraitRefAST>();
    ref->loc = loc;
    ref->name = pool_.intern(ts_.advance().value);
    LUC_LOG_DECL_EXTREME("parseTraitRef: trait name = " << pool_.lookup(ref->name));

    LUC_LOG_DECL("After reading trait name, next token: "
                 << LucDebug::tokenToString(ts_.peek())
                 << " at line " << ts_.peek().line << ", col " << ts_.peek().column);

    // Parse generic arguments if present
    if (ts_.check(TokenType::LESS)) {
        LUC_LOG_DECL_EXTREME("parseTraitRef: found '<' for generic arguments at line "
                             << ts_.peek().line << ", col " << ts_.peek().column);
        // ★ CONSUME the '<' token before calling parseGenericArgs()
        ts_.advance();   // consume '<'

        // parseGenericArgs() now sees the first type argument (or '>' for empty)
        ref->genericArgs = parseGenericArgs();

        LUC_LOG_DECL_EXTREME("parseTraitRef: parsed " << ref->genericArgs.size() << " generic argument(s)");
        LUC_LOG_DECL("After parsing generic args, at token: "
                     << LucDebug::tokenToString(ts_.peek())
                     << " line " << ts_.peek().line << ", col " << ts_.peek().column);
    } else {
        LUC_LOG_DECL("No generic arguments for trait, next token: "
                     << LucDebug::tokenToString(ts_.peek())
                     << " at line " << ts_.peek().line << ", col " << ts_.peek().column);
    }

    return ref;
}