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
#include "diagnostics/Diagnostic.hpp"

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
