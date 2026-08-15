/**
 * @file Helpers.cpp
 * @brief Implementation of parser helper functions.
 * 
 * This file implements helper functions used by the parser:
 * - harvestDocComment: Collects documentation comments
 * - parseAttributes: Parses attribute lists (@[attr1, attr2])
 * - parseAttribute: Parses a single attribute (NO comma handling)
 * - parseAttributeArgLiteral: Parses attribute argument literals
 * - parseGenericParamDecls: Parses generic parameter lists
 * - parseGenericParamDecl: Parses a single generic parameter (NO comma handling)
 * - parseGenericArgs: Parses generic argument lists
 * - parseParamList: Parses function parameter lists
 * - parseArgList: Parses function argument lists (silent on commas)
 * - parseImportPath: Parses import paths
 * 
 * @design_decision Comma handling ONLY at list level
 *   - List parsers handle commas between items
 *   - Item parsers parse a single item with NO comma handling
 *   - This creates a clean separation of concerns
 * 
 * @design_decision Hybrid comma handling for declaration lists
 *   - 1-2 commas: report "expected X" (user probably forgot the element)
 *   - 3+ commas: report "unexpected trailing comma" (user has trailing commas)
 *   - Call sites (parseArgList) silently skip commas (semantic phase handles matching)
 */

#include "../Parser.hpp"
#include "core/Tokens.hpp"
#include "core/ast/TypeAST.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

namespace parser {

// =============================================================================
// Helper: handleCommaGap - Hybrid strategy for comma gaps at LIST level
// =============================================================================

/**
 * @brief Handle comma gaps intelligently at the list level.
 * 
 * - 1-2 commas: report "expected X" once (user probably forgot the element)
 * - 3+ commas: report "unexpected trailing comma" once (user has trailing commas)
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @param what The thing that was expected (e.g., "attribute", "argument", "type")
 * @param isFirst Whether this is the first item in the list
 * @return int The number of commas consumed
 */
int handleCommaGap(TokenStream& stream, ParserContext& ctx, const std::string& what, bool isFirst) {
    int commaCount = stream.consumeTrailing(TokenType::COMMA);
    
    if (commaCount == 0) {
        return 0;
    }
    
    if (isFirst) {
        // Leading commas before first item
        if (commaCount <= 2) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected ", what, ", got ','");
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_TrailingComma, stream.currentLoc(),
                                    "unexpected leading comma in ", what, " list");
        }
        return commaCount;
    }
    
    // Commas between items (after at least one item was parsed)
    if (commaCount <= 2) {
        // Small gap - user probably forgot the element
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected ", what, ", got ','");
    } else {
        // Large gap - user has trailing commas
        ctx.diagnostics.errorAt(DiagCode::Syntax_TrailingComma, stream.currentLoc(),
                                "unexpected trailing comma in ", what, " list");
    }
    
    return commaCount;
}

// =============================================================================
// harvestDocComment
// =============================================================================

std::optional<DocComment> harvestDocComment(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("harvestDocComment: checking for doc comment");
    
    const auto& tokens = stream.getTokens();
    size_t pos = stream.getPos();
    
    if (pos == 0) return std::nullopt;
    
    int declLine = stream.peek().line;
    std::optional<std::string> trailingText;
    std::vector<std::string> stackedLines;
    int stackedTopLine = -1;
    std::optional<std::string> blockText;
    
    for (size_t i = pos; i > 0; ) {
        --i;
        const Token& t = tokens[i];
        
        if (t.type == TokenType::LINE_COMMENT) {
            if (t.line <= 0) continue;
            
            if (t.line == declLine) {
                if (!trailingText.has_value()) {
                    trailingText = t.value;
                }
                continue;
            }
            
            if (stackedLines.empty()) {
                if (declLine - t.line == 1) {
                    stackedLines.push_back(t.value);
                    stackedTopLine = t.line;
                    continue;
                } else {
                    break;
                }
            } else {
                if (stackedTopLine - t.line == 1) {
                    stackedLines.push_back(t.value);
                    stackedTopLine = t.line;
                    continue;
                } else {
                    break;
                }
            }
        }
        
        if (t.type == TokenType::DOC_COMMENT) {
            if (t.line <= 0) continue;
            if (declLine - t.line <= 1) {
                blockText = t.value;
            }
            break;
        }
        
        break;
    }
    
    // Priority: Block > Stacked > Trailing
    if (blockText.has_value()) {
        return DocComment{ctx.pool.intern(*blockText), DocCommentForm::Block};
    }
    
    if (!stackedLines.empty()) {
        std::string combined;
        for (int i = static_cast<int>(stackedLines.size()) - 1; i >= 0; --i) {
            if (!combined.empty()) combined += '\n';
            combined += stackedLines[i];
        }
        return DocComment{ctx.pool.intern(combined), DocCommentForm::Stacked};
    }
    
    if (trailingText.has_value()) {
        return DocComment{ctx.pool.intern(*trailingText), DocCommentForm::Trailing};
    }
    
    return std::nullopt;
}

// =============================================================================
// parseAttributes - LIST LEVEL - handles commas
// =============================================================================

ArenaSpan<AttributePtr> parseAttributes(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER("parseAttributes: checking for attributes");
    
    std::vector<AttributePtr> attrs;
    
    if (!stream.check(TokenType::AT_SIGN)) {
        return ctx.arena.makeBuilder<AttributePtr>().build();
    }
    
    stream.consume(); // Consume '@'
    
    if (!stream.check(TokenType::LBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '[', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return ctx.arena.makeBuilder<AttributePtr>().build();
    }
    stream.consume(); // Consume '['

    ScopedContext attrGuard(ctx, SyntacticContext::Attribute, stream.currentLoc());

    bool isFirst = true;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACKET)) {
        // Handle commas before this item (list-level comma handling)
        handleCommaGap(stream, ctx, "attribute", isFirst);
        isFirst = false;
        
        // Parse single attribute (NO comma handling inside)
        AttributePtr attr = parseAttribute(stream, ctx);
        if (attr) {
            attrs.push_back(attr);
        } else {
            if (synchronizeToContext(stream, ctx) == SyncOutcome::Abandoned) {
                break;
            }
        }
    }
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ']' to close attribute list");
    } else {
        stream.consume(); // Consume ']'
    }
    
    auto builder = ctx.arena.makeBuilder<AttributePtr>();
    for (auto* attr : attrs) {
        builder.push_back(attr);
    }
    return builder.build();
}

// =============================================================================
// parseAttribute - ITEM LEVEL - NO comma handling
// =============================================================================

AttributePtr parseAttribute(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected attribute name, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    Token nameTok = stream.consume();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    auto* attr = ctx.arena.make<AttributeAST>();
    attr->loc = loc;
    attr->name = name;
    
    // Attribute arguments - this is a sub-list, handle commas here
    std::vector<LiteralExprAST*> args;
    if (stream.match(TokenType::LPAREN)) {
        bool isFirstArg = true;
        
        while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN)) {
            // Handle commas before this argument (sub-list level)
            handleCommaGap(stream, ctx, "attribute argument", isFirstArg);
            isFirstArg = false;
            
            LiteralExprAST* arg = parseAttributeArgLiteral(stream, ctx);
            if (arg) {
                args.push_back(arg);
            } else {
                synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
                if (!stream.check(TokenType::COMMA) && !stream.check(TokenType::RPAREN)) {
                    break;
                }
            }
        }
        
        if (stream.isAtEnd()) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected ')' to close attribute arguments");
        } else {
            stream.consume(); // Consume ')'
        }
    }
    
    auto builder = ctx.arena.makeBuilder<LiteralExprAST*>();
    for (auto* arg : args) {
        builder.push_back(arg);
    }
    attr->args = builder.build();
    
    LOG_PARSER("parseAttribute: parsed '", ctx.pool.lookup(name), "' with ", args.size(), " args");
    return attr;
}

// =============================================================================
// parseAttributeArgLiteral - ITEM LEVEL - NO comma handling
// =============================================================================

LiteralExprAST* parseAttributeArgLiteral(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    Token tok = stream.peek();
    
    LiteralKind kind;
    InternedString value;
    
    switch (tok.type) {
        case TokenType::STRING_LITERAL:
            kind = LiteralKind::String;
            value = ctx.pool.intern(tok.value);
            stream.consume();
            break;
            
        case TokenType::INT_LITERAL:
        case TokenType::HEX_LITERAL:
        case TokenType::BINARY_LITERAL:
        case TokenType::CHAR_LITERAL:
            kind = LiteralKind::Int;
            value = ctx.pool.intern(tok.value);
            stream.consume();
            break;
            
        case TokenType::FLOAT_LITERAL:
            kind = LiteralKind::Float;
            value = ctx.pool.intern(tok.value);
            stream.consume();
            break;
            
        case TokenType::TRUE:
        case TokenType::FALSE:
            kind = tok.type == TokenType::TRUE ? LiteralKind::True : LiteralKind::False;
            value = ctx.pool.intern(tok.value);
            stream.consume();
            break;
            
        case TokenType::IDENTIFIER:
            kind = LiteralKind::String;
            value = ctx.pool.intern(tok.value);
            stream.consume();
            break;
            
        default:
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedLiteral, stream.currentLoc(),
                                    "expected literal, got '", stream.peekValue(), "'");
            synchronizeToContext(stream, ctx);
            return nullptr;
    }
    
    auto* literal = ctx.arena.make<LiteralExprAST>(kind, value);
    literal->loc = loc;
    
    return literal;
}

// =============================================================================
// parseGenericParamDecls - LIST LEVEL - handles commas
// =============================================================================

ArenaSpan<GenericParamDeclAST*> parseGenericParamDecls(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseGenericParamDecls: parsing generic parameters");
    
    std::vector<GenericParamDeclAST*> params;
    
    if (!stream.check(TokenType::LESS)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '<', got '", stream.peekValue(), "'");
        return ctx.arena.makeBuilder<GenericParamDeclAST*>().build();
    }
    stream.consume(); // Consume '<'
    
    if (stream.check(TokenType::GREATER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected generic parameter name, got '>'");
        stream.consume(); // Consume '>'
        return ctx.arena.makeBuilder<GenericParamDeclAST*>().build();
    }
    
    ScopedContext guard(ctx, SyntacticContext::GenericParams, stream.currentLoc());

    bool isFirst = true;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::GREATER)) {
        // Handle commas before this parameter (list-level comma handling)
        handleCommaGap(stream, ctx, "generic parameter", isFirst);
        isFirst = false;
        
        // Parse single generic parameter (NO comma handling inside)
        GenericParamDeclAST* param = parseGenericParamDecl(stream, ctx);
        if (param) {
            params.push_back(param);
        } else {
            if (synchronizeToContext(stream, ctx) == SyncOutcome::Abandoned) {
                break;
            }
        }
    }
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '>' to close generic parameter list");
    } else {
        stream.consume(); // Consume '>'
    }
    
    auto builder = ctx.arena.makeBuilder<GenericParamDeclAST*>();
    for (auto* p : params) {
        builder.push_back(p);
    }
    
    return builder.build();
}

// =============================================================================
// parseGenericParamDecl - ITEM LEVEL - NO comma handling
// =============================================================================

GenericParamDeclAST* parseGenericParamDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected generic parameter name, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    Token nameTok = stream.consume();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    auto* param = ctx.arena.make<GenericParamDeclAST>(name);
    param->loc = loc;
    
    // Constraints (sub-list, handle commas here)
    if (stream.match(TokenType::COLON)) {
        std::vector<NamedTypeAST*> constraints;
        bool hasConstraint = false;
        bool isFirstConstraint = true;
        
        while (!stream.isAtEnd() && 
               !stream.check(TokenType::GREATER) && 
               !stream.check(TokenType::COMMA)) {
            
            // Handle pluses before this constraint
            int plusCount = stream.consumeTrailing(TokenType::PLUS);
            if (plusCount > 0) {
                if (!isFirstConstraint) {
                    // Plus before constraint is a separator
                    if (plusCount <= 2) {
                        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                                "expected trait constraint, got '+'");
                    } else {
                        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                                "unexpected multiple '+' in generic constraints");
                    }
                } else {
                    // Leading plus before first constraint
                    ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                            "expected trait constraint, got '+'");
                }
            }
            isFirstConstraint = false;
            
            NamedTypeAST* traitRef = parseNamedType(stream, ctx)->as<NamedTypeAST>();
            if (traitRef) {
                constraints.push_back(traitRef);
                hasConstraint = true;
            } else {
                synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::GREATER);
                break;
            }
            
            // Check for trailing plus
            plusCount = stream.consumeTrailing(TokenType::PLUS);
            if (plusCount == 1) {
                if (stream.check(TokenType::GREATER) || stream.check(TokenType::COMMA)) {
                    ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                            "unexpected trailing '+' in generic constraints");
                }
            } else if (plusCount > 1) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                        "unexpected multiple '+' in generic constraints");
            }
        }
        
        if (!hasConstraint) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected trait constraint after ':'");
        }
        
        auto builder = ctx.arena.makeBuilder<NamedTypeAST*>();
        for (auto* tr : constraints) {
            builder.push_back(tr);
        }
        param->constraints = builder.build();
    }
    
    return param;
}

// =============================================================================
// parseGenericArgs - LIST LEVEL - handles commas
// =============================================================================

ArenaSpan<TypeAST*> parseGenericArgs(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseGenericArgs: parsing generic arguments");

    std::vector<TypeAST*> args;

    if (!stream.check(TokenType::LESS)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '<', got '", stream.peekValue(), "'");
        return ctx.arena.makeBuilder<TypeAST*>().build();
    }
    stream.consume(); // Consume '<'

    if (stream.check(TokenType::GREATER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected generic argument type, got '>'");
        stream.consume(); // Consume '>'
        return ctx.arena.makeBuilder<TypeAST*>().build();
    }

    ScopedContext guard(ctx, SyntacticContext::GenericArgs, stream.currentLoc());

    bool isFirst = true;

    while (!stream.isAtEnd() && !stream.check(TokenType::GREATER)) {
        // Handle commas before this argument (list-level comma handling)
        handleCommaGap(stream, ctx, "generic argument", isFirst);
        isFirst = false;
        
        // Parse single type (NO comma handling inside)
        TypeAST* type = parseType(stream, ctx);
        if (type) {
            args.push_back(type);
        } else {
            if (synchronizeToContext(stream, ctx) == SyncOutcome::Abandoned) {
                break;
            }
        }
    }

    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '>' to close generic argument list");
    } else {
        stream.consume(); // Consume '>'
    }

    auto builder = ctx.arena.makeBuilder<TypeAST*>();
    for (auto* arg : args) {
        builder.push_back(arg);
    }

    return builder.build();
}

// =============================================================================
// parseParamList - LIST LEVEL - handles commas
// =============================================================================

/// @brief Parse a parameter list WITH names.
/// 
/// Grammar: '(' [ IDENTIFIER type { ',' IDENTIFIER type } ] ')'
/// 
/// This is used for the leading cluster of function declarations and anonymous functions.
/// Parameter names are REQUIRED.
/// 
/// Variadic parameters are denoted by `...` after the name: `args ...int`
/// 
/// @param stream The token stream
/// @param ctx The parsing context
/// @param outParams Output: the parsed parameters with names
/// @param outIsVariadic Output: true if the last parameter is variadic
/// @return bool True if parsing succeeded
bool parseParamList(TokenStream& stream, ParserContext& ctx,
                    std::vector<ParamAST*>& outParams,
                    bool& outIsVariadic) {
    LOG_PARSER_DETAIL("parseParamList: parsing parameter list with names");
    
    outParams.clear();
    outIsVariadic = false;
    
    if (!stream.check(TokenType::LPAREN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '(', got '", stream.peekValue(), "'");
        return false;
    }
    stream.consume(); // Consume '('
    
    if (stream.check(TokenType::RPAREN)) {
        stream.consume(); // Consume ')'
        return true;
    }
    
    bool isFirst = true;
    bool hasVariadic = false;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN)) {
        // Handle commas before this parameter (list-level comma handling)
        handleCommaGap(stream, ctx, "parameter", isFirst);
        isFirst = false;
        
        // ─── Parse single parameter ──────────────────────────────────────
        SourceLocation loc = stream.currentLoc();
        
        // Parse 'const' modifier (always allowed)
        bool isConstParam = stream.match(TokenType::CONST);
        
        // ─── Parameter name is REQUIRED ────────────────────────────────
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected parameter name, got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
            if (!stream.check(TokenType::COMMA) && !stream.check(TokenType::RPAREN)) {
                return false;
            }
            continue;
        }
        
        Token nameTok = stream.consume();
        InternedString name = ctx.pool.intern(nameTok.value);
        
        // ─── Parse variadic marker ──────────────────────────────────────
        // In parameter lists, variadic is written as `...` after the name: `args ...int`
        bool isVariadicParam = stream.match(TokenType::VARIADIC);
        
        if (isVariadicParam) {
            if (hasVariadic) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, loc,
                                        "multiple variadic parameters are not allowed");
            }
            hasVariadic = true;
        }
        
        // ─── Parse the type ──────────────────────────────────────────────
        TypeAST* type = parseType(stream, ctx);
        if (!type) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "expected parameter type, got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
            if (!stream.check(TokenType::COMMA) && !stream.check(TokenType::RPAREN)) {
                return false;
            }
            continue;
        }
        
        // ─── If variadic, wrap the type in [*]T ──────────────────────────
        // The grammar says: `...T` is a parameter that collects arguments into [*]T
        // So the parameter type should be [*]T, not T
        TypeAST* finalType = type;
        if (isVariadicParam) {
            // Store as [*]T (dynamic array) - the element type is 'type'
            finalType = ctx.arena.make<ArrayTypeAST>(ArrayKind::Dynamic, 0, type);
        }
        
        // ─── Create ParamAST ──────────────────────────────────────────────
        // Parameters are always `let` by default (mutable bindings)
        ParamAST* param = ctx.arena.make<ParamAST>(name, finalType, isVariadicParam, isConstParam);
        param->loc = loc;
        outParams.push_back(param);
        
        if (isVariadicParam && stream.check(TokenType::COMMA)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                    "variadic parameter must be the last parameter");
        }
    }
    
    if (hasVariadic) {
        outIsVariadic = true;
    }
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ')' to close parameter list");
        return false;
    } else {
        stream.consume(); // Consume ')'
    }
    
    LOG_PARSER_DETAIL("parseParamList: ", outParams.size(), " parameters, variadic=", outIsVariadic);
    return true;
}

/// @brief Parse a parameter type list for a function type.
///        These are unnamed: just types, no parameter names.
/// 
/// Grammar: '(' [ type { ',' type } ] ')'
/// 
/// Variadic parameters are denoted by `...` before the type: `...T`
/// 
/// @param stream The token stream
/// @param ctx The parsing context
/// @param outParamTypes Output: the parameter types
/// @param outIsVariadic Output: true if the last parameter is variadic
/// @return bool True if parsing succeeded
bool parseParamTypeList(TokenStream& stream, ParserContext& ctx,
                        std::vector<TypeAST*>& outParamTypes,
                        bool& outIsVariadic) {
    LOG_PARSER_DETAIL("parseParamTypeList: parsing parameter type list");
    
    outIsVariadic = false;
    outParamTypes.clear();
    
    if (!stream.check(TokenType::LPAREN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '(', got '", stream.peekValue(), "'");
        return false;
    }
    stream.consume(); // Consume '('
    
    if (stream.check(TokenType::RPAREN)) {
        stream.consume(); // Consume ')'
        return true;
    }
    
    bool isFirst = true;
    bool hasVariadic = false;
    size_t paramCount = 0;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN)) {
        // Handle commas before this parameter
        handleCommaGap(stream, ctx, "parameter type", isFirst);
        isFirst = false;
        
        SourceLocation loc = stream.currentLoc();
        
        // ─── Check for variadic marker ──────────────────────────────────
        // In function types, variadic is written as `...T` (the type comes after)
        bool isVariadicParam = stream.match(TokenType::VARIADIC);
        
        if (isVariadicParam) {
            if (hasVariadic) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, loc,
                                        "multiple variadic parameters are not allowed");
            }
            hasVariadic = true;
        }
        
        // ─── Parse the type ──────────────────────────────────────────────
        TypeAST* type = parseType(stream, ctx);
        if (!type) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "expected parameter type, got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
            if (!stream.check(TokenType::COMMA) && !stream.check(TokenType::RPAREN)) {
                return false;
            }
            continue;
        }
        
        // If variadic, the type is the element type - the variadic parameter
        // collects arguments into [*]T in the semantic phase.
        // For the AST, we store the type as-is and mark the function as variadic.
        outParamTypes.push_back(type);
        paramCount++;
        
        if (isVariadicParam && stream.check(TokenType::COMMA)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                    "variadic parameter must be the last parameter");
        }
    }
    
    // ─── Validate variadic position ────────────────────────────────────
    if (hasVariadic) {
        // Variadic must be the LAST parameter
        if (paramCount > 0) {
            // The variadic is at position paramCount-1 (it was the last parameter parsed)
            // But we also need to ensure no parameters came after it, which is
            // already enforced by the loop.
            outIsVariadic = true;
        }
    }
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ')' to close parameter type list");
        return false;
    } else {
        stream.consume(); // Consume ')'
    }
    
    LOG_PARSER_DETAIL("parseParamTypeList: ", paramCount, " parameter types, variadic=", outIsVariadic);
    return true;
}

/// @brief Parse a bound cluster: one or more groups with parameter names.
/// 
/// Grammar: bound_cluster = bound_group { bound_group }
///          bound_group = '(' [ bound_param_list ] ')'
///          bound_param = IDENTIFIER type
/// 
/// @param stream The token stream
/// @param ctx The parsing context
/// @param outParams Output: parameter AST nodes (with names)
/// @param outTypes Output: parameter types (for the FuncTypeAST)
/// @param outIsVariadic Output: true if the last parameter is variadic
void parseBoundCluster(TokenStream& stream, ParserContext& ctx,
                       std::vector<ParamAST*>& outParams,
                       std::vector<TypeAST*>& outTypes,
                       bool& outIsVariadic) {
    LOG_PARSER_DETAIL("parseBoundCluster");
    
    outIsVariadic = false;
    bool clusterHasVariadic = false;
    
    while (stream.check(TokenType::LPAREN) && !stream.isAtEnd()) {
        std::vector<ParamAST*> groupParams;
        bool groupIsVariadic = false;
        parseParamList(stream, ctx, groupParams, groupIsVariadic);
        
        for (auto* p : groupParams) {
            outParams.push_back(p);
            outTypes.push_back(p->type);
        }
        
        if (groupIsVariadic) {
            // Variadic in the leading cluster - check that it's in the last group
            // of the entire function type (handled by the caller)
            clusterHasVariadic = true;
            outIsVariadic = true;
        }
    }
}

/// @brief Parse an unnamed cluster: one or more groups with only types.
/// 
/// Grammar: unnamed_cluster = unnamed_group { unnamed_group }
///          unnamed_group = '(' [ type { ',' type } ] ')'
/// 
/// @param stream The token stream
/// @param ctx The parsing context
/// @param outTypes Output: parameter types (for the FuncTypeAST)
/// @param outIsVariadic Output: true if the last parameter is variadic
void parseUnnamedCluster(TokenStream& stream, ParserContext& ctx,
                         std::vector<TypeAST*>& outTypes,
                         bool& outIsVariadic) {
    LOG_PARSER_DETAIL("parseUnnamedCluster");
    
    outIsVariadic = false;
    bool clusterHasVariadic = false;
    
    while (stream.check(TokenType::LPAREN) && !stream.isAtEnd()) {
        std::vector<TypeAST*> groupTypes;
        bool groupIsVariadic = false;
        parseParamTypeList(stream, ctx, groupTypes, groupIsVariadic);
        
        for (auto* t : groupTypes) {
            outTypes.push_back(t);
        }
        
        if (groupIsVariadic) {
            // Variadic in an unnamed cluster must be the last parameter
            // of the ENTIRE function type
            if (stream.check(TokenType::ARROW)) {
                // If there's a '->' after this group, variadic is not last
                ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                        "variadic parameter must be in the last cluster of a function type");
            }
            clusterHasVariadic = true;
            outIsVariadic = true;
        }
    }
}

// =============================================================================
// parseArgList - CALL SITE - uses handleCommaGap like parseParamList
// =============================================================================

ArenaSpan<ExprAST*> parseArgList(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseArgList: parsing argument list");
    
    std::vector<ExprAST*> args;
    
    if (!stream.check(TokenType::LPAREN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '(', got '", stream.peekValue(), "'");
        return ctx.arena.makeBuilder<ExprAST*>().build();
    }
    stream.consume(); // Consume '('
    
    if (stream.check(TokenType::RPAREN)) {
        stream.consume(); // Consume ')'
        return ctx.arena.makeBuilder<ExprAST*>().build();
    }

    bool isFirst = true;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN)) {
        // Handle commas before this argument (list-level comma handling)
        handleCommaGap(stream, ctx, "argument", isFirst);
        isFirst = false;
        
        ExprAST* arg = parseExpr(stream, ctx);
        if (arg) {
            args.push_back(arg);
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "expected argument expression, got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
            if (!stream.check(TokenType::COMMA) && !stream.check(TokenType::RPAREN)) {
                break;
            }
            continue;
        }
    }
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ')' to close argument list");
    } else {
        stream.consume(); // Consume ')'
    }
    
    auto builder = ctx.arena.makeBuilder<ExprAST*>();
    for (auto* arg : args) {
        builder.push_back(arg);
    }
    
    return builder.build();
}

// =============================================================================
// parseImportPath
// =============================================================================

std::vector<InternedString> parseImportPath(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseImportPath: parsing import path");
    
    std::vector<InternedString> pathParts;
    
    while (!stream.isAtEnd()) {
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected identifier in import path, got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::AS, TokenType::SEMICOLON);
            break;
        }
        
        Token tok = stream.consume();
        pathParts.push_back(ctx.pool.intern(tok.value));
        
        if (stream.check(TokenType::DOT)) {
            stream.consume(); // Consume '.'
            continue;
        }
        
        break;
    }
    
    std::string fullPath;
    for (size_t i = 0; i < pathParts.size(); ++i) {
        if (i > 0) fullPath += ".";
        fullPath += std::string(ctx.pool.lookup(pathParts[i]));
    }
    
    return pathParts;
}

} // namespace parser