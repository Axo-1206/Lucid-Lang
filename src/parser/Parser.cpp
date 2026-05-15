/**
 * @file Parser.cpp
 *
 * @responsibility Core parsing infrastructure and top‑level dispatch.
 *
 * This file implements the fundamental parser operations:
 *   - Token stream management (peek, advance, match, consume)
 *   - Error handling and panic‑mode recovery (synchronize)
 *   - Doc comment harvesting and attachment
 *   - Disambiguation helpers (looksLikeType, looksLikeFuncDecl, …)
 *   - Top‑level parsing (parse, parseDeclaration)
 *   - Grammar rules that are not expression‑ or declaration‑specific:
 *       - Parameter groups (parseParamGroup)
 *       - Return lists (parseReturnList)
 *       - Generic parameters (parseGenericParams, parseGenericParam)
 *       - Visibility modifiers (parseVisibility)
 *
 * @related_files
 *   - Parser.hpp – class declaration and interface
 *   - ParserDecl.cpp – top‑level and local declarations
 *   - ParserExpr.cpp – expressions, patterns, pipelines, composition
 *   - ParserStmt.cpp – statements, blocks, control flow
 *   - ParserType.cpp – type annotations
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * NAVIGATION – Functions in this file (in order of appearance)
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * ██ Constructor & Initialisation
 *   Parser::Parser()                     – initialise with token stream
 *
 * ██ Token Stream Primitives
 *   peek()                               – current token (skip comments)
 *   peekNext()                           – one token ahead
 *   peekAt()                             – Nth token ahead
 *   advance()                            – consume current token, skip comments
 *   check() / checkAny()                 – test current token type(s)
 *   match() / matchAny()                 – test + consume if matches
 *   consume()                            – require token, else error + recover
 *   isAtEnd()                            – EOF reached?
 *   currentLoc() / locOf()               – create SourceLocation from token
 *   isPrimitiveTypeToken()               – static helper for primitive keywords
 *
 *██ Token-skip helpers
 *   skipComments()                       – advance index over LINE/DOC comments
 *   skipType()                           – advance over a full type without allocating
 *
 * ██ Error Handling & Recovery
 *   error() / errorAt()                  – record diagnostic
 *   synchronize()                        – skip to next statement/declaration start
 *
 * ██ Visibility & Modifiers
 *   parseVisibility()                    – pub / export / private
 *
 * ██ Function Signature Helpers
 *   parseParamGroup()                    – '(' [ param_list ] ')'
 *   parseReturnList()                    – → type or ( type { ',' type } )
 *
 * ██ Generic Parameters
 *   parseGenericParams()                 – '<' generic_param { ',' generic_param } '>'
 *   parseGenericParam()                  – IDENTIFIER [ ':' trait_constraints ]
 *
 * ██ Documentation Comments
 *   harvestDocComment()                  – collect attached doc comment
 *
 * ██ Disambiguation / Lookahead Helpers
 *   looksLikeType()                      – current token starts a type?
 *   looksLikeFuncDecl()                  – IDENTIFIER [generics] [qualifiers] '(' … ?
 *   looksLikeAnonFunc()                  – '(' param_list ')' [ '->' type ] block ?
 *   looksLikeStructLiteral()             – IDENTIFIER [generics] '{' … ?
 *   looksLikeStmtStart()                 – token can begin a statement?
 *   looksLikeDeclStart()                 – token can begin a declaration?
 *   looksLikeMultiAssignStart()          – IDENTIFIER [suffix] ',' … '=' ?
 *
 * ██ Top‑Level Parsing
 *   parse()                              – program root: package + declarations
 *   parseTopLevelDecl()                  – wrapper for parseDeclaration(TopLevel)
 *   parseDeclaration()                   – unified entry for top‑level & local decls
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Note: Expression, statement, declaration, and type parsers are implemented
 * in their respective translation units (see @related_files). This file
 * contains only the infrastructure and grammar rules that are not specific
 * to those categories.
 * ─────────────────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────────────────
// Token-skip helpers
// ─────────────────────────────────────────────────────────────────────────────

void Parser::skipComments(std::size_t& i) const {
    while (i < tokens_.size() &&
           (tokens_[i].type == TokenType::LINE_COMMENT ||
            tokens_[i].type == TokenType::DOC_COMMENT)) {
        ++i;
    }
}

bool Parser::skipType(std::size_t& i) const {
    skipComments(i);
    if (i >= tokens_.size()) return false;

    // Skip qualifiers: ~async, ~noinline, etc.
    while (i < tokens_.size() && tokens_[i].type == TokenType::TILDE) {
        ++i;
        skipComments(i);
        if (i < tokens_.size() && tokens_[i].type == TokenType::IDENTIFIER) {
            ++i;
            skipComments(i);
        } else {
            return false;
        }
    }

    if (i >= tokens_.size()) return false;

    TokenType tt = tokens_[i].type;
    if (isPrimitiveTypeToken(tt)) {
        ++i;
    } else if (tt == TokenType::IDENTIFIER) {
        ++i;
        skipComments(i);
        // Generic args? < T, U >
        if (i < tokens_.size() && tokens_[i].type == TokenType::LESS) {
            int depth = 1;
            ++i;
            while (i < tokens_.size() && depth > 0) {
                TokenType innerT = tokens_[i].type;
                if (innerT == TokenType::LESS) ++depth;
                else if (innerT == TokenType::GREATER) --depth;
                else if (innerT == TokenType::SEMICOLON || innerT == TokenType::RBRACE) return false;
                ++i;
            }
            if (depth > 0) return false;
        }
    } else if (tt == TokenType::LBRACKET) {
        ++i;
        skipComments(i);
        // [N] or [] or [*]
        if (i < tokens_.size() && (tokens_[i].type == TokenType::INT_LITERAL || tokens_[i].type == TokenType::MUL)) {
            ++i;
            skipComments(i);
        }
        if (i < tokens_.size() && tokens_[i].type == TokenType::RBRACKET) {
            ++i;
            if (!skipType(i)) return false;
        } else {
            return false;
        }
    } else if (tt == TokenType::AMPERSAND || tt == TokenType::MUL) {
        ++i;
        if (!skipType(i)) return false;
    } else if (tt == TokenType::LPAREN) {
        // Function type: (params) -> ret
        int parenDepth = 1;
        ++i;
        while (i < tokens_.size() && parenDepth > 0) {
            TokenType innerT = tokens_[i].type;
            if (innerT == TokenType::LPAREN) ++parenDepth;
            else if (innerT == TokenType::RPAREN) --parenDepth;
            else if (innerT == TokenType::SEMICOLON || innerT == TokenType::RBRACE) return false;
            ++i;
        }
        if (parenDepth > 0) return false;
        
        skipComments(i);
        // Optional return type
        if (i < tokens_.size() && tokens_[i].type == TokenType::ARROW) {
            ++i;
            skipComments(i);
            // return list can be (T, U) or T
            if (i < tokens_.size() && tokens_[i].type == TokenType::LPAREN) {
                int pDepth = 1;
                ++i;
                while (i < tokens_.size() && pDepth > 0) {
                    TokenType innerT = tokens_[i].type;
                    if (innerT == TokenType::LPAREN) ++pDepth;
                    else if (innerT == TokenType::RPAREN) --pDepth;
                    else if (innerT == TokenType::SEMICOLON || innerT == TokenType::RBRACE) return false;
                    ++i;
                }
                if (pDepth > 0) return false;
            } else {
                if (!skipType(i)) return false;
            }
        }
    } else {
        return false;
    }

    // Optional nullable suffix
    skipComments(i);
    if (i < tokens_.size() && tokens_[i].type == TokenType::QUESTION) {
        ++i;
    }

    return true;
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
// Parses a single parameter group: '(' [ param_list ] ')'
//
// Grammar:
//   param_group := '(' [ param_list ] ')'
//   param_list  := param { [','] param } [ [','] variadic_param ]
//   param       := IDENTIFIER type
//   variadic_param := IDENTIFIER '...' type
//
// Returns a vector of ParamAST nodes (may be empty if no parameters).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the opening '(' (expected, reports error if missing).
// - Iterates until the closing ')' or EOF.
// - For each parameter:
//     * Consumes an IDENTIFIER (parameter name).
//     * Optionally consumes a VARIADIC token '...' (if present).
//     * Calls parseType() to consume the type annotation.
// - Finally consumes the closing ')' (reports error if missing).
//
// ─── Loop Safety & Infinite Loop Prevention ─────────────────────────────────
// - After parsing the name and optional '...', the position is saved (savedPos).
// - If parseType() does NOT advance pos_ (pos_ == savedPos):
//     * Reports an error "expected parameter type, no token consumed".
//     * Forces consumption of one token (advance()) to avoid getting stuck.
//     * Breaks out of the parameter loop (no further parameters in this group).
// - If parseType() returns an UnknownTypeAST (invalid syntax):
//     * Reports an error "invalid parameter type".
//     * Breaks out of the loop (does NOT add this parameter).
// - Normal progress (pos_ != savedPos and valid type) adds the parameter and continues.
//
// ─── Error Recovery ─────────────────────────────────────────────────────────
// - On missing '(' or ')', consume(TokenType::LPAREN/RPAREN) records error and
//   consumes at least one token, then returns whatever was parsed (possibly empty).
// - Malformed parameters (missing name, missing type) are skipped, and the loop
//   either breaks or continues depending on the error type, always guaranteeing
//   forward progress.
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
//   return_list := '(' [ return_type { ',' return_type } ] ')'   -- multiple returns
//                | return_type                                    -- single return
//
//   return_type := type
//                | param_group { param_group } [ '->' return_list ]   -- function type
//
// Called after encountering '->' in a function signature or anon‑func body.
// Returns a vector of TypePtr, one per return value. An empty vector indicates
// a void function (no '->' present in the calling context).
//
// The parser distinguishes three cases:
//
// 1. Single return type without parentheses
//    Example: -> int
//    Directly parses the type and returns a vector with one element.
//
// 2. Function type (single return) – parentheses belong to the function type
//    Examples:
//      -> (x int) -> int          -- one parameter, returns int
//      -> () -> int               -- zero parameters, returns int
//      -> ()                      -- zero parameters, returns void
//    Detection:
//      - Empty parentheses "()" always indicate a function type (zero params).
//      - Parentheses containing an identifier followed by a type start
//        (primitive, identifier, '[', '&', '*', '(') indicate a function type.
//    Action: calls parseFuncType() to consume the entire function type and
//            returns a vector with the resulting FuncTypeAST.
//
// 3. Multi‑return list – parentheses group a comma‑separated list of return types
//    Examples:
//      -> (int)                   -- single element, still a multi‑return list
//      -> (int, string)           -- two elements
//      -> (T)                     -- T is a type alias (identifier not followed by type)
//      -> ((x int) -> int, string) -- first element is a nested function type
//    Detection: any parentheses that are not matched by case 2 fall here.
//    Action: consumes the '(' then parses a comma‑separated sequence of types
//            until the matching ')', pushing each type onto the result vector.
//
// Note: Empty parentheses without a following '->' are still a valid function
//       type (zero‑parameter void function). The grammar does not allow an
//       empty multi‑return list, so no ambiguity exists.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<TypePtr> Parser::parseReturnList() {
    // Lambda to check if a token type can start a type
    auto isTypeStart = [](TokenType tt) -> bool {
        return Parser::isPrimitiveTypeToken(tt) ||
               tt == TokenType::IDENTIFIER ||
               tt == TokenType::LBRACKET ||
               tt == TokenType::AMPERSAND ||
               tt == TokenType::MUL ||
               tt == TokenType::LPAREN;
    };

    std::vector<TypePtr> types;

    // No parentheses → single return type
    if (!check(TokenType::LPAREN)) {
        TypePtr t = parseType();
        if (t && !t->isa<UnknownTypeAST>())
            types.push_back(std::move(t));
        return types;
    }

    // Peek inside the parentheses without consuming them
    std::size_t lookahead = pos_ + 1;
    skipComments(lookahead);
    if (lookahead >= tokens_.size()) {
        errorAt(DiagCode::E2002, "unexpected end of file after '('");
        return types;
    }

    TokenType afterParen = tokens_[lookahead].type;

    // Case: empty parentheses "()" → zero‑parameter function type
    if (afterParen == TokenType::RPAREN) {
        TypePtr funcType = parseFuncType();
        if (funcType && !funcType->isa<UnknownTypeAST>())
            types.push_back(std::move(funcType));
        return types;
    }

    // Case: identifier followed by a type start → function type
    if (afterParen == TokenType::IDENTIFIER) {
        std::size_t afterIdent = lookahead + 1;
        skipComments(afterIdent);
        if (afterIdent < tokens_.size() && isTypeStart(tokens_[afterIdent].type)) {
            TypePtr funcType = parseFuncType();
            if (funcType && !funcType->isa<UnknownTypeAST>())
                types.push_back(std::move(funcType));
            return types;
        }
    }

    // Otherwise: parenthesised multi‑return list
    advance(); // consume '('
    while (!check(TokenType::RPAREN) && !isAtEnd()) {
        match(TokenType::COMMA);
        if (check(TokenType::RPAREN)) break;
        TypePtr t = parseType();
        if (t && !t->isa<UnknownTypeAST>())
            types.push_back(std::move(t));
    }
    consume(TokenType::RPAREN, DiagCode::E2001, "expected ')' to close return type list");
    return types;
}

// ─────────────────────────────────────────────────────────────────────────────
// Generic Parameters
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseGenericParams
//
// Parses a list of generic type parameters on a declaration.
//
// Grammar:
//   generic_params := '<' generic_param { [','] generic_param } '>'
//
// Called on the declaration side (func, struct, trait, impl, type alias).
// For the use side (generic arguments at call sites), see parseGenericArgs()
// in ParserType.cpp.
//
// Returns a vector of GenericParamPtr (may be empty if no parameters or empty list).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the opening '<' (expected, reports error if missing).
// - If the next token is '>', consumes it and returns empty list (zero params).
// - Iterates until '>' or EOF:
//     * Optionally consumes a COMMA (between parameters).
//     * Calls parseGenericParam() to consume one generic parameter.
// - Finally consumes the closing '>' (reports error if missing).
//
// ─── Loop Safety & Infinite Loop Prevention ─────────────────────────────────
// - Uses a consecutive error counter (max 5) to prevent endless error loops.
// - For each iteration:
//     * Saves current position (savedPos) before calling parseGenericParam().
//     * If parseGenericParam() returns nullptr:
//         - If pos_ == savedPos (no progress): consumes one token, increments error
//           counter, sets stalled flag, and breaks out of loop.
//         - Else (progress made but parsing failed): increments error counter,
//           continues to next parameter (does NOT push anything).
//     * If parseGenericParam() succeeds: pushes parameter, resets error counter.
// - If consecutiveErrors reaches MAX_CONSECUTIVE_ERRORS (5):
//     * Reports an error and skips tokens until the closing '>' (or EOF).
//     * Sets stalled flag, then exits loop.
// - After loop exits (either normally or stalled), if stalled, skips remaining
//   tokens until '>' to ensure parser can continue.
// - Finally consumes the closing '>' – if missing, reports error.
//
// ─── Error Recovery ─────────────────────────────────────────────────────────
// - Missing '<' or '>' are caught by consume() which records an error and forces
//   token consumption.
// - Invalid generic parameters are skipped individually; the loop continues to
//   parse the next parameter if possible.
// - Too many errors cause a skip to the closing '>', avoiding a cascading failure.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<GenericParamPtr> Parser::parseGenericParams() {
    LUC_LOG_PARSER_VERBOSE("parseGenericParams: starting");
    std::vector<GenericParamPtr> params;
    int consecutiveErrors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 5;

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

        // Safety: too many errors → skip to '>'
        if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
            errorAt(DiagCode::E2002, "too many consecutive errors in generic parameters; skipping to '>'");
            while (!isAtEnd() && !check(TokenType::GREATER)) {
                advance();
            }
            stalled = true; // we will break after consuming '>'
            break;
        }

        std::size_t savedPos = pos_;
        GenericParamPtr gp = parseGenericParam();

        if (!gp) {
            if (pos_ == savedPos) {
                // No progress: consume one token to avoid infinite loop
                errorAt(DiagCode::E2002, "expected generic parameter, skipping token '" + peek().value + "'");
                if (!isAtEnd()) advance();
                consecutiveErrors++;
                stalled = true;
                break;
            } else {
                // Progress was made but parsing failed (e.g., invalid constraint)
                consecutiveErrors++;
                // Do not push anything; continue to next parameter
                continue;
            }
        }

        // Successfully parsed a generic parameter
        params.push_back(std::move(gp));
        consecutiveErrors = 0; // reset error counter on success

    } while (!check(TokenType::GREATER) && !isAtEnd() && !stalled);

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
// Parses a single generic type parameter (with optional trait constraints).
//
// Grammar:
//   generic_param   := IDENTIFIER
//                    | IDENTIFIER ':' IDENTIFIER
//                    | IDENTIFIER ':' IDENTIFIER { '+' IDENTIFIER }
//
// Examples:
//   T
//   K : Hashable
//   V : Hashable + Comparable + Printable
//
// Returns a GenericParamPtr (never nullptr on success; on error returns nullptr
// after reporting an error).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes an IDENTIFIER (the parameter name). If missing, reports error and
//   returns nullptr (caller must handle recovery).
// - Optionally, if a COLON ':' is present:
//     * Consumes the first trait name (IDENTIFIER) – required after ':'.
//     * While a PLUS '+' is present: consumes the next trait name (IDENTIFIER).
// - Does NOT consume any tokens beyond the constraint list (i.e., stops before
//   the next comma, '>', or end of parameter list).
//
// ─── Loop Safety ────────────────────────────────────────────────────────────
// - No long‑running loops; the only loop is over optional '+' constraints.
// - Each iteration consumes exactly one '+' token and one IDENTIFIER.
// - If a '+' is present but no IDENTIFIER follows, reports error and breaks
//   the loop (prevents infinite loop on malformed input).
// - The loop terminates naturally when no further '+' tokens are found.
//
// ─── Error Recovery ─────────────────────────────────────────────────────────
// - Missing identifier after ':' or '+' triggers an error, but the function still
//   returns the partially constructed GenericParamAST (with whatever constraints
//   were successfully parsed). This allows the caller to continue parsing.
// - The parser does NOT attempt to skip ahead; it simply stops consuming further
//   constraints and returns. The caller (parseGenericParams) may then decide to
//   skip the entire parameter or continue to the next one.
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
// Doc comment harvesting 
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
// Disambiguation / Lookahead Helpers
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// looksLikeType
//
// Predicate that checks whether the current token stream begins with a valid
// type annotation (primitive, named, array, reference, pointer, or function type).
//
// ─── Pure Lookahead – No Token Consumption ─────────────────────────────────
// - Inspects tokens starting at pos_, skipping comments, but does NOT modify pos_.
// - Used by callers to decide whether to parse a type or something else.
//
// ─── Detection Strategy ─────────────────────────────────────────────────────
// - Returns true if the current token is a primitive keyword, IDENTIFIER,
//   LBRACKET, AMPERSAND, MUL, TILDE (qualifier start of function type),
//   or LPAREN (possible function type start).
// - For TILDE '~': peeks at the next token; if it is an IDENTIFIER, returns true
//   (qualifier list is part of a function type). Otherwise false.
// - For LPAREN '(': returns true conservatively; the caller may need to further
//   disambiguate between a parenthesised type and a grouped expression (but
//   in type‑annotation positions, '(' always starts a function type).
// - For all other tokens, returns false.
//
// ─── Loop Safety & Infinite Loop Prevention ─────────────────────────────────
// - No loops; only a single switch statement with constant‑time lookahead.
// - Uses peek() and peekNext() which skip comments internally and never advance pos_.
// - Cannot cause infinite recursion or stalling.
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

// ─────────────────────────────────────────────────────────────────────────────
// looksLikeFuncDecl
//
// Predicate that checks whether the current position looks like a function
// declaration (as opposed to a variable declaration) after 'let' or 'const'.
//
// ─── Pure Lookahead – No Token Consumption ─────────────────────────────────
// - Scans forward using a local index `i` (copy of pos_), never modifies pos_.
// - Skips comments using skipComments(i) where necessary.
//
// ─── Detection Strategy ─────────────────────────────────────────────────────
// 1. Current token must be IDENTIFIER (function name). If not, return false.
// 2. Optional generic parameter list: < ... > – balanced bracket counting.
//    - If unbalanced (depth != 0 after scanning), return false.
// 3. Optional qualifier list: zero or more '~' followed by IDENTIFIER.
// 4. After skipping these, the next token must be '(' (start of parameter group).
//    If not, return false.
//
// ─── Loop Safety & Infinite Loop Prevention ─────────────────────────────────
// - The generic parameter scan uses a depth counter and iterates token by token.
// - Each iteration increments `i`; the loop terminates when depth reaches 0 or
//   end of file/statement boundary (SEMICOLON, RBRACE, EOF) is encountered.
// - The qualifier loop iterates at most once per '~' token; each iteration consumes
//   two tokens ('~' and IDENTIFIER) or fails early.
// - The overall function is bounded by the length of the token stream, and
//   the loops are guaranteed to make progress because `i` is incremented each
//   iteration.
// - Because this is a pure lookahead, infinite recursion is impossible.
// ─────────────────────────────────────────────────────────────────────────────
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
            skipComments(i);
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
        skipComments(i);
    }

    // we need at least one parameter group '('
    if (!(i < tokens_.size() && tokens_[i].type == TokenType::LPAREN)) {
        LUC_LOG_PARSER_VERBOSE("\tno '(' found after name/generics/qualifiers, returning false");
        return false;
    }

    LUC_LOG_PARSER_VERBOSE("\tlooksLikeFuncDecl: returning true");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// looksLikeAnonFunc
//
// Predicate that checks whether the current position looks like an anonymous
// function expression (as opposed to a parenthesised grouped expression).
//
// Grammar of an anonymous function:
//   [ '~' IDENTIFIER ]* '(' param_list ')' { '(' param_list ')' }
//   [ '->' return_list ] block
//
// ─── Pure Lookahead – No Token Consumption ─────────────────────────────────
// - Uses a local index `i` that starts at pos_. Does not modify parser state.
// - Skips comments using skipComments(i) and inline checks.
//
// ─── Detection Strategy ─────────────────────────────────────────────────────
// 1. Skip optional qualifiers ('~' IDENTIFIER) – though anonymous functions
//    should not have them, the predicate still handles them.
// 2. There must be at least one '(' (start of first parameter group).
// 3. Parse the first parameter group by scanning for matching ')' (handles nesting).
//    - Uses a helper lambda parseOneGroup that advances `i` past a complete
//      parameter group, respecting parentheses depth.
// 4. Parse additional curried parameter groups (zero or more) similarly.
// 5. After all parameter groups, skip comments and look at the next token:
//    - If it is '{' → void anonymous function (no return type), return true.
//    - If it is '->' → advance past '->', skip comments, and check if the following
//      token can start a type (primitive, identifier, '[', '&', '*', '(').
//      If yes → return true.
// 6. Otherwise return false.
//
// ─── Loop Safety & Infinite Loop Prevention ─────────────────────────────────
// - The outer while loop over curried groups calls parseOneGroup each time.
// - parseOneGroup advances `j` until the matching ')' is found; it strictly
//   increments the index, so it makes progress.
// - If parseOneGroup returns without making progress (i == startPos), the function
//   returns false immediately, preventing an infinite loop.
// - The total number of iterations is bounded by the number of '(' tokens in the
//   token stream.
// - Because `i` is a local copy, the parser’s actual position (pos_) is untouched.
// ─────────────────────────────────────────────────────────────────────────────
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
        skipComments(i);
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

// ─────────────────────────────────────────────────────────────────────────────
// looksLikeStructLiteral
//
// Predicate that checks whether the current position looks like a struct literal
// expression: IDENTIFIER [ generic_args ] '{' ...
//
// Used in parsePrimaryExpr to distinguish a struct literal from a plain identifier
// or a block statement.
//
// ─── Pure Lookahead – No Token Consumption ─────────────────────────────────
// - Uses a local index `i` starting at pos_ + 1. Does not modify pos_.
// - Skips comments inline where necessary.
//
// ─── Detection Strategy ─────────────────────────────────────────────────────
// 1. Current token must be IDENTIFIER (struct type name). If not, return false.
// 2. Optional generic argument list: `< ... >` – balanced bracket counting.
//    - Depth starts at 0; if a '<' is found, depth becomes 1 and the scan
//      proceeds until depth returns to 0 or a statement boundary is hit.
//    - Comments are skipped during the scan.
// 3. After the optional generic list (or immediately after the identifier),
//    the next token must be LBRACE '{'. If not, return false.
// 4. Returns true only if depth == 0 and a '{' is found.
//
// ─── Loop Safety & Infinite Loop Prevention ─────────────────────────────────
// - The generic argument scan uses a depth counter and strictly increments `i`
//   each iteration. The loop terminates when depth == 0 or end of file/statement
//   boundary is reached.
// - If the generic brackets are unbalanced (depth never returns to 0), the function
//   returns false (since the final condition requires depth == 0).
// - Because the scan is bounded by the token stream length and each iteration
//   advances `i`, infinite loops are impossible.
// - The function does not call any other parsing functions that might modify state.
// ─────────────────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────────────────
// looksLikeStmtStart
//
// Predicate that checks whether the current token can begin a statement.
//
// Used in parseStmt() to decide whether to parse a statement or treat the input
// as an expression statement (fallback).
//
// ─── Pure Lookahead – No Token Consumption ─────────────────────────────────
// - Only inspects the current token (peek()) and possibly its type category.
// - Does not advance pos_ or modify any state.
//
// ─── Detection Strategy ─────────────────────────────────────────────────────
// Returns true if the current token is any of:
//   - Declaration / control‑flow keywords:
//       LET, CONST, IF, FOR, WHILE, DO, RETURN, BREAK, CONTINUE, MATCH, SWITCH
//   - Compiler directives:
//       AT_SIGN, HASH
//   - Type / local declaration keywords:
//       TYPE, STRUCT, ENUM, TRAIT, IMPL, FROM
//   - Otherwise, falls back to checking expression starters:
//       looksLikeType(), IDENTIFIER, primitive type keywords, literals,
//       unary operators (MINUS, NOT, BIT_NOT, AMPERSAND), AWAIT, LPAREN.
//
// ─── Loop Safety & Infinite Loop Prevention ─────────────────────────────────
// - No loops; only a switch statement with constant‑time checks.
// - Calls looksLikeType() which itself is a pure lookahead with no loops.
// - Cannot cause infinite recursion or stalling.
// ─────────────────────────────────────────────────────────────────────────────
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
        case TokenType::TYPE:
        // local declarations
        case TokenType::STRUCT:
        case TokenType::ENUM:
        case TokenType::TRAIT:
        case TokenType::IMPL:
        case TokenType::FROM:
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

// ─────────────────────────────────────────────────────────────────────────────
// looksLikeDeclStart
//
// Predicate that checks whether the current token can begin a top‑level or local
// declaration (as opposed to a statement or expression).
//
// Used in synchronize() to stop skipping tokens when a declaration boundary is
// reached during error recovery.
//
// ─── Pure Lookahead – No Token Consumption ─────────────────────────────────
// - Only inspects the current token (peek()). Does not modify pos_.
//
// ─── Detection Strategy ─────────────────────────────────────────────────────
// Returns true if the current token is any of:
//   - AT_SIGN (attribute, may precede any declaration)
//   - PACKAGE, USE, PUB, EXPORT
//   - STRUCT, ENUM, TRAIT, IMPL, TYPE, FROM
//   - LET, CONST
// For all other token types, returns false.
//
// ─── Loop Safety & Infinite Loop Prevention ─────────────────────────────────
// - No loops; simple switch statement.
// - Cannot cause infinite recursion or stalling.
// ─────────────────────────────────────────────────────────────────────────────
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
// looksLikeMultiAssignStart
//
// Predicate that checks whether the current position looks like a multi‑assignment
// statement (reassignment to multiple existing variables), as opposed to a
// single assignment or a multi‑variable declaration.
//
// Used in parseStmt() to detect patterns like:
//   a, b = f()          -- reassignment
//   arr[i], obj.field = g()   -- with indices and field accesses
//
// ─── Pure Lookahead – No Token Consumption ─────────────────────────────────
// - Uses a local index `i` starting at pos_. Does not modify pos_.
// - Skips comments using skipComments(i) where needed.
// - Does NOT parse full expressions; only scans for syntactic pattern.
//
// ─── Detection Strategy ─────────────────────────────────────────────────────
// 1. Skip leading comments.
// 2. The first token must be IDENTIFIER (start of first lvalue). If not, false.
// 3. Consume the identifier, then optionally parse suffixes that are part of
//    the same lvalue:
//        - '.' IDENTIFIER (field access)
//        - '[' ... ']' (array/slice index, skips bracket content with depth)
//    The scan stops when a token that cannot be part of an lvalue is encountered.
// 4. After the first lvalue, skip comments and expect a COMMA ','.
//    If no comma, return false (not a multi‑assign).
// 5. Once a comma is found, scan forward until we see:
//        - ASSIGN '=' → return true (multi‑assign)
//        - SEMICOLON, LBRACE, RBRACE, EOF → return false (no assignment)
//    Any other tokens are skipped (they are part of subsequent lvalues).
//
// ─── Loop Safety & Infinite Loop Prevention ─────────────────────────────────
// - The suffix scanning loop increments `i` each iteration (consumes '.' + field
//   or bracket contents). Bracket scanning uses a depth counter and strictly
//   advances `i`; it terminates when depth reaches 0 or end of file.
// - The final scan for '=' also increments `i` each iteration, bounded by the
//   distance to the next ASSIGN or a statement‑terminating token.
// - Because `i` is a local copy and always moves forward, the function cannot
//   loop indefinitely. Each pass over a token consumes it exactly once.
// - The function does not allocate AST nodes or call parseType/parseExpr,
//   so no side effects occur.
// ─────────────────────────────────────────────────────────────────────────────
bool Parser::looksLikeMultiAssignStart() const {
    std::size_t i = pos_;  // start from current parser position (without consuming)

    // ── 1. Skip leading comments ─────────────────────────────────────────────
    while (i < tokens_.size() && (tokens_[i].type == TokenType::LINE_COMMENT ||
                                  tokens_[i].type == TokenType::DOC_COMMENT))
        ++i;
    if (i >= tokens_.size()) return false;

    // ── 2. The first token of the first lvalue must be an identifier ─────────
    //    Example: "pair" in "pair.first, ..." or "arr" in "arr[0], ..."
    if (tokens_[i].type != TokenType::IDENTIFIER) return false;
    ++i;

    // ── 3. Parse optional suffixes that are part of the *same* lvalue ────────
    //    Allowed suffixes: .field   or   [expr]   (both are assignable locations)
    //    We stop when we encounter a token that cannot be part of an lvalue.
    while (i < tokens_.size()) {
        // Skip comments before checking suffix
        while (i < tokens_.size() && (tokens_[i].type == TokenType::LINE_COMMENT ||
                                      tokens_[i].type == TokenType::DOC_COMMENT))
            ++i;
        if (i >= tokens_.size()) break;

        // ── Field access: .identifier ────────────────────────────────────────
        if (tokens_[i].type == TokenType::DOT) {
            ++i;
            // Skip comments after '.'
            while (i < tokens_.size() && (tokens_[i].type == TokenType::LINE_COMMENT ||
                                          tokens_[i].type == TokenType::DOC_COMMENT))
                ++i;
            if (i >= tokens_.size()) return false;
            if (tokens_[i].type != TokenType::IDENTIFIER) return false;
            ++i;  // consume the field name
        }
        // ── Array / slice index: [ expr ] ────────────────────────────────────
        //    Handles simple and nested brackets (e.g., arr[0] or matrix[2][3]).
        //    We skip the whole expression inside brackets because we only need
        //    to know that there is an index, not its content.
        else if (tokens_[i].type == TokenType::LBRACKET) {
            ++i;
            int bracketDepth = 1;
            while (i < tokens_.size() && bracketDepth > 0) {
                if (tokens_[i].type == TokenType::LBRACKET)
                    ++bracketDepth;
                else if (tokens_[i].type == TokenType::RBRACKET)
                    --bracketDepth;
                ++i;  // move past this token
                // Note: We do NOT check for comments inside brackets – they
                // are already ignored because they are not LBRACKET/RBRACKET.
            }
            // If brackets are unbalanced, i may go out of bounds; treat as false.
            if (bracketDepth != 0) return false;
        }
        else {
            break;  // No more lvalue suffixes – end of the first lvalue
        }
    }

    // ── 4. After finishing the first lvalue, look for a comma ────────────────
    skipComments(i);
    if (i >= tokens_.size()) return false;
    if (tokens_[i].type != TokenType::COMMA) return false;

    // ── 5. A comma means there are at least two lvalues. Now scan for '=' ─────
    ++i;  // skip the comma
    while (i < tokens_.size()) {
        TokenType tt = tokens_[i].type;
        if (tt == TokenType::ASSIGN) return true;          // found the '=' – it's a multi‑assign
        if (tt == TokenType::SEMICOLON || tt == TokenType::LBRACE ||
            tt == TokenType::RBRACE || tt == TokenType::EOF_TOKEN)
            return false;                                  // statement ends without '='
        ++i;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Top‑Level Parsing
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parse
//
// Parses the entire token stream and returns the root ProgramAST node.
//
// Grammar:
//   program := package_decl { top_level_decl }
//
// ─── Overall Flow ───────────────────────────────────────────────────────────
// 1. Creates a ProgramAST node.
// 2. Parses the mandatory package declaration (first non‑comment line).
// 3. Repeatedly parses top‑level declarations (functions, structs, enums, etc.)
//    until EOF_TOKEN is reached.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes tokens sequentially from pos_ = 0 to end.
// - After parsing the package declaration, pos_ is positioned at the first token
//   after the package name.
// - For each top‑level declaration, calls parseTopLevelDecl() which consumes the
//   entire declaration (including any trailing semicolons or whitespace).
// - Stops when peek() returns EOF_TOKEN.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - If the package declaration is missing or invalid, a dummy PackageDeclAST is
//   inserted and synchronize() is called to skip to the next valid token.
// - If parseTopLevelDecl() returns nullptr (parsing failure), an UnknownDeclAST
//   is inserted and synchronize() is called to resume parsing at the next
//   declaration boundary.
// - All errors are reported via the diagnostic engine (dc_).
// - The function never returns nullptr; it always returns a ProgramAST, even if
//   it contains UnknownDeclAST nodes. The caller should check dc_.hasErrors()
//   to determine whether the parse was successful.
//
// ─── Infinite Loop Prevention ───────────────────────────────────────────────
// - The main loop advances by consuming at least one token per iteration:
//     * On success, parseTopLevelDecl() consumes the declaration.
//     * On failure, synchronize() consumes tokens until a declaration boundary.
// - The loop terminates when isAtEnd() returns true (EOF_TOKEN reached).
// - No unbounded recursion; each top‑level declaration is parsed independently.
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
//
// Convenience wrapper that calls parseDeclaration(DeclContext::TopLevel).
//
// ─── Purpose ────────────────────────────────────────────────────────────────
// Provides a named entry point for top‑level declaration parsing, used by
// parse() and potentially other callers that need to parse a declaration in
// file scope.
//
// ─── Token Consumption ──────────────────────────────────────────────────────
// - Delegates entirely to parseDeclaration(TopLevel). The token consumption,
//   error recovery, and loop safety are the same as parseDeclaration.
// - Does not consume any tokens on its own; merely forwards the call.
//
// ─── Error Handling ─────────────────────────────────────────────────────────
// - Returns nullptr if no valid declaration could be parsed. The caller
//   (parse()) is responsible for inserting an UnknownDeclAST and calling
//   synchronize().
// - Errors are reported via the diagnostic engine inside parseDeclaration.
// ─────────────────────────────────────────────────────────────────────────────
DeclPtr Parser::parseTopLevelDecl() {
    return parseDeclaration(DeclContext::TopLevel);
}

// ─────────────────────────────────────────────────────────────────────────────
// parseDeclaration
//
// Unified entry point for parsing a declaration (either top‑level or local).
//
// Grammar (common prefixes):
//   declaration := [ attributes ] [ visibility ] actual_decl
//
//   actual_decl := use_decl | struct_decl | enum_decl | trait_decl | type_decl
//                | impl_decl | from_decl | var_decl | func_decl
//
// ─── Context (DeclContext) ──────────────────────────────────────────────────
// - TopLevel   : File scope. Visibility modifiers (pub/export) and attributes
//                are allowed. The entire file is parsed with this context.
// - Local      : Inside a block. Visibility modifiers are forbidden (error).
//                Attributes are still allowed.
//
// ─── Token Consumption ──────────────────────────────────────────────────────
// 1. Parses zero or more attributes (parseAttributes()) – consumes '@' tokens.
// 2. If context == TopLevel, parses optional visibility (pub/export) token.
//    If context == Local and pub/export appears, reports error and consumes it.
// 3. Dispatches to the appropriate sub‑parser based on the current token:
//        USE, STRUCT, ENUM, TRAIT, IMPL, FROM, TYPE, LET, CONST.
//    Each sub‑parser consumes the complete declaration (including its body,
//    trailing semicolon, etc.).
// 4. After successful dispatch, attaches the parsed attributes to the declaration
//    node and sets its source location.
//
// ─── Distinguishing Variable vs Function Declaration ───────────────────────
// - For LET and CONST, the sub‑parser cannot be determined by the keyword alone.
//   The function looks ahead using looksLikeFuncDecl() to decide:
//        true  → parseFuncDecl()
//        false → parseVarDecl()
//   This lookahead does not consume tokens; it only inspects the token stream.
//
// ─── Error Handling & Recovery ─────────────────────────────────────────────
// - If the current token does not match any known declaration start:
//     * Reports an error (appropriate message based on context).
//     * Returns nullptr; the caller (parse()) inserts an UnknownDeclAST and calls
//       synchronize() to recover.
// - If an attribute or visibility modifier is present but no valid declaration
//   follows, an error is reported and nullptr is returned.
// - Sub‑parsers are responsible for their own internal error recovery and
//   progress guarantees.
//
// ─── Infinite Loop Prevention ───────────────────────────────────────────────
// - The function does not contain long loops; dispatch is a single pass.
// - Sub‑parsers (e.g., parseFuncDecl, parseStructDecl) are responsible for
//   making progress and not stalling (they have their own safety guards).
// - The lookahead for looksLikeFuncDecl() is pure and does not modify state.
// - If no dispatch matches, the function returns nullptr without consuming
//   any tokens – the caller must consume one token or call synchronize() to
//   avoid infinite loops.
// ─────────────────────────────────────────────────────────────────────────────
DeclPtr Parser::parseDeclaration(DeclContext ctx) {
    SourceLocation loc = currentLoc();
    LUC_LOG_PARSER("parseDeclaration: ctx=" << (ctx == DeclContext::TopLevel ? "TopLevel" : "Local"));

    // ── Parse attributes (allowed in both contexts) ──────────────────────
    std::vector<AttributePtr> attrs = parseAttributes();

    // ── Visibility modifier (only at top‑level) ───────────────────────────
    Visibility vis = Visibility::Private;
    if (ctx == DeclContext::TopLevel) {
        vis = parseVisibility();
    } else {
        // In local context, pub/export are errors
        if (checkAny({TokenType::PUB, TokenType::EXPORT})) {
            errorAt(DiagCode::E2006, "'pub'/'export' not allowed inside a block");
            advance(); // consume the modifier and ignore it
        }
    }

    // ── Dispatch based on the current token ──────────────────────────────
    DeclPtr decl;

    if (check(TokenType::USE)) {
        if (!attrs.empty())
            errorAt(DiagCode::E2002, "attributes are not valid on 'use' declarations");
        decl = parseUseDecl(vis);
    }
    else if (check(TokenType::STRUCT)) {
        decl = parseStructDecl(vis);
    }
    else if (check(TokenType::ENUM)) {
        if (ctx == DeclContext::TopLevel && !attrs.empty())
            errorAt(DiagCode::E2002, "attributes are not valid on 'enum' declarations");
        decl = parseEnumDecl(vis);
    }
    else if (check(TokenType::TRAIT)) {
        if (ctx == DeclContext::TopLevel && !attrs.empty())
            errorAt(DiagCode::E2002, "attributes are not valid on 'trait' declarations");
        decl = parseTraitDecl(vis);
    }
    else if (check(TokenType::IMPL)) {
        decl = parseImplDecl(vis);
    }
    else if (check(TokenType::FROM)) {
        decl = parseFromDecl(vis);
    }
    else if (check(TokenType::TYPE)) {
        decl = parseTypeAliasDecl(vis);
    }
    else if (checkAny({TokenType::LET, TokenType::CONST})) {
        // For let/const we need to distinguish var vs func, but the low‑level parsers
        // expect the keyword to be consumed already. However, they also need the
        // identifier and lookahead. We'll handle let/const as a special case here
        // because they are the only ones that can be either a variable or a function.
        // Better: create a helper that consumes the keyword, checks for function shape,
        // and calls parseVarDecl or parseFuncDecl.
        Token kwTok = advance();
        DeclKeyword kw = (kwTok.type == TokenType::LET) ? DeclKeyword::Let : DeclKeyword::Const;

        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected name after '" + kwTok.value + "'");
            return nullptr;
        }

        if (looksLikeFuncDecl()) {
            decl = parseFuncDecl(kw, vis, std::move(attrs));
        } else {
            decl = parseVarDecl(vis, std::move(attrs));
        }
    }
    else {
        // No valid declaration start
        if (!attrs.empty()) {
            errorAt(DiagCode::E2002, "expected a declaration after '@' attribute(s)");
        } else if (ctx == DeclContext::TopLevel && vis != Visibility::Private) {
            errorAt(DiagCode::E2002, "expected a declaration after modifier");
        } else {
            errorAt(DiagCode::E2002, "unexpected token '" + peek().value + "'");
        }
        return nullptr;
    }

    if (decl) {
        // Attach attributes to the declaration (BaseAST field)
        decl->attributes = std::move(attrs);
        decl->loc = loc;
    }
    return decl;
}