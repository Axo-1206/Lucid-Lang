#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

/**
 * @brief Parses a struct type declaration.
 * 
 * Grammar: `struct` IDENTIFIER [ `<` generic_params `>` ] `{` field_decl* `}`
 * 
 * Example: `pub struct Vec2<T> { x T, y T }`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'struct' keyword
 * On exit:  positioned after the closing '}'
 * 
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 * Uses saved position pattern when parsing fields. If parseFieldDecl() makes
 * no progress, consumes one token and continues (prevents infinite loop).
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing struct name: returns nullptr
 * - Missing '{' after name: reports error, returns nullptr
 * - Invalid field: skips field, continues parsing remaining fields
 * - Missing '}': consume() reports error
 */
ASTPtr<StructDeclAST> Parser::parseStructDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::STRUCT, "expected 'struct'");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected struct name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    auto node = arena_.make<StructDeclAST>();
    node->loc = loc;
    node->name = name;
    node->visibility = vis;

    if (ts_.check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    ts_.consume(TokenType::LBRACE, "expected '{' to open struct body");

    std::vector<FieldDeclPtr> fields;
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t savedPos = ts_.getPos();
        FieldDeclPtr field = parseFieldDecl();
        if (field) {
            fields.push_back(std::move(field));
        } else {
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) ts_.advance();
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::IDENTIFIER)) ts_.advance();
        }
    }

    auto builder = arena_.makeBuilder<FieldDeclPtr>();
    for (auto& f : fields) builder.push_back(std::move(f));
    node->fields = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close struct body");
    return node;
}

/**
 * @brief Parses a struct field declaration.
 * 
 * Grammar: IDENTIFIER type [ `=` expr ]
 * 
 * Example: `r float = 1.0`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at field name
 * On exit:  positioned after optional default value expression
 * 
 * ─── Default Values ───────────────────────────────────────────────────────
 *   - Struct literals may omit fields with default values
 *   - Default value must be a compile‑time constant (semantic pass)
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing field name: returns nullptr
 * - Missing type after name: reports error, returns nullptr
 * - Missing expression after '=': reports error (field still created)
 */
FieldDeclPtr Parser::parseFieldDecl() {
    SourceLocation loc = ts_.currentLoc();

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected field name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    TypePtr type = parseType();
    if (!type) {
        errorAt(DiagCode::E2005, "expected type for field '" + std::string(pool_.lookup(name)) + "'");
        return nullptr;
    }

    ExprPtr defaultVal;
    if (ts_.match(TokenType::ASSIGN)) {
        defaultVal = parseExpr();
        if (!defaultVal) {
            errorAt(DiagCode::E2008, "expected expression after '=' in field default value");
        }
    }

    auto field = arena_.make<FieldDeclAST>();
    field->loc = loc;
    field->name = name;
    field->type = std::move(type);
    field->defaultVal = std::move(defaultVal);
    return field;
}