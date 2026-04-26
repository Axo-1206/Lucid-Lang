#include "Lexer.hpp"
#include <cctype>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor — keyword table
// ─────────────────────────────────────────────────────────────────────────────

Lexer::Lexer(const std::string &source)
    : src(source), pos(0), line(1), column(1) {
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
	keywords["async"] = TokenType::ASYNC;
	keywords["await"] = TokenType::AWAIT;
	keywords["parallel"] = TokenType::PARALLEL;

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
  	return {type, value, line, column};
}

char Lexer::peek() { return isAtEnd() ? '\0' : src[pos]; }

char Lexer::peekNext() { return (pos + 1 >= src.size()) ? '\0' : src[pos + 1]; }

char Lexer::advance() {
	column++;
	return src[pos++];
}

bool Lexer::isAtEnd() { return pos >= src.size(); }

bool Lexer::match(char expected) {
	if (isAtEnd() || src[pos] != expected)
		return false;
	pos++;
	column++;
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Whitespace & Comments
// ─────────────────────────────────────────────────────────────────────────────

void Lexer::skipWhitespace() {
	while (!isAtEnd()) {
		char c = peek();

		if (c == ' ' || c == '\r' || c == '\t') {
		advance();
		} else if (c == '\n') {
		line++;
		column = 1;
		advance();
		}
		// Single-line comment: --
		// Break out so getNextToken can emit a LINE_COMMENT token.
		// This allows the Parser to harvest stacked and trailing doc comments.
		else if (c == '-' && peekNext() == '-') {
			break;
		}
		// Block comment: /- ... -/
		// Doc comment:   /-- ... --/
		//
		// Both open with '/'. Disambiguate by peeking a third character BEFORE
		// consuming anything. If it is /-- we must NOT eat it here — break out
		// so getNextToken can capture and return it as a DOC_COMMENT token.
		else if (c == '/' && peekNext() == '-') {
            // pos points at '-' (not yet consumed). Check the char after that.
            bool isDoc = (pos + 1 < src.size() && src[pos + 1] == '-');
            if (isDoc)
                break; // leave /-- for getNextToken

            // Plain block comment /- ... -/
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
            } // consume '-/'
		} else {
			break;
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Literal readers
// ─────────────────────────────────────────────────────────────────────────────

Token Lexer::readNumber(char first) {
  std::string num(1, first);

  // Hex: 0xFF
  if (first == '0' && (peek() == 'x' || peek() == 'X')) {
    num += advance(); // consume 'x'
    while (isxdigit(peek()) || peek() == '_')
      num += advance();
    return makeToken(TokenType::HEX_LITERAL, num);
  }

  // Binary: 0b1010
  if (first == '0' && (peek() == 'b' || peek() == 'B')) {
    num += advance(); // consume 'b'
    while (peek() == '0' || peek() == '1' || peek() == '_')
      num += advance();
    return makeToken(TokenType::BINARY_LITERAL, num);
  }

  // Integer or float
  bool isFloat = false;
  while (isdigit(peek()) || peek() == '_')
    num += advance();

  if (peek() == '.' && peekNext() != '.') // avoid consuming '..' range
  {
    isFloat = true;
    num += advance(); // consume '.'
    while (isdigit(peek()) || peek() == '_')
      num += advance();
  }

  // Exponent: 1e10, 1.5e-3
  if (peek() == 'e' || peek() == 'E') {
    isFloat = true;
    num += advance();
    if (peek() == '+' || peek() == '-')
      num += advance();
    while (isdigit(peek()))
      num += advance();
  }

  return makeToken(isFloat ? TokenType::FLOAT_LITERAL : TokenType::INT_LITERAL,
                   num);
}

Token Lexer::readString() {
  std::string str;
  while (!isAtEnd() && peek() != '"') {
    if (peek() == '\n') {
      line++;
      column = 1;
    }
    if (peek() == '\\') {
      advance(); // consume '\'
      char esc = advance();
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
        // \xHH — two hex digits
        std::string hex;
        for (int i = 0; i < 2 && isxdigit(peek()); i++)
          hex += advance();
        str += (char)std::stoi(hex, nullptr, 16);
        break;
      }
      case 'u': {
        // \uXXXX — four hex digits (Unicode BMP codepoint, UTF-8 encoded)
        std::string hex;
        for (int i = 0; i < 4 && isxdigit(peek()); i++)
          hex += advance();
        unsigned long cp = std::stoul(hex, nullptr, 16);
        // encode as UTF-8
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
        // \UXXXXXXXX — eight hex digits (full Unicode codepoint, UTF-8 encoded)
        std::string hex;
        for (int i = 0; i < 8 && isxdigit(peek()); i++)
          hex += advance();
        unsigned long cp = std::stoul(hex, nullptr, 16);
        // encode as UTF-8
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
  return makeToken(TokenType::STRING_LITERAL, str);
}

Token Lexer::readChar() {
	std::string ch;
	if (!isAtEnd() && peek() != '\'') {
		if (peek() == '\\') {
		advance();
		ch += '\\';
		if (!isAtEnd())
			ch += advance();
		} else {
		ch += advance();
		}
	}
	if (!isAtEnd() && peek() == '\'')
		advance(); // consume closing '\''
	return makeToken(TokenType::CHAR_LITERAL, ch);
}

// ─────────────────────────────────────────────────────────────────────────────
// Raw string reader  r"..."
// ─────────────────────────────────────────────────────────────────────────────
//
// Called from getNextToken after 'r' has been identified as an identifier
// followed immediately by '"'. No escape processing — every character is
// stored literally, including backslashes.
//
Token Lexer::readRawString() {
	// opening '"' already consumed by caller
	std::string str;
	while (!isAtEnd() && peek() != '"') {
		if (peek() == '\n') {
		line++;
		column = 1;
		}
		str += advance();
	}
	if (!isAtEnd())
		advance(); // consume closing '"'
	return makeToken(TokenType::RAW_STRING_LITERAL, str);
}

// ─────────────────────────────────────────────────────────────────────────────
// Doc comment reader  /-- ... --/
// ─────────────────────────────────────────────────────────────────────────────
//
// Called from getNextToken after '/' has already been consumed and we have
// confirmed the next two chars are '--'.
//
// The raw text between the delimiters is stored in the token value exactly as
// written — leading ' -' line prefixes are preserved for the parser / tooling
// to interpret. The opening /-- and closing --/ are NOT included in the value.
//
Token Lexer::readDocComment() {
	advance(); // consume first  '-'  (of opening --)
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

	return makeToken(TokenType::DOC_COMMENT, doc);
}

// ─────────────────────────────────────────────────────────────────────────────
// Line comment reader  -- text
// ─────────────────────────────────────────────────────────────────────────────
//
// Called from getNextToken after the first '-' of '--' has been consumed and
// the next char is confirmed to be '-'. Consumes the second '-', strips the
// optional leading space, and collects the rest of the line as the value.
// Does NOT consume the trailing newline — skipWhitespace will do that on the
// next call so the Lexer's line counter stays correct.
//
Token Lexer::readLineComment() {
	advance(); // consume the second '-'

	// Strip a single optional leading space (the common "-- text" style).
	if (peek() == ' ')
		advance();

	std::string text;
	while (!isAtEnd() && peek() != '\n')
		text += advance();

	return makeToken(TokenType::LINE_COMMENT, text);
}



Token Lexer::getNextToken() {
	skipWhitespace();
	if (isAtEnd())
		return makeToken(TokenType::EOF_TOKEN, "EOF");

	char c = advance();

	// ── Identifiers & Keywords ─────────────────────────────────────────────────
	if (isalpha(c) || c == '_') {
		std::string ident(1, c);
		while (isalnum(peek()) || peek() == '_')
		ident += advance();

			// r"..." raw string literal — 'r' immediately followed by '"'
			if (ident == "r" && peek() == '"') {
				advance(); // consume opening '"'
				return readRawString();
			}

		// Standalone _ is a wildcard token — only valid in match pattern position
		// The parser enforces that _ does not appear in expression context
		if (ident == "_") return makeToken(TokenType::WILDCARD, "_");

		auto it = keywords.find(ident);
		if (it != keywords.end())
			return makeToken(it->second, ident);
		return makeToken(TokenType::IDENTIFIER, ident);
	}

	// ── Numbers ────────────────────────────────────────────────────────────────
	if (isdigit(c))
		return readNumber(c);

	// ── String literals ────────────────────────────────────────────────────────
	if (c == '"')
		return readString();

	// ── Char literals ──────────────────────────────────────────────────────────
	if (c == '\'')
		return readChar();

	// ── Operators & Symbols ────────────────────────────────────────────────────
	switch (c) {
	// ── Access ─────────────────────────────────────────────────────────────────
	case '.':
		if (match('?'))
		    return makeToken(TokenType::DOT_QUESTION, ".?"); // nullable chain
		if (match('.')) {
		    if (match('.'))
		        return makeToken(TokenType::VARIADIC, "..."); // variadic
		    return makeToken(TokenType::RANGE, "..");       // range
		}
		return makeToken(TokenType::DOT, ".");

	case ':':
		return makeToken(TokenType::COLON, ":");

	// ── Nullable ───────────────────────────────────────────────────────────────
	case '?':
		if (match('?'))
		    return makeToken(TokenType::QUESTION_QUESTION, "??"); // null coalescing
		return makeToken(TokenType::QUESTION, "?");             // nullable suffix

	// ── Assignment & Comparison ────────────────────────────────────────────────
	case '=':
		if (match('=')) {
			if (match('='))
			    return makeToken(TokenType::EQUAL_EQUAL_EQUAL, "==="); // reference equality
			return makeToken(TokenType::EQUAL_EQUAL, "==");            // value equality
		}
        return makeToken(TokenType::ASSIGN, "=");

	case '!':
		if (match('='))
		    return makeToken(TokenType::NOT_EQUAL, "!=");
		return makeToken(TokenType::BANG, "!"); // pipeline argument pack annotation

	case '<':
		if (match('<'))
		    return makeToken(TokenType::SHL, "<<");
		if (match('='))
		    return makeToken(TokenType::LESS_EQUAL, "<=");
        return makeToken(TokenType::LESS, "<");

	case '>':
		if (match('>'))
		    return makeToken(TokenType::SHR, ">>");
		if (match('='))
		    return makeToken(TokenType::GREATER_EQUAL, ">=");
        return makeToken(TokenType::GREATER, ">");

	// ── Math ───────────────────────────────────────────────────────────────────
	case '+':
		if (match('>'))
		    return makeToken(TokenType::COMPOSE, "+>"); // function composition
		if (match('='))
		    return makeToken(TokenType::PLUS_ASSIGN, "+=");
        return makeToken(TokenType::PLUS, "+");

	case '-':
		// Check for line comment BEFORE checking for -= and ->
		if (peek() == '-')
			return readLineComment(); // -- line comment
		if (match('='))
		    return makeToken(TokenType::MINUS_ASSIGN, "-=");
		if (match('>'))
		    return makeToken(TokenType::ARROW, "->"); // pipeline
		return makeToken(TokenType::MINUS, "-");

	case '*':
		if (match('='))
		    return makeToken(TokenType::MUL_ASSIGN, "*=");
		return makeToken(TokenType::MUL, "*");

	case '/':
		// /-- doc comment — must check before /= and bare DIV
		// At this point '/' is already consumed; peek() is the next char.
		if (peek() == '-' && pos + 1 < src.size() && src[pos + 1] == '-')
		    return readDocComment();
		if (match('='))
		    return makeToken(TokenType::DIV_ASSIGN, "/=");
		return makeToken(TokenType::DIV, "/");

	case '%':
		if (match('='))
		    return makeToken(TokenType::MOD_ASSIGN, "%=");
		return makeToken(TokenType::MOD, "%");

	case '^':
		if (match('='))
		    return makeToken(TokenType::POW_ASSIGN, "^=");
		return makeToken(TokenType::POW, "^");

	// ── Bitwise ────────────────────────────────────────────────────────────────
	// '&' in expression position is always the unary reference operator (&T, &x).
	// Bitwise AND uses '&&' to avoid ambiguity with the reference operator.
	case '&':
		if (match('&'))
		    return makeToken(TokenType::BIT_AND, "&&"); // bitwise AND
		return makeToken(TokenType::AMPERSAND, "&");    // reference type &T

	// '|' in type position is the union type separator (int | string).
	// Bitwise OR uses '||' to avoid ambiguity with the union type operator.
	case '|':
		if (match('|'))
		    return makeToken(TokenType::BIT_OR, "||"); // bitwise OR
		return makeToken(TokenType::PIPE, "|");        // union type

	case '~':
		if (match('^'))
		    return makeToken(TokenType::BIT_XOR, "~^");
		return makeToken(TokenType::BIT_NOT, "~");

	// ── FFI ────────────────────────────────────────────────────────────────────
	case '@':
		return makeToken(TokenType::AT_SIGN, "@"); // compiler directive: @extern, @inline, @sizeof, etc.

	// ── Delimiters ─────────────────────────────────────────────────────────────
	case ',':
		return makeToken(TokenType::COMMA, ",");
	case ';':
		return makeToken(TokenType::SEMICOLON, ";");
	case '(':
		return makeToken(TokenType::LPAREN, "(");
	case ')':
		return makeToken(TokenType::RPAREN, ")");
	case '{':
		return makeToken(TokenType::LBRACE, "{");
	case '}':
		return makeToken(TokenType::RBRACE, "}");
	case '[':
		return makeToken(TokenType::LBRACKET, "[");
	case ']':
		return makeToken(TokenType::RBRACKET, "]");
	}

	// Unknown character — emit as UNKNOWN for proper error reporting in the
	// parser Using EOF here would cause silent corruption of the token stream
	return makeToken(TokenType::UNKNOWN, std::string(1, c));
}

// ─────────────────────────────────────────────────────────────────────────────
// Public tokenize
// ─────────────────────────────────────────────────────────────────────────────

std::vector<Token> Lexer::tokenize() {
	std::vector<Token> tokens;
	Token t = makeToken(TokenType::EOF_TOKEN, "");

	do {
		t = getNextToken();
		tokens.push_back(t);
	} while (t.type != TokenType::EOF_TOKEN);

	return tokens;
}