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
#include "debug/DebugUtils.hpp"

#include <algorithm>
#include <cassert>
#include <sstream>

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

Parser::Parser(std::vector<Token> tokens, DiagnosticEngine &dc,
               std::string filePath)
    : tokens_(std::move(tokens)), filePath_(std::move(filePath)), dc_(dc) {
    LUC_LOG_PARSER("=== Parser constructed ===");
    LUC_LOG_PARSER("\tToken count: " << tokens_.size());
    LUC_LOG_PARSER("\tFile path: " << filePath_);
    
    // The Lexer guarantees a trailing EOF_TOKEN. If for any reason the stream
    // is empty, push one now so all peek/advance logic remains branch-free.
    if (tokens_.empty() || tokens_.back().type != TokenType::EOF_TOKEN) {
        LUC_LOG_PARSER("\tWarning: No EOF_TOKEN found, adding one");
        tokens_.push_back({TokenType::EOF_TOKEN, "EOF", 0, 0});
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Token stream primitives
// ─────────────────────────────────────────────────────────────────────────────

const Token &Parser::peek() const {
    // Skip any LINE_COMMENT tokens at the current position — they are
    // transparent to the grammar; only harvestDocComment() reads them directly.
    std::size_t i = pos_;
    while (i < tokens_.size() && tokens_[i].type == TokenType::LINE_COMMENT)
        ++i;
    if (i >= tokens_.size())
        return tokens_.back();
    return tokens_[i];
}

const Token &Parser::peekNext() const {
    return peekAt(1);
}

const Token &Parser::peekAt(std::size_t offset) const {
    // Skip LINE_COMMENT tokens when computing the offset — they are invisible
    // to the grammar.  We need to find the Nth non-comment token from pos_.
    std::size_t found = 0;
    std::size_t i = pos_;
    // Start by skipping comments at pos_ itself (same as peek()).
    while (i < tokens_.size() && tokens_[i].type == TokenType::LINE_COMMENT)
        ++i;
    // Now advance 'offset' more non-comment tokens.
    while (offset > 0 && i < tokens_.size()) {
        ++i;
        while (i < tokens_.size() && tokens_[i].type == TokenType::LINE_COMMENT)
            ++i;
        --offset;
    }
    if (i >= tokens_.size())
        return tokens_.back();
    
    return tokens_[i];
}

Token Parser::advance() {
    // Find the current non-comment token index (same logic as peek()).
    std::size_t i = pos_;
    while (i < tokens_.size() && tokens_[i].type == TokenType::LINE_COMMENT)
        ++i;

    Token t = (i < tokens_.size()) ? tokens_[i] : tokens_.back();

    pos_ = (i < tokens_.size()) ? i + 1 : i;
    while (!isAtEnd() && tokens_[pos_].type == TokenType::LINE_COMMENT)
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
    if (check(type)) {
        return advance();
    }
    errorAt(code, msg);
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
    LUC_LOG_PARSER("ERROR at " << loc.line << ":" << loc.column 
                   << " - " << msg << " (code=" << static_cast<int>(code) << ")");
    dc_.report(DiagnosticSeverity::Error, DiagnosticCategory::Syntax, loc, code,
               msg);
}

void Parser::errorAt(DiagCode code, const std::string &msg) {
    error(currentLoc(), code, msg);
}

void Parser::synchronize() {
    LUC_LOG_PARSER("SYNCHRONIZE: skipping tokens until declaration/statement boundary");
    LUC_LOG_PARSER("\tCurrent token: '" << peek().value << "' type=" << static_cast<int>(peek().type));

    advance();
    while (!isAtEnd()) {
        TokenType t = peek().type;
        LUC_LOG_PARSER_EXTREME("\tChecking token: '" << peek().value << "' type=" << static_cast<int>(t));

        switch (t) {
        case TokenType::AT_SIGN:
        case TokenType::PACKAGE:
        case TokenType::USE:
        case TokenType::PUB:
        case TokenType::EXPORT:
        case TokenType::STRUCT:
        case TokenType::ENUM:
        case TokenType::TRAIT:
        case TokenType::IMPL:
        case TokenType::TYPE:
        case TokenType::FROM:
        // Statement / control-flow starters.
        case TokenType::LET:
        case TokenType::CONST:
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
            LUC_LOG_PARSER("SYNCHRONIZE: stopped at token '" << peek().value << "'");
            return;
        default:
            break;
        }

        advance();
    }
    LUC_LOG_PARSER("SYNCHRONIZE: reached end of file");
}

// ─────────────────────────────────────────────────────────────────────────────
// Visibility & modifier helpers
// ─────────────────────────────────────────────────────────────────────────────

Visibility Parser::parseVisibility() {
    if (match(TokenType::PUB)) {
        LUC_LOG_PARSER("parseVisibility: found 'pub' -> Package");
        return Visibility::Package;
    }
    if (match(TokenType::EXPORT)) {
        LUC_LOG_PARSER("parseVisibility: found 'export' -> Export");
        return Visibility::Export;
    }
    LUC_LOG_PARSER_EXTREME("parseVisibility: no modifier -> Private");
    return Visibility::Private;
}

// ─────────────────────────────────────────────────────────────────────────────
// Doc comment harvesting (keep existing implementation, just add logging)
// ─────────────────────────────────────────────────────────────────────────────

std::optional<DocComment> Parser::harvestDocComment() {
    if (pos_ == 0) {
        LUC_LOG_PARSER_EXTREME("harvestDocComment: pos=0, returning nullopt");
        return std::nullopt;
    }

    int declLine = peek().line;
    LUC_LOG_PARSER_VERBOSE("harvestDocComment: declLine=" << declLine);

    // ── Pass 1: scan backward, skip only LINE_COMMENT and DOC_COMMENT tokens.
    // Stop at the first non-comment token.  Record what we find.

    // Check for a trailing comment: a LINE_COMMENT on the same line.
    // (We only take it as trailing if there is no stacked run above.)
    std::optional<std::string> trailingText;
    int trailingIdx = -1;

    // Collect a stacked run: consecutive LINE_COMMENTs ending on declLine-1.
    std::vector<std::string> stackedLines;
    int stackedTopLine = -1; // line of the topmost comment in the run

    // Check for a block doc comment immediately above.
    std::optional<std::string> blockText;

    for (std::size_t i = pos_; i > 0;) {
        --i;
        const Token& t = tokens_[i];

        if (t.type == TokenType::LINE_COMMENT) {
            if (t.line == declLine) {
                // Same line as the declaration → candidate for trailing.
                if (trailingIdx < 0) {
                    trailingText = t.value;
                    trailingIdx  = static_cast<int>(i);
                }
                // Keep scanning — there may be stacked comments above too.
                continue;
            }

            // Comment above the declaration line.
            if (stackedLines.empty()) {
                // First comment we see above — it must be on declLine-1 to start a run.
                if (declLine - t.line == 1) {
                    stackedLines.push_back(t.value);
                    stackedTopLine = t.line;
                    continue;
                } else {
                    // Gap > 1 line: floating comment, no attachment.
                    break;
                }
            } else {
                // We already have a partial run. This next comment must be
                // on the line immediately before the current top of the run.
                if (stackedTopLine - t.line == 1) {
                    stackedLines.push_back(t.value);
                    stackedTopLine = t.line;
                    continue;
                } else {
                    // Gap breaks the run; stop collecting.
                    break;
                }
            }
        }

        if (t.type == TokenType::DOC_COMMENT) {
            // Block doc: accept if the closing line is immediately before declLine.
            if (declLine - t.line <= 1) {
                blockText = t.value;
            }
            break; // nothing above a block doc attaches.
        }

        // Any real token breaks the run.
        break;
    }

    // ── Priority resolution ──────────────────────────────────────────────────

    // Block doc wins over everything.
    if (blockText)
        return DocComment{*blockText, DocCommentForm::Block};

    // Stacked run wins over trailing.
    if (!stackedLines.empty()) {
        // stackedLines[0] is the line nearest the declaration; reverse to get
        // top-to-bottom order, then join with newlines.
        std::string combined;
        for (int i = static_cast<int>(stackedLines.size()) - 1; i >= 0; --i) {
            if (!combined.empty()) combined += '\n';
            combined += stackedLines[i];
        }
        return DocComment{combined, DocCommentForm::Stacked};
    }

    // Trailing comment (same line as declaration).
    if (trailingText)
        return DocComment{*trailingText, DocCommentForm::Trailing};

    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Disambiguation helpers
// ─────────────────────────────────────────────────────────────────────────────

bool Parser::looksLikeType() const {
    LUC_LOG_PARSER_EXTREME("looksLikeType: checking token '" << peek().value 
                           << "' type=" << static_cast<int>(peek().type));
    
    bool result = false;
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
    case TokenType::LBRACKET:
    case TokenType::AMPERSAND:
    case TokenType::MUL:
    case TokenType::IDENTIFIER:
        return true;

    // Type qualifiers: ~async, ~noinline, ~cdecl, etc.
    // A '~' followed by an identifier indicates a type qualifier,
    // which is part of a function type.
    case TokenType::TILDE:
        // Look ahead: after '~' there must be an IDENTIFIER
        // to consider this as a type. If not, it might be a syntax error.
        if (peekNext().type == TokenType::IDENTIFIER) {
            LUC_LOG_PARSER_EXTREME("looksLikeType: '~' followed by identifier -> true (type qualifier)");
            return true;
        }
        result = false;
        break;

    // Function type starts with '(' — must distinguish from a grouped
    // expression.  A function type '(' has at least a ')' somewhere and
    // then optionally a return type.  We accept '(' conservatively here;
    // the caller is responsible for resolving the ambiguity with more
    // context.
    case TokenType::LPAREN:
        result = true;
        break;
        
    default:
        result = false;
        break;
    }
    
    LUC_LOG_PARSER_EXTREME("looksLikeType: returning " << (result ? "true" : "false"));
    return result;
}

bool Parser::looksLikeFuncDecl() const {
    LUC_LOG_PARSER_VERBOSE("looksLikeFuncDecl: starting at pos=" << pos_ 
                           << ", token='" << peek().value << "'");
    
    std::size_t i = pos_;

    // Skip the name IDENTIFIER
    if (i < tokens_.size() && tokens_[i].type == TokenType::IDENTIFIER) {
        LUC_LOG_PARSER_VERBOSE("\tfound IDENTIFIER: '" << tokens_[i].value << "'");
        ++i;
    } else {
        LUC_LOG_PARSER_VERBOSE("\tnot an IDENTIFIER, returning false");
        return false;
    }

    // Skip generic params if present: < ... >
    if (i < tokens_.size() && tokens_[i].type == TokenType::LESS) {
        LUC_LOG_PARSER_VERBOSE("\tfound generic params start '<'");
        int depth = 1;
        ++i;
        while (i < tokens_.size() && depth > 0) {
            if (tokens_[i].type == TokenType::LESS) ++depth;
            else if (tokens_[i].type == TokenType::GREATER) --depth;
            else if (tokens_[i].type == TokenType::SEMICOLON || 
                     tokens_[i].type == TokenType::RBRACE) 
                break;
            else if (tokens_[i].type == TokenType::EOF_TOKEN)
                break;
            ++i;
        }
        LUC_LOG_PARSER_VERBOSE("\tafter generic params, at token '" << tokens_[i].value << "'");
    }

    // Skip type qualifiers (~async, ~noinline, etc.) that appear after the name
    // These are part of the function's type, not the declaration syntax
    while (i < tokens_.size() && tokens_[i].type == TokenType::TILDE) {
        ++i; // skip '~'
        // Expect an identifier after '~'
        if (i < tokens_.size() && tokens_[i].type == TokenType::IDENTIFIER) {
            LUC_LOG_PARSER_VERBOSE("\tskipping qualifier '~" << tokens_[i].value << "'");
            ++i;
        } else {
            // Invalid: '~' without identifier - not a valid function
            LUC_LOG_PARSER_VERBOSE("\t'~' without identifier, returning false");
            return false;
        }
        // Skip any comments between qualifiers
        while (i < tokens_.size() && tokens_[i].type == TokenType::LINE_COMMENT) {
            ++i;
        }
    }

    // we need at least one parameter group '('
    if (!(i < tokens_.size() && tokens_[i].type == TokenType::LPAREN)) {
        LUC_LOG_PARSER_VERBOSE("\tno '(' found after name/generics/qualifiers, returning false");
        return false;
    }

    LUC_LOG_PARSER_VERBOSE("\tlooksLikeFuncDecl: returning true");
    return true;
}

bool Parser::looksLikeStructLiteral() const {
    if (!check(TokenType::IDENTIFIER)) {
        LUC_LOG_PARSER_EXTREME("looksLikeStructLiteral: not IDENTIFIER, false");
        return false;
    }

    // Peek ahead: after optional generic args, must find '{'.
    std::size_t i = pos_ + 1;

    // Skip generic args: < ... >
    if (i < tokens_.size() && tokens_[i].type == TokenType::LESS) {
        int depth = 1;
        ++i;
        while (i < tokens_.size() && depth > 0) {
            TokenType tt = tokens_[i].type;
            if (tt == TokenType::LESS) ++depth;
            else if (tt == TokenType::GREATER) --depth;
            else if (tt == TokenType::EOF_TOKEN) break;
            ++i;
        }
    }

    bool result = (i < tokens_.size() && tokens_[i].type == TokenType::LBRACE);
    LUC_LOG_PARSER_EXTREME("looksLikeStructLiteral: " << (result ? "true" : "false"));
    return result;
}

bool Parser::looksLikeStmtStart() const {
    switch (peek().type) {
        case TokenType::LET:
        case TokenType::CONST:
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
        case TokenType::AT_SIGN:
            LUC_LOG_PARSER_EXTREME("looksLikeStmtStart: true (keyword)");
            return true;
        default:
            // Also allow primitive type keywords as expression starters (e.g., string(x) conversion)
            bool isPrimitiveType = false;
            switch (peek().type) {
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
                    isPrimitiveType = true;
                    break;
                default:
                    break;
            }
        
            bool result = looksLikeType() || check(TokenType::IDENTIFIER) || isPrimitiveType ||
                   check(TokenType::INT_LITERAL) || check(TokenType::FLOAT_LITERAL) ||
                   check(TokenType::STRING_LITERAL) || check(TokenType::RAW_STRING_LITERAL) ||
                   check(TokenType::CHAR_LITERAL) || check(TokenType::HEX_LITERAL) ||
                   check(TokenType::BINARY_LITERAL) || check(TokenType::TRUE) ||
                   check(TokenType::FALSE) || check(TokenType::NIL) ||
                   check(TokenType::MINUS) || check(TokenType::NOT) ||
                   check(TokenType::BIT_NOT) || check(TokenType::AMPERSAND) ||
                   check(TokenType::AWAIT) || check(TokenType::LPAREN);
            LUC_LOG_PARSER_EXTREME("looksLikeStmtStart: " << (result ? "true" : "false") 
                                   << " (expression starter)");
            return result;
    }
}

bool Parser::looksLikeDeclStart() const {
    switch (peek().type) {
        case TokenType::AT_SIGN:
        case TokenType::PACKAGE:
        case TokenType::USE:
        case TokenType::PUB:
        case TokenType::EXPORT:
        case TokenType::STRUCT:
        case TokenType::ENUM:
        case TokenType::TRAIT:
        case TokenType::IMPL:
        case TokenType::TYPE:
        case TokenType::FROM:
        case TokenType::LET:
        case TokenType::CONST:
            LUC_LOG_PARSER_EXTREME("looksLikeDeclStart: true");
            return true;
    default:
        LUC_LOG_PARSER_EXTREME("looksLikeDeclStart: false");
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
    LUC_LOG_PARSER("\n === PARSE START ===");
    
    auto program = std::make_unique<ProgramAST>();
    program->filePath = filePath_;
    program->loc = currentLoc();

    // ── 1. package declaration ────────────────────────────────────────────────
    {
        LUC_LOG_PARSER("Parsing package declaration...");
        std::optional<DocComment> pkgDoc = harvestDocComment();

        if (!check(TokenType::PACKAGE)) {
            LUC_LOG_PARSER("ERROR: No 'package' declaration found at start of file");
            errorAt(DiagCode::E2001,
                    "expected 'package' declaration at the start of the file");
            synchronize();
            
            // Create a dummy package with unknown name
            auto dummyPkg = std::make_unique<PackageDeclAST>("<unknown>");
            dummyPkg->loc = currentLoc();
            attachDoc(*dummyPkg, std::move(pkgDoc));
            program->packageName = "<unknown>";
            program->decls.push_back(std::move(dummyPkg));
        } else {
            auto pkgDecl = parsePackageDecl();
            attachDoc(*pkgDecl, std::move(pkgDoc));
            if (pkgDecl) {
                program->packageName = pkgDecl->name;
                program->decls.push_back(std::move(pkgDecl));
            } else {
                // parsePackageDecl returned nullptr - insert UnknownDeclAST
                LUC_LOG_PARSER("parsePackageDecl returned nullptr, inserting UnknownDeclAST");
                auto unknown = std::make_unique<UnknownDeclAST>();
                unknown->loc = currentLoc();
                program->packageName = "<error>";
                program->decls.push_back(std::move(unknown));
            }
        }
        LUC_LOG_PARSER("\tPackage name: '" << program->packageName << "'");
    }

    // ── 2. top-level declarations ─────────────────────────────────────────────
    LUC_LOG_PARSER("Parsing top-level declarations...");
    int declCount = 0;
    int errorCount = 0;
    
    while (!isAtEnd()) {
        std::optional<DocComment> doc = harvestDocComment();

        LUC_LOG_PARSER("Parsing top-level declaration at line " << peek().line 
                       << ", token: " << LucDebug::tokenTypeToString(peek().type));

        DeclPtr decl = parseTopLevelDecl();
        
        if (decl) {
            declCount++;
            LUC_LOG_PARSER("\tSuccessfully parsed declaration #" << declCount 
                           << " of kind: " << LucDebug::kindToString(decl->kind));
            attachDoc(*decl, std::move(doc));
            program->decls.push_back(std::move(decl));
        } else {
            // parseTopLevelDecl returned nullptr - insert UnknownDeclAST
            errorCount++;
            LUC_LOG_PARSER("\tFailed to parse declaration #" << (declCount + errorCount) 
                           << ", inserting UnknownDeclAST");
            auto unknown = std::make_unique<UnknownDeclAST>();
            unknown->loc = currentLoc();
            attachDoc(*unknown, std::move(doc));
            program->decls.push_back(std::move(unknown));
            synchronize();
        }
    }

    LUC_LOG_PARSER("\n === parse() END: parsed " << declCount 
                   << " declarations, " << errorCount << " errors ===");
    
    // Always return a program, even if it contains UnknownDeclAST nodes
    // Caller should check dc_.hasErrors() to know if parsing succeeded
    return program;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseTopLevelDecl
// ─────────────────────────────────────────────────────────────────────────────

DeclPtr Parser::parseTopLevelDecl() {
    LUC_LOG_PARSER("parseTopLevelDecl: current token = '" << peek().value 
                   << "', type = " << static_cast<int>(peek().type));

    // ── Collect leading '@' attributes ───────────────────────────────────────
    LUC_LOG_PARSER_VERBOSE("\tParsing attributes...");
    std::vector<AttributePtr> attrs = parseAttributes();
    LUC_LOG_PARSER_VERBOSE("\tFound " << attrs.size() << " attribute(s)");

    // ── Visibility modifier ───────────────────────────────────────────────────
    // All remaining top-level declarations may carry a visibility modifier.
    Visibility vis = parseVisibility();
    LUC_LOG_PARSER_VERBOSE("\tVisibility: " << (vis == Visibility::Private ? "private" : 
                                                 vis == Visibility::Package ? "package" : "export"));

    // ── 'use' ─────────────────────────────────────────────────────────────────
    // 'export use math.vec2' is supported, so use can take a visibility tier.
    if (check(TokenType::USE)) {
        LUC_LOG_PARSER("\tDetected 'use' declaration");
        if (!attrs.empty())
            errorAt(DiagCode::E2002, "attributes are not valid on 'use' declarations");
        return parseUseDecl(vis);
    }

    // ── 'struct' ──────────────────────────────────────────────────────────────
    if (check(TokenType::STRUCT)) {
        LUC_LOG_PARSER("\tDetected 'struct' declaration");
        auto decl = parseStructDecl(vis);
        if (decl) decl->attributes = std::move(attrs);
        return decl;
    }

    // ── 'enum' ────────────────────────────────────────────────────────────────
    if (check(TokenType::ENUM)) {
        LUC_LOG_PARSER("\tDetected 'enum' declaration");
        if (!attrs.empty())
            errorAt(DiagCode::E2002, "attributes are not valid on 'enum' declarations");
        return parseEnumDecl(vis);
    }

    // ── 'trait' ───────────────────────────────────────────────────────────────
    if (check(TokenType::TRAIT)) {
        LUC_LOG_PARSER("\tDetected 'trait' declaration");
        if (!attrs.empty())
            errorAt(DiagCode::E2002, "attributes are not valid on 'trait' declarations");
        return parseTraitDecl(vis);
    }

    // ── 'impl' ────────────────────────────────────────────────────────────────
    if (check(TokenType::IMPL)) {
        LUC_LOG_PARSER("\tDetected 'impl' declaration");
        if (!attrs.empty())
            errorAt(DiagCode::E2002, "attributes are not valid on 'impl' declarations");
        return parseImplDecl(vis);
    }

    // ── 'from' ────────────────────────────────────────────────────────────────
    if (check(TokenType::FROM)) {
        LUC_LOG_PARSER("\tDetected 'from' declaration");
        if (!attrs.empty())
            errorAt(DiagCode::E2002, "attributes are not valid on 'from' declarations");
        return parseFromDecl(vis);
    }

    // ── 'type' ────────────────────────────────────────────────────────────────
    if (check(TokenType::TYPE)) {
        LUC_LOG_PARSER("\tDetected 'type' alias declaration");
        if (!attrs.empty())
            errorAt(DiagCode::E2002, "attributes are not valid on 'type' alias declarations");
        return parseTypeAliasDecl(vis);
    }

    // ── 'let' / 'const' ──────────────────────────────────────────────────────
    // Could be a variable declaration or a function declaration.
    // After consuming the keyword and name, looksLikeFuncDecl() inspects
    // whether a '(' follows (with optional generic params) to decide.
    if (checkAny({TokenType::LET, TokenType::CONST})) {
        Token kwTok = advance();
        LUC_LOG_PARSER("\tDetected keyword: '" << kwTok.value << "'");
        
        DeclKeyword kw;
        switch (kwTok.type) {
        case TokenType::LET:
            kw = DeclKeyword::Let;
            LUC_LOG_PARSER_VERBOSE("\t\t-> DeclKeyword::Let");
            break;
        default:
            kw = DeclKeyword::Const;
            LUC_LOG_PARSER_VERBOSE("\t\t-> DeclKeyword::Const");
            break;
        }

        // After the keyword we expect the name.
        if (!check(TokenType::IDENTIFIER)) {
            LUC_LOG_PARSER("\tERROR: expected name after keyword");
            errorAt(DiagCode::E2003, "expected name after '" + kwTok.value + "'");
            return nullptr;
        }

        // Check if this is @extern
        bool hasExternAttr = false;
        for (const auto& attr : attrs) {
            if (attr->name == "extern") {
                hasExternAttr = true;
                LUC_LOG_PARSER("\tFound @extern attribute");
                break;
            }
        }

        if (hasExternAttr) {
            LUC_LOG_PARSER("\tProcessing @extern declaration...");
            
            // Look ahead to see if there's a '(' after the name (skip comments)
            std::size_t lookAhead = pos_ + 1;
            while (lookAhead < tokens_.size() && tokens_[lookAhead].type == TokenType::LINE_COMMENT) {
                lookAhead++;
            }
            
            bool hasParenAfterName = (lookAhead < tokens_.size() && tokens_[lookAhead].type == TokenType::LPAREN);
            LUC_LOG_PARSER("\thasParenAfterName: " << (hasParenAfterName ? "true" : "false"));
            
            if (hasParenAfterName) {
                LUC_LOG_PARSER("\t-> Parsing as @extern function");
                return parseFuncDecl(kw, vis, std::move(attrs));
            } else {
                LUC_LOG_PARSER("\t-> Parsing as @extern variable");
                auto decl = parseVarDecl(vis, std::move(attrs));
                return decl;
            }
        }

        LUC_LOG_PARSER("\tChecking if this looks like a function declaration...");
        bool isFunc = looksLikeFuncDecl();
        LUC_LOG_PARSER("\tlooksLikeFuncDecl: " << (isFunc ? "true" : "false"));
        
        if (isFunc) {
            LUC_LOG_PARSER("\t-> Parsing as function declaration");
            return parseFuncDecl(kw, vis, std::move(attrs));
        } else {
            LUC_LOG_PARSER("\t-> Parsing as variable declaration");
            auto decl = parseVarDecl(vis);
            if (decl) decl->attributes = std::move(attrs);
            return decl;
        }
    }

    // ── Unrecognised token ────────────────────────────────────────────────────
    LUC_LOG_PARSER("\tUnrecognised declaration start");
    if (!attrs.empty()) {
        errorAt(DiagCode::E2002, "expected a declaration after '@' attribute(s)");
    } else if (vis != Visibility::Private) {
        // A modifier was consumed but no valid declaration followed.
        errorAt(DiagCode::E2002, "expected a declaration after modifier");
    } else {
        errorAt(DiagCode::E2002, "unexpected token '" + peek().value + "' at top level");
    }
    return nullptr;
}