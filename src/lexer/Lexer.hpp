/**
 * @file Lexer.hpp
 * @project LUC Compiler
 * @responsibility The Front-End Scanner. Converts raw source code (strings) into a Token stream.
 * 
 * @related_files
 *   - src/Tokens.hpp (The vocabulary: Defines what a Token is)
 *   - src/lexer/Lexer.cpp (The implementation: Heavy regex/scanning logic)
 *   - src/parser/Parser.hpp (The consumer: Reads the tokens produced here)
 * 
 * @spec_references docs/LUC_GRAMMAR.md (Legal identifiers, literal formats)
 * 
 * @note This Lexer is responsible for emitting DOC_COMMENT tokens (started with /--) 
 *       which the Parser later attaches to AST nodes.
 */

#pragma once
#include "Tokens.hpp"
#include <string>
#include <vector>
#include <unordered_map>

class Lexer
{
public:
    explicit Lexer(const std::string& source);

    // Returns all tokens including the final EOF_TOKEN.
    // DOC_COMMENT tokens are emitted inline — the parser attaches each one
    // to the declaration that immediately follows it.
    std::vector<Token> tokenize();

private:
    // ── State ──────────────────────────────────────────────────────────────────
    std::string src;
    size_t      pos;
    int         line;
    int         column;
    std::unordered_map<std::string, TokenType> keywords;

    // ── Primitives ─────────────────────────────────────────────────────────────
    bool  isAtEnd();
    char  peek();
    char  peekNext();
    char  advance();
    bool  match(char expected);

    Token makeToken(TokenType type, const std::string& value);

    // ── Skipping ───────────────────────────────────────────────────────────────
    // Eats whitespace, single-line comments (--) and block comments (/- ... -/).
    // Stops without consuming when it encounters a doc comment /-- so that
    // getNextToken can emit it as a DOC_COMMENT token.
    void skipWhitespace();

    // ── Literal readers ────────────────────────────────────────────────────────
    Token readNumber(char first);
    Token readString();
    Token readRawString();
    Token readChar();

    // Reads a /-- ... --/ doc comment. Called from getNextToken after '/' has
    // been consumed and the next two chars are confirmed to be '--'.
    Token readDocComment();

    // ── Scanner ────────────────────────────────────────────────────────────────
    Token getNextToken();
};