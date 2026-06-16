#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

/**
 * @brief Parses an enum type declaration.
 * 
 * Grammar: `enum` IDENTIFIER `{` variant (`,` variant)* `}`
 * 
 * Example: `enum ShaderStage { Vertex = 0x01, Fragment = 0x02 }`
 * 
 * ─── Precondition ─────────────────────────────────────────────────────────
 * Caller MUST have already verified that the current token is ENUM.
 * This function assumes it is positioned at the 'enum' keyword.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'enum' keyword
 * On exit:  positioned after the closing '}'
 * 
 * ─── Note on Metadata ─────────────────────────────────────────────────────
 * Doc comments and attributes are handled by the dispatcher (parseDeclaration).
 * This function should NOT call harvestDocComment() or parseAttributes().
 * 
 * ─── Variant Values ───────────────────────────────────────────────────────
 *   - Auto variants start at 0, increment by 1
 *   - Explicit values reset the counter
 *   - Duplicate values are a semantic error
 * 
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 * Uses saved position pattern with parseEnumVariant()
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing enum name: reports error, returns nullptr
 * - Missing '{' after name: reports error, returns nullptr
 * - Invalid variant: skips variant, continues
 * - Missing '}': reports error (returns partially built node)
 */
EnumDeclPtr Parser::parseEnumDecl(Visibility vis) {
    LOG_DECL_VERBOSE("parseEnumDecl: entering");
    SourceLocation loc = ts_.currentLoc();
    
    // Check for 'enum' keyword (should be present if called correctly)
    if (!ts_.check(TokenType::ENUM)) {
        LOG_DECL("parseEnumDecl: ERROR - expected 'enum' keyword");
        errorAt(DiagCode::E1001, "enum", ts_.peek().value);
        return nullptr;
    }
    ts_.advance(); // Consume 'enum' keyword

    // Parse enum name
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LOG_DECL("parseEnumDecl: ERROR - expected enum name");
        errorAt(DiagCode::E1002, "enum name", ts_.peek().value);
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    LOG_DECL_EXTREME("parseEnumDecl: enum name = " << pool_.lookup(name));

    // Expect opening brace
    if (!ts_.check(TokenType::LBRACE)) {
        LOG_DECL("parseEnumDecl: ERROR - expected '{' to open enum body");
        errorAt(DiagCode::E1004, "{", "enum body", ts_.peek().value);
        return nullptr;
    }
    ts_.advance(); // Consume '{'

    // Parse variants
    std::vector<EnumVariantPtr> variants;
    int variantCount = 0;
    int consecutiveFailures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 10;
    
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd() && consecutiveFailures < MAX_CONSECUTIVE_FAILURES) {
        // Skip optional separators (commas or semicolons between variants)
        ts_.match(TokenType::COMMA);
        ts_.match(TokenType::SEMICOLON);

        size_t savedPos = ts_.getPos();
        EnumVariantPtr variant = parseEnumVariant();
        
        if (variant) {
            variantCount++;
            LOG_DECL_EXTREME("parseEnumDecl: parsed variant #" << variantCount);
            variants.push_back(variant);
            consecutiveFailures = 0;
        } else {
            consecutiveFailures++;
            LOG_DECL("parseEnumDecl: ERROR - failed to parse variant (attempt " 
                     << consecutiveFailures << ")");
            
            // Check for progress
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) {
                // No progress - force consume a token
                LOG_DECL("parseEnumDecl: no progress, forcing token consumption");
                ts_.advance();
            }
            
            // Skip to next potential variant start
            while (!ts_.isAtEnd() && 
                   !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::IDENTIFIER)) {
                ts_.advance();
            }
        }
        
        // Safety: prevent infinite loops
        if (consecutiveFailures > 5) {
            LOG_DECL("parseEnumDecl: too many consecutive failures, forcing skip to RBRACE");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE)) {
                ts_.advance();
            }
            // Let the loop condition handle exit
        }
    }

    // Consume the closing brace
    if (ts_.check(TokenType::RBRACE)) {
        ts_.advance(); // Consume '}'
    } else {
        // We're not at RBRACE - this means we hit EOF or max consecutive failures
        LOG_DECL("parseEnumDecl: ERROR - expected '}' to close enum body");
        errorAt(DiagCode::E1005, "}", "enum body", ts_.peek().value);
    }

    // Build the variants span
    auto builder = arena_.makeBuilder<EnumVariantPtr>();
    for (auto& v : variants) builder.push_back(v);
    ArenaSpan<EnumVariantPtr> variantSpan = builder.build();
    
    // Create the AST node
    auto* node = arena_.make<EnumDeclAST>();
    node->loc = loc;
    node->name = name;
    node->visibility = vis;
    node->variants = variantSpan;
    
    LOG_DECL_VERBOSE("parseEnumDecl: parsed " << variantCount << " variant(s)");
    return node;
}

/**
 * @brief Parses a single enum variant.
 * 
 * Grammar: IDENTIFIER [ `=` ( INT_LITERAL | HEX_LITERAL | BINARY_LITERAL ) ]
 * 
 * Example: `Vertex = 0x01`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at variant name
 * On exit:  positioned after optional explicit value
 * 
 * ─── Value Parsing ────────────────────────────────────────────────────────
 *   - Strips underscore separators (e.g., `0xFF_FF`)
 *   - Supports decimal, hexadecimal, and binary
 *   - Overflow detection (reports error, variant created with no value)
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing variant name: returns nullptr
 * - Invalid literal after '=': reports error (E1108), variant created with no value
 * 
 * @note Why use error() instead of errorAt()?
 *   After consuming the literal token with ts_.advance(), the current location
 *   points to the NEXT token. Using errorAt() would report the error at the
 *   wrong position. We capture the token's location BEFORE or during consumption
 *   and pass it explicitly to error().
 */
EnumVariantPtr Parser::parseEnumVariant() {
    LOG_DECL_EXTREME("parseEnumVariant: entering");
    SourceLocation loc = ts_.currentLoc();

    // Check for variant name
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LOG_DECL("parseEnumVariant: ERROR - expected enum variant name");
        errorAt(DiagCode::E1002, "enum variant name", ts_.peek().value);
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    LOG_DECL_EXTREME("parseEnumVariant: variant name = " << pool_.lookup(name));

    auto* variant = arena_.make<EnumVariantAST>(name);
    variant->loc = loc;

    // Parse optional explicit value
    if (ts_.match(TokenType::ASSIGN)) {
        LOG_DECL_EXTREME("parseEnumVariant: explicit value");
        
        // Check for valid literal types
        if (ts_.check(TokenType::INT_LITERAL) || 
            ts_.check(TokenType::HEX_LITERAL) ||
            ts_.check(TokenType::BINARY_LITERAL)) {
            
            // Capture the token BEFORE advancing so we have its location
            Token valTok = ts_.peek();
            ts_.advance(); // Consume the literal token
            
            std::string raw = valTok.value;
            
            // Remove underscore separators
            raw.erase(std::remove(raw.begin(), raw.end(), '_'), raw.end());

            // Determine base from token type
            int base = 10;
            if (valTok.type == TokenType::HEX_LITERAL) {
                base = 16;
                // Remove '0x' or '0X' prefix if present
                if (raw.size() > 2 && (raw[0] == '0' && (raw[1] == 'x' || raw[1] == 'X'))) {
                    raw = raw.substr(2);
                }
            } else if (valTok.type == TokenType::BINARY_LITERAL) {
                base = 2;
                // Remove '0b' or '0B' prefix if present
                if (raw.size() > 2 && (raw[0] == '0' && (raw[1] == 'b' || raw[1] == 'B'))) {
                    raw = raw.substr(2);
                }
            }
            
            char* endPtr = nullptr;
            errno = 0;
            long long val = std::strtoll(raw.c_str(), &endPtr, base);

            if (endPtr != raw.c_str() && *endPtr == '\0' && errno != ERANGE) {
                variant->explicitValue = val;
                LOG_DECL_EXTREME("parseEnumVariant: value = " << val);
            } else {
                // Use error() with the specific token's location (not errorAt)
                LOG_DECL("parseEnumVariant: ERROR - invalid integer literal '" << valTok.value << "'");
                error(ts_.locOf(valTok), DiagCode::E1108, valTok.value);
            }
        } else {
            // No literal after '=' - report error at current token position
            LOG_DECL("parseEnumVariant: ERROR - expected integer literal after '='");
            errorAt(DiagCode::E1108, ts_.peek().value);
        }
    }

    return variant;
}