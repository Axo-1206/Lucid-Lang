/// @file core/JSONFormatter.cpp
/// @brief Implementation of JSON pretty-printing formatter.

#include "JSONFormatter.hpp"

#include <sstream>
#include <iomanip>

bool JSONFormatter::isWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool JSONFormatter::isDigit(char c) {
    return c >= '0' && c <= '9';
}

bool JSONFormatter::isLetter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

std::string JSONFormatter::format(const std::string& input, int tabSize) {
    if (input.empty()) {
        return input;
    }

    std::ostringstream output;
    int indentLevel = 0;
    bool inString = false;
    bool inEscape = false;
    bool isFirst = true;
    char lastChar = 0;
    
    std::string indentString(tabSize, ' ');

    for (char ch : input) {
        // ─── Handle string boundaries ──────────────────────────────
        // Don't format anything inside strings
        if (ch == '"' && lastChar != '\\' && !inEscape) {
            inString = !inString;
            output << ch;
            lastChar = ch;
            continue;
        }

        // Handle escape sequences inside strings
        if (inString) {
            if (ch == '\\' && !inEscape) {
                inEscape = true;
            } else {
                inEscape = false;
            }
            output << ch;
            lastChar = ch;
            continue;
        }

        // ─── Handle formatting outside strings ──────────────────────

        switch (ch) {
            case '{':
            case '[':
                // If not first item, add newline before object/array
                if (!isFirst) {
                    output << '\n' << std::string(indentLevel * tabSize, ' ');
                }
                output << ch;
                indentLevel++;
                output << '\n' << std::string(indentLevel * tabSize, ' ');
                isFirst = true;
                break;

            case '}':
            case ']':
                indentLevel--;
                output << '\n' << std::string(indentLevel * tabSize, ' ');
                output << ch;
                isFirst = false;
                break;

            case ',':
                output << ch;
                output << '\n' << std::string(indentLevel * tabSize, ' ');
                isFirst = true;
                break;

            case ':':
                output << ch;
                output << ' ';
                isFirst = false;
                break;

            case ' ':
            case '\t':
            case '\n':
            case '\r':
                // Skip existing whitespace outside strings
                break;

            default:
                output << ch;
                isFirst = false;
                break;
        }
        lastChar = ch;
    }

    return output.str();
}
