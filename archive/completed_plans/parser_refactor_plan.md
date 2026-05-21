## Confirmed bugs (fix immediately)

### BehaviorAccessExprAST copies typeName into method field

After consuming the type name, colon, and method name, both typeName and method are set from name (the type). The method variable — which holds the actual method name — is never stored.

ParserExpr.cpp:724–725
```cpp
node->typeName = std::move(pool_.intern(name));
node->method   = std::move(pool_.intern(name));  // BUG: should be pool_.intern(method)
```

### parsePackageDecl() result used after move

The code calls attachDoc(*pkgDecl, ...) and then checks if (pkgDecl) — but after the attachDoc call the pointer may already have been consumed by the std::move inside attachDoc. The if (pkgDecl) null check is essentially dead code and will always be true because parsePackageDecl never returns null. The real danger is that the order of operations could differ if the function is refactored.

Parser.cpp:618–621
```cpp
attachDoc(*pkgDecl, std::move(pkgDoc));  // pkgDoc moved
if (pkgDecl) { ... }  // always true; logic relies on this assumption implicitly
```

### @extern lookahead ignores DOC_COMMENT tokens

The manual lookahead to find '(' after the name skips LINE_COMMENT tokens but not DOC_COMMENT tokens. If a doc comment sits between the name and the parameter list, the lookahead will see DOC_COMMENT instead of LPAREN, causing an @extern function to be misclassified as a variable.

Parser.cpp:791–796
```cpp
std::size_t lookAhead = pos_ + 1;
while (lookAhead < tokens_.size()
       && tokens_[lookAhead].type == TokenType::LINE_COMMENT)  // misses DOC_COMMENT
    lookAhead++;
```

### parseMatchExpr continues parsing after 'default' arm

After successfully parsing the default arm, the loop just calls continue and keeps iterating. Any arm that appears after default in the source is silently parsed and added to node->arms. Unreachable arms should be rejected with an error, not silently accepted.

ParserExpr.cpp:1243–1259

### Anon-func detection for () case uses looksLikeType() on stale context

When the token stream is (), the code sets anonFunc = looksLikeType() as a fallback, but looksLikeType() inspects peek() which at that moment is still the opening ( — not the token at offset 2. This makes the check incorrect; the correct token is already captured in n2 and the dedicated isTypeStart lambda is the right check, but the bool anonFunc assignment on line 567 is redundant and misleading.

ParserExpr.cpp:567–568

### Summary

The most critical one is in `parsePrimaryExpr` (line 724–725): when parsing a behavior access like `Vec2:normalize`, both `typeName` and `method` on the AST node are set to the type name — the method name is thrown away. Every behavior member reference in your AST is currently broken.

The `@extern` lookahead also skips `LINE_COMMENT` tokens but not `DOC_COMMENT` tokens, so an `@extern` function preceded by a doc comment will be misclassified as a variable declaration.

After parsing the `default` arm in a match expression, the loop just continues instead of stopping — any arms written after `default` are silently accepted into the arm list rather than rejected.

--- 

## Infinite loop risks (high priority)

### parseStructLiteralExpr synchronize() does not advance on every path

When the current token is not an IDENTIFIER inside a struct literal, synchronize() is called and the loop continues. If synchronize() stops at a RBRACE (the struct's own closing brace), the outer while sees check(RBRACE) == false immediately after, loops again, and calls synchronize() again on the same token forever. The fix is to check for RBRACE or EOF after synchronize() and break.

ParserExpr.cpp:1014–1017

```cpp
if (!check(TokenType::IDENTIFIER)) {
    errorAt(...);
    synchronize();   // may stop at RBRACE — then outer while loops again
    continue;        // ← could spin forever
}
```

### parseMatchExpr: arm parsing failure produces no progress

If parseMatchArm() returns a null/empty arm (e.g. because parsePattern() returns null and consume(FAT_ARROW,...) creates a dummy token without advancing), the outer while loop re-enters with the same token. There is no synchronize() call or mandatory advancement on the failure path.

ParserExpr.cpp:1257–1259

### parseParamGroup: malformed param with no type loops forever

If the token after the parameter name is not a valid type start, parseType() returns an UnknownTypeAST (not null) but the token is not consumed. The loop then rechecks the same non-RPAREN token and re-enters the loop body. The match(COMMA) at the top will return false, so no progress is made.

ParserDecl.cpp:598–630
```cpp
while (!check(TokenType::RPAREN) && !isAtEnd()) {
    match(TokenType::COMMA);
    // ...
    TypePtr paramType = parseType();
    if (!paramType) { errorAt(...); break; }  // parseType never returns null
    // if type parse consumed 0 tokens, this loops forever
}
```

### parseArrayLiteralExpr: parseExpr returns UnknownExprAST (non-null) on error

parseExpr() (via parsePrattExpr) always returns a non-null node (it returns UnknownExprAST on failure). So the if (!elem) check is dead code — if parseExpr makes no progress (e.g. because the current token is ']'), the loop spins. The check needs to track whether pos_ advanced

ParserExpr.cpp:971–982

### Summary

The most dangerous one is in `parseStructLiteralExpr`. When an unexpected token appears inside a struct literal body, `synchronize()` is called and then `continue` is called — but `synchronize()` can stop at the struct's own closing `}`, causing the outer `while(!RBRACE)` to re-enter and call `synchronize()` again forever.

The same class of problem affects `parseMatchExpr` and `parseParamGroup`: when a sub-parse makes zero progress, the outer loop re-enters with the same token. The `parseBlock` has a saved-position check to catch this, but the other loops don't.

---

## Logic errors & design issues

### parseSwitchCase do-while starts by discarding first comma

The do { match(COMMA); ... } while (check(COMMA)) structure discards a leading comma on the very first iteration before reading any value. A valid first value like case 42: will parse fine, but case ,42: would silently consume the leading comma. The canonical pattern should be a while with a trailing comma check, or move match(COMMA) to between iterations.

ParserStmt.cpp:393–394

### parsePipelineStep treats any '(' as anonymous function

The heuristic sets isAnonFunc = true whenever the current token is '(', with a comment saying "parseAnonFuncExpr will validate." This means a mistyped step like -> (wrongExpr) will attempt to parse a full anonymous function body and produce confusing errors far from the actual mistake, instead of reporting a clean "expected function name" message.

ParserExpr.cpp:1582–1584

### harvestDocComment: declLine subtraction can underflow on line 1

The checks declLine - t.line == 1 and stackedTopLine - t.line == 1 use signed subtraction on int. If declLine is 1 and a token has line 0 (constructed dummy tokens from consume() failures), 1 - 0 == 1 accidentally matches, and an unrelated comment gets attached as documentation.

Parser.cpp:274, 285

### parseFuncType: nullable detection scan does not restore pos_ on failure path

The nullable detection code advances pos_ speculatively through the entire function type to check whether a '?' follows the closing ')', then resets via pos_ = savedPos. If the scan exits early (e.g. on TILDE hit in the loop), savedPos is still correctly restored — but any tokens consumed by the inner advance() calls inside the qualifier loop before the scan starts are not included in savedPos. This is a latent bug that will surface if qualifiers appear before the leading '('.

ParserType.cpp — parseFuncType

### looksLikeFuncDecl generic param scan doesn't skip LINE_COMMENT tokens

The manual lookahead loop inside the <...> scan increments raw indices into tokens_ and never skips LINE_COMMENT tokens. A comment inside a generic param list — e.g. let f<T -- constraint\n> (...) — will cause the scan to see a non-GREATER token at depth 1 and leave depth > 0, so the '(' check fails and the function is misclassified as a variable.

Parser.cpp:425–438

### parseMatchArm do-while on exprs can silently accept trailing commas

The do { exp = parseExpr(); ... } while (match(COMMA)) loop will consume the comma between the primary and secondary value correctly, but if the secondary expression fails to parse (returns UnknownExprAST), the if (!exp) break check will not fire because parseExpr always returns non-null. The loop then tries another match(COMMA), which may consume the next arm's leading comma.

ParserExpr.cpp:1912–1916

## Code quality & improvements

### isTypeStart lambda defined twice in parsePrimaryExpr - Duplication

The same 20-case isTypeStart lambda is defined verbatim twice inside the same function scope. It should be hoisted once before the if (check(LPAREN)) block.

ParserExpr.cpp:571–605, 614–647

### Primitive type switch repeated 5+ times across files - Duplication

The full 22-case switch over all primitive type tokens (TYPE_BOOL through TYPE_ANY) appears in looksLikeStmtStart, parsePipelineStep, parseComposeOperand, parseFuncType, and parsePrimaryExpr. A single static bool isPrimitiveTypeToken(TokenType) helper would remove all five copies.

ParserExpr.cpp, Parser.cpp, ParserType.cpp

### parseNullCoalesceExpr is a dead stub - Dead code

The function exists solely to satisfy the declaration, ignores its argument with (void)lhs, and returns UnknownExprAST. The comment says "currently unused." It should be removed or the declaration removed from the header.

ParserExpr.cpp:1866–1870

### pool_.intern("extern") called in a loop on every let/const declaration - Performance

pool_.intern("extern") is called inside a for loop over attributes on every let/const declaration. While intern() is a hash lookup and not expensive, the string "extern" (and "packed", "inline", etc.) should be interned once at parser construction and stored as member fields.

Parser.cpp:780, ParserDecl.cpp:403

### consume() silently succeeds when the token is already missing - Improvement

When check(type) fails, consume() reports an error and returns a dummy token with value="" — without advancing. Any caller that does not explicitly check the error state will continue as if the token was present, potentially cascading into further incorrect state. Consider returning an std::optional<Token> so callers must handle the failure explicitly.

Parser.cpp:120–126

### looksLikeStructLiteral generic scan ignores EOF and unterminated <> - Improvement

If the source contains a bare < with no matching >, the scan loop will walk all the way to end of token list with depth > 0. At that point i equals tokens_.size() and the final tokens_[i].type access is an out-of-bounds read (undefined behaviour). The same issue exists in looksLikeFuncDecl.

Parser.cpp:481–491

### parseIntrinsicCallExpr: typeParamIntrinsics uses a static initializer_list with string comparison - Improvement

A static const std::initializer_list<std::string> is linear-scanned with string comparison on every intrinsic call. Since these names are interned, comparing InternedString IDs would be O(1). Better: intern the known names once at construction and compare IDs.

ParserExpr.cpp:1139–1144

---

**Memory / UB risk**

The generic lookahead loops in `looksLikeStructLiteral` and `looksLikeFuncDecl` walk raw indices into `tokens_` without an `EOF_TOKEN` guard. With an unterminated `<`, the loop exits with `i == tokens_.size()` and then accesses `tokens_[i]` — out-of-bounds read.

**Code quality**

The `isTypeStart` lambda is defined identically twice in the same function. The 22-case primitive-type switch is copy-pasted across five different locations. Both should be factored into a single `static` helper. The `parseNullCoalesceExpr` function is a dead stub that should be removed.