/// @file Tokens.cpp
/// @brief Definitions of the token-name lookup tables.
/// 
/// These tables are long enough that putting them in Tokens.hpp would pull
/// the entire string table into every translation unit that includes the
/// header — which is every translation unit in the frontend. One .cpp keeps
/// the header light.

#include "core/Tokens.hpp"

const char* tokenTypeName(TokenType t) noexcept {
    switch (t) {
        case TokenType::EOF_TOKEN:          return "EOF_TOKEN";
        case TokenType::UNKNOWN:            return "UNKNOWN";
        case TokenType::IDENTIFIER:         return "IDENTIFIER";

        // Frame keywords
        case TokenType::KW_TYPE:            return "KW_TYPE";
        case TokenType::KW_FN:              return "KW_FN";
        case TokenType::KW_DEF:             return "KW_DEF";
        case TokenType::KW_REQUIRE:         return "KW_REQUIRE";
        case TokenType::KW_CONST:           return "KW_CONST";
        case TokenType::KW_LET:             return "KW_LET";
        case TokenType::KW_IMPORT:          return "KW_IMPORT";
        case TokenType::KW_TRAIT:           return "KW_TRAIT";
        case TokenType::KW_SATISFY:         return "KW_SATISFY";

        // Content markers
        case TokenType::KW_STRUCT:          return "KW_STRUCT";
        case TokenType::KW_ENUM:            return "KW_ENUM";
        case TokenType::KW_FN_MARKER:       return "KW_FN_MARKER";
        case TokenType::KW_CLS_MARKER:      return "KW_CLS_MARKER";
        case TokenType::KW_AS:              return "KW_AS";
        case TokenType::KW_SELF:            return "KW_SELF";

        // Statement keywords
        case TokenType::KW_IF:              return "KW_IF";
        case TokenType::KW_ELSE:            return "KW_ELSE";
        case TokenType::KW_FOR:             return "KW_FOR";
        case TokenType::KW_WHILE:           return "KW_WHILE";
        case TokenType::KW_DO:              return "KW_DO";
        case TokenType::KW_SWITCH:          return "KW_SWITCH";
        case TokenType::KW_CASE:            return "KW_CASE";
        case TokenType::KW_DEFAULT:         return "KW_DEFAULT";
        case TokenType::KW_BREAK:           return "KW_BREAK";
        case TokenType::KW_CONTINUE:        return "KW_CONTINUE";
        case TokenType::KW_RETURN:          return "KW_RETURN";

        // Concurrency keywords
        case TokenType::KW_ASYNC:           return "KW_ASYNC";
        case TokenType::KW_SPAWN:           return "KW_SPAWN";
        case TokenType::KW_START:           return "KW_START";
        case TokenType::KW_AWAIT:           return "KW_AWAIT";
        case TokenType::KW_ALL:             return "KW_ALL";
        case TokenType::KW_ANY:             return "KW_ANY";

        // Literal keywords
        case TokenType::KW_NIL:             return "KW_NIL";
        case TokenType::KW_ERR:             return "KW_ERR";
        case TokenType::KW_TRUE:            return "KW_TRUE";
        case TokenType::KW_FALSE:           return "KW_FALSE";

        // Literals with a payload
        case TokenType::INT_LITERAL:        return "INT_LITERAL";
        case TokenType::FLOAT_LITERAL:      return "FLOAT_LITERAL";
        case TokenType::HEX_LITERAL:        return "HEX_LITERAL";
        case TokenType::BINARY_LITERAL:     return "BINARY_LITERAL";
        case TokenType::CHAR_LITERAL:       return "CHAR_LITERAL";
        case TokenType::STRING_HEAD:        return "STRING_HEAD";
        case TokenType::STRING_MIDDLE:      return "STRING_MIDDLE";
        case TokenType::STRING_END:         return "STRING_END";
        case TokenType::RAW_STRING_LITERAL: return "RAW_STRING_LITERAL";

        // Comments
        case TokenType::DOC_COMMENT:        return "DOC_COMMENT";

        // Delimiters
        case TokenType::LPAREN:             return "LPAREN";
        case TokenType::RPAREN:             return "RPAREN";
        case TokenType::LBRACE:             return "LBRACE";
        case TokenType::RBRACE:             return "RBRACE";
        case TokenType::LBRACKET:           return "LBRACKET";
        case TokenType::RBRACKET:           return "RBRACKET";
        case TokenType::COMMA:              return "COMMA";
        case TokenType::SEMICOLON:          return "SEMICOLON";
        case TokenType::COLON:              return "COLON";
        case TokenType::DOUBLE_COLON:       return "DOUBLE_COLON";
        case TokenType::DOT:                return "DOT";
        case TokenType::ARROW:              return "ARROW";
        case TokenType::RANGE:              return "RANGE";
        case TokenType::RANGE_EXCLUSIVE:    return "RANGE_EXCLUSIVE";
        case TokenType::VARIADIC:           return "VARIADIC";
        case TokenType::AT_SIGN:            return "AT_SIGN";
        case TokenType::HASH:               return "HASH";

        // Assignment operators
        case TokenType::ASSIGN:             return "ASSIGN";
        case TokenType::PLUS_ASSIGN:        return "PLUS_ASSIGN";
        case TokenType::MINUS_ASSIGN:       return "MINUS_ASSIGN";
        case TokenType::MUL_ASSIGN:         return "MUL_ASSIGN";
        case TokenType::DIV_ASSIGN:         return "DIV_ASSIGN";
        case TokenType::MOD_ASSIGN:         return "MOD_ASSIGN";
        case TokenType::POW_ASSIGN:         return "POW_ASSIGN";
        case TokenType::BIT_AND_ASSIGN:     return "BIT_AND_ASSIGN";
        case TokenType::BIT_OR_ASSIGN:      return "BIT_OR_ASSIGN";
        case TokenType::BIT_XOR_ASSIGN:     return "BIT_XOR_ASSIGN";
        case TokenType::SHL_ASSIGN:         return "SHL_ASSIGN";
        case TokenType::SHR_ASSIGN:         return "SHR_ASSIGN";

        // Arithmetic operators
        case TokenType::PLUS:               return "PLUS";
        case TokenType::MINUS:              return "MINUS";
        case TokenType::MUL:                return "MUL";
        case TokenType::DIV:                return "DIV";
        case TokenType::MOD:                return "MOD";
        case TokenType::POW:                return "POW";

        // Comparison operators
        case TokenType::EQUAL_EQUAL:        return "EQUAL_EQUAL";
        case TokenType::NOT_EQUAL:          return "NOT_EQUAL";
        case TokenType::LESS:               return "LESS";
        case TokenType::LESS_EQUAL:         return "LESS_EQUAL";
        case TokenType::GREATER:            return "GREATER";
        case TokenType::GREATER_EQUAL:      return "GREATER_EQUAL";

        // Bitwise operators
        case TokenType::BIT_AND:            return "BIT_AND";
        case TokenType::BIT_OR:             return "BIT_OR";
        case TokenType::BIT_XOR:            return "BIT_XOR";
        case TokenType::BIT_NOT:            return "BIT_NOT";
        case TokenType::SHL:                return "SHL";
        case TokenType::SHR:                return "SHR";

        // Suffix markers
        case TokenType::QUESTION:           return "QUESTION";
        case TokenType::BANG:               return "BANG";
        case TokenType::QUESTION_BANG:      return "QUESTION_BANG";

        // Special operators
        case TokenType::PIPELINE:           return "PIPELINE";
        case TokenType::QUESTION_QUESTION:  return "QUESTION_QUESTION";
    }
    return "<unknown-token>";
}

const char* tokenTypeDescription(TokenType t) noexcept {
    switch (t) {
        case TokenType::EOF_TOKEN:          return "end of input";
        case TokenType::UNKNOWN:            return "unknown token";
        case TokenType::IDENTIFIER:         return "identifier";

        // Frame keywords — spell them as they appear in source
        case TokenType::KW_TYPE:            return "'TYPE'";
        case TokenType::KW_FN:              return "'FN'";
        case TokenType::KW_DEF:             return "'DEF'";
        case TokenType::KW_REQUIRE:         return "'REQUIRE'";
        case TokenType::KW_CONST:           return "'const'";
        case TokenType::KW_LET:             return "'let'";
        case TokenType::KW_IMPORT:          return "'import'";
        case TokenType::KW_TRAIT:           return "'trait'";
        case TokenType::KW_SATISFY:         return "'satisfy'";

        // Content markers
        case TokenType::KW_STRUCT:          return "'struct'";
        case TokenType::KW_ENUM:            return "'enum'";
        case TokenType::KW_FN_MARKER:       return "'fn'";
        case TokenType::KW_CLS_MARKER:      return "'cls'";
        case TokenType::KW_AS:              return "'as'";
        case TokenType::KW_SELF:            return "'Self'";

        // Statement keywords
        case TokenType::KW_IF:              return "'if'";
        case TokenType::KW_ELSE:            return "'else'";
        case TokenType::KW_FOR:             return "'for'";
        case TokenType::KW_WHILE:           return "'while'";
        case TokenType::KW_DO:              return "'do'";
        case TokenType::KW_SWITCH:          return "'switch'";
        case TokenType::KW_CASE:            return "'case'";
        case TokenType::KW_DEFAULT:         return "'default'";
        case TokenType::KW_BREAK:           return "'break'";
        case TokenType::KW_CONTINUE:        return "'continue'";
        case TokenType::KW_RETURN:          return "'return'";

        // Concurrency keywords
        case TokenType::KW_ASYNC:           return "'async'";
        case TokenType::KW_SPAWN:           return "'spawn'";
        case TokenType::KW_START:           return "'start'";
        case TokenType::KW_AWAIT:           return "'await'";
        case TokenType::KW_ALL:             return "'all'";
        case TokenType::KW_ANY:             return "'any'";

        // Literal keywords
        case TokenType::KW_NIL:             return "'nil'";
        case TokenType::KW_ERR:             return "'err'";
        case TokenType::KW_TRUE:            return "'true'";
        case TokenType::KW_FALSE:           return "'false'";

        // Literals with a payload
        case TokenType::INT_LITERAL:        return "integer literal";
        case TokenType::FLOAT_LITERAL:      return "float literal";
        case TokenType::HEX_LITERAL:        return "hex literal";
        case TokenType::BINARY_LITERAL:     return "binary literal";
        case TokenType::CHAR_LITERAL:       return "character literal";
        case TokenType::STRING_HEAD:        return "string";
        case TokenType::STRING_MIDDLE:      return "string segment";
        case TokenType::STRING_END:         return "string terminator";
        case TokenType::RAW_STRING_LITERAL: return "raw string";

        // Comments
        case TokenType::DOC_COMMENT:        return "doc comment";

        // Delimiters
        case TokenType::LPAREN:             return "'('";
        case TokenType::RPAREN:             return "')'";
        case TokenType::LBRACE:             return "'{'";
        case TokenType::RBRACE:             return "'}'";
        case TokenType::LBRACKET:           return "'['";
        case TokenType::RBRACKET:           return "']'";
        case TokenType::COMMA:              return "','";
        case TokenType::SEMICOLON:          return "';'";
        case TokenType::COLON:              return "':'";
        case TokenType::DOUBLE_COLON:       return "'::'";
        case TokenType::DOT:                return "'.'";
        case TokenType::ARROW:              return "'->'";
        case TokenType::RANGE:              return "'..'";
        case TokenType::RANGE_EXCLUSIVE:    return "'..<'";
        case TokenType::VARIADIC:           return "'...'";
        case TokenType::AT_SIGN:            return "'@'";
        case TokenType::HASH:               return "'#'";

        // Assignment operators
        case TokenType::ASSIGN:             return "'='";
        case TokenType::PLUS_ASSIGN:        return "'+='";
        case TokenType::MINUS_ASSIGN:       return "'-='";
        case TokenType::MUL_ASSIGN:         return "'*='";
        case TokenType::DIV_ASSIGN:         return "'/='";
        case TokenType::MOD_ASSIGN:         return "'%='";
        case TokenType::POW_ASSIGN:         return "'**='";
        case TokenType::BIT_AND_ASSIGN:     return "'&='";
        case TokenType::BIT_OR_ASSIGN:      return "'|='";
        case TokenType::BIT_XOR_ASSIGN:     return "'^='";
        case TokenType::SHL_ASSIGN:         return "'<<='";
        case TokenType::SHR_ASSIGN:         return "'>>='";

        // Arithmetic operators
        case TokenType::PLUS:               return "'+'";
        case TokenType::MINUS:              return "'-'";
        case TokenType::MUL:                return "'*'";
        case TokenType::DIV:                return "'/'";
        case TokenType::MOD:                return "'%'";
        case TokenType::POW:                return "'**'";

        // Comparison operators
        case TokenType::EQUAL_EQUAL:        return "'=='";
        case TokenType::NOT_EQUAL:          return "'!='";
        case TokenType::LESS:               return "'<'";
        case TokenType::LESS_EQUAL:         return "'<='";
        case TokenType::GREATER:            return "'>'";
        case TokenType::GREATER_EQUAL:      return "'>='";

        // Bitwise operators
        case TokenType::BIT_AND:            return "'&'";
        case TokenType::BIT_OR:             return "'|'";
        case TokenType::BIT_XOR:            return "'^'";
        case TokenType::BIT_NOT:            return "'~'";
        case TokenType::SHL:                return "'<<'";
        case TokenType::SHR:                return "'>>'";

        // Suffix markers
        case TokenType::QUESTION:           return "'?'";
        case TokenType::BANG:               return "'!'";
        case TokenType::QUESTION_BANG:      return "'?!'";

        // Special operators
        case TokenType::PIPELINE:           return "'|>'";
        case TokenType::QUESTION_QUESTION:  return "'?\?'";
    }
    return "<unknown-token>";
}