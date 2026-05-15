Bug
parseParamGroup infinite loop: pos_ check compares wrong baseline
ParserDecl.cpp:602, 616
savedPos is captured at the top of the loop body, before advance() is called to consume the parameter name (line 610). If parseType() returns UnknownTypeAST without consuming any tokens, the check pos_ == savedPos is already false (because the name was consumed), so the loop never breaks. The fix: capture savedPos after consuming the name and the variadic, just before calling parseType().
size_t savedPos = pos_;           // captured before name is consumed
InternedString paramName = pool_.intern(advance().value); // advances pos_
bool isVariadic = match(TokenType::VARIADIC);             // may advance again
TypePtr paramType = parseType();
if (paramType->isa<UnknownTypeAST>() && pos_ == savedPos) // always false!

Bug
parseFuncType nullable scan does not restore pos_ on early exit
ParserType.cpp:506–536
The scan loop walks raw token indices to find the matching ')' and check for '?'. This version is correct in that it doesn't modify pos_ during the scan — it only advances pos_ when it decides isNullableFunction = true (line 535: advance()). However, if allowQualifiers is true, qualifiers have already been consumed by the time the nullable scan runs. If the scan finds no '?' and sets isNullableFunction = false, the subsequent consume(LPAREN) on line 540 correctly consumes the real '('. The logic is sound but very fragile — the consumed qualifiers are not part of the scan, so a qualifier like ~async before ((int) string)? would cause the scan to start at the inner '(' rather than the outer one, silently misidentifying the form. Needs a comment or assertion.

Bug
looksLikeStructLiteral: depth check wrong when no generic args present
Parser.cpp:618–641
depth is initialised to 1 on line 619. The code only modifies it inside the if (tokens_[i].type == TokenType::LESS) branch. If there are no generic args, the branch is skipped entirely and depth stays 1. But the final result check on line 639 requires depth == 0, so every non-generic struct literal (Vec2 { ... }) always returns false. This breaks all non-generic struct literals in expression position.
std::size_t i = pos_ + 1;
int depth = 1;                                       // BUG: initialised to 1
if (i < tokens_.size() && tokens_[i].type == TokenType::LESS) {
    // ...depth decremented to 0 for generic case
}
// For non-generic, depth is still 1 here:
bool result = (depth == 0 && ...);                   // always false for non-generic!

Bug
parseDefaultArm: do-while on exprs has same dead !exp check as before
ParserExpr.cpp:1827–1831
parseDefaultArm still uses the original do { exp = parseExpr(); if (!exp) break; ... } while (match(COMMA)) pattern. As in the previous version, parseExpr never returns null — it returns UnknownExprAST on failure. The if (!exp) break guard is dead code. If parsing makes no progress, this loop will spin consuming commas. The parseMatchArm was fixed to use progress tracking, but parseDefaultArm was not.

🟠 Potential infinite loops

Infinite loop risk
looksLikeAnonFunc: unmatched '(' loops to end of token stream
Parser.cpp:540–552
The inner while (i < tokens_.size() && parenDepth > 0) loop walks the raw token array looking for the closing ')'. The loop correctly checks i < tokens_.size(), so it won't access out of bounds — but if EOF is reached before the matching ')', it exits with parenDepth != 0 and returns false. That part is safe. The concern is structural: this path is taken on every single '(' token in expression position, potentially scanning large amounts of source. A deeply nested expression with a missing ')' will cause it to walk the entire remaining token stream on every call from parsePipelineStep. Not a hard infinite loop but a pathological O(N) scan per expression token in a long file.

Infinite loop risk
parseGenericParams: parseGenericParam failure stalls the loop
ParserDecl.cpp:654–663
The do { ... } while (!check(GREATER) && !isAtEnd()) loop calls parseGenericParam(). If parseGenericParam() returns nullptr without consuming a token (e.g. because the current token is not an IDENTIFIER), the loop re-enters with the same token. The loop has no position-tracking guard. If the offending token is not GREATER or EOF (e.g. a stray + operator), this loops forever.

Infinite loop risk
parseIntrinsicCallExpr value-arg loop: parseExpr makes no progress on bad token
ParserExpr.cpp:1105–1120
The while (!check(RPAREN) && !isAtEnd()) loop calls parseExpr(). If parseExpr() returns an UnknownExprAST without consuming any tokens (e.g. because the current token is a keyword that is not a valid expression start), the if (!arg) break check is dead and the loop re-enters. There is no position-tracking guard. The fix: record pos_ before calling parseExpr() and break if no progress was made.


🔵 Logic errors & new design issues

Logic error
AttributeRegistry: pending is a namespace-level static — shared across compilation units
AttributeRegistry.cpp:40
The anonymous-namespace std::vector<PendingAttr> pending is a program-wide static. Because registerAttribute() only pushes to pending without anything actually calling it (the constructor is empty), the vector will always be empty when setStringPool() runs. No attributes will ever be registered. The intended design requires something — typically a static initialiser somewhere or a call in the constructor — to call registerAttribute() for each known attribute (extern, packed, inline, etc.). That code appears to be missing entirely.

Logic error
IntrinsicRegistry: const_cast on a static array is undefined behaviour at runtime
IntrinsicRegistry.cpp:320
The kEntries array is declared const at namespace scope. setStringPool() uses const_cast<IntrinsicEntry&>(kEntries[i]) to write the id field. Writing through a const_cast of an originally-const object is undefined behaviour in C++, regardless of comments claiming safety. Some linkers place const arrays in read-only data segments — this will crash at runtime on those platforms. The fix: remove const from the declaration, or make id a mutable field.
// kEntries is declared 'const' at namespace scope — UB to write through const_cast
auto& entry = const_cast<IntrinsicEntry&>(kEntries[i]);
entry.id = pool.intern(entry.lucName);   // writes to const storage — UB

Logic error
parsePackageDecl result used after attachDoc moves pkgDoc, but the if-check is still dead
Parser.cpp (parse())
The previous report noted this. Looking at the new file: attachDoc(*pkgDecl, std::move(pkgDoc)) is still called unconditionally before if (pkgDecl). attachDoc takes the node by reference so pkgDecl itself is not moved — but if parsePackageDecl ever returns null (it currently can't from the new version), attachDoc(*pkgDecl, ...) would dereference null. The null check after attach is still dead.

Logic error
looksLikeAnonFunc: ~qualifier followed by '(' not handled — triggers false negative
Parser.cpp:522–609
The function only checks for TILDE as a first token when the current token is not LPAREN (the else if (check(TILDE)) branch at line 585 of the old file is now gone; the new implementation starts with if (!check(LPAREN)) return false). So ~async (x int) int { ... } at pipeline step position is no longer recognised as an anonymous function — looksLikeAnonFunc() returns false because the first token is TILDE, not LPAREN. This regression is a direct result of removing the old heuristic without adding a corresponding TILDE path to the new implementation.

Logic error
parseSwitchCase do-while: leading match(COMMA) on first iteration unchanged
ParserStmt.cpp (parseSwitchCase)
The previous report flagged the do { match(COMMA); ... } while (check(COMMA)) structure discarding a potential leading comma on the first iteration. Checking the new file: this code is unchanged. It is still possible to write case ,42: and have the comma silently consumed.

⚪ Remaining code quality
Incomplete fix
looksLikeAnonFunc: isPrimitiveTypeToken not used — qualifier loop still duplicates it
Parser.cpp:576–607
The new looksLikeAnonFunc() contains its own inline switch over all 22 primitive type tokens instead of calling isPrimitiveTypeToken(retStart). This is the same duplication that was fixed everywhere else. One more call site to update.

Incomplete fix
parseFuncType: nextIsType check still inlines isPrimitiveTypeToken
ParserType.cpp:554–559
The nextIsType computation now correctly calls Parser::isPrimitiveTypeToken(nextType) — that part is fixed. However the return-type-start check inside looksLikeAnonFunc (in Parser.cpp) still has its own inline 22-case switch, leaving one remaining copy.

Design issue
AttributeRegistry::setStringPool called with StringPool& but stored as raw pointer — no lifetime enforcement
AttributeRegistry.hpp:line holding stringPool*
The registry stores the pool as a raw StringPool*. If the StringPool is destroyed before the registry (which is a static singleton), any subsequent call to validateAttribute or lookup will dereference a dangling pointer. The same issue exists in IntrinsicRegistry. Both singletons should at minimum assert that stringPool is non-null before every use, and ideally be reset to nullptr when the pool goes out of scope.

Design issue
parseMatchArm: guard parseExpr() result used after "!exp" dead check
ParserExpr.cpp:1771–1774
The guard branch calls arm->guard = parseExpr() and then checks if (!arm->guard). Since parseExpr() always returns non-null, the null check will never fire. The guard expression slot will always be populated, even if parsing failed and produced an UnknownExprAST. This is a minor consistency issue — the check should use position tracking like the rest of the updated code.