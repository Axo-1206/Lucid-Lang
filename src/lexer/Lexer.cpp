#include "Lexer.hpp"
#include "Tokens.hpp"
#include "debug/DebugMacros.hpp" 
#include <cctype>

// Helper to convert token to readable string for debugging
static std::string tokenTypeToString(const Token& token) {
    switch (token.type) {
        case TokenType::IDENTIFIER:     return "IDENTIFIER('" + token.value + "')";
        case TokenType::INT_LITERAL:    return "INT_LITERAL(" + token.value + ")";
        case TokenType::FLOAT_LITERAL:  return "FLOAT_LITERAL(" + token.value + ")";
        case TokenType::STRING_LITERAL: return "STRING_LITERAL(\"" + token.value + "\")";
        case TokenType::CHAR_LITERAL:   return "CHAR_LITERAL('" + token.value + "')";
        case TokenType::HEX_LITERAL:    return "HEX_LITERAL(" + token.value + ")";
        case TokenType::BINARY_LITERAL: return "BINARY_LITERAL(" + token.value + ")";
        case TokenType::RAW_STRING_LITERAL: return "RAW_STRING_LITERAL(\"\"\"" + token.value + "\"\"\")";
        case TokenType::LINE_COMMENT:   return "LINE_COMMENT(--" + token.value + ")";
        case TokenType::DOC_COMMENT:    return "DOC_COMMENT(/--" + token.value + "--/)";
        case TokenType::EOF_TOKEN:      return "EOF";
        case TokenType::UNKNOWN:        return "UNKNOWN('" + token.value + "')";
        
        // Keywords
        case TokenType::PUB:            return "PUB(pub)";
        case TokenType::EXPORT:         return "EXPORT(export)";
        case TokenType::PACKAGE:        return "PACKAGE(package)";
        case TokenType::USE:            return "USE(use)";
        case TokenType::AS:             return "AS(as)";
        case TokenType::IMPL:           return "IMPL(impl)";
        case TokenType::TYPE:           return "TYPE(type)";
        case TokenType::STRUCT:         return "STRUCT(struct)";
        case TokenType::ENUM:           return "ENUM(enum)";
        case TokenType::TRAIT:          return "TRAIT(trait)";
        case TokenType::FROM:           return "FROM(from)";
        case TokenType::LET:            return "LET(let)";
        case TokenType::CONST:          return "CONST(const)";
        case TokenType::AWAIT:          return "AWAIT(await)";
        case TokenType::IF:             return "IF(if)";
        case TokenType::ELSE:           return "ELSE(else)";
        case TokenType::MATCH:          return "MATCH(match)";
        case TokenType::SWITCH:         return "SWITCH(switch)";
        case TokenType::CASE:           return "CASE(case)";
        case TokenType::DEFAULT:        return "DEFAULT(default)";
        case TokenType::IS:             return "IS(is)";
        case TokenType::WHILE:          return "WHILE(while)";
        case TokenType::FOR:            return "FOR(for)";
        case TokenType::IN:             return "IN(in)";
        case TokenType::DO:             return "DO(do)";
        case TokenType::RETURN:         return "RETURN(return)";
        case TokenType::BREAK:          return "BREAK(break)";
        case TokenType::CONTINUE:       return "CONTINUE(continue)";
        case TokenType::AND:            return "AND(and)";
        case TokenType::OR:             return "OR(or)";
        case TokenType::NOT:            return "NOT(not)";
        case TokenType::TRUE:           return "TRUE(true)";
        case TokenType::FALSE:          return "FALSE(false)";
        case TokenType::NIL:            return "NIL(nil)";
        case TokenType::RESOLVE:        return "RESOLVE(resolve)";
        case TokenType::OK:             return "OK(ok)";
        case TokenType::ERR:            return "ERR(err)";
        
        // Type keywords
        case TokenType::TYPE_BOOL:      return "TYPE_BOOL(bool)";
        case TokenType::TYPE_BYTE:      return "TYPE_BYTE(byte)";
        case TokenType::TYPE_SHORT:     return "TYPE_SHORT(short)";
        case TokenType::TYPE_INT:       return "TYPE_INT(int)";
        case TokenType::TYPE_LONG:      return "TYPE_LONG(long)";
        case TokenType::TYPE_UBYTE:     return "TYPE_UBYTE(ubyte)";
        case TokenType::TYPE_USHORT:    return "TYPE_USHORT(ushort)";
        case TokenType::TYPE_UINT:      return "TYPE_UINT(uint)";
        case TokenType::TYPE_ULONG:     return "TYPE_ULONG(ulong)";
        case TokenType::TYPE_INT8:      return "TYPE_INT8(int8)";
        case TokenType::TYPE_INT16:     return "TYPE_INT16(int16)";
        case TokenType::TYPE_INT32:     return "TYPE_INT32(int32)";
        case TokenType::TYPE_INT64:     return "TYPE_INT64(int64)";
        case TokenType::TYPE_UINT8:     return "TYPE_UINT8(uint8)";
        case TokenType::TYPE_UINT16:    return "TYPE_UINT16(uint16)";
        case TokenType::TYPE_UINT32:    return "TYPE_UINT32(uint32)";
        case TokenType::TYPE_UINT64:    return "TYPE_UINT64(uint64)";
        case TokenType::TYPE_FLOAT:     return "TYPE_FLOAT(float)";
        case TokenType::TYPE_DOUBLE:    return "TYPE_DOUBLE(double)";
        case TokenType::TYPE_DECIMAL:   return "TYPE_DECIMAL(decimal)";
        case TokenType::TYPE_STRING:    return "TYPE_STRING(string)";
        case TokenType::TYPE_CHAR:      return "TYPE_CHAR(char)";
        case TokenType::TYPE_ANY:       return "TYPE_ANY(any)";
        
        // Single character tokens
        case TokenType::DOT:            return "DOT(.)";
        case TokenType::COLON:          return "COLON(:)";
        case TokenType::COMMA:          return "COMMA(,)";
        case TokenType::SEMICOLON:      return "SEMICOLON(;)";
        case TokenType::LPAREN:         return "LPAREN(()";
        case TokenType::RPAREN:         return "RPAREN())";
        case TokenType::LBRACE:         return "LBRACE({)";
        case TokenType::RBRACE:         return "RBRACE(})";
        case TokenType::LBRACKET:       return "LBRACKET([)";
        case TokenType::RBRACKET:       return "RBRACKET(])";
        case TokenType::PLUS:           return "PLUS(+)";
        case TokenType::MINUS:          return "MINUS(-)";
        case TokenType::MUL:            return "MUL(*)";
        case TokenType::DIV:            return "DIV(/)";
        case TokenType::MOD:            return "MOD(%)";
        case TokenType::POW:            return "POW(^)";
        case TokenType::ASSIGN:         return "ASSIGN(=)";
        case TokenType::BANG:           return "BANG(!)";
        case TokenType::QUESTION:       return "QUESTION(?)";
        case TokenType::TILDE:          return "TILDE(~)";
        case TokenType::AT_SIGN:        return "AT_SIGN(@)";
        case TokenType::HASH:           return "HASH(#)";
        case TokenType::AMPERSAND:      return "AMPERSAND(&)";
        case TokenType::PIPE:           return "PIPE(|)";
        case TokenType::WILDCARD:       return "WILDCARD(_)";
        
        // Two+ character tokens
        case TokenType::RANGE:          return "RANGE(..)";
        case TokenType::VARIADIC:       return "VARIADIC(...)";
        case TokenType::QUESTION_DOT:   return "QUESTION_DOT(?.)";
        case TokenType::QUESTION_QUESTION: return "QUESTION_QUESTION(?\?)";
        case TokenType::EQUAL_EQUAL:    return "EQUAL_EQUAL(==)";
        case TokenType::EQUAL_EQUAL_EQUAL: return "EQUAL_EQUAL_EQUAL(===)";
        case TokenType::NOT_EQUAL:      return "NOT_EQUAL(!=)";
        case TokenType::LESS_EQUAL:     return "LESS_EQUAL(<=)";
        case TokenType::GREATER_EQUAL:  return "GREATER_EQUAL(>=)";
        case TokenType::SHL:            return "SHL(<<)";
        case TokenType::SHR:            return "SHR(>>)";
        case TokenType::ARROW:          return "ARROW(->)";
        case TokenType::FAT_ARROW:      return "FAT_ARROW(=>)";
        case TokenType::COMPOSE:        return "COMPOSE(+>)";
        case TokenType::PIPELINE:       return "PIPELINE(|>)";
        case TokenType::BIT_AND:        return "BIT_AND(&&)";
        case TokenType::BIT_OR:         return "BIT_OR(||)";
        case TokenType::BIT_NOT:        return "BIT_NOT(~~)";
        case TokenType::BIT_XOR:        return "BIT_XOR(~^)";
        case TokenType::PLUS_ASSIGN:    return "PLUS_ASSIGN(+=)";
        case TokenType::MINUS_ASSIGN:   return "MINUS_ASSIGN(-=)";
        case TokenType::MUL_ASSIGN:     return "MUL_ASSIGN(*=)";
        case TokenType::DIV_ASSIGN:     return "DIV_ASSIGN(/=)";
        case TokenType::MOD_ASSIGN:     return "MOD_ASSIGN(%=)";
        case TokenType::POW_ASSIGN:     return "POW_ASSIGN(^=)";
        case TokenType::SHL_ASSIGN:     return "SHL_ASSIGN(<<=)";
        case TokenType::SHR_ASSIGN:     return "SHR_ASSIGN(>>=)";
        
        default: return "UNKNOWN_TOKEN(" + std::to_string(static_cast<int>(token.type)) + ")";
    }
}

// Helper to dump all tokens (similar to dumpAST)
static std::string dumpTokens(const std::vector<Token>& tokens, int verbosity = 1) {
    std::stringstream ss;
    ss << "\n========== TOKEN STREAM ==========\n";
    ss << "Total tokens: " << tokens.size() << "\n\n";
    
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto& token = tokens[i];
        ss << "Token " << std::setw(4) << i << ": ";
        ss << std::left << std::setw(30) << tokenTypeToString(token);
        ss << " at line " << std::setw(4) << token.line;
        ss << ", col " << std::setw(3) << token.column;
        
        // Show more details in verbose mode
        if (verbosity >= 2) {
            ss << " [type=" << static_cast<int>(token.type) << "]";
        }
        
        ss << "\n";
    }
    
    ss << "==================================\n";
    return ss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor — keyword table
// ─────────────────────────────────────────────────────────────────────────────

Lexer::Lexer(const std::string &source)
    : src(source), pos(0), line(1), column(1) {
    LUC_LOG_LEXER("Lexer constructed, source size: " << source.size() << " bytes");
    
    // ── Modifiers ──────────────────────────────────────────────────────────────
    keywords["pub"]     = TokenType::PUB;
    keywords["export"]  = TokenType::EXPORT;

    // ── Top Level ──────────────────────────────────────────────────────────────
    keywords["package"] = TokenType::PACKAGE;
    keywords["use"]     = TokenType::USE;
    keywords["as"]      = TokenType::AS;
    keywords["impl"]    = TokenType::IMPL;
    keywords["type"]    = TokenType::TYPE;
    keywords["struct"]  = TokenType::STRUCT;
    keywords["enum"]    = TokenType::ENUM;
    keywords["trait"]   = TokenType::TRAIT;
    keywords["from"]    = TokenType::FROM;

    // ── Declarations ───────────────────────────────────────────────────────────
    keywords["let"]     = TokenType::LET;
    keywords["const"]   = TokenType::CONST;

    // ── Concurrency ────────────────────────────────────────────────────────────
    keywords["await"]   = TokenType::AWAIT;
    // keywords["async"] = TokenType::ASYNC; deprecated use ~async instead
    // keywords["parallel"] = TokenType::PARALLEL; deprecated use ~parallel instead

    // ── Primary Types ──────────────────────────────────────────────────────────
    keywords["bool"]    = TokenType::TYPE_BOOL;

    // Signed integers
    keywords["byte"]    = TokenType::TYPE_BYTE;
    keywords["short"]   = TokenType::TYPE_SHORT;
    keywords["int"]     = TokenType::TYPE_INT;
    keywords["long"]    = TokenType::TYPE_LONG;

    // Unsigned integers
    keywords["ubyte"]   = TokenType::TYPE_UBYTE;
    keywords["ushort"]  = TokenType::TYPE_USHORT;
    keywords["uint"]    = TokenType::TYPE_UINT;
    keywords["ulong"]   = TokenType::TYPE_ULONG;

    // Fixed-width (critical for Vulkan struct layouts)
    keywords["int8"]    = TokenType::TYPE_INT8;
    keywords["int16"]   = TokenType::TYPE_INT16;
    keywords["int32"]   = TokenType::TYPE_INT32;
    keywords["int64"]   = TokenType::TYPE_INT64;
    keywords["uint8"]   = TokenType::TYPE_UINT8;
    keywords["uint16"]  = TokenType::TYPE_UINT16;
    keywords["uint32"]  = TokenType::TYPE_UINT32;
    keywords["uint64"]  = TokenType::TYPE_UINT64;

    // Floating point
    keywords["float"]   = TokenType::TYPE_FLOAT;
    keywords["double"]  = TokenType::TYPE_DOUBLE;
    keywords["decimal"] = TokenType::TYPE_DECIMAL;

    // Text
    keywords["string"]  = TokenType::TYPE_STRING;
    keywords["char"]    = TokenType::TYPE_CHAR;

    // Special
    keywords["any"] = TokenType::TYPE_ANY;
    keywords["nil"] = TokenType::NIL;

    // ── Control Flow ───────────────────────────────────────────────────────────
    keywords["if"]      = TokenType::IF;
    keywords["else"]    = TokenType::ELSE;
    keywords["match"]   = TokenType::MATCH;
    keywords["switch"]  = TokenType::SWITCH;
    keywords["case"]    = TokenType::CASE;
    keywords["default"] = TokenType::DEFAULT;
    keywords["is"]      = TokenType::IS;
    keywords["while"]   = TokenType::WHILE;
    keywords["for"]     = TokenType::FOR;
    keywords["in"]      = TokenType::IN;
    keywords["do"]      = TokenType::DO;
    keywords["return"]  = TokenType::RETURN;
    keywords["break"]   = TokenType::BREAK;
    keywords["continue"] = TokenType::CONTINUE;

    // ── Logical ────────────────────────────────────────────────────────────────
    keywords["and"]     = TokenType::AND;
    keywords["or"]      = TokenType::OR;
    keywords["not"]     = TokenType::NOT;
    keywords["true"]    = TokenType::TRUE;
    keywords["false"]   = TokenType::FALSE;

    // ─── Error Handling ───────────────────────────────────────────────────────
    keywords["resolve"] = TokenType::RESOLVE;
    keywords["ok"]      = TokenType::OK;
    keywords["err"]     = TokenType::ERR;

}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

Token Lexer::makeToken(TokenType type, const std::string &value) {
    LUC_LOG_LEXER_EXTREME("makeToken: type=" << static_cast<int>(type) << ", value='" << value << "', line=" << line << ", col=" << column);
    return {type, value, line, column};
}

char Lexer::peek() { 
    char c = isAtEnd() ? '\0' : src[pos];
    LUC_LOG_LEXER_EXTREME("peek: '" << (c == '\n' ? '\\' : c) << "' at pos=" << pos);
    return c;
}

char Lexer::peekNext() { 
    char c = (pos + 1 >= src.size()) ? '\0' : src[pos + 1];
    LUC_LOG_LEXER_EXTREME("peekNext: '" << (c == '\n' ? '\\' : c) << "'");
    return c;
}

char Lexer::peekNext(int offset) const {
    size_t idx = pos + offset;
    return (idx < src.size()) ? src[idx] : '\0';
}

char Lexer::advance() {
    char c = src[pos++];
    column++;
    LUC_LOG_LEXER_EXTREME("advance: consumed '" << (c == '\n' ? '\\' : c) << "', new pos=" << pos);
    return c;
}

bool Lexer::isAtEnd() { 
    bool end = pos >= src.size();
    LUC_LOG_LEXER_EXTREME("isAtEnd: " << (end ? "true" : "false"));
    return end;
}

bool Lexer::match(char expected) {
    if (isAtEnd() || src[pos] != expected) {
        LUC_LOG_LEXER_EXTREME("match('" << expected << "'): false");
        return false;
    }
    pos++;
    column++;
    LUC_LOG_LEXER_EXTREME("match('" << expected << "'): true");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Whitespace & Comments
// ─────────────────────────────────────────────────────────────────────────────

void Lexer::skipWhitespace() {
    LUC_LOG_LEXER_VERBOSE("skipWhitespace: pos=" << pos << ", line=" << line);
    
    while (!isAtEnd()) {
        char c = peek();

        if (c == ' ' || c == '\r' || c == '\t') {
            LUC_LOG_LEXER_EXTREME("skipWhitespace: skipping whitespace char '" << c << "'");
            advance();
        } else if (c == '\n') {
            LUC_LOG_LEXER_VERBOSE("skipWhitespace: newline, line=" << line << "->" << line + 1);
            line++;
            column = 1;
            advance();
        }
        // Single-line comment: --
        else if (c == '-' && peekNext() == '-') {
            LUC_LOG_LEXER_VERBOSE("skipWhitespace: found line comment start '--', consuming it");
            // Consume the entire line comment
            advance(); // consume first '-'
            advance(); // consume second '-'
            while (!isAtEnd() && peek() != '\n') {
                advance();
            }
            // Don't break, continue loop to skip more whitespace
        }
        // Block comment: /- ... -/
        else if (c == '/' && peekNext() == '-') {
            // Check if it's a doc comment /-- (three characters: /, -, -)
            if (pos + 1 < src.size() && src[pos + 1] == '-' && 
                pos + 2 < src.size() && src[pos + 2] == '-') {
                // This is a doc comment, don't skip it - let getNextToken handle it
                LUC_LOG_LEXER_VERBOSE("skipWhitespace: found doc comment '/--', breaking to let getNextToken handle it");
                break;
            }
            
            // Plain block comment /- ... -/
            LUC_LOG_LEXER_VERBOSE("skipWhitespace: skipping block comment");
            advance(); // consume '/'
            advance(); // consume '-'
            
            int nestedLevel = 1;
            while (!isAtEnd() && nestedLevel > 0) {
                if (peek() == '\n') {
                    line++;
                    column = 1;
                }
                
                // Check for nested block comment start /-
                if (peek() == '/' && peekNext() == '-') {
                    nestedLevel++;
                    advance(); // consume '/'
                    advance(); // consume '-'
                }
                // Check for block comment end -/
                else if (peek() == '-' && peekNext() == '/') {
                    nestedLevel--;
                    advance(); // consume '-'
                    advance(); // consume '/'
                }
                else {
                    advance();
                }
            }
            // Continue loop to skip more whitespace
        } else {
            break;
        }
    }
    LUC_LOG_LEXER_VERBOSE("skipWhitespace: done, pos=" << pos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Literal readers
// ─────────────────────────────────────────────────────────────────────────────

Token Lexer::readNumber(char first) {
    LUC_LOG_LEXER_VERBOSE("readNumber: first char='" << first << "'");
    std::string num(1, first);

    // Hex: 0xFF
    if (first == '0' && (peek() == 'x' || peek() == 'X')) {
        LUC_LOG_LEXER_VERBOSE("readNumber: hex literal");
        num += advance(); // consume 'x'
        while (isxdigit(peek()) || peek() == '_')
            num += advance();
        LUC_LOG_LEXER("readNumber: hex literal: " << num);
        return makeToken(TokenType::HEX_LITERAL, num);
    }

    // Binary: 0b1010
    if (first == '0' && (peek() == 'b' || peek() == 'B')) {
        LUC_LOG_LEXER_VERBOSE("readNumber: binary literal");
        num += advance(); // consume 'b'
        while (peek() == '0' || peek() == '1' || peek() == '_')
            num += advance();
        LUC_LOG_LEXER("readNumber: binary literal: " << num);
        return makeToken(TokenType::BINARY_LITERAL, num);
    }

    // Integer or float
    bool isFloat = false;
    while (isdigit(peek()) || peek() == '_')
        num += advance();

    if (peek() == '.' && peekNext() != '.') {
        isFloat = true;
        LUC_LOG_LEXER_VERBOSE("readNumber: decimal point detected");
        num += advance(); // consume '.'
        while (isdigit(peek()) || peek() == '_')
            num += advance();
    }

    // Exponent: 1e10, 1.5e-3
    if (peek() == 'e' || peek() == 'E') {
        isFloat = true;
        LUC_LOG_LEXER_VERBOSE("readNumber: exponent detected");
        num += advance();
        if (peek() == '+' || peek() == '-')
            num += advance();
        while (isdigit(peek()))
            num += advance();
    }

    LUC_LOG_LEXER("readNumber: " << (isFloat ? "float" : "int") << " literal: " << num);
    return makeToken(isFloat ? TokenType::FLOAT_LITERAL : TokenType::INT_LITERAL, num);
}

Token Lexer::readString() {
    LUC_LOG_LEXER_VERBOSE("readString: starting");
    std::string str;
    while (!isAtEnd() && peek() != '"') {
        if (peek() == '\n') {
            line++;
            column = 1;
        }
        if (peek() == '\\') {
            advance(); // consume '\'
            char esc = advance();
            LUC_LOG_LEXER_EXTREME("readString: escape sequence '\\" << esc << "'");
            switch (esc) {
            case 'n':
                str += '\n';
                break;
            case 't':
                str += '\t';
                break;
            case 'r':
                str += '\r';
                break;
            case '"':
                str += '"';
                break;
            case '\\':
                str += '\\';
                break;
            case '\'':
                str += '\'';
                break;
            case '0':
                str += '\0';
                break;
            case 'x': {
                std::string hex;
                for (int i = 0; i < 2 && isxdigit(peek()); i++)
                    hex += advance();
                str += (char)std::stoi(hex, nullptr, 16);
                break;
            }
            case 'u': {
                std::string hex;
                for (int i = 0; i < 4 && isxdigit(peek()); i++)
                    hex += advance();
                unsigned long cp = std::stoul(hex, nullptr, 16);
                // UTF-8 encoding...
                if (cp < 0x80) {
                    str += (char)cp;
                } else if (cp < 0x800) {
                    str += (char)(0xC0 | (cp >> 6));
                    str += (char)(0x80 | (cp & 0x3F));
                } else {
                    str += (char)(0xE0 | (cp >> 12));
                    str += (char)(0x80 | ((cp >> 6) & 0x3F));
                    str += (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            case 'U': {
                std::string hex;
                for (int i = 0; i < 8 && isxdigit(peek()); i++)
                    hex += advance();
                unsigned long cp = std::stoul(hex, nullptr, 16);
                // UTF-8 encoding for up to 4 bytes...
                if (cp < 0x80) {
                    str += (char)cp;
                } else if (cp < 0x800) {
                    str += (char)(0xC0 | (cp >> 6));
                    str += (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    str += (char)(0xE0 | (cp >> 12));
                    str += (char)(0x80 | ((cp >> 6) & 0x3F));
                    str += (char)(0x80 | (cp & 0x3F));
                } else {
                    str += (char)(0xF0 | (cp >> 18));
                    str += (char)(0x80 | ((cp >> 12) & 0x3F));
                    str += (char)(0x80 | ((cp >> 6) & 0x3F));
                    str += (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default:
                str += '\\';
                str += esc;
                break;
            }
        } else {
            str += advance();
        }
    }
    if (!isAtEnd())
        advance(); // consume closing '"'
    
    LUC_LOG_LEXER_VERBOSE("readString: result length=" << str.size());
    return makeToken(TokenType::STRING_LITERAL, str);
}

Token Lexer::readChar() {
    LUC_LOG_LEXER_VERBOSE("readChar: starting");
    std::string result;
    bool closed = false;

    while (!isAtEnd()) {
        char c = peek();
        if (c == '\'') {
            advance();          // consume closing '
            closed = true;
            break;
        }
        if (c == '\\') {
            advance();          // consume backslash
            char esc = advance();
            switch (esc) {
                case 'n':  result += '\n'; break;
                case 't':  result += '\t'; break;
                case 'r':  result += '\r'; break;
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case '\'': result += '\''; break;
                case '0':  result += '\0'; break;
                case 'x': {
                    std::string hex;
                    for (int i = 0; i < 2 && isxdigit(peek()); ++i)
                        hex += advance();
                    if (hex.size() != 2) {
                        // incomplete hex escape – error recovery
                        return makeToken(TokenType::UNKNOWN, "");
                    }
                    unsigned long val = std::stoul(hex, nullptr, 16);
                    result += static_cast<char>(val);
                    break;
                }
                case 'u': {
                    std::string hex;
                    for (int i = 0; i < 4 && isxdigit(peek()); ++i)
                        hex += advance();
                    if (hex.size() != 4) {
                        return makeToken(TokenType::UNKNOWN, "");
                    }
                    unsigned long cp = std::stoul(hex, nullptr, 16);
                    // UTF‑8 encoding (same as string)
                    if (cp < 0x80) {
                        result += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        result += static_cast<char>(0xC0 | (cp >> 6));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        result += static_cast<char>(0xE0 | (cp >> 12));
                        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        result += static_cast<char>(0xF0 | (cp >> 18));
                        result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                case 'U': {
                    std::string hex;
                    for (int i = 0; i < 8 && isxdigit(peek()); ++i)
                        hex += advance();
                    if (hex.size() != 8) {
                        return makeToken(TokenType::UNKNOWN, "");
                    }
                    unsigned long cp = std::stoul(hex, nullptr, 16);
                    // UTF‑8 encoding (same as string)
                    if (cp < 0x80) {
                        result += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        result += static_cast<char>(0xC0 | (cp >> 6));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        result += static_cast<char>(0xE0 | (cp >> 12));
                        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        result += static_cast<char>(0xF0 | (cp >> 18));
                        result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default:
                    // unknown escape – keep as two characters (error may be reported later)
                    result += '\\';
                    result += esc;
                    break;
            }
        } else {
            // normal character (not a backslash)
            result += advance();
        }
    }

    if (!closed) {
        // unterminated character literal
        return makeToken(TokenType::UNKNOWN, "");
    }

    return makeToken(TokenType::CHAR_LITERAL, result);
}

Token Lexer::readRawString(int hashCount) {
    LUC_LOG_LEXER_VERBOSE("readRawString: starting with " << hashCount << " hash delimiters");
    std::string str;
    while (!isAtEnd()) {
        if (peek() == '"') {
            // Check if the next hashCount characters are '#' and then end
            bool match = true;
            for (int i = 0; i < hashCount; ++i) {
                if (peekNext(i + 1) != '#') {
                    match = false;
                    break;
                }
            }
            if (match) {
                advance(); // consume "
                for (int i = 0; i < hashCount; ++i) advance(); // consume the #s
                break;
            }
        }
        if (peek() == '\n') {
            line++;
            column = 1;
        }
        str += advance();
        if (isAtEnd()) {
            // Unterminated raw string – error recovery
            break;
        }
    }
    return makeToken(TokenType::RAW_STRING_LITERAL, str);
}

Token Lexer::readDocComment() {
    LUC_LOG_LEXER_VERBOSE("readDocComment: starting");
    
    // The '/' has already been consumed by getNextToken
    // Now consume the '--' part
    if (peek() == '-') {
        advance(); // consume first '-'
        if (peek() == '-') {
            advance(); // consume second '-'
        } else {
            LUC_LOG_LEXER("readDocComment: expected '--' after '/', got '" << peek() << "'");
            return makeToken(TokenType::UNKNOWN, "");
        }
    } else {
        LUC_LOG_LEXER("readDocComment: expected '-' after '/', got '" << peek() << "'");
        return makeToken(TokenType::UNKNOWN, "");
    }

    std::string doc;
    bool closed = false;

    while (!isAtEnd()) {
        // Check for closing sequence: --/
        if (peek() == '-' && peekNext() == '-' && peekNext(2) == '/') {
            advance(); // consume first '-'
            advance(); // consume second '-'
            advance(); // consume '/'
            closed = true;
            break;
        }
        
        if (peek() == '\n') {
            line++;
            column = 1;
        }
        doc += advance();
    }

    if (!closed) {
        LUC_LOG_LEXER("readDocComment: unclosed doc comment");
        // Return what we have as an error token
    }

    LUC_LOG_LEXER_VERBOSE("readDocComment: length=" << doc.size());
    return makeToken(TokenType::DOC_COMMENT, doc);
}

Token Lexer::readLineComment() {
    LUC_LOG_LEXER_VERBOSE("readLineComment: starting");
    advance(); // consume the second '-'

    // Strip a single optional leading space (the common "-- text" style).
    if (peek() == ' ')
        advance();

    std::string text;
    while (!isAtEnd() && peek() != '\n')
        text += advance();

    LUC_LOG_LEXER_VERBOSE("readLineComment: '" << text << "'");
    return makeToken(TokenType::LINE_COMMENT, text);
}

Token Lexer::getNextToken() {
    LUC_LOG_LEXER_VERBOSE("getNextToken: pos=" << pos << ", line=" << line << ", col=" << column);
    
    skipWhitespace();
    if (isAtEnd()) {
        LUC_LOG_LEXER("getNextToken: EOF");
        return makeToken(TokenType::EOF_TOKEN, "EOF");
    }

    char c = advance();
    LUC_LOG_LEXER_EXTREME("getNextToken: processing char '" << c << "'");

    // ── Identifiers & Keywords ─────────────────────────────────────────────────
    if (isalpha(c) || c == '_') {
        std::string ident(1, c);
        while (isalnum(peek()) || peek() == '_')
            ident += advance();

        // r"..." raw string literal with optional # delimiters
        if (ident == "r") {
            int hashCount = 0;
            while (peek() == '#') {
                ++hashCount;
                advance();
            }
            if (peek() == '"') {
                advance(); // consume opening "
                return readRawString(hashCount);
            }
        }

        // Standalone _ is a wildcard token
        if (ident == "_") {
            LUC_LOG_LEXER_EXTREME("getNextToken: wildcard '_'");
            return makeToken(TokenType::WILDCARD, "_");
        }

        auto it = keywords.find(ident);
        if (it != keywords.end()) {
            LUC_LOG_LEXER_VERBOSE("getNextToken: keyword '" << ident << "'");
            return makeToken(it->second, ident);
        }
        LUC_LOG_LEXER_VERBOSE("getNextToken: identifier '" << ident << "'");
        return makeToken(TokenType::IDENTIFIER, ident);
    }

    // ── Numbers ────────────────────────────────────────────────────────────────
    if (isdigit(c)) {
        return readNumber(c);
    }

    // ── String literals ────────────────────────────────────────────────────────
    if (c == '"') {
        LUC_LOG_LEXER_VERBOSE("getNextToken: string literal");
        return readString();
    }

    // ── Char literals ──────────────────────────────────────────────────────────
    if (c == '\'') {
        LUC_LOG_LEXER_VERBOSE("getNextToken: char literal");
        return readChar();
    }

    // ── Operators & Symbols ────────────────────────────────────────────────────
    switch (c) {
    // ── Access ─────────────────────────────────────────────────────────────────
    case '.':
        if (match('.')) {
            if (match('.')) {
                LUC_LOG_LEXER_EXTREME("getNextToken: '...' (variadic)");
                return makeToken(TokenType::VARIADIC, "...");
            }
            LUC_LOG_LEXER_EXTREME("getNextToken: '..' (range)");
            return makeToken(TokenType::RANGE, "..");
        }
        LUC_LOG_LEXER_EXTREME("getNextToken: '.'");
        return makeToken(TokenType::DOT, ".");

    case ':':
        // if (match(':')) {
        //     LUC_LOG_LEXER_EXTREME("getNextToken: '::'");
        //     return makeToken(TokenType::DOUBLE_COLON, "::");
        // }
        LUC_LOG_LEXER_EXTREME("getNextToken: ':'");
        return makeToken(TokenType::COLON, ":");

    case '?':
        if (match('.')) {
            LUC_LOG_LEXER_EXTREME("getNextToken: '?.'");
            return makeToken(TokenType::QUESTION_DOT, "?.");
        }
        if (match('?')) {
            LUC_LOG_LEXER_EXTREME("getNextToken: '\?\?'");
            return makeToken(TokenType::QUESTION_QUESTION, "??");
        }
        LUC_LOG_LEXER_EXTREME("getNextToken: '?'");
        return makeToken(TokenType::QUESTION, "?");

    case '=':
        if (match('=')) {
            if (match('=')) {
                LUC_LOG_LEXER_EXTREME("getNextToken: '==='");
                return makeToken(TokenType::EQUAL_EQUAL_EQUAL, "===");
            }
            LUC_LOG_LEXER_EXTREME("getNextToken: '=='");
            return makeToken(TokenType::EQUAL_EQUAL, "==");
        }
        if (peek() == '>') {
            advance();
            return Token{TokenType::FAT_ARROW, "=>", line, column};
        }
        LUC_LOG_LEXER_EXTREME("getNextToken: '='");
        return makeToken(TokenType::ASSIGN, "=");

    case '!':
        if (match('=')) {
            LUC_LOG_LEXER_EXTREME("getNextToken: '!='");
            return makeToken(TokenType::NOT_EQUAL, "!=");
        }
        LUC_LOG_LEXER_EXTREME("getNextToken: '!'");
        return makeToken(TokenType::BANG, "!");

    case '<':
        if (match('<')) {
            if (match('=')) {
                LUC_LOG_LEXER_EXTREME("getNextToken: '<<='");
                return makeToken(TokenType::SHL_ASSIGN, "<<=");
            }
            LUC_LOG_LEXER_EXTREME("getNextToken: '<<'");
            return makeToken(TokenType::SHL, "<<");
        }
        if (match('=')) {
            LUC_LOG_LEXER_EXTREME("getNextToken: '<='");
            return makeToken(TokenType::LESS_EQUAL, "<=");
        }
        LUC_LOG_LEXER_EXTREME("getNextToken: '<'");
        return makeToken(TokenType::LESS, "<");

    case '>':
        if (match('>')) {
            if (match('=')) {
                LUC_LOG_LEXER_EXTREME("getNextToken: '>>='");
                return makeToken(TokenType::SHR_ASSIGN, ">>=");
            }
            LUC_LOG_LEXER_EXTREME("getNextToken: '>>'");
            return makeToken(TokenType::SHR, ">>");
        }
        if (match('=')) {
            LUC_LOG_LEXER_EXTREME("getNextToken: '>='");
            return makeToken(TokenType::GREATER_EQUAL, ">=");
        }
        LUC_LOG_LEXER_EXTREME("getNextToken: '>'");
        return makeToken(TokenType::GREATER, ">");

    case '+':
        if (match('>')) {
            LUC_LOG_LEXER_EXTREME("getNextToken: '+>'");
            return makeToken(TokenType::COMPOSE, "+>");
        }
        if (match('=')) {
            LUC_LOG_LEXER_EXTREME("getNextToken: '+='");
            return makeToken(TokenType::PLUS_ASSIGN, "+=");
        }
        LUC_LOG_LEXER_EXTREME("getNextToken: '+'");
        return makeToken(TokenType::PLUS, "+");

    case '-':
        if (peek() == '-') {
            LUC_LOG_LEXER_EXTREME("getNextToken: line comment '--'");
            return readLineComment();
        }
        if (match('=')) {
            LUC_LOG_LEXER_EXTREME("getNextToken: '-='");
            return makeToken(TokenType::MINUS_ASSIGN, "-=");
        }
        if (match('>')) {
            LUC_LOG_LEXER_EXTREME("getNextToken: '->'");
            return makeToken(TokenType::ARROW, "->");
        }
        LUC_LOG_LEXER_EXTREME("getNextToken: '-'");
        return makeToken(TokenType::MINUS, "-");

    case '*':
        if (match('=')) {
            LUC_LOG_LEXER_EXTREME("getNextToken: '*='");
            return makeToken(TokenType::MUL_ASSIGN, "*=");
        }
        LUC_LOG_LEXER_EXTREME("getNextToken: '*'");
        return makeToken(TokenType::MUL, "*");

    case '/':
        // Check for doc comment '/--' (three characters: /, -, -)
        if (peek() == '-' && pos + 1 < src.size() && src[pos + 1] == '-') {
            LUC_LOG_LEXER_EXTREME("getNextToken: doc comment '/--'");
            return readDocComment();
        }
        if (match('=')) {
            LUC_LOG_LEXER_EXTREME("getNextToken: '/='");
            return makeToken(TokenType::DIV_ASSIGN, "/=");
        }
        LUC_LOG_LEXER_EXTREME("getNextToken: '/'");
        return makeToken(TokenType::DIV, "/");

    case '%':
        if (match('=')) {
            LUC_LOG_LEXER_EXTREME("getNextToken: '%='");
            return makeToken(TokenType::MOD_ASSIGN, "%=");
        }
        LUC_LOG_LEXER_EXTREME("getNextToken: '%'");
        return makeToken(TokenType::MOD, "%");

    case '^':
        if (match('=')) {
            LUC_LOG_LEXER_EXTREME("getNextToken: '^='");
            return makeToken(TokenType::POW_ASSIGN, "^=");
        }
        LUC_LOG_LEXER_EXTREME("getNextToken: '^'");
        return makeToken(TokenType::POW, "^");

    case '&':
        if (match('&')) {
            if (match('=')) {
                LUC_LOG_LEXER_EXTREME("getNextToken: '&&='");
                return makeToken(TokenType::BIT_AND_ASSIGN, "&&=");
            }
            LUC_LOG_LEXER_EXTREME("getNextToken: '&&'");
            return makeToken(TokenType::BIT_AND, "&&");
        }
        LUC_LOG_LEXER_EXTREME("getNextToken: '&'");
        return makeToken(TokenType::AMPERSAND, "&");

    case '|':
        if (match('>')) {
            LUC_LOG_LEXER_EXTREME("getNextToken: '|>' (pipeline)");
            return makeToken(TokenType::PIPELINE, "|>");
        }
        if (match('|')) {
            if (match('=')) {
                LUC_LOG_LEXER_EXTREME("getNextToken: '||='");
                return makeToken(TokenType::BIT_OR_ASSIGN, "||=");
            }
            LUC_LOG_LEXER_EXTREME("getNextToken: '||'");
            return makeToken(TokenType::BIT_OR, "||");
        }
        LUC_LOG_LEXER_EXTREME("getNextToken: '|'");
        return makeToken(TokenType::PIPE, "|");

    case '~':
        // Check for ~~ first (new bitwise NOT)
        if (match('~')) {
            // ~~ is bitwise NOT
            if (match('^')) {
                // ~~^ is bitwise XOR? This is getting messy
                // Better: keep ~^ as XOR, only change single ~ to TILDE
                // But then ~~ and ~ are different...
            }
            return makeToken(TokenType::BIT_NOT, "~~");
        }
        // Check for ~^ (bitwise XOR) - keep as is
        if (match('^')) {
            if (match('=')) {
                return makeToken(TokenType::BIT_XOR_ASSIGN, "~^=");
            }
            return makeToken(TokenType::BIT_XOR, "~^");
        }
        // Single ~ is type qualifier prefix
        return makeToken(TokenType::TILDE, "~");
        
    case '@':
        LUC_LOG_LEXER_EXTREME("getNextToken: '@'");
        return makeToken(TokenType::AT_SIGN, "@");
    case '#':
        LUC_LOG_LEXER_EXTREME("getNextToken: '#'");
        return makeToken(TokenType::HASH, "#");

    case ',':
        LUC_LOG_LEXER_EXTREME("getNextToken: ','");
        return makeToken(TokenType::COMMA, ",");
    case ';':
        LUC_LOG_LEXER_EXTREME("getNextToken: ';'");
        return makeToken(TokenType::SEMICOLON, ";");
    case '(':
        LUC_LOG_LEXER_EXTREME("getNextToken: '('");
        return makeToken(TokenType::LPAREN, "(");
    case ')':
        LUC_LOG_LEXER_EXTREME("getNextToken: ')'");
        return makeToken(TokenType::RPAREN, ")");
    case '{':
        LUC_LOG_LEXER_EXTREME("getNextToken: '{'");
        return makeToken(TokenType::LBRACE, "{");
    case '}':
        LUC_LOG_LEXER_EXTREME("getNextToken: '}'");
        return makeToken(TokenType::RBRACE, "}");
    case '[':
        LUC_LOG_LEXER_EXTREME("getNextToken: '['");
        return makeToken(TokenType::LBRACKET, "[");
    case ']':
        LUC_LOG_LEXER_EXTREME("getNextToken: ']'");
        return makeToken(TokenType::RBRACKET, "]");
    }

    // Unknown character
    LUC_LOG_LEXER("getNextToken: UNKNOWN character '" << c << "'");
    return makeToken(TokenType::UNKNOWN, std::string(1, c));
}

// ─────────────────────────────────────────────────────────────────────────────
// Public tokenize
// ─────────────────────────────────────────────────────────────────────────────

std::vector<Token> Lexer::tokenize() {
    LUC_LOG_LEXER("tokenize: starting");
    std::vector<Token> tokens;
    Token t = makeToken(TokenType::EOF_TOKEN, "");
    int tokenCount = 0;

    do {
        t = getNextToken();
        tokens.push_back(t);
        tokenCount++;
        LUC_LOG_LEXER_EXTREME("tokenize: token " << tokenCount << " - type=" << static_cast<int>(t.type) << ", value='" << t.value << "'");
        
        // Log each token using the regular LEXER debug macro
        // This will only output when LUC_DEBUG_LEXER is enabled
        LUC_LOG_LEXER("Token " << tokenCount << ": " << tokenTypeToString(t) << " at line " << t.line << ", col " << t.column);
        
    } while (t.type != TokenType::EOF_TOKEN);

    LUC_LOG_LEXER("tokenize: complete, " << tokenCount << " tokens produced");
    
    // Dump all tokens if LEXER_TOKENS debug is enabled
    #ifdef LUC_DEBUG_LEXER_TOKENS
        LucDebug::getDebugStream() << dumpTokens(tokens, LucDebug::getVerbosity());
    #endif
    
    return tokens;
}