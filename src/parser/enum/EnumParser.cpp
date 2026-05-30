#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

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
ASTPtr<EnumDeclAST> Parser::parseEnumDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::ENUM, "expected 'enum'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected enum name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    auto node = arena_.make<EnumDeclAST>();
    node->loc = loc;
    node->name = name;
    node->visibility = vis;

    ts_.consume(TokenType::LBRACE, "expected '{' to open enum body");

    std::vector<EnumVariantPtr> variants;
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::COMMA);
        ts_.match(TokenType::SEMICOLON);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t savedPos = ts_.getPos();
        EnumVariantPtr variant = parseEnumVariant();
        if (variant) {
            variants.push_back(std::move(variant));
        } else {
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) ts_.advance();
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::IDENTIFIER)) ts_.advance();
        }
    }

    auto builder = arena_.makeBuilder<EnumVariantPtr>();
    for (auto& v : variants) builder.push_back(std::move(v));
    node->variants = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close enum body");
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
    SourceLocation loc = ts_.currentLoc();

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected enum variant name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    auto variant = arena_.make<EnumVariantAST>(name);
    variant->loc = loc;

    if (ts_.match(TokenType::ASSIGN)) {
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
            } else {
                error(ts_.locOf(valTok), DiagCode::E2009,
                      "enum variant value '" + valTok.value + "' is not a valid integer");
            }
        } else {
            errorAt(DiagCode::E2009, "expected integer literal after '=' in enum variant");
        }
    }

    return variant;
}
