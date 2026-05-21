Bug
parseFuncType: redundant dead inner check always evaluates to false
ParserType.cpp:511–518
The outer guard is if (check(TokenType::LPAREN)). Inside that block, the very next statement is if (!check(TokenType::LPAREN)). Because the outer guard just verified the token is LPAREN and no token has been consumed between the two checks, the inner condition is always false. The error path inside it is dead code and the comment "should not happen" is actually a guarantee. The inner block should be removed entirely to avoid confusion.
if (check(TokenType::LPAREN)) {          // token IS '('
    if (!check(TokenType::LPAREN)) {     // always false — dead branch
        errorAt(...);
        return arena_.make<UnknownTypeAST>();
    }
    // real scan starts here
}
Bug
parseMatchArm: third+ comma consumed but third expression silently discarded
ParserExpr.cpp:1819–1821
When a match arm has three or more expressions (e.g. 200 => "ok", "detail", "extra"), the code reports the error and then calls match(TokenType::COMMA) — consuming the second comma. But it never parses or skips the third expression. The match loop then sees the third expression token as the start of the next arm, producing a confusing "expected '=>'" error on a valid expression. The fix: after reporting the error, skip to the next arm boundary (a token that looks like a pattern start or RBRACE).
Bug
AttributeRegistry::lookup(InternedString) calls getPool() which asserts — crashes after resetStringPool()
AttributeRegistry.cpp:149–150
lookup(InternedString) calls getPool() which unconditionally asserts stringPool != nullptr. But after resetStringPool() is called, stringPool is set to null. Any code path that calls lookup(InternedString id) after reset (e.g. a diagnostic pass or a second parse session during testing) will hit the assert and crash — even though a null return would be the correct and safe behaviour. The string-overload lookup(const std::string&) correctly does a null check; the ID-overload should do the same.
// Current:
const AttributeInfo* AttributeRegistry::lookup(InternedString id) const {
    getPool();          // asserts — will crash after resetStringPool()
    auto it = byId.find(id);
    return (it != byId.end()) ? &it->second : nullptr;
}

// Fix:
const AttributeInfo* AttributeRegistry::lookup(InternedString id) const {
    if (!stringPool) return nullptr;   // match the string-overload behaviour
    auto it = byId.find(id);
    return (it != byId.end()) ? &it->second : nullptr;
}
🟠 Potential infinite loops
Infinite loop risk
parseSwitchCase additional-values loop: parsePrattExpr makes no progress on bad token
ParserStmt.cpp:405–416
The while (check(COMMA)) loop advances past the comma, then calls parsePrattExpr(0). If parsePrattExpr makes no progress (returns UnknownExprAST without consuming a token), the next iteration calls check(COMMA). If the current token is still a COMMA (e.g. two consecutive commas: case 1,,2), the loop re-enters, advances the second comma, and calls parsePrattExpr again. This will loop until commas are exhausted but will not hang indefinitely on most inputs — however if a non-literal keyword appears between commas without triggering a parse advance, it could loop. A position check before and after parsePrattExpr would make this robust.
Infinite loop risk
parseFuncType parameter loop: parseType on unknown token returns UnknownTypeAST without advancing
ParserType.cpp:556–589
The do { ... } while (match(COMMA)) loop in the parameter list will call parseType(). parseType() returns UnknownTypeAST but does not advance on an unrecognised token. If the source has something like (a @invalid), parseType returns without consuming @invalid, and the loop checks match(COMMA) — false — so it exits. That path is actually safe. But if the unrecognised token is followed immediately by a , (e.g. (a @x, b int)), parseType returns, the comma is consumed, and the loop re-enters. On the second iteration, the name-skip heuristic may consume b and then parseType fails again on int after the bad token was never consumed — leading to incorrect parameter parsing rather than an infinite loop, but still a correctness issue.
🔵 Logic errors & design issues
Logic error
AttributeRegistry::lookup(string) calls lookup(InternedString) which may assert
AttributeRegistry.cpp:155–159
The string-overload does its own null check and returns early if stringPool == nullptr. But it then calls lookup(it->second) which is the InternedString overload — which calls getPool() and asserts. So if resetStringPool() is called but the byId map is cleared (which it is), the nameToId find will fail and return null before reaching the inner call. In practice this is safe only because resetStringPool() also clears nameToId. The logic is sound but fragile — it relies on nameToId and byId always being cleared together. A single explicit null guard in the ID-overload removes the dependency entirely.
Logic error
parseFuncType nullable scan: qualifier inside outer paren is mis-skipped
ParserType.cpp:533–537
The nullable detection scan skips TILDE + IDENTIFIER pairs when it encounters them inside the paren scan. But this is wrong for the nullable-function detection: the qualifiers that matter here are on the inner function type, between the outer ( and the inner (. Skipping them changes the paren depth accounting. Consider ( ~async (int) string )? — the scanner enters with parenDepth=1, sees ~, skips it, sees async (an identifier), skips it, then sees ( and increments depth to 2. This is correct behaviour. But if the qualifier name itself contains a LPAREN character (impossible in this language, but the skip logic uses raw token type), any future qualifier syntax change could break the scan silently. Worth a comment noting the invariant.
Logic error
parseMatchArm: patterns do-while consumes comma before checking if second pattern is valid
ParserExpr.cpp:1769–1776
The do { pat = parsePattern(); if (!pat) break; ... } while (match(COMMA)) loop consumes the comma before parsing the next pattern. If the token after the comma is not a valid pattern start (e.g. a stray keyword), parsePattern() reports an error and returns null, breaking the loop. The comma has already been consumed, so it is gone from the stream. This means the arm has been partially built with a missing second pattern, but the outer match loop's progress check (pos_ == beforePos) still passes because the comma was consumed. The semantic pass will see an arm with fewer patterns than written. A lookahead check before consuming the comma would prevent the partial state.
Logic error
looksLikeAnonFunc only checks first parameter group for curried anon funcs
Parser.cpp:522–590
The new implementation scans the first parameter group to find its closing ')' and then checks if the next token is '{' or a return type start. It does not consider curried anonymous functions like (a int) (b int) int { ... }. After the first group's closing ')', the next token is '(' (the second group), which is not in the return-type switch. So looksLikeAnonFunc() returns false for curried anonymous functions, causing parsePipelineStep to reject them. The check after the first group should allow additional '(' groups before the return type.
⚪ Code quality
Design issue
AttributeRegistry::pending is a namespace-static that persists across test runs
AttributeRegistry.cpp:72–80
The anonymous-namespace std::vector<PendingAttr> pending is a program-wide static. The constructor pushes to it and setStringPool() calls pending.clear(). In a single-process test suite that creates multiple parsers, the singleton constructor only runs once and pending is cleared on first setStringPool() call. That's fine. However, if resetStringPool() is called and a second setStringPool() is called on the same singleton, the if (stringPool) return; guard will fire and pending (already cleared) will not be re-processed, so the registry stays empty after the second init. The re-init path is broken.
Inconsistency
IntrinsicRegistry::lookup(string) skips getPool assert; isKnown(string) does too — but lookup(InternedString) and isKnown(InternedString) always assert
IntrinsicRegistry.cpp:354–383
The string overloads check if (!stringPool) return nullptr/false silently. The ID overloads call getPool() which asserts. This asymmetry means calling the same logical operation through a string vs an interned ID gives different error behaviour after resetStringPool(). The ID-based overloads should use the same null-return pattern as the string overloads, or both should assert. Choose one policy and apply it consistently.
Minor issue
parseGenericParams: break on stall exits loop but then consume(GREATER) will fail on the stalling token
ParserDecl.cpp:665–672
When a stall is detected (pos_ == savedPos), the code reports an error and calls break to exit the loop. The break falls through to consume(GREATER, ...). If the stalling token is not GREATER, consume will report a second error ("expected '>'") on the same token. This produces a redundant error message. After the break, consider calling synchronize() to advance to a saner position before the consume, or skip to the next GREATER manually.
Unused variable
peekAt has an unused local 'found'
Parser.cpp:71
std::size_t found = 0; is declared and never read or incremented anywhere in peekAt(). It should be removed to avoid a compiler warning.