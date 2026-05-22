/**
 * @file ParserLookahead.cpp
 * 
 * Lookahead helpers for disambiguating syntax during parsing.
 * These functions inspect the token stream without consuming tokens.
 */

#include "Parser.hpp"
#include "debug/DebugUtils.hpp"

// -----------------------------------------------------------------------------
// looksLikeType
// -----------------------------------------------------------------------------

bool Parser::looksLikeType() const {
    TokenType tt = ts_.peekType();
    
    if (isPrimitiveTypeToken(tt)) return true;
    if (tt == TokenType::IDENTIFIER) return true;
    if (tt == TokenType::LBRACKET) return true;
    if (tt == TokenType::AMPERSAND) return true;
    if (tt == TokenType::MUL) return true;
    if (tt == TokenType::TILDE) return true;
    if (tt == TokenType::LPAREN) return true;
    
    return false;
}

// -----------------------------------------------------------------------------
// looksLikeFuncDecl
// -----------------------------------------------------------------------------

bool Parser::looksLikeFuncDecl() const {
    const auto& tokens = ts_.getTokens();
    size_t tokenCount = ts_.getTokenCount();
    size_t i = ts_.getPos();
    
    // Helper to skip comments
    auto skipComments = [&](size_t& idx) {
        while (idx < tokenCount && 
               (tokens[idx].type == TokenType::LINE_COMMENT ||
                tokens[idx].type == TokenType::DOC_COMMENT)) {
            ++idx;
        }
    };

    // Skip the name IDENTIFIER
    skipComments(i);
    if (i >= tokenCount || tokens[i].type != TokenType::IDENTIFIER) {
        return false;
    }
    ++i;

    // Skip generic params if present: < ... >
    skipComments(i);
    if (i < tokenCount && tokens[i].type == TokenType::LESS) {
        int depth = 1;
        ++i;
        while (i < tokenCount && depth > 0) {
            skipComments(i);
            if (i >= tokenCount) break;
            TokenType tt = tokens[i].type;
            if (tt == TokenType::LESS) ++depth;
            else if (tt == TokenType::GREATER) --depth;
            else if (tt == TokenType::SEMICOLON || tt == TokenType::RBRACE) break;
            else if (tt == TokenType::EOF_TOKEN) break;
            ++i;
        }
        if (depth != 0) return false;
    }

    // Skip type qualifiers (~async, ~noinline, etc.)
    skipComments(i);
    while (i < tokenCount && tokens[i].type == TokenType::TILDE) {
        ++i;
        skipComments(i);
        if (i < tokenCount && tokens[i].type == TokenType::IDENTIFIER) {
            ++i;
        } else {
            return false;
        }
        skipComments(i);
    }

    // Need at least one parameter group '('
    skipComments(i);
    if (i >= tokenCount || tokens[i].type != TokenType::LPAREN) {
        return false;
    }

    return true;
}

// -----------------------------------------------------------------------------
// looksLikeAnonFunc
// -----------------------------------------------------------------------------

bool Parser::looksLikeAnonFunc() const {
    const auto& tokens = ts_.getTokens();
    size_t tokenCount = ts_.getTokenCount();
    size_t i = ts_.getPos();
    
    // Helper to skip comments
    auto skipComments = [&](size_t& idx) {
        while (idx < tokenCount && 
               (tokens[idx].type == TokenType::LINE_COMMENT ||
                tokens[idx].type == TokenType::DOC_COMMENT)) {
            ++idx;
        }
    };

    // Skip type qualifiers (though anonymous functions shouldn't have them)
    while (i < tokenCount) {
        skipComments(i);
        if (i >= tokenCount || tokens[i].type != TokenType::TILDE)
            break;
        ++i;
        skipComments(i);
        if (i < tokenCount && tokens[i].type == TokenType::IDENTIFIER) {
            ++i;
        } else {
            return false;
        }
    }

    // First parameter group is required
    skipComments(i);
    if (i >= tokenCount || tokens[i].type != TokenType::LPAREN)
        return false;

    // Helper to parse a parameter group and return index after matching ')'
    auto parseOneGroup = [&](size_t start) -> size_t {
        if (start >= tokenCount || tokens[start].type != TokenType::LPAREN)
            return start;
        int parenDepth = 1;
        size_t j = start + 1;
        while (j < tokenCount && parenDepth > 0) {
            while (j < tokenCount && 
                   (tokens[j].type == TokenType::LINE_COMMENT ||
                    tokens[j].type == TokenType::DOC_COMMENT)) {
                ++j;
            }
            if (j >= tokenCount) break;
            TokenType tt = tokens[j].type;
            if (tt == TokenType::LPAREN) {
                ++parenDepth;
            } else if (tt == TokenType::RPAREN) {
                --parenDepth;
            } else if (tt == TokenType::TILDE) {
                ++j;
                if (j < tokenCount && tokens[j].type == TokenType::IDENTIFIER) ++j;
                continue;
            }
            ++j;
        }
        return j;
    };

    size_t startPos = i;
    i = parseOneGroup(i);
    if (i == startPos) return false;

    // Parse additional curried parameter groups
    while (i < tokenCount) {
        skipComments(i);
        if (i >= tokenCount || tokens[i].type != TokenType::LPAREN)
            break;
        startPos = i;
        i = parseOneGroup(i);
        if (i == startPos) return false;
    }

    // Skip comments after the last ')'
    skipComments(i);
    if (i >= tokenCount) return false;

    // Immediate '{' → void anonymous function
    if (tokens[i].type == TokenType::LBRACE)
        return true;

    // Otherwise, require '->' followed by a return type
    if (tokens[i].type != TokenType::ARROW)
        return false;

    ++i;
    skipComments(i);
    if (i >= tokenCount) return false;

    // The token after '->' must start a type
    TokenType retStart = tokens[i].type;
    if (isPrimitiveTypeToken(retStart)) return true;
    switch (retStart) {
        case TokenType::IDENTIFIER:
        case TokenType::LBRACKET:
        case TokenType::AMPERSAND:
        case TokenType::MUL:
        case TokenType::LPAREN:
        case TokenType::TILDE:
            return true;
        default:
            return false;
    }
}

// -----------------------------------------------------------------------------
// looksLikeStructLiteral
// -----------------------------------------------------------------------------

bool Parser::looksLikeStructLiteral() const {
    const auto& tokens = ts_.getTokens();
    size_t tokenCount = ts_.getTokenCount();
    size_t i = ts_.getPos();
    
    auto skipComments = [&](size_t& idx) {
        while (idx < tokenCount && 
               (tokens[idx].type == TokenType::LINE_COMMENT ||
                tokens[idx].type == TokenType::DOC_COMMENT)) {
            ++idx;
        }
    };

    skipComments(i);
    if (i >= tokenCount || tokens[i].type != TokenType::IDENTIFIER) {
        return false;
    }

    ++i;
    skipComments(i);
    if (i >= tokenCount) return false;

    // Optional generic args: < ... >
    if (tokens[i].type == TokenType::LESS) {
        int depth = 1;
        ++i;
        while (i < tokenCount && depth > 0) {
            skipComments(i);
            if (i >= tokenCount) break;
            TokenType tt = tokens[i].type;
            if (tt == TokenType::LESS) ++depth;
            else if (tt == TokenType::GREATER) --depth;
            else if (tt == TokenType::EOF_TOKEN) break;
            ++i;
        }
        if (depth != 0) return false;
        skipComments(i);
        if (i >= tokenCount) return false;
    }

    return (i < tokenCount && tokens[i].type == TokenType::LBRACE);
}

// -----------------------------------------------------------------------------
// looksLikeStmtStart
// -----------------------------------------------------------------------------

bool Parser::looksLikeStmtStart() const {
    TokenType tt = ts_.peekType();
    
    switch (tt) {
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
        case TokenType::STRUCT:
        case TokenType::ENUM:
        case TokenType::TRAIT:
        case TokenType::IMPL:
        case TokenType::FROM:
            return true;
        default:
            return looksLikeType() || tt == TokenType::IDENTIFIER ||
                   tt == TokenType::INT_LITERAL || tt == TokenType::FLOAT_LITERAL ||
                   tt == TokenType::STRING_LITERAL || tt == TokenType::RAW_STRING_LITERAL ||
                   tt == TokenType::CHAR_LITERAL || tt == TokenType::HEX_LITERAL ||
                   tt == TokenType::BINARY_LITERAL || tt == TokenType::TRUE ||
                   tt == TokenType::FALSE || tt == TokenType::NIL ||
                   tt == TokenType::MINUS || tt == TokenType::NOT ||
                   tt == TokenType::BIT_NOT || tt == TokenType::AMPERSAND ||
                   tt == TokenType::AWAIT || tt == TokenType::LPAREN;
    }
}

// -----------------------------------------------------------------------------
// looksLikeDeclStart
// -----------------------------------------------------------------------------

bool Parser::looksLikeDeclStart() const {
    TokenType tt = ts_.peekType();
    
    switch (tt) {
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
            return true;
        default:
            return false;
    }
}

// -----------------------------------------------------------------------------
// looksLikeMultiAssignStart
// -----------------------------------------------------------------------------

bool Parser::looksLikeMultiAssignStart() const {
    const auto& tokens = ts_.getTokens();
    size_t tokenCount = ts_.getTokenCount();
    size_t i = ts_.getPos();
    
    auto skipComments = [&](size_t& idx) {
        while (idx < tokenCount && 
               (tokens[idx].type == TokenType::LINE_COMMENT ||
                tokens[idx].type == TokenType::DOC_COMMENT)) {
            ++idx;
        }
    };

    // Skip leading comments
    skipComments(i);
    if (i >= tokenCount) return false;

    // First token must be an identifier
    if (tokens[i].type != TokenType::IDENTIFIER) return false;
    ++i;

    // Parse optional suffixes for the first lvalue
    while (i < tokenCount) {
        skipComments(i);
        if (i >= tokenCount) break;

        // Field access: .identifier
        if (tokens[i].type == TokenType::DOT) {
            ++i;
            skipComments(i);
            if (i >= tokenCount || tokens[i].type != TokenType::IDENTIFIER) {
                return false;
            }
            ++i;
        }
        // Array index: [ expr ]
        else if (tokens[i].type == TokenType::LBRACKET) {
            ++i;
            int bracketDepth = 1;
            while (i < tokenCount && bracketDepth > 0) {
                skipComments(i);
                if (i >= tokenCount) break;
                if (tokens[i].type == TokenType::LBRACKET)
                    ++bracketDepth;
                else if (tokens[i].type == TokenType::RBRACKET)
                    --bracketDepth;
                ++i;
            }
            if (bracketDepth != 0) return false;
        } else {
            break;
        }
    }

    // After the first lvalue, look for a comma
    skipComments(i);
    if (i >= tokenCount || tokens[i].type != TokenType::COMMA) return false;

    // Found a comma, now scan for '='
    ++i;
    while (i < tokenCount) {
        skipComments(i);
        if (i >= tokenCount) break;
        TokenType tt = tokens[i].type;
        if (tt == TokenType::ASSIGN) return true;
        if (tt == TokenType::SEMICOLON || tt == TokenType::LBRACE ||
            tt == TokenType::RBRACE || tt == TokenType::EOF_TOKEN) {
            return false;
        }
        ++i;
    }
    
    return false;
}

// -----------------------------------------------------------------------------
// looksLikeBehaviorAccess
// -----------------------------------------------------------------------------

bool Parser::looksLikeBehaviorAccess() const {
    const auto& tokens = ts_.getTokens();
    size_t tokenCount = ts_.getTokenCount();
    size_t i = ts_.getPos();
    
    auto skipComments = [&](size_t& idx) {
        while (idx < tokenCount && 
               (tokens[idx].type == TokenType::LINE_COMMENT ||
                tokens[idx].type == TokenType::DOC_COMMENT)) {
            ++idx;
        }
    };

    skipComments(i);
    
    if (i >= tokenCount || tokens[i].type != TokenType::IDENTIFIER) {
        return false;
    }
    ++i;

    // Optional generic args: '<' ... '>'
    if (i < tokenCount && tokens[i].type == TokenType::LESS) {
        int depth = 1;
        ++i;
        while (i < tokenCount && depth > 0) {
            skipComments(i);
            if (i >= tokenCount) break;
            TokenType tt = tokens[i].type;
            if (tt == TokenType::LESS) ++depth;
            else if (tt == TokenType::GREATER) --depth;
            else if (tt == TokenType::SEMICOLON || tt == TokenType::RBRACE) return false;
            ++i;
        }
        if (depth != 0) return false;
    }

    skipComments(i);
    if (i >= tokenCount || tokens[i].type != TokenType::COLON) {
        return false;
    }
    ++i;

    skipComments(i);
    if (i >= tokenCount || tokens[i].type != TokenType::IDENTIFIER) {
        return false;
    }

    return true;
}

// -----------------------------------------------------------------------------
// isPrimitiveTypeToken
// -----------------------------------------------------------------------------

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