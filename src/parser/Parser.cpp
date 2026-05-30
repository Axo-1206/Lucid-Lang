/**
 * @file Parser.cpp
 * @brief Core parsing infrastructure and top-level dispatch.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements the foundational components of the Luc parser:
 *   - TokenStream: Safe token consumption with comment skipping
 *   - Error handling: Panic-mode recovery and per-function error reporting
 *   - List helpers: Temporary vector builders for expression/type/stmt/param lists
 *   - Attribute parsing: @inline, @deprecated, @extern, etc.
 *   - Top-level dispatch: parse() and parseDeclaration()
 * 
 * ## TokenStream Purpose
 * 
 * TokenStream wraps the raw token vector and provides:
 *   - Automatic comment skipping (LINE_COMMENT, DOC_COMMENT are invisible to grammar)
 *   - Position manipulation (getPos/setPos) for lookahead and error recovery
 *   - Safe token consumption with consume() that reports errors on mismatch
 * 
 * ## Error Recovery Strategy
 * 
 *   - **Panic mode**: synchronize() skips tokens until a statement or declaration
 *     boundary (IF, FOR, WHILE, LET, CONST, RBRACE, etc.)
 *   - **Custom recovery**: synchronizeTo() stops at caller-specified token types
 *   - **Per-function recovery**: On parse failure, functions may return nullptr
 *     and the caller calls synchronize()
 * 
 * ## List Helpers
 * 
 * These functions return std::vector (temporary) for collecting sequences:
 *   - parseExprList()   : comma-separated expressions until end token
 *   - parseTypeList()   : comma-separated types until end token
 *   - parseStmtList()   : statements until end token
 *   - parseParamList()  : function parameters until ')'
 * 
 * The caller is responsible for converting to ArenaSpan using SpanBuilder.
 * 
 * ## Attribute Parsing
 * 
 * Attributes use the syntax @name or @name(args). They are collected as
 * temporary vectors and attached to declarations via attachMetadata().
 * 
 * ## Top-Level Parsing Flow
 * 
 *   parse()
 *     ├── parsePackageDecl()     (mandatory, first non‑comment)
 *     └── while (!ts_.isAtEnd())
 *           └── parseTopLevelDecl()
 *                 └── parseDeclaration()
 *                       └── dispatches to specific parser (use, struct, etc.)
 * 
 * @see ParserDecl.cpp for declaration parsers
 * @see ParserExpr.cpp for expression parsers
 * @see ParserStmt.cpp for statement parsers
 * @see ParserType.cpp for type parsers
 */

#include "Parser.hpp"
#include "diagnostics/Diagnostic.hpp"   // new diagnostic API
#include "debug/DebugUtils.hpp"

#include <algorithm>
#include <cassert>
#include <sstream>

// ============================================================================
// TokenStream Implementation
// ============================================================================

const Token TokenStream::eofToken = {TokenType::EOF_TOKEN, "", 0, 0};

TokenStream::TokenStream(std::vector<Token> tokens, InternedString filePath)
    : tokens_(std::move(tokens)), filePath_(filePath) {}

size_t TokenStream::skipCommentsFrom(size_t start) const {
    size_t i = start;
    while (i < tokens_.size() &&
           (tokens_[i].type == TokenType::LINE_COMMENT ||
            tokens_[i].type == TokenType::DOC_COMMENT)) {
        ++i;
    }
    return i;
}

bool TokenStream::isAtEnd() const {
    return skipCommentsFrom(pos_) >= tokens_.size();
}

const Token& TokenStream::peek() const {
    size_t idx = skipCommentsFrom(pos_);
    if (idx >= tokens_.size()) return eofToken;
    return tokens_[idx];
}

Token TokenStream::advance() {
    size_t idx = skipCommentsFrom(pos_);
    if (idx >= tokens_.size()) {
        ++pos_;
        return eofToken;
    }
    const Token& tok = tokens_[idx];
    pos_ = idx + 1;
    return tok;
}

bool TokenStream::check(TokenType type) const {
    return !isAtEnd() && peek().type == type;
}

bool TokenStream::checkAny(std::initializer_list<TokenType> types) const {
    for (TokenType t : types)
        if (check(t)) return true;
    return false;
}

bool TokenStream::match(TokenType type) {
    if (!check(type)) return false;
    advance();
    return true;
}

bool TokenStream::matchAny(std::initializer_list<TokenType> types) {
    for (TokenType t : types)
        if (match(t)) return true;
    return false;
}

std::optional<Token> TokenStream::consumeIf(TokenType type) {
    if (check(type)) return advance();
    return std::nullopt;
}

Token TokenStream::consume(TokenType type, DiagCode code, const std::string& msg) {
    if (check(type)) return advance();
    SourceLocation loc = currentLoc();
    diagnostic::error(DiagnosticCategory::Syntax, filePath_, loc, code, {msg});
    // Return a dummy token for recovery
    return {type, "", 0, 0};
}

Token TokenStream::consume(TokenType type, const std::string& msg) {
    return consume(type, DiagCode::E2001, msg);
}

SourceLocation TokenStream::currentLoc() const {
    return locOf(peek());
}

SourceLocation TokenStream::locOf(const Token& tok) const {
    return SourceLocation(static_cast<uint32_t>(tok.line),
                          static_cast<uint32_t>(tok.column));
}

TokenType TokenStream::peekNextType() const {
    return peekNext().type;
}

const Token& TokenStream::peekNext() const {
    size_t idx = skipCommentsFrom(pos_);
    if (idx >= tokens_.size()) return eofToken;
    // Move one step forward (skip the current non‑comment token)
    size_t nextIdx = skipCommentsFrom(idx + 1);
    if (nextIdx >= tokens_.size()) return eofToken;
    return tokens_[nextIdx];
}

const Token& TokenStream::peekAt(size_t offset) const {
    // Raw access – caller must handle comments manually if needed.
    // This is used only in lookahead that already uses skipCommentsFrom.
    if (offset >= tokens_.size()) return eofToken;
    return tokens_[offset];
}

// ============================================================================
// Parser construction
// ============================================================================

Parser::Parser(std::vector<Token> tokens, InternedString filePath,
               StringPool& pool, ASTArena& arena)
    : ts_(std::move(tokens), filePath),
      filePath_(std::move(filePath)),
      pool_(pool), arena_(arena) {}

// ============================================================================
// Error handling
// ============================================================================

void Parser::error(const SourceLocation& loc, DiagCode code, const std::string& msg) {
    diagnostic::error(DiagnosticCategory::Syntax, filePath_, loc, code, {msg});
}

void Parser::errorAt(DiagCode code, const std::string& msg) {
    error(ts_.currentLoc(), code, msg);
}

void Parser::synchronize() {
    synchronizeTo({
        TokenType::AT_SIGN, TokenType::PACKAGE, TokenType::USE,
        TokenType::PUB, TokenType::EXPORT, TokenType::STRUCT,
        TokenType::ENUM, TokenType::TRAIT, TokenType::IMPL,
        TokenType::TYPE, TokenType::FROM, TokenType::LET,
        TokenType::CONST, TokenType::IF, TokenType::FOR,
        TokenType::WHILE, TokenType::DO, TokenType::RETURN,
        TokenType::BREAK, TokenType::CONTINUE, TokenType::MATCH,
        TokenType::SWITCH, TokenType::RBRACE
    });
}

void Parser::synchronizeTo(std::initializer_list<TokenType> stopTokens) {
    while (!ts_.isAtEnd()) {
        if (ts_.checkAny(stopTokens))
            return;
        ts_.advance();
    }
}

// ============================================================================
// Visibility
// ============================================================================

Visibility Parser::parseVisibility() {
    if (ts_.match(TokenType::PUB)) return Visibility::Package;
    if (ts_.match(TokenType::EXPORT)) return Visibility::Export;
    return Visibility::Private;
}

// ============================================================================
// Qualifiers
// ============================================================================

QualifierSet Parser::parseQualifiers() {
    QualifierSet qs;
    auto& registry = QualifierRegistry::instance();
    
    while (ts_.check(TokenType::TILDE)) {
        SourceLocation loc = ts_.currentLoc();
        ts_.advance();
        
        if (!ts_.check(TokenType::IDENTIFIER)) {
            error(loc, DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        InternedString name = pool_.intern(ts_.advance().value);
        
        const QualifierInfo* info = registry.lookup(name);
        // if (!info) {
        //     error(loc, DiagCode::E2010, 
        //           "unknown qualifier '~" + std::string(pool_.lookup(name)) + 
        //           "'; known qualifiers: " + registry.allNames());
        //     continue;
        // }
        
        qs.raw.push_back(name);
        qs.bitmask |= info->bit;
    }
    return qs;
}

// ============================================================================
// Module path parsing
// ============================================================================

std::vector<InternedString> Parser::parseModulePath() {
    std::vector<InternedString> path;
    if (!ts_.check(TokenType::IDENTIFIER)) return path;
    path.push_back(pool_.intern(ts_.advance().value));
    while (ts_.match(TokenType::DOT)) {
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected identifier after '.'");
            break;
        }
        path.push_back(pool_.intern(ts_.advance().value));
    }
    return path;
}

// ============================================================================
// List helpers (temporary vector builders)
// ============================================================================

std::vector<ExprPtr> Parser::parseExprList(TokenType endType) {
    std::vector<ExprPtr> list;
    while (!ts_.check(endType) && !ts_.isAtEnd()) {
        if (!list.empty() && !ts_.match(TokenType::COMMA))
            break;
        ExprPtr expr = parseExpr();
        if (expr) list.push_back(std::move(expr));
    }
    ts_.consume(endType, "expected '" + LucDebug::tokenTypeToString(endType) + "'");
    return list;
}

std::vector<TypePtr> Parser::parseTypeList(TokenType endType) {
    std::vector<TypePtr> list;
    while (!ts_.check(endType) && !ts_.isAtEnd()) {
        if (!list.empty() && !ts_.match(TokenType::COMMA))
            break;
        TypePtr ty = parseType();
        if (ty) list.push_back(std::move(ty));
    }
    ts_.consume(endType, "expected '" + LucDebug::tokenTypeToString(endType) + "'");
    return list;
}

std::vector<StmtPtr> Parser::parseStmtList(TokenType endType) {
    std::vector<StmtPtr> list;
    while (!ts_.check(endType) && !ts_.isAtEnd()) {
        StmtPtr stmt = parseStmt();
        if (stmt) list.push_back(std::move(stmt));
    }
    ts_.consume(endType, "expected '" + LucDebug::tokenTypeToString(endType) + "'");
    return list;
}

std::vector<ParamPtr> Parser::parseParamList() {
    std::vector<ParamPtr> list;
    while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
        if (!list.empty() && !ts_.match(TokenType::COMMA))
            break;
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected parameter name");
            break;
        }
        auto param = arena_.make<ParamAST>();
        param->name = pool_.intern(ts_.advance().value);
        param->isVariadic = ts_.match(TokenType::VARIADIC);
        param->type = parseType();
        if (param->type) list.push_back(std::move(param));
    }
    return list;
}

// ============================================================================
// Parameter Group Parsing
// ============================================================================
// 
// parseParamGroup() is called for each `( param_list )` in function types and
// function declarations.
// 
// Grammar: '(' [ param_list ] ')'
// 
// The function is implemented in Parser.cpp but documented here for context.
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at '('
// On exit:  positioned after the closing ')'
//
// ─── Return Value ─────────────────────────────────────────────────────────
//   Returns ParamGroup (std::vector<ParamPtr>) – temporary collection
//   Caller is responsible for converting to ArenaSpan using SpanBuilder
//
// ─── Loop Safety ──────────────────────────────────────────────────────────
// Uses parseParamList() which has its own loop safety.
//
// ─── Error Recovery ───────────────────────────────────────────────────────
// - Missing '(': consume() reports error
// - Missing ')': consume() reports error
//
// ============================================================================

ParamGroup Parser::parseParamGroup() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LPAREN, "expected '(' to start parameter group");
    
    std::vector<ParamPtr> params = parseParamList();
    
    ts_.consume(TokenType::RPAREN, "expected ')' to close parameter group");
    
    return params;
}

/**
 * @brief Parses the return list after '->' in function signatures.
 * 
 * Grammar:
 *   return_list := '(' [ return_type { ',' return_type } ] ')'   -- multiple
 *                | return_type                                    -- single
 * 
 * where `return_type` can itself be a function type with its own '->'.
 * 
 * Examples:
 *   -> int                                    -- single return
 *   -> (int, string)                          -- multi-return
 *   -> (x int) -> int                         -- function type
 *   -> (Result, int)                          -- multi-return with named type
 *   -> ((x int) -> int, string)               -- multi-return with function type
 *   -> () -> int                              -- zero-parameter function type
 * 
 * ─── Detection Strategy ─────────────────────────────────────────────────────
 *   To distinguish between multi-return `(Type, Type)` and function type
 *   `(param Type) -> Ret`, the parser:
 * 
 *   1. Looks inside the parentheses
 *   2. If it sees `IDENTIFIER` followed by a type start (parameter pattern)
 *      and later finds `->` after a complete parameter group, it parses as
 *      a function type
 *   3. Otherwise, it parses as a multi-return list
 * 
 *   This correctly handles ambiguous cases like `(Result, int)` where `Result`
 *   is a named type, not a parameter name.
 * 
 * ─── Function Type Detection Details ───────────────────────────────────────
 *   - Empty parentheses `()` are treated as a function type (zero parameters)
 *   - Single identifier without following type is NOT a function type
 *   - The presence of `->` after parameter group(s) confirms function type
 *   - Uses temporary parsing to test hypothesis without consuming tokens
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at the token after '->' (may be '(' or type start)
 * On exit:  positioned after the return list
 * 
 * ─── Return Value ─────────────────────────────────────────────────────────
 *   Returns ArenaSpan<TypePtr> – empty span indicates void function.
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing return type: reports error, returns empty span
 * - Missing ')' in multi-return: consume() reports error
 * - Invalid type in list: skips type, continues
 * 
 * @return ArenaSpan<TypePtr> – span of return types (empty = void)
 */
ArenaSpan<TypePtr> Parser::parseReturnList() {
    // Helper to check if a token type can start a type
    auto isTypeStart = [this](TokenType tt) -> bool {
        return isPrimitiveTypeToken(tt) ||
               tt == TokenType::IDENTIFIER ||
               tt == TokenType::LBRACKET ||
               tt == TokenType::AMPERSAND ||
               tt == TokenType::MUL ||
               tt == TokenType::LPAREN ||
               tt == TokenType::TILDE;
    };

    // Case 1: No parentheses → single return type
    if (!ts_.check(TokenType::LPAREN)) {
        TypePtr t = parseType();
        if (!t || t->isa<UnknownTypeAST>()) {
            errorAt(DiagCode::E2005, "expected return type after '->'");
            return ArenaSpan<TypePtr>();
        }
        auto builder = arena_.makeBuilder<TypePtr>();
        builder.push_back(std::move(t));
        return builder.build();
    }

    // We have '(' - need to determine if it's a function type or multi-return
    // Strategy: Peek inside and look for '->' after a complete parameter group
    
    size_t savedPos = ts_.getPos();
    const auto& tokens = ts_.getTokens();
    size_t tokenCount = ts_.getTokenCount();
    
    // Try to parse as a function type first
    // We'll attempt to parse a complete function type and see if it succeeds
    // without consuming tokens permanently
    
    // Create a temporary copy of the parser position
    size_t testPos = savedPos;
    
    // Skip comments at the start
    testPos = ts_.skipCommentsFrom(testPos);
    
    // Check if the '(' is followed by something that looks like a parameter group
    if (testPos < tokenCount && tokens[testPos].type == TokenType::LPAREN) {
        ++testPos; // consume '('
        testPos = ts_.skipCommentsFrom(testPos);
        
        // If we see a closing ')' immediately, this could be empty function type
        bool isEmptyParen = (testPos < tokenCount && tokens[testPos].type == TokenType::RPAREN);
        
        if (!isEmptyParen) {
            // Need to see if this looks like a parameter: IDENTIFIER followed by type start
            // vs just a type name (multi-return case)
            
            // Check if the next token is an identifier AND the token after that starts a type
            bool looksLikeParameter = false;
            if (testPos < tokenCount && tokens[testPos].type == TokenType::IDENTIFIER) {
                size_t afterIdent = testPos + 1;
                afterIdent = ts_.skipCommentsFrom(afterIdent);
                if (afterIdent < tokenCount && isTypeStart(tokens[afterIdent].type)) {
                    // This looks like a parameter: "name Type"
                    looksLikeParameter = true;
                }
            }
            
            if (looksLikeParameter) {
                // Try to parse a complete function type
                // We need to see if there's a '->' after the parameter group(s)
                
                // Simulate parsing up to the closing ')' of the first parameter group
                int parenDepth = 1;
                size_t paramEnd = testPos;
                while (paramEnd < tokenCount && parenDepth > 0) {
                    paramEnd = ts_.skipCommentsFrom(paramEnd);
                    if (paramEnd >= tokenCount) break;
                    TokenType tt = tokens[paramEnd].type;
                    if (tt == TokenType::LPAREN) ++parenDepth;
                    else if (tt == TokenType::RPAREN) --parenDepth;
                    ++paramEnd;
                }
                
                // Check after the closing ')' for '->' or more parameter groups
                size_t afterParams = paramEnd;
                afterParams = ts_.skipCommentsFrom(afterParams);
                
                // Look for additional parameter groups or '->'
                bool hasArrow = false;
                while (afterParams < tokenCount) {
                    afterParams = ts_.skipCommentsFrom(afterParams);
                    if (afterParams >= tokenCount) break;
                    TokenType tt = tokens[afterParams].type;
                    if (tt == TokenType::ARROW) {
                        hasArrow = true;
                        break;
                    }
                    if (tt == TokenType::LPAREN) {
                        // More parameter groups - continue scanning
                        ++afterParams;
                        continue;
                    }
                    break;
                }
                
                if (hasArrow) {
                    // This is definitely a function type
                    TypePtr funcType = parseFuncType();
                    if (!funcType || funcType->isa<UnknownTypeAST>()) {
                        errorAt(DiagCode::E2005, "expected function type");
                        return ArenaSpan<TypePtr>();
                    }
                    auto builder = arena_.makeBuilder<TypePtr>();
                    builder.push_back(std::move(funcType));
                    return builder.build();
                }
            }
        } else {
            // Empty parentheses - this is a function type with zero parameters
            // Check if there's a '->' after the closing ')'
            size_t afterParen = testPos + 1; // skip the ')'
            afterParen = ts_.skipCommentsFrom(afterParen);
            if (afterParen < tokenCount && tokens[afterParen].type == TokenType::ARROW) {
                TypePtr funcType = parseFuncType();
                if (!funcType || funcType->isa<UnknownTypeAST>()) {
                    errorAt(DiagCode::E2005, "expected function type");
                    return ArenaSpan<TypePtr>();
                }
                auto builder = arena_.makeBuilder<TypePtr>();
                builder.push_back(std::move(funcType));
                return builder.build();
            }
        }
    }
    
    // Not a function type (or detection inconclusive) → parse as multi-return list
    // Restore position and parse as multi-return
    ts_.setPos(savedPos);
    ts_.advance(); // consume '('
    
    std::vector<TypePtr> types;
    
    while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
        if (!types.empty() && !ts_.match(TokenType::COMMA)) {
            // Missing comma - try to continue
            if (ts_.check(TokenType::RPAREN)) break;
            errorAt(DiagCode::E2001, "expected ',' between return types");
            // Skip to next comma or closing parenthesis
            while (!ts_.isAtEnd() && !ts_.check(TokenType::COMMA) && !ts_.check(TokenType::RPAREN)) {
                ts_.advance();
            }
            continue;
        }
        
        if (ts_.check(TokenType::RPAREN)) break;
        
        size_t typeSavedPos = ts_.getPos();
        TypePtr t = parseType();
        if (ts_.getPos() == typeSavedPos) {
            errorAt(DiagCode::E2005, "expected return type");
            // Skip to closing parenthesis
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RPAREN)) {
                ts_.advance();
            }
            break;
        }
        if (t && !t->isa<UnknownTypeAST>()) {
            types.push_back(std::move(t));
        }
    }
    
    ts_.consume(TokenType::RPAREN, "expected ')' to close return type list");
    
    auto builder = arena_.makeBuilder<TypePtr>();
    for (auto& t : types) builder.push_back(std::move(t));
    return builder.build();
}

// ============================================================================
// Generic Parameters and Arguments Helpers
// ============================================================================

ArenaSpan<GenericParamPtr> Parser::parseGenericParams() {
    if (!ts_.match(TokenType::LESS)) return ArenaSpan<GenericParamPtr>();
    
    if (ts_.check(TokenType::GREATER)) {
        ts_.advance();
        return ArenaSpan<GenericParamPtr>();
    }
    
    std::vector<GenericParamPtr> params;
    do {
        if (ts_.match(TokenType::COMMA)) continue;
        GenericParamPtr gp = parseGenericParam();
        if (gp) params.push_back(std::move(gp));
    } while (!ts_.check(TokenType::GREATER) && !ts_.isAtEnd());
    
    ts_.consume(TokenType::GREATER, "expected '>' after generic parameters");
    
    auto builder = arena_.makeBuilder<GenericParamPtr>();
    for (auto& p : params) builder.push_back(std::move(p));
    return builder.build();
}

ArenaSpan<TypePtr> Parser::parseGenericArgs() {
    if (!ts_.match(TokenType::LESS)) return ArenaSpan<TypePtr>();
    
    if (ts_.check(TokenType::GREATER)) {
        ts_.advance();
        return ArenaSpan<TypePtr>();
    }
    
    std::vector<TypePtr> args;
    do {
        if (ts_.match(TokenType::COMMA)) continue;
        
        size_t savedPos = ts_.getPos();
        TypePtr arg = parseType();
        if (ts_.getPos() == savedPos) {
            errorAt(DiagCode::E2005, "expected type in generic argument list");
            if (!ts_.isAtEnd()) ts_.advance();
            break;
        }
        args.push_back(std::move(arg));
    } while (!ts_.check(TokenType::GREATER) && !ts_.isAtEnd());
    
    ts_.consume(TokenType::GREATER, "expected '>' to close generic arguments");
    
    auto builder = arena_.makeBuilder<TypePtr>();
    for (auto& a : args) builder.push_back(std::move(a));
    return builder.build();
}

ArenaSpan<ExprPtr> Parser::parseArgList() {
    std::vector<ExprPtr> args;
    int consecutiveErrors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 5;

    while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
        if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
            errorAt(DiagCode::E2002, "too many consecutive errors in argument list; skipping to ')'");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RPAREN)) ts_.advance();
            break;
        }

        size_t savedPos = ts_.getPos();
        ExprPtr arg = parseExpr();

        if (ts_.getPos() == savedPos) {
            errorAt(DiagCode::E2008, "expected argument expression");
            if (!ts_.isAtEnd()) ts_.advance();
            consecutiveErrors++;
            if (ts_.check(TokenType::COMMA)) ts_.advance();
            continue;
        }

        consecutiveErrors = 0;
        args.push_back(std::move(arg));

        if (ts_.check(TokenType::RPAREN)) break;
        if (!ts_.match(TokenType::COMMA)) {
            errorAt(DiagCode::E2001, "expected ',' after argument");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::COMMA) && !ts_.check(TokenType::RPAREN)) {
                ts_.advance();
            }
            if (ts_.check(TokenType::COMMA)) ts_.advance();
            break;
        }
    }
    
    auto builder = arena_.makeBuilder<ExprPtr>();
    for (auto& a : args) builder.push_back(std::move(a));
    return builder.build();
}

// ============================================================================
// Doc Comment Harvesting
// ============================================================================
// 
// harvestDocComment() scans backward from the current position to collect
// documentation comments attached to the next declaration.
// 
// Attachment priority (from LUC_GRAMMAR.md):
//   1. Block doc comment   : /-- ... --/ immediately above declaration
//   2. Stacked line comments: consecutive -- lines above declaration
//   3. Trailing comment    : -- comment on the same line as declaration
// 
// ─── Scanning Logic ────────────────────────────────────────────────────────
//   - Starts from pos-1 and moves backward
//   - Stops at first non-comment token
//   - Collects LINE_COMMENT tokens that are contiguous and end on declLine-1
//   - DOC_COMMENT token immediately above (line difference ≤ 1) becomes block
//   - LINE_COMMENT on the same line becomes trailing
// 
// ─── Priority Resolution ────────────────────────────────────────────────────
//   Block doc > Stacked lines > Trailing comment
// ============================================================================

std::optional<DocComment> Parser::harvestDocComment() {
    const auto& tokens = ts_.getTokens();
    size_t pos = ts_.getPos();
    
    if (pos == 0) return std::nullopt;
    
    int declLine = ts_.peek().line;
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
    
    if (blockText.has_value()) {
        return DocComment{pool_.intern(*blockText), DocCommentForm::Block};
    }
    if (!stackedLines.empty()) {
        std::string combined;
        for (int i = static_cast<int>(stackedLines.size()) - 1; i >= 0; --i) {
            if (!combined.empty()) combined += '\n';
            combined += stackedLines[i];
        }
        return DocComment{pool_.intern(combined), DocCommentForm::Stacked};
    }
    if (trailingText.has_value()) {
        return DocComment{pool_.intern(*trailingText), DocCommentForm::Trailing};
    }
    
    return std::nullopt;
}

// ============================================================================
// Attribute Parsing
// ============================================================================
// 
// Attributes use the syntax: @name or @name(arg1, arg2, ...)
// 
// Supported arguments (from LUC_GRAMMAR.md):
//   - String literals     : "hello"
//   - Integer literals    : 42, 0xFF, 0b1010
//   - Boolean literals    : true, false
//   - Type identifiers    : TypeName (e.g., in @extern("malloc", C))
// 
// Attributes are collected as temporary vectors and attached to declarations
// via attachMetadata() in parseDeclaration(). The semantic pass validates
// attribute names and arguments.
// 
// Example:
//   @inline
//   @deprecated("Use newAPI")
//   @extern("malloc") const malloc (size uint64) -> *uint8?
// ============================================================================

std::vector<AttributePtr> Parser::parseAttributes() {
    std::vector<AttributePtr> attrs;
    while (ts_.check(TokenType::AT_SIGN)) {
        AttributePtr attr = parseAttribute();
        if (attr) attrs.push_back(std::move(attr));
    }
    return attrs;
}

AttributePtr Parser::parseAttribute() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::AT_SIGN, "expected '@'");
    
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected attribute name after '@'");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    
    auto attr = arena_.make<AttributeAST>();
    attr->name = name;
    attr->loc = loc;

    if (ts_.match(TokenType::LPAREN)) {
        std::vector<AttributeArgPtr> args;
        while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
            if (!args.empty() && !ts_.match(TokenType::COMMA))
                break;
            AttributeArgPtr arg = parseAttributeArgLiteral();
            if (arg) args.push_back(std::move(arg));
        }
        ts_.consume(TokenType::RPAREN, "expected ')' after attribute arguments");
        
        auto builder = arena_.makeBuilder<AttributeArgPtr>();
        for (auto& a : args) builder.push_back(std::move(a));
        attr->args = builder.build();
    }
    return attr;
}

AttributeArgPtr Parser::parseAttributeArgLiteral() {
    if (ts_.check(TokenType::STRING_LITERAL)) {
        return arena_.make<AttributeArgAST>(AttributeArgKind::StringLit,
                                            pool_.intern(ts_.advance().value));
    }
    if (ts_.check(TokenType::INT_LITERAL) || ts_.check(TokenType::HEX_LITERAL) ||
        ts_.check(TokenType::BINARY_LITERAL)) {
        return arena_.make<AttributeArgAST>(AttributeArgKind::IntLit,
                                            pool_.intern(ts_.advance().value));
    }
    if (ts_.check(TokenType::TRUE) || ts_.check(TokenType::FALSE)) {
        return arena_.make<AttributeArgAST>(AttributeArgKind::BoolLit,
                                            pool_.intern(ts_.advance().value));
    }
    if (ts_.check(TokenType::IDENTIFIER)) {
        return arena_.make<AttributeArgAST>(AttributeArgKind::TypeIdent,
                                            pool_.intern(ts_.advance().value));
    }
    errorAt(DiagCode::E2002, "expected string, integer, boolean, or type identifier in attribute argument");
    return nullptr;
}

// ============================================================================
// Top-Level Parsing
// ============================================================================
// 
// parse() is the entry point for the entire parser. It:
//   1. Creates the ProgramAST root node
//   2. Parses the mandatory package declaration
//   3. Parses top-level declarations until EOF
//   4. Builds ArenaSpan<DeclPtr> for program->decls
// 
// ─── Error Recovery ────────────────────────────────────────────────────────
//   - Missing package declaration: inserts dummy node, calls synchronize()
//   - Failed top-level declaration: inserts UnknownDeclAST, calls synchronize()
//   - Never returns nullptr; errors are reported via DiagnosticEngine
// 
// ─── Declaration Dispatch ──────────────────────────────────────────────────
//   parseDeclaration() handles both top-level and local declarations.
//   It parses attributes, visibility, then dispatches to specific parsers.
// ============================================================================

ASTPtr<ProgramAST> Parser::parse() {
    auto program = arena_.make<ProgramAST>();
    program->filePath = filePath_;
    program->loc = ts_.currentLoc();

    std::vector<DeclPtr> decls;

    // Package declaration
    if (!ts_.check(TokenType::PACKAGE)) {
        errorAt(DiagCode::E2001, "expected 'package' declaration at start of file");
        synchronize();
        auto dummy = arena_.make<PackageDeclAST>(pool_.intern("<unknown>"));
        dummy->loc = ts_.currentLoc();
        program->packageName = pool_.intern("<error>");
        decls.push_back(std::move(dummy));
    } else {
        auto pkg = parsePackageDecl();
        if (pkg) {
            program->packageName = pkg->name;
            decls.push_back(std::move(pkg));
        } else {
            auto dummy = arena_.make<PackageDeclAST>(pool_.intern("<error>"));
            dummy->loc = ts_.currentLoc();
            program->packageName = pool_.intern("<error>");
            decls.push_back(std::move(dummy));
        }
    }

    // Top-level declarations
    while (!ts_.isAtEnd()) {
        auto doc = harvestDocComment();
        DeclPtr decl = parseTopLevelDecl();
        if (decl) {
            if (doc) decl->doc = std::move(doc);
            decls.push_back(std::move(decl));
        } else {
            synchronize();
        }
    }

    // Build the ArenaSpan for program->decls
    auto builder = arena_.makeBuilder<DeclPtr>();
    for (auto& d : decls) builder.push_back(std::move(d));
    program->decls = builder.build();

    return program;
}

DeclPtr Parser::parseTopLevelDecl() {
    return parseDeclaration(DeclContext::TopLevel);
}

/**
 * @brief Parses any declaration (top-level or local).
 * 
 * Grammar (simplified):
 *   declaration := [ '@' attribute ]* [ 'pub' | 'export' ]? actual_decl
 * 
 *   actual_decl := use_decl | struct_decl | enum_decl | trait_decl
 *                | impl_decl | from_decl | type_decl | var_decl | func_decl
 * 
 * ─── Context Rules ─────────────────────────────────────────────────────────
 *   TopLevel: All declaration types allowed, visibility modifiers allowed
 *   Local:    Only struct, enum, trait, impl, from, type, let, const allowed
 *             Visibility modifiers (pub/export) are FORBIDDEN (E2014)
 *             'use' declarations are FORBIDDEN (E2006)
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Invalid use in local context: reports error, skips entire use declaration
 * - Visibility in local context: reports error, continues
 * - Unknown declaration: reports error, returns nullptr (caller synchronizes)
 * 
 * @param ctx Whether this declaration appears at top level or inside a block
 * @return DeclPtr – parsed declaration node, or nullptr on error
 */
DeclPtr Parser::parseDeclaration(DeclContext ctx) {
    // Parse attributes (temporary vector)
    std::vector<AttributePtr> attrs = parseAttributes();

    // Parse visibility (top-level only)
    Visibility vis = Visibility::Private;
    if (ctx == DeclContext::TopLevel) {
        vis = parseVisibility();
    } else {
        if (ts_.checkAny({TokenType::PUB, TokenType::EXPORT})) {
            errorAt(DiagCode::E2014, "visibility modifier not allowed in local declaration");
            ts_.advance();
        }
    }

    // Dispatch to specific declaration parser
    DeclPtr decl;
    
    if (ts_.check(TokenType::USE)) {
        // REJECT 'use' inside local contexts
        if (ctx == DeclContext::Local) {
            errorAt(DiagCode::E2006, "'use' declaration is not allowed inside a block; use declarations must be at top level");
            // Skip the 'use' declaration to recover
            ts_.advance(); // consume 'use'
            
            // Skip the rest of the use declaration to avoid cascading errors
            while (!ts_.isAtEnd() && 
                   !ts_.checkAny({TokenType::SEMICOLON, TokenType::RBRACE, 
                                  TokenType::LET, TokenType::CONST, TokenType::IF,
                                  TokenType::FOR, TokenType::WHILE, TokenType::RETURN})) {
                ts_.advance();
            }
            return nullptr;
        }
        decl = parseUseDecl(vis);
    } else if (ts_.check(TokenType::STRUCT)) {
        decl = parseStructDecl(vis);
    } else if (ts_.check(TokenType::ENUM)) {
        decl = parseEnumDecl(vis);
    } else if (ts_.check(TokenType::TRAIT)) {
        decl = parseTraitDecl(vis);
    } else if (ts_.check(TokenType::IMPL)) {
        decl = parseImplDecl(vis);
    } else if (ts_.check(TokenType::FROM)) {
        decl = parseFromDecl(vis);
    } else if (ts_.check(TokenType::TYPE)) {
        decl = parseTypeAliasDecl(vis);
    } else if (ts_.checkAny({TokenType::LET, TokenType::CONST})) {
        Token kwTok = ts_.advance();
        DeclKeyword kw = (kwTok.type == TokenType::LET) ? DeclKeyword::Let : DeclKeyword::Const;
        if (looksLikeFuncDecl()) {
            decl = parseFuncDecl(kw, vis);
        } else {
            decl = parseVarDecl(vis);
        }
    } else {
        errorAt(DiagCode::E2002, "expected declaration");
        return nullptr;
    }

    if (decl) {
        // Attach attributes (convert vector to span)
        if (!attrs.empty()) {
            auto builder = arena_.makeBuilder<AttributePtr>();
            for (auto& a : attrs) builder.push_back(std::move(a));
            decl->attributes = builder.build();
        }
        decl->loc = ts_.currentLoc();
    }
    
    return decl;
}