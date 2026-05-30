#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

ArenaSpan<TypePtr> Parser::parseGenericArgs() {
    if (!ts_.match(TokenType::LESS)) return ArenaSpan<TypePtr>();
    
    if (ts_.check(TokenType::GREATER)) {
        ts_.advance();
        return ArenaSpan<TypePtr>();
    }
    
    std::vector<TypePtr> args;
    do {
        if (ts_.match(TokenType::COMMA)) continue;
        
        size_t savedPos = ts_.getPos();
        TypePtr arg = parseType();
        if (ts_.getPos() == savedPos) {
            errorAt(DiagCode::E2005, "expected type in generic argument list");
            if (!ts_.isAtEnd()) ts_.advance();
            break;
        }
        args.push_back(std::move(arg));
    } while (!ts_.check(TokenType::GREATER) && !ts_.isAtEnd());
    
    ts_.consume(TokenType::GREATER, "expected '>' to close generic arguments");
    
    auto builder = arena_.makeBuilder<TypePtr>();
    for (auto& a : args) builder.push_back(std::move(a));
    return builder.build();
}