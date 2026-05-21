#include "Lexer.hpp"
#include "debug/DebugMacros.hpp" 
#include <cctype>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor — keyword table
// ─────────────────────────────────────────────────────────────────────────────

Lexer::Lexer(const std::string &source)
    : src(source), pos(0), line(1), column(1) {
    LUC_LOG_LEXER("Lexer constructed, source size: " << source.size() << " bytes");
    
    // ── Modifiers ──────────────────────────────────────────────────────────────
    keywords["pub"] = TokenType::PUB;
    keywords["export"] = TokenType::EXPORT;

    // ── Top Level ──────────────────────────────────────────────────────────────
    keywords["package"] = TokenType::PACKAGE;
    keywords["use"] = TokenType::USE;
    keywords["as"] = TokenType::AS;
    keywords["impl"] = TokenType::IMPL;
    keywords["type"] = TokenType::TYPE;
    keywords["struct"] = TokenType::STRUCT;
    keywords["enum"] = TokenType::ENUM;
    keywords["trait"] = TokenType::TRAIT;
    keywords["from"] = TokenType::FROM;

    // ── Declarations ───────────────────────────────────────────────────────────
    keywords["let"] = TokenType::LET;
    keywords["const"] = TokenType::CONST;

    // ── Concurrency ────────────────────────────────────────────────────────────
    keywords["await"] = TokenType::AWAIT;
    // keywords["async"] = TokenType::ASYNC; deprecated use ~async instead
    // keywords["parallel"] = TokenType::PARALLEL; deprecated use ~parallel instead

    // ── Primary Types ──────────────────────────────────────────────────────────
    keywords["bool"] = TokenType::TYPE_BOOL;

    // Signed integers
    keywords["byte"] = TokenType::TYPE_BYTE;
    keywords["short"] = TokenType::TYPE_SHORT;
    keywords["int"] = TokenType::TYPE_INT;
    keywords["long"] = TokenType::TYPE_LONG;

    // Unsigned integers
    keywords["ubyte"] = TokenType::TYPE_UBYTE;
    keywords["ushort"] = TokenType::TYPE_USHORT;
    keywords["uint"] = TokenType::TYPE_UINT;
    keywords["ulong"] = TokenType::TYPE_ULONG;

    // Fixed-width (critical for Vulkan struct layouts)
    keywords["int8"] = TokenType::TYPE_INT8;
    keywords["int16"] = TokenType::TYPE_INT16;
    keywords["int32"] = TokenType::TYPE_INT32;
    keywords["int64"] = TokenType::TYPE_INT64;
    keywords["uint8"] = TokenType::TYPE_UINT8;
    keywords["uint16"] = TokenType::TYPE_UINT16;
    keywords["uint32"] = TokenType::TYPE_UINT32;
    keywords["uint64"] = TokenType::TYPE_UINT64;

    // Floating point
    keywords["float"] = TokenType::TYPE_FLOAT;
    keywords["double"] = TokenType::TYPE_DOUBLE;
    keywords["decimal"] = TokenType::TYPE_DECIMAL;

    // Text
    keywords["string"] = TokenType::TYPE_STRING;
    keywords["char"] = TokenType::TYPE_CHAR;

    // Special
    keywords["any"] = TokenType::TYPE_ANY;
    keywords["nil"] = TokenType::NIL;

    // ── Control Flow ───────────────────────────────────────────────────────────
    keywords["if"] = TokenType::IF;
    keywords["else"] = TokenType::ELSE;
    keywords["match"] = TokenType::MATCH;
    keywords["switch"] = TokenType::SWITCH;
    keywords["case"] = TokenType::CASE;
    keywords["default"] = TokenType::DEFAULT;
    keywords["is"] = TokenType::IS;
    keywords["while"] = TokenType::WHILE;
    keywords["for"] = TokenType::FOR;
    keywords["in"] = TokenType::IN;
    keywords["do"] = TokenType::DO;
    keywords["return"] = TokenType::RETURN;
    keywords["break"] = TokenType::BREAK;
    keywords["continue"] = TokenType::CONTINUE;

    // ── Logical ────────────────────────────────────────────────────────────────
    keywords["and"] = TokenType::AND;
    keywords["or"] = TokenType::OR;
    keywords["not"] = TokenType::NOT;
    keywords["true"] = TokenType::TRUE;
    keywords["false"] = TokenType::FALSE;
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
            LUC_LOG_LEXER_VERBOSE("skipWhitespace: found line comment start '--', breaking");
            break;
        }
        // Block comment: /- ... -/
        // Doc comment:   /-- ... --/
        else if (c == '/' && peekNext() == '-') {
            bool isDoc = (pos + 1 < src.size() && src[pos + 1] == '-');
            if (isDoc) {
                LUC_LOG_LEXER_VERBOSE("skipWhitespace: found doc comment '/--', breaking");
                break;
            }

            // Plain block comment /- ... -/
            LUC_LOG_LEXER_VERBOSE("skipWhitespace: skipping block comment");
            advance(); // consume '/'
            advance(); // consume '-'
            while (!isAtEnd() && !(peek() == '-' && peekNext() == '/')) {
                if (peek() == '\n') {
                    line++;
                    column = 1;
                }
                advance();
            }
            if (!isAtEnd()) {
                advance();
                advance();
            }
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
    advance(); // consume first '-' (of opening --)
    advance(); // consume second '-'

    std::string doc;

    while (!isAtEnd()) {
        // Closing sequence is --/
        if (peek() == '-' && pos + 1 < src.size() && src[pos + 1] == '-' &&
            pos + 2 < src.size() && src[pos + 2] == '/') {
            advance();
            advance();
            advance(); // consume --/
            break;
        }
        if (peek() == '\n') {
            line++;
            column = 1;
        }
        doc += advance();
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
    } while (t.type != TokenType::EOF_TOKEN);

    LUC_LOG_LEXER("tokenize: complete, " << tokenCount << " tokens produced");
    return tokens;
}