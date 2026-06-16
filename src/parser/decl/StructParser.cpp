#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

/**
 * @brief Parses a struct type declaration.
 * 
 * Grammar: `struct` IDENTIFIER [ `<` generic_params `>` ] `{` field_decl* `}`
 * 
 * Example: `pub struct Vec2<T> { x T, y T }`
 * 
 * ─── Precondition ─────────────────────────────────────────────────────────
 * Caller MUST have already verified that the current token is STRUCT.
 * This function assumes it is positioned at the 'struct' keyword.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'struct' keyword
 * On exit:  positioned after the closing '}'
 * 
 * ─── Note on Metadata ─────────────────────────────────────────────────────
 * Doc comments and attributes are handled by the dispatcher (parseDeclaration).
 * This function should NOT call harvestDocComment() or parseAttributes().
 * 
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 * Uses saved position pattern when parsing fields. If parseFieldDecl() makes
 * no progress, consumes one token and continues (prevents infinite loop).
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing struct name: reports error, returns nullptr
 * - Missing '{' after name: reports error, returns nullptr
 * - Invalid field: skips field, continues parsing remaining fields
 * - Missing '}': reports error (returns partially built node)
 */
StructDeclPtr Parser::parseStructDecl(Visibility vis) {
    LOG_DECL_VERBOSE("parseStructDecl: entering");
    
    SourceLocation loc = ts_.currentLoc();
    
    // Check for 'struct' keyword (should be present if called correctly)
    if (!ts_.check(TokenType::STRUCT)) {
        LOG_DECL("parseStructDecl: ERROR - expected 'struct' keyword");
        errorAt(DiagCode::E1001, "struct", ts_.peek().value);
        return nullptr;
    }
    ts_.advance(); // Consume 'struct' keyword

    // Parse struct name
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LOG_DECL("parseStructDecl: ERROR - expected struct name");
        errorAt(DiagCode::E1002, "struct name", ts_.peek().value);
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    LOG_DECL_EXTREME("parseStructDecl: struct name = " << pool_.lookup(name));

    // Parse generic parameters if present
    ArenaSpan<GenericParamDeclPtr> genericParams;
    if (ts_.check(TokenType::LESS)) {
        LOG_DECL_EXTREME("parseStructDecl: parsing generic parameters");
        genericParams = parseGenericParamDecls();
        LOG_DECL_EXTREME("parseStructDecl: " << genericParams.size() << " generic parameter(s)");
    }

    // Expect opening brace
    if (!ts_.check(TokenType::LBRACE)) {
        LOG_DECL("parseStructDecl: ERROR - expected '{' to open struct body");
        errorAt(DiagCode::E1004, "{", "struct body");
        return nullptr;
    }
    ts_.advance(); // Consume '{'

    // Parse fields
    std::vector<FieldDeclPtr> fields;
    int fieldCount = 0;
    int consecutiveFailures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 10;
    
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd() && consecutiveFailures < MAX_CONSECUTIVE_FAILURES) {
        // Skip optional separators (commas or semicolons between fields)
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);
        
        // Check if we've reached the end after skipping separators
        if (ts_.check(TokenType::RBRACE)) break;

        size_t savedPos = ts_.getPos();
        
        // Parse field (doc comments and attributes are NOT handled here)
        FieldDeclPtr field = parseFieldDecl();
        
        if (field) {
            fieldCount++;
            LOG_DECL_EXTREME("parseStructDecl: parsed field #" << fieldCount 
                                 << " (" << pool_.lookup(field->name) << ")");
            fields.push_back(field);
            consecutiveFailures = 0;
        } else {
            consecutiveFailures++;
            LOG_DECL("parseStructDecl: ERROR - failed to parse field (attempt " 
                     << consecutiveFailures << ")");
            
            // Check for progress
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) {
                // No progress - force consume a token
                LOG_DECL("parseStructDecl: no progress, forcing token consumption");
                ts_.advance();
            }
            
            // Skip to next potential field start
            while (!ts_.isAtEnd() && 
                   !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::IDENTIFIER) &&
                   !ts_.check(TokenType::AT_SIGN)) {
                ts_.advance();
            }
        }
        
        // Safety: prevent infinite loops
        if (consecutiveFailures > 5) {
            LOG_DECL("parseStructDecl: too many consecutive failures, forcing skip to RBRACE");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE)) {
                ts_.advance();
            }
            break;
        }
    }

    // Expect closing brace
    if (!ts_.check(TokenType::RBRACE)) {
        LOG_DECL("parseStructDecl: ERROR - expected '}' to close struct body");
        errorAt(DiagCode::E1004, "}", "struct body");
    } else {
        ts_.advance(); // Consume '}'
    }

    // Build the fields span
    auto builder = arena_.makeBuilder<FieldDeclPtr>();
    for (auto& f : fields) builder.push_back(f);
    ArenaSpan<FieldDeclPtr> fieldSpan = builder.build();
    
    // Create the AST node
    auto* node = arena_.make<StructDeclAST>();
    node->loc = loc;
    node->name = name;
    node->visibility = vis;
    node->genericParams = genericParams;
    node->fields = fieldSpan;
    
    LOG_DECL_VERBOSE("parseStructDecl: parsed " << fieldCount << " field(s)");
    return node;
}

/**
 * @brief Parses a struct field declaration.
 * 
 * Grammar: IDENTIFIER type [ `=` expr ]
 * 
 * Example: `r float = 1.0`
 * 
 * ─── Precondition ─────────────────────────────────────────────────────────
 * Caller should be positioned at the field name.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at field name
 * On exit:  positioned after optional default value expression
 * 
 * ─── Note on Metadata ─────────────────────────────────────────────────────
 * Fields do NOT have doc comments or attributes in Luc.
 * (Attributes on fields are not supported by the grammar)
 * 
 * ─── Visibility ────────────────────────────────────────────────────────────
 * Fields do NOT have visibility modifiers in Luc.
 * Visibility is controlled at the struct level only.
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
    LOG_DECL_EXTREME("parseFieldDecl: entering");
    
    SourceLocation loc = ts_.currentLoc();

    // Parse field name
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LOG_DECL("parseFieldDecl: ERROR - expected field name");
        errorAt(DiagCode::E1002, "field name", ts_.peek().value);
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    LOG_DECL_EXTREME("parseFieldDecl: field name = " << pool_.lookup(name));

    // Parse field type
    TypePtr type = parseType();
    if (!type) {
        LOG_DECL("parseFieldDecl: ERROR - expected type for field");
        errorAt(DiagCode::E1003, ts_.peek().value);
        return nullptr;
    }

    // Parse optional default value
    ExprPtr defaultVal = nullptr;
    if (ts_.match(TokenType::ASSIGN)) {
        LOG_DECL_EXTREME("parseFieldDecl: parsing default value");
        defaultVal = parseExpr();
        if (!defaultVal) {
            LOG_DECL("parseFieldDecl: ERROR - expected expression after '='");
            errorAt(DiagCode::E1006, ts_.peek().value);
            // Continue without default value (error already reported)
        }
    }

    // Create the field node
    auto* field = arena_.make<FieldDeclAST>();
    field->loc = loc;
    field->name = name;
    field->type = type;
    field->defaultVal = defaultVal;
    
    LOG_DECL_EXTREME("parseFieldDecl: success - field '" << pool_.lookup(name) << "'");
    return field;
}