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
#include "diagnostics/Diagnostic.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "debug/DebugUtils.hpp"

#include <algorithm>
#include <cassert>
#include <sstream>

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

Parser::Parser(std::vector<Token> tokens, DiagnosticEngine &dc, 
               InternedString filePath, StringPool& pool, ASTArena& arena)
    : tokens_(std::move(tokens)), filePath_(std::move(filePath)), dc_(dc),
      pool_(pool), arena_(arena) {

    LUC_LOG_PARSER("=== Parser constructed ===");
    LUC_LOG_PARSER("\tToken count: " << tokens_.size());
    LUC_LOG_PARSER("\tFile path: " << pool.lookup(filePath_));
    
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
    // Skip any LINE_COMMENT, DOC_COMMENT tokens at the current position — they are
    // transparent to the grammar; only harvestDocComment() reads them directly.
    std::size_t i = pos_;
    while (i < tokens_.size() && (tokens_[i].type == TokenType::LINE_COMMENT ||
                                   tokens_[i].type == TokenType::DOC_COMMENT))
        ++i;
    if (i >= tokens_.size())
        return tokens_.back();
    return tokens_[i];
}

const Token &Parser::peekNext() const {
    return peekAt(1);
}

const Token &Parser::peekAt(std::size_t offset) const {
    // Skip LINE_COMMENT, DOC_COMMENT tokens when computing the offset — they are invisible
    // to the grammar.  We need to find the Nth non-comment token from pos_.
    std::size_t i = pos_;
    // Start by skipping comments at pos_ itself (same as peek()).
    while (i < tokens_.size() && (tokens_[i].type == TokenType::LINE_COMMENT ||
                                   tokens_[i].type == TokenType::DOC_COMMENT))
        ++i;
    // Now advance 'offset' more non-comment tokens.
    while (offset > 0 && i < tokens_.size()) {
        ++i;
        while (i < tokens_.size() && (tokens_[i].type == TokenType::LINE_COMMENT ||
                                       tokens_[i].type == TokenType::DOC_COMMENT))
            ++i;
        --offset;
    }
    if (i >= tokens_.size())
        return tokens_.back();
    
    return tokens_[i];
}

Token Parser::advance() {
    // Skip any comment tokens at the current position (both LINE and DOC)
    while (!isAtEnd() && (tokens_[pos_].type == TokenType::LINE_COMMENT ||
                          tokens_[pos_].type == TokenType::DOC_COMMENT)) {
        ++pos_;
    }

    std::size_t i = pos_;
    Token t = (i < tokens_.size()) ? tokens_[i] : tokens_.back();

    pos_ = (i < tokens_.size()) ? i + 1 : i;
    // Skip any comment tokens after the consumed token
    while (!isAtEnd() && (tokens_[pos_].type == TokenType::LINE_COMMENT ||
                          tokens_[pos_].type == TokenType::DOC_COMMENT)) {
        ++pos_;
    }

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
    // Consume at least one token to avoid infinite loop.
    if (!isAtEnd()) {
        advance();
    }
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

void Parser::error(const SourceLocation &loc, DiagCode code, const std::string &msg) {
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
// Function signature helpers
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseParamGroup
//
// Grammar:
//   param_group := '(' [ param_list ] ')'
//   param_list  := param { [','] param } [ [','] variadic_param ]
//
// Returns the list of params for a single group. Variadic must be last.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<ASTPtr<ParamAST>> Parser::parseParamGroup() {
    LUC_LOG_PARSER_VERBOSE("parseParamGroup: parsing parameter group");
    SourceLocation loc = currentLoc();
    consume(TokenType::LPAREN, "expected '(' to start parameter group");
    
    std::vector<ASTPtr<ParamAST>> group;
    
    while (!check(TokenType::RPAREN) && !isAtEnd()) {
        match(TokenType::COMMA);
        if (check(TokenType::RPAREN)) break;

        SourceLocation paramLoc = currentLoc();

        // Parse parameter name
        if (!check(TokenType::IDENTIFIER)) { 
            errorAt(DiagCode::E2003, "expected parameter name, got '" + peek().value + "'");
            break;
        }
        InternedString paramName = pool_.intern(advance().value);
        
        // Parse variadic '...' if present
        bool isVariadic = match(TokenType::VARIADIC);

        // Save position right before parsing the type
        std::size_t savedPos = pos_;
        TypePtr paramType = parseType();

        // Case 1: No progress → infinite loop risk
        if (pos_ == savedPos) {
            errorAt(DiagCode::E2005, "expected parameter type, no token consumed");
            if (!isAtEnd()) advance(); // consume offending token to avoid infinite loop
            break;
        }

        // Case 2: parseType returned an unknown type (invalid syntax)
        if (paramType->isa<UnknownTypeAST>()) {
            errorAt(DiagCode::E2005, "invalid parameter type");
            break;  // do NOT add this parameter
        }
        
        auto paramNode = arena_.make<ParamAST>();
        paramNode->loc = paramLoc;
        paramNode->name = paramName;
        paramNode->type = std::move(paramType);
        paramNode->isVariadic = isVariadic;
        group.push_back(std::move(paramNode));
    }
    
    consume(TokenType::RPAREN, "expected ')' to close parameter group");
    return group;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseReturnList
//
// Grammar:
//   return_list := '(' [ type { ',' type } ] ')'   -- multiple returns
//                | type                            -- single return
//
// Called after encountering '->' in a function signature or anon‑func body.
// Returns a vector of TypePtr, one per return value. Empty vector indicates
// a void function (no '->' present in the calling context).
//
// Note: When a single return type is a function type that begins with (, 
// the parentheses are not interpreted as a multiple‑return list.
// The parser distinguishes based on the content inside the parentheses.
// For example, -> (x int) -> int is a single function type,
// while -> (int, string) is a multiple return.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<TypePtr> Parser::parseReturnList() {
    std::vector<TypePtr> types;

    // Single return that is a function type starting with '(' ?
    if (check(TokenType::LPAREN)) {
        TokenType next = peekNext().type;
        // Case 1: '(' IDENTIFIER type... → function type parameter
        if (next == TokenType::IDENTIFIER) {
            TokenType next2 = peekAt(2).type;
            if (isPrimitiveTypeToken(next2) || next2 == TokenType::IDENTIFIER ||
                next2 == TokenType::LBRACKET || next2 == TokenType::AMPERSAND ||
                next2 == TokenType::MUL || next2 == TokenType::LPAREN) {
                // Looks like a parameter: IDENTIFIER type -> parse as function type
                TypePtr funcType = parseFuncType();
                if (funcType && !funcType->isa<UnknownTypeAST>()) {
                    types.push_back(std::move(funcType));
                    return types;
                }
            }
        }
        // Case 2: '(' ')' '->' → function type with no parameters
        else if (next == TokenType::RPAREN) {
            TokenType next2 = peekAt(2).type;
            if (next2 == TokenType::ARROW) {
                TypePtr funcType = parseFuncType();
                if (funcType && !funcType->isa<UnknownTypeAST>()) {
                    types.push_back(std::move(funcType));
                    return types;
                }
            }
        }

        // Otherwise, parse as parenthesised multiple‑return list: ( type, type, ... )
        advance(); // consume '('
        while (!check(TokenType::RPAREN) && !isAtEnd()) {
            match(TokenType::COMMA);
            if (check(TokenType::RPAREN)) break;

            std::size_t savedPos = pos_;
            TypePtr t = parseType();
            if (pos_ == savedPos) {
                errorAt(DiagCode::E2005, "expected return type in parenthesised list");
                while (!check(TokenType::RPAREN) && !isAtEnd()) advance();
                break;
            }
            if (t) types.push_back(std::move(t));
        }
        consume(TokenType::RPAREN, DiagCode::E2001, "expected ')' to close return type list");
        return types;
    }

    // Single return (not starting with '(')
    std::size_t savedPos = pos_;
    TypePtr t = parseType();
    if (pos_ == savedPos) {
        errorAt(DiagCode::E2005, "expected return type after '->'");
    } else if (t) {
        types.push_back(std::move(t));
    }
    return types;
}

// ─────────────────────────────────────────────────────────────────────────────
// Generic
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseGenericParams
//
// Grammar:
//   generic_params := '<' generic_param { [','] generic_param } '>'
//
// Called on the declaration side (func, struct, trait, impl, type alias).
// For the use side (call sites, named types), use parseGenericArgs() in
// ParserType.cpp.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<GenericParamPtr> Parser::parseGenericParams() {
    LUC_LOG_PARSER_VERBOSE("parseGenericParams: starting");
    std::vector<GenericParamPtr> params;

    consume(TokenType::LESS, "expected '<' to open generic parameters");

    if (check(TokenType::GREATER)) {
        advance();
        return params;
    }

    bool stalled = false;
    do {
        match(TokenType::COMMA);
        if (check(TokenType::GREATER))
            break;

        // Record position before parsing the generic parameter
        std::size_t savedPos = pos_;
        GenericParamPtr gp = parseGenericParam();

        if (!gp) {
            if (pos_ == savedPos) {
                errorAt(DiagCode::E2002, "expected generic parameter, skipping token '" + peek().value + "'");
                advance(); // consume the unexpected token to avoid infinite loop
                stalled = true;
                break;      // exit loop early to avoid cascading errors
            }
            // Continue to next iteration (loop condition will be re-evaluated)
            continue;
        }

        params.push_back(std::move(gp));

    } while (!check(TokenType::GREATER) && !isAtEnd());

    // If we stalled, we might not be at GREATER. Skip to it or to end.
    if (stalled) {
        while (!isAtEnd() && !check(TokenType::GREATER)) {
            advance();
        }
    }

    // Now consume the closing '>'
    consume(TokenType::GREATER, DiagCode::E2001, "expected '>' to close generic parameters");
    
    LUC_LOG_PARSER_VERBOSE("parseGenericParams: found " << params.size() << " generic params");
    return params;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseGenericParam
//
// Grammar:
//   generic_param   := IDENTIFIER
//                    | IDENTIFIER ':' IDENTIFIER
//                    | IDENTIFIER ':' IDENTIFIER { '+' IDENTIFIER }
//
// Examples:  T     K : Hashable     V : Hashable + Comparable
// ─────────────────────────────────────────────────────────────────────────────
GenericParamPtr Parser::parseGenericParam() {
    LUC_LOG_PARSER_VERBOSE("parseGenericParam: parsing");
    SourceLocation loc = currentLoc();

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected type parameter name");
        return nullptr;
    }
    std::string nameRaw = advance().value;
    InternedString name = pool_.intern(nameRaw);

    // Allocate via arena
    auto gp = arena_.make<GenericParamAST>(name);
    gp->loc = loc;

    // Optional constraints: ':' IDENTIFIER { '+' IDENTIFIER }
    if (match(TokenType::COLON)) {
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected trait name after ':' in generic parameter");
        } else {
            std::string traitRaw = advance().value;
            gp->constraints.push_back(pool_.intern(traitRaw));

            while (match(TokenType::PLUS)) {
                if (!check(TokenType::IDENTIFIER)) {
                    errorAt(DiagCode::E2003, "expected trait name after '+' in generic constraint");
                    break;
                }
                traitRaw = advance().value;
                gp->constraints.push_back(pool_.intern(traitRaw));
            }
        }
    }

    LUC_LOG_PARSER_VERBOSE("\tgeneric param: '" << nameRaw << "' with " << gp->constraints.size() << " constraints");
    return gp;
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
    int stackedTopLine = -1;

    // Check for a block doc comment immediately above.
    std::optional<std::string> blockText;

    for (std::size_t i = pos_; i > 0;) {
        --i;
        const Token& t = tokens_[i];

        if (t.type == TokenType::LINE_COMMENT) {
            // Ignore tokens with invalid line number (0)
            if (t.line <= 0) continue;

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
            // Ignore tokens with invalid line number (0)
            if (t.line <= 0) continue;
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
    if (blockText) {
        return DocComment{pool_.intern(*blockText), DocCommentForm::Block};
    }

    // Stacked run wins over trailing.
    if (!stackedLines.empty()) {
        // stackedLines[0] is the line nearest the declaration; reverse to get
        // top-to-bottom order, then join with newlines.
        std::string combined;
        for (int i = static_cast<int>(stackedLines.size()) - 1; i >= 0; --i) {
            if (!combined.empty()) combined += '\n';
            combined += stackedLines[i];
        }
        return DocComment{pool_.intern(combined), DocCommentForm::Stacked};
    }

    // Trailing comment (same line as declaration).
    if (trailingText) {
        return DocComment{pool_.intern(*trailingText), DocCommentForm::Trailing};
    }

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

bool Parser::isPrimitiveTypeToken(TokenType type) {
    switch (type) {
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
            return true;
        default:
            return false;
    }
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
            // Skip comments before evaluating token type
            while (i < tokens_.size() && (tokens_[i].type == TokenType::LINE_COMMENT ||
                                          tokens_[i].type == TokenType::DOC_COMMENT)) {
                ++i;
            }
            if (i >= tokens_.size()) break;
            TokenType tt = tokens_[i].type;
            if (tt == TokenType::LESS) ++depth;
            else if (tt == TokenType::GREATER) --depth;
            else if (tt == TokenType::SEMICOLON || tt == TokenType::RBRACE) 
                break;
            else if (tt == TokenType::EOF_TOKEN)
                break;
            ++i;
        }
        LUC_LOG_PARSER_VERBOSE("\tafter generic params, at token '" << tokens_[i].value << "'");

        // If depth > 0, the generic list is unterminated – treat as not a function.
        if (depth != 0) return false;
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

bool Parser::looksLikeAnonFunc() const {
    std::size_t i = pos_;

    // Skip type qualifiers (though anonymous functions should not have them)
    while (i < tokens_.size() && tokens_[i].type == TokenType::TILDE) {
        ++i;
        if (i < tokens_.size() && tokens_[i].type == TokenType::IDENTIFIER) {
            ++i;
        } else {
            return false;
        }
        while (i < tokens_.size() && (tokens_[i].type == TokenType::LINE_COMMENT ||
                                      tokens_[i].type == TokenType::DOC_COMMENT)) {
            ++i;
        }
    }

    // First parameter group is required
    if (i >= tokens_.size() || tokens_[i].type != TokenType::LPAREN)
        return false;

    // Helper to parse a parameter group and return index after matching ')'
    auto parseOneGroup = [&](std::size_t start) -> std::size_t {
        if (start >= tokens_.size() || tokens_[start].type != TokenType::LPAREN)
            return start;
        int parenDepth = 1;
        std::size_t j = start + 1;
        while (j < tokens_.size() && parenDepth > 0) {
            TokenType tt = tokens_[j].type;
            if (tt == TokenType::LPAREN) {
                ++parenDepth;
            } else if (tt == TokenType::RPAREN) {
                --parenDepth;
            } else if (tt == TokenType::LINE_COMMENT || tt == TokenType::DOC_COMMENT) {
                ++j;
                continue;
            } else if (tt == TokenType::TILDE) {
                ++j;
                if (j < tokens_.size() && tokens_[j].type == TokenType::IDENTIFIER) ++j;
                continue;
            }
            ++j;
        }
        return j;
    };

    std::size_t startPos = i;
    i = parseOneGroup(i);
    if (i == startPos) return false; // no progress

    // Parse additional curried parameter groups
    while (i < tokens_.size() && tokens_[i].type == TokenType::LPAREN) {
        startPos = i;
        i = parseOneGroup(i);
        if (i == startPos) return false;
    }

    // Skip comments after the last ')'
    while (i < tokens_.size() && (tokens_[i].type == TokenType::LINE_COMMENT ||
                                  tokens_[i].type == TokenType::DOC_COMMENT))
        ++i;
    if (i >= tokens_.size()) return false;

    // Immediate '{' → void anonymous function
    if (tokens_[i].type == TokenType::LBRACE)
        return true;

    // Otherwise, require '->' followed by a return type
    if (tokens_[i].type != TokenType::ARROW)
        return false;

    // Consume '->' in the lookahead (i is a copy, pos_ unchanged)
    ++i;
    while (i < tokens_.size() && (tokens_[i].type == TokenType::LINE_COMMENT ||
                                  tokens_[i].type == TokenType::DOC_COMMENT))
        ++i;
    if (i >= tokens_.size()) return false;

    // The token after '->' must start a type
    TokenType retStart = tokens_[i].type;
    if (Parser::isPrimitiveTypeToken(retStart)) return true;
    switch (retStart) {
        case TokenType::IDENTIFIER:
        case TokenType::LBRACKET:
        case TokenType::AMPERSAND:
        case TokenType::MUL:
        case TokenType::LPAREN:
            return true;
        default:
            return false;
    }
}

bool Parser::looksLikeStructLiteral() const {
    if (!check(TokenType::IDENTIFIER)) {
        LUC_LOG_PARSER_EXTREME("looksLikeStructLiteral: not IDENTIFIER, false");
        return false;
    }

    // Peek ahead: after optional generic args, must find '{'.
    std::size_t i = pos_ + 1;
    int depth = 0;   // Initialize to 0 (no open brackets yet)

    // Skip generic args: < ... >
    if (i < tokens_.size() && tokens_[i].type == TokenType::LESS) {
        depth = 1;   // We've seen the opening '<', start depth at 1
        ++i;         // move past '<'
        while (i < tokens_.size() && depth > 0) {
            // Skip comments
            while (i < tokens_.size() && (tokens_[i].type == TokenType::LINE_COMMENT ||
                                        tokens_[i].type == TokenType::DOC_COMMENT)) {
                ++i;
            }
            if (i >= tokens_.size()) break;
            TokenType tt = tokens_[i].type;
            if (tt == TokenType::LESS) ++depth;
            else if (tt == TokenType::GREATER) --depth;
            else if (tt == TokenType::EOF_TOKEN) break;
            ++i;
        }
    }
    // After the loop, depth must be 0. For non-generic literals, depth is already 0.
    bool result = (depth == 0 && i < tokens_.size() && tokens_[i].type == TokenType::LBRACE);
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
        case TokenType::MATCH:
        case TokenType::SWITCH:
        case TokenType::AT_SIGN:
        case TokenType::HASH:
            LUC_LOG_PARSER_EXTREME("looksLikeStmtStart: true (keyword)");
            return true;
        default:
            // Also allow primitive type keywords as expression starters (e.g., string(x) conversion)
            bool isPrimitiveType = isPrimitiveTypeToken(peek().type);
        
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
ASTPtr<ProgramAST> Parser::parse() {
    LUC_LOG_PARSER("\n === PARSE START ===");
    
    auto program = arena_.make<ProgramAST>();
    program->filePath = filePath_;
    program->loc = currentLoc();

    // ── 1. package declaration ────────────────────────────────────────────────
    {
        LUC_LOG_PARSER("Parsing package declaration...");
        std::optional<DocComment> pkgDoc = harvestDocComment();

        if (!check(TokenType::PACKAGE)) {
            errorAt(DiagCode::E2001,
                    "expected 'package' declaration at the start of the file");
            synchronize();
            
            // Create a dummy package with unknown name
            auto dummyPkg = arena_.make<PackageDeclAST>(pool_.intern("<unknown>"));
            dummyPkg->loc = currentLoc();
            attachDoc(*dummyPkg, std::move(pkgDoc));
            program->packageName = pool_.intern("<error>");
            program->decls.push_back(std::move(dummyPkg));
        } else {
            auto pkgDecl = parsePackageDecl();
            if (!pkgDecl) {
                // Should not happen with current implementation, but be defensive.
                auto dummyPkg = arena_.make<PackageDeclAST>(pool_.intern("<error>"));
                dummyPkg->loc = currentLoc();
                attachDoc(*dummyPkg, std::move(pkgDoc));
                program->packageName = pool_.intern("<error>");
                program->decls.push_back(std::move(dummyPkg));
            } else {
                attachDoc(*pkgDecl, std::move(pkgDoc));
                program->packageName = pkgDecl->name;
                program->decls.push_back(std::move(pkgDecl));
            }
        }   
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
            auto unknown = arena_.make<UnknownDeclAST>();
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
    // looksLikeFuncDecl() inspects whether a '(' follows (with optional
    // generic params / qualifiers) to decide — this works for @extern too,
    // since an @extern function still has a parameter group.
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

        // looksLikeFuncDecl() checks: IDENTIFIER [<generics>] [~qualifiers] '('
        // This correctly distinguishes functions from variables for both ordinary
        // and @extern declarations without any registry involvement.
        LUC_LOG_PARSER("\tChecking if this looks like a function declaration...");
        bool isFunc = looksLikeFuncDecl();
        LUC_LOG_PARSER("\tlooksLikeFuncDecl: " << (isFunc ? "true" : "false"));
        
        if (isFunc) {
            LUC_LOG_PARSER("\t-> Parsing as function declaration");
            return parseFuncDecl(kw, vis, std::move(attrs));
        } else {
            LUC_LOG_PARSER("\t-> Parsing as variable declaration");
            auto decl = parseVarDecl(vis, std::move(attrs));
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