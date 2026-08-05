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

LiteralExprPtr parseAttributeArgLiteral(TokenStream& stream, ParserContext& ctx) {
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

ArenaSpan<GenericParamDeclPtr> parseGenericParamDecls(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseGenericParamDecls: parsing generic parameters");
    
    std::vector<GenericParamDeclPtr> params;
    
    if (!stream.check(TokenType::LESS)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '<', got '", stream.peekValue(), "'");
        return ctx.arena.makeBuilder<GenericParamDeclPtr>().build();
    }
    stream.consume(); // Consume '<'
    
    if (stream.check(TokenType::GREATER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected generic parameter name, got '>'");
        stream.consume(); // Consume '>'
        return ctx.arena.makeBuilder<GenericParamDeclPtr>().build();
    }
    
    ScopedContext guard(ctx, SyntacticContext::GenericParams, stream.currentLoc());

    bool isFirst = true;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::GREATER)) {
        // Handle commas before this parameter (list-level comma handling)
        handleCommaGap(stream, ctx, "generic parameter", isFirst);
        isFirst = false;
        
        // Parse single generic parameter (NO comma handling inside)
        GenericParamDeclPtr param = parseGenericParamDecl(stream, ctx);
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
    
    auto builder = ctx.arena.makeBuilder<GenericParamDeclPtr>();
    for (auto* p : params) {
        builder.push_back(p);
    }
    
    return builder.build();
}

// =============================================================================
// parseGenericParamDecl - ITEM LEVEL - NO comma handling
// =============================================================================

GenericParamDeclPtr parseGenericParamDecl(TokenStream& stream, ParserContext& ctx) {
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

ArenaSpan<TypePtr> parseGenericArgs(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseGenericArgs: parsing generic arguments");

    std::vector<TypePtr> args;

    if (!stream.check(TokenType::LESS)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '<', got '", stream.peekValue(), "'");
        return ctx.arena.makeBuilder<TypePtr>().build();
    }
    stream.consume(); // Consume '<'

    if (stream.check(TokenType::GREATER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected generic argument type, got '>'");
        stream.consume(); // Consume '>'
        return ctx.arena.makeBuilder<TypePtr>().build();
    }

    ScopedContext guard(ctx, SyntacticContext::GenericArgs, stream.currentLoc());

    bool isFirst = true;

    while (!stream.isAtEnd() && !stream.check(TokenType::GREATER)) {
        // Handle commas before this argument (list-level comma handling)
        handleCommaGap(stream, ctx, "generic argument", isFirst);
        isFirst = false;
        
        // Parse single type (NO comma handling inside)
        TypePtr type = parseType(stream, ctx);
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

    auto builder = ctx.arena.makeBuilder<TypePtr>();
    for (auto* arg : args) {
        builder.push_back(arg);
    }

    return builder.build();
}

// =============================================================================
// parseParamList - LIST LEVEL - handles commas
// =============================================================================

std::vector<ParamPtr> parseParamList(TokenStream& stream, ParserContext& ctx, bool allowNames) {
    LOG_PARSER_DETAIL("parseParamList: parsing parameter list (allowNames=", allowNames, ")");
    
    std::vector<ParamPtr> params;
    
    if (!stream.check(TokenType::LPAREN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '(', got '", stream.peekValue(), "'");
        return params;
    }
    stream.consume(); // Consume '('
    
    if (stream.check(TokenType::RPAREN)) {
        stream.consume(); // Consume ')'
        return params;
    }

    bool isFirst = true;
    bool hasVariadic = false;
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN)) {
        // Handle commas before this parameter (list-level comma handling)
        handleCommaGap(stream, ctx, "parameter", isFirst);
        isFirst = false;
        
        // Parse single parameter (NO comma handling inside)
        SourceLocation loc = stream.currentLoc();
        
        bool isConst = stream.match(TokenType::CONST);
        
        InternedString name;
        bool hasName = false;
        
        if (allowNames) {
            if (!stream.check(TokenType::IDENTIFIER)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                        "expected parameter name, got '", stream.peekValue(), "'");
                synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
                if (!stream.check(TokenType::COMMA) && !stream.check(TokenType::RPAREN)) {
                    break;
                }
                continue;
            }
            
            Token nameTok = stream.consume();
            name = ctx.pool.intern(nameTok.value);
            hasName = true;
        }
        
        bool isVariadic = stream.match(TokenType::VARIADIC);
        
        // ─── Parse the type ──────────────────────────────────────────────────
        TypePtr type = parseType(stream, ctx);
        if (!type) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "expected parameter type, got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
            if (!stream.check(TokenType::COMMA) && !stream.check(TokenType::RPAREN)) {
                break;
            }
            continue;
        }
        
        // ─── If variadic, wrap the type in [*]T ──────────────────────────────
        // The grammar says: `...T` is a parameter that collects arguments into [*]T
        // So the parameter type should be [*]T, not T
        if (isVariadic) {
            // Store as [*]T (dynamic array) - the element type is 'type'
            type = ctx.arena.make<ArrayTypeAST>(ArrayKind::Dynamic, 0, type);
        }
        
        auto* param = ctx.arena.make<ParamAST>();
        param->loc = loc;
        param->name = name;
        param->type = type;
        param->isConst = isConst;
        param->isVariadic = isVariadic;
        params.push_back(param);
        
        if (allowNames && !hasName) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, loc,
                                    "expected parameter name before type");
        }
        
        if (!allowNames && hasName) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, loc,
                                    "parameter name '", ctx.pool.lookup(name), 
                                    "' is not allowed after first '->'");
        }
        
        if (isVariadic) {
            hasVariadic = true;
        }
        
        if (isVariadic && stream.check(TokenType::COMMA)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                    "variadic parameter must be the last parameter");
        }
    }
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ')' to close parameter list");
    } else {
        stream.consume(); // Consume ')'
    }
    
    return params;
}

// =============================================================================
// parseArgList - CALL SITE - SILENT on commas (semantic phase handles matching)
// =============================================================================

ArenaSpan<ExprAST*> parseArgList(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseArgList: parsing argument list");
    
    std::vector<ExprPtr> args;
    
    if (!stream.check(TokenType::LPAREN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '(', got '", stream.peekValue(), "'");
        return ctx.arena.makeBuilder<ExprPtr>().build();
    }
    stream.consume(); // Consume '('
    
    if (stream.check(TokenType::RPAREN)) {
        stream.consume(); // Consume ')'
        return ctx.arena.makeBuilder<ExprPtr>().build();
    }
    
    // Leading commas are silently skipped (not reported)
    stream.consumeTrailing(TokenType::COMMA);
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN)) {
        ExprPtr arg = parseExpr(stream, ctx);
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
        
        // Consecutive commas are silently skipped (not reported)
        stream.consumeTrailing(TokenType::COMMA);
    }
    
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ')' to close argument list");
    } else {
        stream.consume(); // Consume ')'
    }
    
    auto builder = ctx.arena.makeBuilder<ExprPtr>();
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