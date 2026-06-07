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
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'enum' keyword
 * On exit:  positioned after the closing '}'
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
 * - Missing enum name: returns nullptr
 * - Missing '{' after name: reports error, returns nullptr
 * - Invalid variant: skips variant, continues
 * - Missing '}': consume() reports error
 */
EnumDeclPtr Parser::parseEnumDecl(Visibility vis) {
    LUC_LOG_DECL_VERBOSE("parseEnumDecl: entering");
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::ENUM, "expected 'enum'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_DECL("parseEnumDecl: ERROR - expected enum name");
        errorAt(DiagCode::E1003, "expected enum name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    LUC_LOG_DECL_EXTREME("parseEnumDecl: enum name = " << pool_.lookup(name));

    auto node = arena_.make<EnumDeclAST>();
    node->loc = loc;
    node->name = name;
    node->visibility = vis;

    ts_.consume(TokenType::LBRACE, "expected '{' to open enum body");

    std::vector<EnumVariantPtr> variants;
    int variantCount = 0;
    
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::COMMA);
        ts_.match(TokenType::SEMICOLON);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t savedPos = ts_.getPos();
        EnumVariantPtr variant = parseEnumVariant();
        if (variant) {
            variantCount++;
            LUC_LOG_DECL_EXTREME("parseEnumDecl: parsed variant #" << variantCount);
            variants.push_back(variant);
        } else {
            LUC_LOG_DECL("parseEnumDecl: ERROR - failed to parse variant");
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) ts_.advance();
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::IDENTIFIER)) ts_.advance();
        }
    }

    auto builder = arena_.makeBuilder<EnumVariantPtr>();
    for (auto& v : variants) builder.push_back(v);
    node->variants = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close enum body");
    
    LUC_LOG_DECL_VERBOSE("parseEnumDecl: parsed " << variantCount << " variant(s)");
    return node;
}

/**
 * @brief Parses a single enum variant.
 * 
 * Grammar: IDENTIFIER [ `=` ( INT_LITERAL | HEX_LITERAL ) ]
 * 
 * Example: `Vertex = 0x01`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at variant name
 * On exit:  positioned after optional explicit value
 * 
 * ─── Value Parsing ────────────────────────────────────────────────────────
 *   - Strips underscore separators (e.g., `0xFF_FF`)
 *   - Supports decimal and hexadecimal
 *   - Overflow detection (reports error, variant created with no value)
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing variant name: returns nullptr
 * - Invalid literal after '=': reports error, variant created with no value
 */
EnumVariantPtr Parser::parseEnumVariant() {
    LUC_LOG_DECL_EXTREME("parseEnumVariant: entering");
    SourceLocation loc = ts_.currentLoc();

    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_DECL("parseEnumVariant: ERROR - expected enum variant name");
        errorAt(DiagCode::E1003, "expected enum variant name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    LUC_LOG_DECL_EXTREME("parseEnumVariant: variant name = " << pool_.lookup(name));

    auto variant = arena_.make<EnumVariantAST>(name);
    variant->loc = loc;

    if (ts_.match(TokenType::ASSIGN)) {
        LUC_LOG_DECL_EXTREME("parseEnumVariant: explicit value");
        if (ts_.check(TokenType::INT_LITERAL) || ts_.check(TokenType::HEX_LITERAL) ||
            ts_.check(TokenType::BINARY_LITERAL)) {
            Token valTok = ts_.advance();
            std::string raw = valTok.value;
            raw.erase(std::remove(raw.begin(), raw.end(), '_'), raw.end());

            int base = (valTok.type == TokenType::HEX_LITERAL) ? 16 : 10;
            char* endPtr = nullptr;
            errno = 0;
            long long val = std::strtoll(raw.c_str(), &endPtr, base);

            if (endPtr != raw.c_str() && *endPtr == '\0' && errno != ERANGE) {
                variant->explicitValue = val;
                LUC_LOG_DECL_EXTREME("parseEnumVariant: value = " << val);
            } else {
                LUC_LOG_DECL("parseEnumVariant: ERROR - invalid integer literal '" << valTok.value << "'");
                error(ts_.locOf(valTok), DiagCode::E1007,
                      "enum variant value '" + valTok.value + "' is not a valid integer");
            }
        } else {
            LUC_LOG_DECL("parseEnumVariant: ERROR - expected integer literal after '='");
            errorAt(DiagCode::E1007, "expected integer literal after '=' in enum variant");
        }
    }

    return variant;
}