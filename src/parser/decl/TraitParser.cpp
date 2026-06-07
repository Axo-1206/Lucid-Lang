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
TraitDeclPtr Parser::parseTraitDecl(Visibility vis) {
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
            methods.push_back(method);
        } else {
            LUC_LOG_DECL("parseTraitDecl: ERROR - failed to parse trait method");
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) ts_.advance();
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::IDENTIFIER)) ts_.advance();
        }
    }

    auto builder = arena_.makeBuilder<TraitMethodPtr>();
    for (auto& m : methods) builder.push_back(m);
    node->methods = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close trait body");
    
    LUC_LOG_DECL_VERBOSE("parseTraitDecl: parsed " << methodCount << " method(s)");
    return node;
}

/**
 * @brief Parses a single trait method signature.
 * 
 * Grammar: IDENTIFIER [ qualifier_list ] param_group { param_group } [ '->' return_list ]
 * 
 * Example: `draw ()`, `bounds () -> Rect`, `compareTo (other T) -> int`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at method name
 * On exit:  positioned after the signature (no body)
 * 
 * ─── Important Notes ───────────────────────────────────────────────────────
 *   - Trait methods have NO body and NO '='
 *   - Signature is parsed using parseFuncType()
 * 
 * @return TraitMethodPtr – parsed method node, or nullptr on error
 */
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
    
    TypePtr funcType = parseFuncType();
    if (!funcType || funcType->isa<UnknownTypeAST>()) {
        LUC_LOG_DECL("parseTraitMethod: ERROR - invalid method signature");
        errorAt(DiagCode::E1005, "invalid method signature for trait method '" 
                + std::string(pool_.lookup(method->name)) + "'");
        return nullptr;
    }
    method->funcType = funcType->as<FuncTypeAST>();
    
    LUC_LOG_DECL_EXTREME("parseTraitMethod: success");
    return method;
}

/**
 * @brief Parses a trait reference in impl declarations.
 * 
 * Grammar: ':' IDENTIFIER [ '<' type_args '>' ]
 * 
 * Example: `: Drawable`, `: Comparable<int>`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at ':' token
 * On exit:  positioned after the trait name (and optional generic arguments)
 * 
 * @return TraitRefPtr – parsed trait reference node, or nullptr on error
 */
TraitRefPtr Parser::parseTraitRef() {
    LUC_LOG_DECL_EXTREME("parseTraitRef: entering at line " << ts_.currentLoc().line()
                         << ", col " << ts_.currentLoc().column());
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::COLON, "expected ':' before trait name");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_DECL("parseTraitRef: ERROR - expected trait name after ':'");
        errorAt(DiagCode::E1003, "expected trait name after ':'");
        return nullptr;
    }

    auto ref = arena_.make<TraitRefAST>();
    ref->loc = loc;
    ref->name = pool_.intern(ts_.advance().value);
    LUC_LOG_DECL_EXTREME("parseTraitRef: trait name = " << pool_.lookup(ref->name));

    // Parse generic arguments if present
    if (ts_.check(TokenType::LESS)) {
        LUC_LOG_DECL_EXTREME("parseTraitRef: parsing generic arguments");
        ts_.advance(); // consume '<'
        ref->genericArgs = parseGenericArgs();
        LUC_LOG_DECL_EXTREME("parseTraitRef: parsed " << ref->genericArgs.size() << " generic argument(s)");
    }

    return ref;
}