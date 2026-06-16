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
 * ─── Precondition ─────────────────────────────────────────────────────────
 * Caller MUST have already verified that the current token is TRAIT.
 * This function assumes it is positioned at the 'trait' keyword.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'trait' keyword
 * On exit:  positioned after the closing '}'
 * 
 * ─── Note on Metadata ─────────────────────────────────────────────────────
 * Doc comments and attributes are handled by the dispatcher (parseDeclaration).
 * This function should NOT call harvestDocComment() or parseAttributes().
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
 * - Missing trait name: reports error, returns nullptr
 * - Missing '{' after name: reports error, returns nullptr
 * - Invalid method: skips method, continues
 * - Missing '}': reports error (returns partially built node)
 */
TraitDeclPtr Parser::parseTraitDecl(Visibility vis) {
    LOG_DECL_VERBOSE("parseTraitDecl: entering");
    SourceLocation loc = ts_.currentLoc();
    
    // Check for 'trait' keyword (should be present if called correctly)
    if (!ts_.check(TokenType::TRAIT)) {
        LOG_DECL("parseTraitDecl: ERROR - expected 'trait' keyword");
        errorAt(DiagCode::E1001, "trait", ts_.peek().value);
        return nullptr;
    }
    ts_.advance(); // Consume 'trait' keyword

    // Parse trait name
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LOG_DECL("parseTraitDecl: ERROR - expected trait name");
        errorAt(DiagCode::E1002, "trait name", ts_.peek().value);
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    LOG_DECL_EXTREME("parseTraitDecl: trait name = " << pool_.lookup(name));

    // Parse generic parameters if present
    ArenaSpan<GenericParamDeclPtr> genericParams;
    if (ts_.check(TokenType::LESS)) {
        LOG_DECL_EXTREME("parseTraitDecl: parsing generic parameters");
        genericParams = parseGenericParamDecls();
        LOG_DECL_EXTREME("parseTraitDecl: " << genericParams.size() << " generic parameter(s)");
    }

    // Expect opening brace
    if (!ts_.check(TokenType::LBRACE)) {
        LOG_DECL("parseTraitDecl: ERROR - expected '{' to open trait body");
        errorAt(DiagCode::E1004, "{", "trait body", ts_.peek().value);
        return nullptr;
    }
    ts_.advance(); // Consume '{'

    // Parse methods
    std::vector<TraitMethodPtr> methods;
    int methodCount = 0;
    int consecutiveFailures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 10;
    
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd() && consecutiveFailures < MAX_CONSECUTIVE_FAILURES) {
        // Skip optional separators
        ts_.match(TokenType::COMMA);
        ts_.match(TokenType::SEMICOLON);

        size_t savedPos = ts_.getPos();
        TraitMethodPtr method = parseTraitMethod();
        
        if (method) {
            methodCount++;
            LOG_DECL_EXTREME("parseTraitDecl: parsed method #" << methodCount);
            methods.push_back(method);
            consecutiveFailures = 0;
        } else {
            consecutiveFailures++;
            LOG_DECL("parseTraitDecl: ERROR - failed to parse trait method (attempt " 
                     << consecutiveFailures << ")");
            
            // Check for progress
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) {
                LOG_DECL("parseTraitDecl: no progress, forcing token consumption");
                ts_.advance();
            }
            
            // Skip to next potential method start
            while (!ts_.isAtEnd() && 
                   !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::IDENTIFIER)) {
                ts_.advance();
            }
        }
        
        // Safety: prevent infinite loops
        if (consecutiveFailures > 5) {
            LOG_DECL("parseTraitDecl: too many consecutive failures, forcing skip to RBRACE");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE)) {
                ts_.advance();
            }
            // Note: The loop will exit because consecutiveFailures >= MAX_CONSECUTIVE_FAILURES
            // or because we reached RBRACE (which will cause the loop condition to fail)
            // We don't break here - let the loop condition handle it
        }
    }

    // Consume the closing brace
    if (ts_.check(TokenType::RBRACE)) {
        ts_.advance(); // Consume '}'
    } else {
        // We're not at RBRACE - this means we hit EOF or max consecutive failures
        LOG_DECL("parseTraitDecl: ERROR - expected '}' to close trait body");
        errorAt(DiagCode::E1005, "}", "trait body", ts_.peek().value);
    }

    // Build the methods span
    auto builder = arena_.makeBuilder<TraitMethodPtr>();
    for (auto& m : methods) builder.push_back(m);
    ArenaSpan<TraitMethodPtr> methodSpan = builder.build();
    
    // Create the AST node
    auto* node = arena_.make<TraitDeclAST>();
    node->loc = loc;
    node->name = name;
    node->visibility = vis;
    node->genericParams = genericParams;
    node->methods = methodSpan;
    
    LOG_DECL_VERBOSE("parseTraitDecl: parsed " << methodCount << " method(s)");
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
    LOG_DECL_EXTREME("parseTraitMethod: entering");
    SourceLocation loc = ts_.currentLoc();
    
    // Check for method name
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LOG_DECL("parseTraitMethod: ERROR - expected trait method name");
        errorAt(DiagCode::E1002, "trait method name", ts_.peek().value);
        return nullptr;
    }
    
    auto* method = arena_.make<TraitMethodAST>();
    method->loc = loc;
    method->name = pool_.intern(ts_.advance().value);
    LOG_DECL_EXTREME("parseTraitMethod: method name = " << pool_.lookup(method->name));
    
    // Parse function type signature
    TypePtr funcType = parseFuncType();
    if (!funcType || funcType->isa<UnknownTypeAST>()) {
        LOG_DECL("parseTraitMethod: ERROR - invalid method signature");
        errorAt(DiagCode::E1008, "function signature", ts_.peek().value);
        return nullptr;
    }
    
    if (!funcType->isa<FuncTypeAST>()) {
        LOG_DECL("parseTraitMethod: ERROR - expected function type");
        errorAt(DiagCode::E1008, "function signature", ts_.peek().value);
        return nullptr;
    }
    
    method->funcType = funcType->as<FuncTypeAST>();
    
    LOG_DECL_EXTREME("parseTraitMethod: success");
    return method;
}