/**
 * @file Parser.cpp
 * 
 * @responsibility Entry point and basic primitives (peek, advance, synchronize).
 * 
 * @logic Root parse() loop and Doc-Comment harvesting.
 * 
 * @related src/diagnostics/DiagnosticEngine.hpp, DiagnosticCodes.hpp
 */

#include "Parser.hpp"
#include "diagnostics/DiagnosticEngine.hpp"

#include <algorithm>
#include <cassert>
#include <sstream>

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

Parser::Parser(std::vector<Token> tokens, DiagnosticEngine &dc,
               std::string filePath)
    : tokens_(std::move(tokens)), filePath_(std::move(filePath)), dc_(dc) {
    // The Lexer guarantees a trailing EOF_TOKEN. If for any reason the stream
    // is empty, push one now so all peek/advance logic remains branch-free.
    if (tokens_.empty() || tokens_.back().type != TokenType::EOF_TOKEN) {
        tokens_.push_back({TokenType::EOF_TOKEN, "EOF", 0, 0});
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Token stream primitives
// ─────────────────────────────────────────────────────────────────────────────

const Token &Parser::peek() const {
    return tokens_[pos_];
}

const Token &Parser::peekNext() const {
    return peekAt(1);
}

const Token &Parser::peekAt(std::size_t offset) const {
    std::size_t idx = pos_ + offset;
    if (idx >= tokens_.size())
        return tokens_.back(); // EOF_TOKEN
    return tokens_[idx];
}

Token Parser::advance() {
    Token t = tokens_[pos_];
    if (!isAtEnd())
        ++pos_;
    return t;
}

bool Parser::check(TokenType type) const {
    return peek().type == type;
}

bool Parser::checkAny(std::initializer_list<TokenType> types) const {
    for (TokenType t : types)
        if (check(t))
            return true;
    return false;
}

bool Parser::match(TokenType type) {
    if (!check(type))
        return false;
    advance();
    return true;
}

bool Parser::matchAny(std::initializer_list<TokenType> types) {
    for (TokenType t : types) {
        if (match(t))
            return true;
    }
    return false;
}

Token Parser::consume(TokenType type, DiagCode code, const std::string &msg) {
    if (check(type))
        return advance();
    errorAt(code, msg);
    // Return a synthetic token so callers can keep running without crashing.
    return {type, "", peek().line, peek().column};
}

Token Parser::consume(TokenType type, const std::string &msg) {
    return consume(type, DiagCode::E2001, msg);
}

bool Parser::isAtEnd() const {
    return peek().type == TokenType::EOF_TOKEN;
}

SourceLocation Parser::currentLoc() const {
    return locOf(peek());
}

SourceLocation Parser::locOf(const Token &tok) const {
    SourceLocation sl;
    sl.line = tok.line;
    sl.column = tok.column;
    sl.file = filePath_;
    return sl;
}

// ─────────────────────────────────────────────────────────────────────────────
// Error handling & recovery
// ─────────────────────────────────────────────────────────────────────────────

void Parser::error(const SourceLocation &loc, DiagCode code,
                   const std::string &msg) {
    dc_.report(DiagnosticSeverity::Error, DiagnosticCategory::Syntax, loc, code,
               msg);
}

void Parser::errorAt(DiagCode code, const std::string &msg) {
    error(currentLoc(), code, msg);
}

void Parser::synchronize() {
    // Advance until we land on a token that is likely to be the start of a
    // fresh statement or declaration.  These are the same tokens that
    // looksLikeDeclStart() and looksLikeStmtStart() test — we duplicate the
    // check here to avoid the extra call overhead inside the hot loop.
    while (!isAtEnd()) {
        TokenType t = peek().type;

        switch (t) {
        // Declaration starters — we are back at the top level.
        case TokenType::PACKAGE:
        case TokenType::USE:
        case TokenType::PUB:
        case TokenType::EXPORT:
        case TokenType::EXTERN:
        case TokenType::STRUCT:
        case TokenType::ENUM:
        case TokenType::TRAIT:
        case TokenType::IMPL:
        case TokenType::TYPE:
        case TokenType::FROM:
        // Statement / control-flow starters.
        case TokenType::LET:
        case TokenType::IMT:
        case TokenType::VAL:
        case TokenType::IF:
        case TokenType::FOR:
        case TokenType::WHILE:
        case TokenType::DO:
        case TokenType::RETURN:
        case TokenType::BREAK:
        case TokenType::CONTINUE:
        case TokenType::PARALLEL:
        case TokenType::MATCH:
        case TokenType::SWITCH:
        // A closing brace ends the current block — caller handles it.
        case TokenType::RBRACE:
            return;
        default:
            break;
        }

        advance();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Visibility & modifier helpers
// ─────────────────────────────────────────────────────────────────────────────

Visibility Parser::parseVisibility() {
    if (match(TokenType::PUB))
        return Visibility::Package;
    if (match(TokenType::EXPORT))
        return Visibility::Export;
    return Visibility::Private;
}

bool Parser::consumeExtern() {
    return match(TokenType::EXTERN);
}

// ─────────────────────────────────────────────────────────────────────────────
// Doc comment harvesting
//
// Strategy: walk backwards through the already-consumed tokens starting at
// pos_ - 1 (the token immediately before the current position).  The Lexer
// strips line comments and block comments as whitespace, but DOC_COMMENT
// tokens (/-- ... --/) are emitted into the stream.  Stacked -- comments
// are also emitted as individual tokens by the Lexer only if they are
// doc-comment candidates — see Lexer note: we rely on the lexer NOT
// stripping them when they form a consecutive run ending on (decl_line - 1).
//
// NOTE: The current Lexer.cpp strips single-line -- comments in
// skipWhitespace(), so we cannot recover them from the token stream
// retroactively.  harvestDocComment() therefore handles only the
// DOC_COMMENT token form (/-- ... --/).  Stacked -- support requires a
// Lexer change to emit them as DOC_COMMENT or a separate STACKED_DOC token;
// that change is tracked as a future task. For now the function finds the
// nearest DOC_COMMENT token immediately preceding the current position
// (allowing only whitespace/other-DOC_COMMENT tokens between it and pos_).
// ─────────────────────────────────────────────────────────────────────────────
std::optional<DocComment> Parser::harvestDocComment() {
    // Walk backwards to find a DOC_COMMENT token whose line is exactly
    // (current_token.line - N) for some small N, with no intervening
    // non-comment, non-whitespace tokens.
    //
    // Because the Lexer compresses whitespace we check line adjacency instead
    // of positional adjacency.

    if (pos_ == 0)
        return std::nullopt;

    // Current token line — the declaration starts here.
    int declLine = peek().line;

    // Scan backwards from pos_-1.
    for (std::size_t i = pos_; i > 0;) {
        --i;
        const Token &t = tokens_[i];

        if (t.type == TokenType::DOC_COMMENT) {
            // Accept only if the closing --/ is on the line immediately
            // before the declaration (or on the same line for trailing form).
            // We use a generous window of <= 2 lines to account for edge cases
            // where the lexer's line tracking places the closing delimiter one
            // line off.
            if (declLine - t.line <= 2) {
                return DocComment{t.value, DocCommentForm::Block};
            }
            // Found a doc comment but it's too far away — no attachment.
            return std::nullopt;
        }

        // Any non-doc-comment, non-whitespace token breaks the run.
        // (Whitespace is stripped by the Lexer, so any token here is real.)
        break;
    }

    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Disambiguation helpers
// ─────────────────────────────────────────────────────────────────────────────

bool Parser::looksLikeType() const {
    switch (peek().type) {
    // Primitive type keywords
    case TokenType::TYPE_BOOL:
    case TokenType::TYPE_BYTE:
    case TokenType::TYPE_SHORT:
    case TokenType::TYPE_INT:
    case TokenType::TYPE_LONG:
    case TokenType::TYPE_UBYTE:
    case TokenType::TYPE_USHORT:
    case TokenType::TYPE_UINT:
    case TokenType::TYPE_ULONG:
    case TokenType::TYPE_INT8:
    case TokenType::TYPE_INT16:
    case TokenType::TYPE_INT32:
    case TokenType::TYPE_INT64:
    case TokenType::TYPE_UINT8:
    case TokenType::TYPE_UINT16:
    case TokenType::TYPE_UINT32:
    case TokenType::TYPE_UINT64:
    case TokenType::TYPE_FLOAT:
    case TokenType::TYPE_DOUBLE:
    case TokenType::TYPE_DECIMAL:
    case TokenType::TYPE_STRING:
    case TokenType::TYPE_CHAR:
    case TokenType::TYPE_ANY:
    // Composite type starters
    case TokenType::LBRACKET:  // array types: [], [N], [*]
    case TokenType::AMPERSAND: // reference &T
    case TokenType::AT:        // raw pointer @T
        return true;

    // Named type: bare IDENTIFIER used as a type name.
    // We must be careful not to claim every IDENTIFIER is a type — this
    // helper is used in param parsing where IDENTIFIER IDENTIFIER means
    // "name type", so the second IDENTIFIER is looksLikeType() = true.
    case TokenType::IDENTIFIER:
        return true;

    // Function type starts with '(' — must distinguish from a grouped
    // expression.  A function type '(' has at least a ')' somewhere and
    // then optionally a return type.  We accept '(' conservatively here;
    // the caller is responsible for resolving the ambiguity with more
    // context.
    case TokenType::LPAREN:
        return true;

    default:
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// We have just consumed the declaration keyword (let/imt/val) and the
// function name IDENTIFIER.  Current position is AFTER the name.
//
// A function starts with optional generic params '<...>' then '('.
// A variable starts with a type annotation — which may also start with
// an IDENTIFIER (named type) or '[' etc.
//
// Strategy: skip over a potential '<...>' generic param block, then check
// whether the resulting token is '('.
// ─────────────────────────────────────────────────────────────────────────────
bool Parser::looksLikeFuncDecl() const {
    std::size_t i = pos_;

    // Skip generic params if present: < ... >
    // We match angle brackets by depth so nested generics don't confuse us.
    if (i < tokens_.size() && tokens_[i].type == TokenType::LESS) {
        int depth = 1;
        ++i;
        while (i < tokens_.size() && depth > 0) {
            if (tokens_[i].type == TokenType::LESS)
                ++depth;
            else if (tokens_[i].type == TokenType::GREATER)
                --depth;
            else if (tokens_[i].type == TokenType::EOF_TOKEN)
                break;
            ++i;
        }
    }

    // After optional generics, a '(' means function declaration.
    return (i < tokens_.size() && tokens_[i].type == TokenType::LPAREN);
}

// ─────────────────────────────────────────────────────────────────────────────
// Current token is IDENTIFIER, next token is '{'.
// We need to confirm that the '{' is a struct literal initialiser, not a
// block body.
//
// In Luc, a struct literal can appear:
//   - on the RHS of '=': let x Vec2 = Vec2 { ... }
//   - as a function argument
//   - in any expression position
//
// A bare IDENTIFIER '{' at the START of a statement is ambiguous — it
// could theoretically be a struct literal used as an expression statement.
// However, idiomatic Luc never writes a struct-literal as a standalone
// statement (there's nothing useful to do with the value), so we treat
// IDENTIFIER '{' at a pure statement-start position as a block only when
// the identifier is a keyword-like name.  For safety we simply return true
// here and let the caller (parsePrimaryExpr) decide based on its context.
//
// The caller already knows it's in expression position, so returning true
// is safe — if the struct literal parse fails the error will be reported
// there.
// ─────────────────────────────────────────────────────────────────────────────
bool Parser::looksLikeStructLiteral() const {

    if (!check(TokenType::IDENTIFIER))
        return false;

    // Peek ahead: after optional generic args, must find '{'.
    std::size_t i = pos_ + 1;

    // Skip generic args: < ... >
    if (i < tokens_.size() && tokens_[i].type == TokenType::LESS) {
        int depth = 1;
        ++i;
        while (i < tokens_.size() && depth > 0) {
            TokenType tt = tokens_[i].type;
            if (tt == TokenType::LESS)
                ++depth;
            else if (tt == TokenType::GREATER)
                --depth;
            else if (tt == TokenType::EOF_TOKEN)
                break;
            ++i;
        }
    }

    return (i < tokens_.size() && tokens_[i].type == TokenType::LBRACE);
}

bool Parser::looksLikeStmtStart() const {
    switch (peek().type) {
        case TokenType::LET:
        case TokenType::IMT:
        case TokenType::VAL:
        case TokenType::IF:
        case TokenType::FOR:
        case TokenType::WHILE:
        case TokenType::DO:
        case TokenType::RETURN:
        case TokenType::BREAK:
        case TokenType::CONTINUE:
        case TokenType::PARALLEL:
        case TokenType::MATCH:
        case TokenType::SWITCH:
        return true;
    default:
        // Any expression-starter is also a valid statement start.
        return looksLikeType() // covers type-conv calls like float(x)
               || check(TokenType::IDENTIFIER) || check(TokenType::INT_LITERAL) ||
               check(TokenType::FLOAT_LITERAL) ||
               check(TokenType::STRING_LITERAL) ||
               check(TokenType::RAW_STRING_LITERAL) ||
               check(TokenType::CHAR_LITERAL) || check(TokenType::HEX_LITERAL) ||
               check(TokenType::BINARY_LITERAL) || check(TokenType::TRUE) ||
               check(TokenType::FALSE) || check(TokenType::NIL) ||
               check(TokenType::MINUS)        // unary -
               || check(TokenType::NOT)       // unary not
               || check(TokenType::BIT_NOT)   // unary ~
               || check(TokenType::AMPERSAND) // reference &x
               || check(TokenType::AWAIT) ||
               check(TokenType::LPAREN);      // grouped expr or anon func
    }
}

bool Parser::looksLikeDeclStart() const {
    switch (peek().type) {
        case TokenType::PACKAGE:
        case TokenType::USE:
        case TokenType::PUB:
        case TokenType::EXPORT:
        case TokenType::EXTERN:
        case TokenType::STRUCT:
        case TokenType::ENUM:
        case TokenType::TRAIT:
        case TokenType::IMPL:
        case TokenType::TYPE:
        case TokenType::FROM:
        case TokenType::LET:
        case TokenType::IMT:
        case TokenType::VAL:
        return true;
    default:
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// parse()  — program root
//
// Grammar:
//   program := package_decl { top_level_decl }
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<ProgramAST> Parser::parse() {
    auto program = std::make_unique<ProgramAST>();
    program->filePath = filePath_;
    program->loc = currentLoc();

    // ── 1. package declaration ────────────────────────────────────────────────
    //
    // Must be the first non-comment token in the file.  harvestDocComment()
    // is called here because a doc comment may appear before 'package'.
    {
        std::optional<DocComment> pkgDoc = harvestDocComment();

        if (!check(TokenType::PACKAGE)) {
            errorAt(DiagCode::E2001,
                    "expected 'package' declaration at the start of the file");
            synchronize();
        }

        auto pkgDecl = parsePackageDecl();
        attachDoc(*pkgDecl, std::move(pkgDoc));
        program->packageName = pkgDecl->name;

        // Store the package decl as the first entry in decls so the AST is
        // self-contained and visitors can find it at decls[0].
        program->decls.push_back(std::move(pkgDecl));
    }

    // ── 2. top-level declarations ─────────────────────────────────────────────
    //
    // Loop until EOF.  Before each declaration attempt:
    //   a) harvest any preceding doc comment
    //   b) parse the declaration
    while (!isAtEnd()) {
        std::optional<DocComment> doc = harvestDocComment();
        DeclPtr decl = parseTopLevelDecl();
        if (decl) {
            // If the node supports doc comments, attach it.
            // (Simplified logic natively supported by the AST)
            // AST attachments logic typically happens here.
            program->decls.push_back(std::move(decl));
        } else {
            synchronize(); // Panic mode recovery
        }
    }

    return program;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseTopLevelDecl
// ─────────────────────────────────────────────────────────────────────────────

DeclPtr Parser::parseTopLevelDecl() {
    // ── 'extern' ──────────────────────────────────────────────────────────────
    // extern let name (params) [ret]  — no pub, no body
    if (check(TokenType::EXTERN)) {
        return parseExternDecl();
    }

    // ── Visibility modifier ───────────────────────────────────────────────────
    // All remaining top-level declarations may carry a visibility modifier.
    Visibility vis = parseVisibility();

    // ── 'use' ─────────────────────────────────────────────────────────────────
    // 'export use math.vec2' is supported, so use can take a visibility tier.
    if (check(TokenType::USE)) {
        return parseUseDecl(vis);
    }

    // ── 'struct' ──────────────────────────────────────────────────────────────
    if (check(TokenType::STRUCT)) {
        return parseStructDecl(vis);
    }

    // ── 'enum' ────────────────────────────────────────────────────────────────
    if (check(TokenType::ENUM)) {
        return parseEnumDecl(vis);
    }

    // ── 'trait' ───────────────────────────────────────────────────────────────
    if (check(TokenType::TRAIT)) {
        return parseTraitDecl(vis);
    }

    // ── 'impl' ────────────────────────────────────────────────────────────────
    if (check(TokenType::IMPL)) {
        return parseImplDecl(vis);
    }

    // ── 'from' ────────────────────────────────────────────────────────────────
    if (check(TokenType::FROM)) {
        return parseFromDecl(vis);
    }

    // ── 'type' ────────────────────────────────────────────────────────────────
    if (check(TokenType::TYPE)) {
        return parseTypeAliasDecl(vis);
    }

    // ── 'let' / 'imt' / 'val' ────────────────────────────────────────────────
    // Could be a variable declaration or a function declaration.
    // After consuming the keyword and name, looksLikeFuncDecl() inspects
    // whether a '(' follows (with optional generic params) to decide.
    if (checkAny({TokenType::LET, TokenType::IMT, TokenType::VAL})) {
        Token kwTok = advance();
        DeclKeyword kw;
        switch (kwTok.type) {
        case TokenType::LET:
            kw = DeclKeyword::Let;
            break;
        case TokenType::IMT:
            kw = DeclKeyword::Imt;
            break;
        default:
            kw = DeclKeyword::Val;
            break;
        }

        // After the keyword we expect the name.
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected name after '" + kwTok.value + "'");
            return nullptr;
        }

        // Do NOT consume the name yet — parseFuncDecl / parseVarDecl each read
        // it themselves so they can record the correct location.
        if (looksLikeFuncDecl()) {
            return parseFuncDecl(kw, vis);
        } else {
            return parseVarDecl(vis);
        }
    }

    // ── Unrecognised token ────────────────────────────────────────────────────
    if (vis != Visibility::Private) {
        // A modifier was consumed but no valid declaration followed.
        errorAt(DiagCode::E2002, "expected a declaration after modifier");
    } else {
        errorAt(DiagCode::E2002, "unexpected token '" + peek().value + "' at top level");
    }
    return nullptr;
}