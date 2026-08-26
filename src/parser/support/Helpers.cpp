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
#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/trace/Trace.hpp"
#include "debug/DebugUtils.hpp"

namespace parser {

std::optional<DocComment> harvestDocComment(TokenStream& stream, ParserContext& ctx) {
    Trace::detail("harvestDocComment: checking for doc comment");
    
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


/// NOTE: this function should synchronize to the start of declaration when error
///  happen
ArenaSpan<AttributeAST*> parseAttributes(TokenStream& stream, ParserContext& ctx) {
    Trace::detail("parseAttributes: checking for attributes");
    
    std::vector<AttributeAST*> attrs;
    
    if (!stream.match(TokenType::AT_SIGN)) {
        return ctx.arena.makeBuilder<AttributeAST*>().build();
    }
    
    if (!stream.match(TokenType::LBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '[' for attribute list, got '", stream.peekValue(), "'");
        synchronizeToDeclBoundary(stream, ctx, {TokenType::IDENTIFIER});
        return ctx.arena.makeBuilder<AttributeAST*>().build();
    }
    
    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACKET)) {
        // ─── Parse single attribute ──────────────────────────────────────
        AttributeAST* attr = parseAttribute(stream, ctx);
        attrs.push_back(attr);
        synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RBRACKET);
        if (!stream.match(TokenType::COMMA)) {
            if (stream.check(TokenType::RBRACKET)) {
                break;
            }
            // Hit EOF
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected ',' to separate attributes");
        }
    }
    
    // ─── Handle missing closing ']' ──────────────────────────────────────
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ']' to close attribute list");
    } else {
        stream.consume(); // Consume ']'
    }
    
    auto builder = ctx.arena.makeBuilder<AttributeAST*>();
    for (auto* attr : attrs) {
        builder.push_back(attr);
    }
    return builder.build();
}

// This function will not attempt to recover on error, the parseAttributes above
// will handle it
AttributeAST* parseAttribute(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();

    if (!stream.check(TokenType::IDENTIFIER)) {
        if (stream.check(TokenType::COMMA)) { // We do not use match here, the parseAttributes will handle this comma
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected attribute name before ','");
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected attribute name, got '", stream.peekValue(), "'");
        }

        auto* placeholder = ctx.arena.make<AttributeAST>();
        placeholder->loc = loc;
        placeholder->name = ctx.pool.intern("");
        placeholder->hasError = true;
    }
    
    Token nameTok = stream.consume();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    auto* attr = ctx.arena.make<AttributeAST>();
    attr->loc = loc;
    attr->name = name;
    
    // ─── Parse attribute arguments ──────────────────────────────────────────
    if (stream.match(TokenType::LPAREN)) {
        std::vector<LiteralExprAST*> args;
        
        while (
            !stream.isAtEnd() && 
            !stream.check(TokenType::RPAREN) && 
            !stream.check(TokenType::RBRACKET)
        ) {
            LiteralExprAST* arg = parseAttributeArgLiteral(stream, ctx);
            args.push_back(arg);
            if (arg->hasError) {
                attr->hasError = true;
            }
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN, TokenType::RBRACKET);
            if (!stream.match(TokenType::COMMA)) {
                if (stream.check(TokenType::RPAREN)) {
                    break;
                } else if (stream.check(TokenType::RBRACKET)) {
                    ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                        "expected ')' to close attribute literal arguments list");
                    attr->hasError = true;
                    break;
                }
                // Hit EOF
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                        "expected ',' to separate attribute literal arguments");
                attr->hasError = true;
            }
        }
        
        // ─── Handle missing closing ')' ──────────────────────────────────────
        if (stream.isAtEnd()) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected ')' to close attribute arguments");
        } else {
            stream.consume(); // Consume ')'
        }
        
        auto builder = ctx.arena.makeBuilder<LiteralExprAST*>();
        for (auto* arg : args) {
            builder.push_back(arg);
        }
        attr->args = builder.build();
    }
    return attr;
}

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
            if (stream.check(TokenType::COMMA)) { // we do not use match here, parseAttribute will handle the comma
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, loc,
                                        "expected attribute literal argument before ','");
            } else {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedLiteral, loc,
                                    "expected literal, got '", stream.peekValue(), "'");
            }
            auto* placeholder = ctx.arena.make<LiteralExprAST>(LiteralKind::Unknown, ctx.pool.intern(""));
                    placeholder->hasError = true;
                    placeholder->loc = loc;
            return placeholder;
    }
    
    auto* literal = ctx.arena.make<LiteralExprAST>(kind, value);
    literal->loc = loc;
    
    return literal;
}


/// Generic parameter declaration can appear in generic struct/trait/function
///
/// - for struct or trait the best recovery is stop at '{'
/// - for function declaration the best recovery is stop at '('
/// NOTE: parseGenericParamDecl will handle synchronize here
ArenaSpan<GenericParamDeclAST*> parseGenericParamDecls(TokenStream& stream, ParserContext& ctx) {
    std::vector<GenericParamDeclAST*> params;
    
    if (!stream.match(TokenType::LESS)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '<' for generic parameter list, got '", stream.peekValue(), "'");
        return ctx.arena.makeBuilder<GenericParamDeclAST*>().build();
    }
    
    if (stream.match(TokenType::GREATER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "empty generic list, please fill a parameter or remove empty '<>'");
        return ctx.arena.makeBuilder<GenericParamDeclAST*>().build();
    }
    
    while (!stream.isAtEnd() && !stream.check(TokenType::GREATER)) {
        GenericParamDeclAST* param = parseGenericParamDecl(stream, ctx);
        params.push_back(param);
        synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::GREATER, TokenType::LBRACE, TokenType::LPAREN, TokenType::SEMICOLON);
        if (!stream.match(TokenType::COMMA)) {
            if (stream.check(TokenType::GREATER)) {
                break;
            } else if (stream.checkAny(TokenType::LBRACE, TokenType::LPAREN, TokenType::SEMICOLON)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_IncompleteDeclaration, stream.currentLoc(),
                                        "incomplete generic parameter list");
                break;
            }
            // Hit EOF
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected ',' to separate generic parameters");
        }
    }
    
    // ─── Handle missing closing '>' ──────────────────────────────────────
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

GenericParamDeclAST* parseGenericParamDecl(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    /// NOTE: this case should only happen when the next token is ',' instead of a name
    /// ex: <param, , param> // got ',' instead of IDENTIFIER
    if (!stream.check(TokenType::IDENTIFIER)) {
        if (stream.check(TokenType::COMMA)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected generic parameter name before ','");
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected generic parameter name, got '", stream.peekValue(), "'");
        }

        auto* placeholder = ctx.arena.make<GenericParamDeclAST>(ctx.pool.intern(""));
        placeholder->hasError = true;
        
        return placeholder;
    }
    
    Token nameTok = stream.consume();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    auto* param = ctx.arena.make<GenericParamDeclAST>(name);
    param->loc = loc;
    
    // ─── Parse constraints ──────────────────────────────────────────────────
    if (stream.match(TokenType::COLON)) {
        std::vector<TypeAST*> constraints;
        bool hasConstraint = false;
        
        while (!stream.isAtEnd() && !stream.check(TokenType::COMMA) &&
               !stream.check(TokenType::GREATER) &&
               !stream.check(TokenType::LBRACE) &&
               !stream.check(TokenType::LPAREN) &&
               !stream.check(TokenType::SEMICOLON)) {
            
            // ─── Parse the constraint type ────────────────────────────────
            TypeAST* traitRef = parseNamedType(stream, ctx);
            if (traitRef) {
                constraints.push_back(traitRef);
                hasConstraint = true;

                if (!stream.match(TokenType::PLUS)) {
                    if (stream.check(TokenType::COMMA)) { // ',' is used to separate eacg generic parameter
                        break;
                    }
                    ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                            "expected '+' to separate trait constraints");
                    param->hasError = true;

                    synchronizeTo(stream, ctx, TokenType::PLUS, TokenType::COMMA, TokenType::GREATER, TokenType::LBRACE, TokenType::LPAREN, TokenType::SEMICOLON);
                    if (stream.match(TokenType::PLUS)) {
                        continue;
                    }
                    break;
                }
            } else {
                auto* placeholder = ctx.arena.make<UnknownTypeAST>();
                placeholder->hasError = true;
                constraints.push_back(placeholder);
                param->hasError = true;

                if (stream.match(TokenType::PLUS)) {
                    ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                            "expected trait constraint name before '+'");
                    continue;
                } 

                synchronizeTo(stream, ctx, TokenType::PLUS, TokenType::COMMA, TokenType::GREATER, TokenType::LBRACE, TokenType::LPAREN, TokenType::SEMICOLON);
                if (stream.match(TokenType::PLUS)) {
                    continue;
                }
                break;
            }
        }
        
        if (!hasConstraint) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected trait constraint after ':'");
            param->hasError = true;
            return param;
        }
        
        auto builder = ctx.arena.makeBuilder<TypeAST*>();
        for (auto* tr : constraints) {
            builder.push_back(tr);
        }
        param->constraints = builder.build();
    }
    
    return param;
}


ArenaSpan<TypeAST*> parseGenericArgs(TokenStream& stream, ParserContext& ctx) {
    std::vector<TypeAST*> args;

    if (!stream.match(TokenType::LESS)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '<' for generic argument list, got '", stream.peekValue(), "'");
        return ctx.arena.makeBuilder<TypeAST*>().build();
    }

    if (stream.match(TokenType::GREATER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected generic argument type, please fill a type or remove empty '<>'");
        return ctx.arena.makeBuilder<TypeAST*>().build();
    }

    while (!stream.isAtEnd() && !stream.check(TokenType::GREATER)) {
        TypeAST* type = parseType(stream, ctx);
        if (type) {
            args.push_back(type);

            if (!stream.match(TokenType::COMMA)) {
                if (stream.check(TokenType::GREATER)) {
                    break;
                }
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                        "expected ',' to separate generic arguments");

                synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::GREATER);
                if (stream.match(TokenType::COMMA)) {
                    continue;
                }
                break;
            }
        } else {
            auto* placeholder = ctx.arena.make<UnknownTypeAST>();
            placeholder->hasError = true;
            args.push_back(placeholder);

            // handle case 'arg,, arg' missing arg between ','
            if (stream.match(TokenType::COMMA)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                        "expected generic argument type before ','");
                continue;
            }

            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "failed to parse generic argument, got '", stream.peekValue(), "'");
            
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::GREATER);
            if (stream.match(TokenType::COMMA)) {
                continue;
            }
            break;
        }
    }

    // ─── Handle missing closing '>' ──────────────────────────────────────
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

ArenaSpan<ExprAST*> parseArgList(TokenStream& stream, ParserContext& ctx) {
    std::vector<ExprAST*> args;
    
    if (!stream.match(TokenType::LPAREN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '(' for argument list, got '", stream.peekValue(), "'");
        return ctx.arena.makeBuilder<ExprAST*>().build();
    }
    
    if (stream.match(TokenType::RPAREN)) {
        return ctx.arena.makeBuilder<ExprAST*>().build();
    }

    while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN)) {
        ExprAST* arg = parseExpr(stream, ctx);
        if (arg) {
            args.push_back(arg);

            if (!stream.match(TokenType::COMMA)) {
                if (stream.check(TokenType::RPAREN)) {
                    break;
                }
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                        "expected ',' to separate arguments");

                synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
                if (stream.match(TokenType::COMMA)) {
                    continue;
                }
                break;
            }
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                    "failed to parse argument, got '", stream.peekValue(), "'");
            
            auto* placeholder = ctx.arena.make<UnknownExprAST>();
            placeholder->hasError = true;
            args.push_back(placeholder);

            if (stream.match(TokenType::COMMA)) {
                ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedExpression, stream.currentLoc(),
                                        "expected argument expression before ','");
                continue;
            }

            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
            if (stream.match(TokenType::COMMA)) {
                continue;
            }
            break;
        }
    }
    
    // ─── Handle missing closing ')' ──────────────────────────────────────────
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

std::vector<ParamAST*> parseParamList(TokenStream& stream, ParserContext& ctx, bool allowNames) {
    std::vector<ParamAST*> params;
    
    if (!stream.match(TokenType::LPAREN)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '(' for parameter list, got '", stream.peekValue(), "'");
        return params;
    }
    
    if (stream.match(TokenType::RPAREN)) {
        return params;
    }

    while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN)) {
        ParamAST* param = parseSingleParameter(stream, ctx, allowNames);
        params.push_back(param);
        synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
        if (!stream.match(TokenType::COMMA)) {
            if (stream.match(TokenType::RPAREN)) {
                break;
            }
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                        "expected ',' to separate parameters");
        }
    }
    
    // ─── Handle missing closing ')' ──────────────────────────────────────────
    if (stream.isAtEnd()) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ')' to close parameter list");
    } else {
        stream.consume(); // Consume ')'
    }
    
    return params;
}

ParamAST* parseSingleParameter(TokenStream& stream, ParserContext& ctx, bool allowNames) {
    SourceLocation loc = stream.currentLoc();

    if (stream.check(TokenType::COMMA)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected parameter before ','");
        // Create a dummy ParamAST with empty name and unknown type
        auto* placeholder = ctx.arena.make<ParamAST>(
            ctx.pool.intern(""), 
            ctx.arena.make<UnknownTypeAST>(), 
            false, 
            false
        );
        placeholder->hasError = true;
        placeholder->loc = loc;
        return placeholder;
    }
    
    // ─── Parse const modifier (only allowed when names are allowed) ────────
    bool isConstParam = false;
    if (allowNames) {
        isConstParam = stream.match(TokenType::CONST);
    }
    
    // ─── Parse parameter name (if allowed) ────────────────────────────────
    InternedString name;
    bool hasName = false;
    
    if (allowNames) {
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected parameter name, got '", stream.peekValue(), "'");
            // Create a dummy ParamAST with empty name and unknown type
            auto* placeholder = ctx.arena.make<ParamAST>(
                ctx.pool.intern(""), 
                ctx.arena.make<UnknownTypeAST>(), 
                false, 
                isConstParam
            );
            placeholder->hasError = true;
            placeholder->loc = loc;
            return placeholder;
        }
        
        Token nameTok = stream.consume();
        name = ctx.pool.intern(nameTok.value);
        hasName = true;
    }
    
    // ─── Parse variadic modifier ───────────────────────────────────────────
    bool isVariadic = stream.match(TokenType::VARIADIC);
    
    // ─── Parse parameter type ──────────────────────────────────────────────
    TypeAST* type = parseType(stream, ctx);
    if (!type) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected parameter type, got '", stream.peekValue(), "'");
        
        // Create a dummy ParamAST with whatever name we have and unknown type
        auto* placeholder = ctx.arena.make<ParamAST>(
            name, 
            ctx.arena.make<UnknownTypeAST>(), 
            isVariadic, 
            isConstParam
        );
        placeholder->hasError = true;
        placeholder->loc = loc;
        return placeholder;
    }
    
    // ─── If variadic, wrap the type in [*]T ──────────────────────────────
    TypeAST* finalType = type;
    if (isVariadic) {
        finalType = ctx.arena.make<ArrayTypeAST>(ArrayKind::Dynamic, 0, type);
    }
    
    // ─── Create ParamAST ──────────────────────────────────────────────────
    auto* param = ctx.arena.make<ParamAST>(name, finalType, isVariadic, isConstParam);
    param->loc = loc;
    
    // ─── Validation ──────────────────────────────────────────────────────
    if (allowNames && !hasName) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, loc,
                                "expected parameter name before type");
    }
    
    if (!allowNames && hasName) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, loc,
                                "parameter name '", ctx.pool.lookup(name), 
                                "' is not allowed after first '->'");
    }
    
    if (isVariadic && stream.check(TokenType::COMMA)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_UnexpectedToken, stream.currentLoc(),
                                "variadic parameter must be the last parameter");
    }
    
    return param;
}

// =============================================================================
// parseImportPath
// =============================================================================

std::vector<InternedString> parseImportPath(TokenStream& stream, ParserContext& ctx) {
    std::vector<InternedString> pathParts;
    
    while (!stream.isAtEnd()) {
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected identifier in import path, got '", stream.peekValue(), "'");
            synchronizeToDeclBoundary(stream, ctx);
            break;
        }
        
        Token tok = stream.consume();
        pathParts.push_back(ctx.pool.intern(tok.value));
        
        if (stream.match(TokenType::DOT)) {
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