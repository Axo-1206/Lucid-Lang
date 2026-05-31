#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

/**
 * @brief Parses a `from` block defining implicit conversions to a type.
 * 
 * Grammar:
 *   from_decl := [ visibility_mod ] 'from' type [ generic_params ] '{' from_entry* '}'
 * 
 *   from_entry := param_group { param_group } '->' type '=' func_body
 * 
 * Examples:
 *   export from Fahrenheit {
 *       (c Celsius) -> Fahrenheit = { return Fahrenheit { value = c.value * 9/5 + 32 } }
 *   }
 * 
 *   from Wrapper<T> {
 *       (val T) -> Wrapper<T> = { return Wrapper<T> { value = val } }
 *   }
 * 
 *   from int {
 *       (s string) -> int = { return #parseInt(s) }
 *   }
 * 
 * ─── Parsing Strategy ──────────────────────────────────────────────────────
 *   1. Parse 'from' keyword and visibility
 *   2. Parse target type:
 *      a. If target is generic (IDENTIFIER '<'), parse generic parameters
 *         as declarations (GenericParamAST) and store on FromDeclAST
 *      b. Otherwise, parse as normal type (primitive or named)
 *   3. Parse '{' and zero or more from entries
 *   4. Parse '}' to close block
 * 
 * ─── Generic Parameters on From ────────────────────────────────────────────
 *   - Generic parameters are parsed as declarations (GenericParamAST)
 *   - They are stored in `node->genericParams`, NOT as arguments on the type
 *   - The target type's `NamedTypeAST` has empty `genericArgs`
 *   - Generic parameters are bound in the from entry bodies
 * 
 * ─── From Entry Format ────────────────────────────────────────────────────
 *   - Parameter groups define the source type(s) (may be curried)
 *   - '->' followed by return type (must match target type after substitution)
 *   - '=' followed by conversion body (block or expression)
 *   - No qualifiers (~async, ~nullable, ~parallel) allowed
 * 
 * ─── Target Type Rules (Semantic Pass) ─────────────────────────────────────
 *   - Target type can be ANY type (primitive, struct, enum, alias)
 *   - For generic structs, generic parameters must be declared on the from
 *   - Return types of entries must match the target type after substitution
 *   - Array and function types must be wrapped in a type alias first
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'from' keyword
 * On exit:  positioned after the closing '}'
 * 
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 * Uses saved position pattern when parsing from entries. If parseFromEntry()
 * makes no progress:
 *   - Consumes one token (advance)
 *   - Skips to next '(' or closing brace
 *   - Continues (does NOT push an entry)
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing target type: returns nullptr
 * - Missing '{' after target: reports error, returns nullptr
 * - Invalid entry: skips entry, continues
 * - Missing '->' in entry: reports error, skips entry
 * - Missing return type after '->': reports error, skips entry
 * - Missing '=' before body: reports error, skips entry
 * - Missing '}': consume() reports error
 * 
 * ─── Semantic Pass Validation (Not Parser Responsibility) ──────────────────
 * - Target type resolution (E3021)
 * - Return type matches target (E3022)
 * - Generic parameter usage correctness
 * - Conversion uniqueness in scope
 * 
 * @param vis Visibility modifier (Private, Package, or Export)
 *        determined by caller from 'pub'/'export' keywords
 * 
 * @return ASTPtr<FromDeclAST> – from node on success, nullptr on error
 */
ASTPtr<FromDeclAST> Parser::parseFromDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::FROM, "expected 'from'");

    auto node = arena_.make<FromDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    // Parse target type: supports ANY type (primitive, struct, enum, alias)
    // But generic parameters are parsed separately for FromDeclAST
    
    // Check if we have a primitive type, identifier, or other type start
    if (!looksLikeType()) {
        errorAt(DiagCode::E2005, "expected target type in from block");
        return nullptr;
    }
    
    // Peek ahead to see if this is a generic type with '<' 
    // We need to parse the base type name separately from generic parameters
    bool isGenericType = false;
    size_t beforeTypePos = ts_.getPos();
    
    // Check if current token is an identifier followed by '<' (generic type)
    if (ts_.check(TokenType::IDENTIFIER)) {
        size_t lookahead = ts_.skipCommentsFrom(ts_.getPos() + 1);

        if (lookahead < ts_.getTokenCount() && ts_.getTokenAt(lookahead).type == TokenType::LESS) {
            isGenericType = true;
        }
    }
    
    TypePtr targetType;
    
    if (isGenericType) {
        // Parse as named type with generic parameters as PART OF the type
        // For FromDeclAST, these become generic parameters, not arguments
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected type name");
            return nullptr;
        }
        
        InternedString typeName = pool_.intern(ts_.advance().value);
        
        // Parse generic parameters (these are declarations, not arguments)
        ArenaSpan<GenericParamPtr> genericParams = parseGenericParams();
        
        // Store generic parameters on the node
        node->genericParams = genericParams;
        
        // Build the target type as a NamedTypeAST with NO generic arguments
        // (generic parameters are stored separately on FromDeclAST)
        auto namedType = arena_.make<NamedTypeAST>(typeName);
        namedType->loc = loc;
        // Intentionally leave genericArgs empty - the type is generic but
        // the parameters are stored at the from declaration level
        targetType = std::move(namedType);
    } else {
        // Non-generic type: parse normally
        targetType = parseType();
        if (!targetType || targetType->isa<UnknownTypeAST>()) {
            errorAt(DiagCode::E2005, "invalid target type in from block");
            return nullptr;
        }
        
        // Check if the parsed type already contains generic arguments
        // (e.g., from Wrapper<int> where int is a concrete type argument)
        // In this case, genericParams should remain empty
        if (targetType->isa<NamedTypeAST>()) {
            auto namedType = static_cast<NamedTypeAST*>(targetType.get());
            if (!namedType->genericArgs.empty()) {
                // This is a concrete instantiation, not a generic declaration
                // No generic parameters to store
            }
        }
    }

    node->targetType = std::move(targetType);

    // Parse from block body
    ts_.consume(TokenType::LBRACE, "expected '{' to open from block");

    std::vector<FromEntryPtr> entries;
    while (!ts_.check(TokenType::RBRACE) && !ts_.isAtEnd()) {
        ts_.match(TokenType::SEMICOLON);
        ts_.match(TokenType::COMMA);
        if (ts_.check(TokenType::RBRACE)) break;

        size_t entrySavedPos = ts_.getPos();

        if (!ts_.check(TokenType::LPAREN)) {
            errorAt(DiagCode::E2001, "expected '(' to start parameter list for conversion entry");
            if (ts_.getPos() == entrySavedPos && !ts_.isAtEnd()) ts_.advance();
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RBRACE) && 
                   !ts_.check(TokenType::LPAREN)) ts_.advance();
            continue;
        }

        auto entry = arena_.make<FromEntryAST>();
        entry->loc = ts_.currentLoc();

        // Parameter groups: flat accumulation
        std::vector<ParamPtr> allParams;
        std::vector<size_t> groupSizes;
        while (ts_.check(TokenType::LPAREN)) {
            std::vector<ParamPtr> group = parseParamGroup();
            groupSizes.push_back(group.size());
            for (auto& p : group) {
                allParams.push_back(std::move(p));
            }
        }
        auto paramsBuilder = arena_.makeBuilder<ParamPtr>();
        for (auto& p : allParams) paramsBuilder.push_back(std::move(p));
        entry->sig.allParams = paramsBuilder.build();

        auto gsBuilder = arena_.makeBuilder<size_t>();
        for (auto& sz : groupSizes) gsBuilder.push_back(sz);
        entry->sig.groupSizes = gsBuilder.build();

        if (!ts_.check(TokenType::ARROW)) {
            errorAt(DiagCode::E2001, "expected '->' before return type for conversion entry");
            continue;
        }
        ts_.advance();

        TypePtr returnType = parseType();
        if (!returnType) {
            errorAt(DiagCode::E2005, "expected return type after '->'");
            continue;
        }
        entry->returnType = std::move(returnType);

        if (!ts_.check(TokenType::ASSIGN)) {
            errorAt(DiagCode::E2001, "expected '=' before body for conversion entry");
            continue;
        }
        ts_.advance();

        if (ts_.check(TokenType::LBRACE)) {
            entry->body = parseBlock();
        } else {
            SourceLocation bodyLoc = ts_.currentLoc();
            ExprPtr expr = parseExpr();
            if (expr) {
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

                entry->body = std::move(block);
            } else {
                errorAt(DiagCode::E2008, "expected expression after '=' in conversion entry");
            }
        }

        entries.push_back(std::move(entry));
    }

    auto builder = arena_.makeBuilder<FromEntryPtr>();
    for (auto& e : entries) builder.push_back(std::move(e));
    node->entries = builder.build();

    ts_.consume(TokenType::RBRACE, "expected '}' to close from block");
    return node;
}