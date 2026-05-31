#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Function Type
// ============================================================================
// 
// parseFuncType() parses a function type annotation.
// 
// Grammar: [ qualifier_list ] param_group { param_group } [ '->' return_list ]
// 
// Examples:
//   (x int) -> int
//   ~async (url string) -> string
//   (a int)(b int) -> int                    (curried)
//   (src string) -> (int, string)            (multiple returns)
//   () -> int                                (zero parameters)
// 
// ─── Qualifier Handling ───────────────────────────────────────────────────
//   - Qualifiers stored raw in rawQualifiers (InternedString)
//   - Validation and bitmask computation deferred to semantic phase
//   - `~parallel` does NOT affect type equality; `~async`/`~nullable` do
// 
// ─── Parameter Groups (Flat Representation) ───────────────────────────────
//   - Parameters are flattened into `allParams` vector
//   - `groupSizes` records how many parameters belong to each curry group
//   - Example: `(a int)(b int)(c int)` → allParams=[a,b,c], groupSizes=[1,1,1]
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at first '(' or '~' (qualifier)
// On exit:  positioned after return list (or after last param group if void)
// 
// ─── Loop Safety ──────────────────────────────────────────────────────────
//   - Parameter group loop continues while '(' is found
//   - parseParamGroup() guarantees progress
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
//   - Missing '(' after qualifiers: reports error, returns UnknownTypeAST
//   - Empty parameter list `()` is allowed (zero parameters)
//   - Return list errors reported by parseReturnList()
// ============================================================================

TypePtr Parser::parseFuncType() {
    SourceLocation loc = ts_.currentLoc();
    auto funcType = arena_.make<FuncTypeAST>();
    funcType->loc = loc;

    // Parse raw qualifiers and build bitmask
    std::vector<InternedString> rawQuals;
    uint32_t qualMask = 0;
    while (ts_.check(TokenType::TILDE)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        InternedString q = pool_.intern(ts_.advance().value);
        rawQuals.push_back(q);
        std::string_view qstr = pool_.lookup(q);
        if (qstr == "async") qualMask |= QualifierBits::Async;
        else if (qstr == "nullable") qualMask |= QualifierBits::Nullable;
        else if (qstr == "parallel") qualMask |= QualifierBits::Parallel;
        // Other qualifiers are ignored here; semantic pass will report errors
    }
    auto qBuilder = arena_.makeBuilder<InternedString>();
    for (auto& q : rawQuals) qBuilder.push_back(std::move(q));
    funcType->rawQualifiers = qBuilder.build();
    funcType->qualifiers = qualMask;

    // Parameter groups: flat accumulation
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    
    if (!ts_.check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' for function type parameters");
        return arena_.make<UnknownTypeAST>();
    }
    
    while (ts_.check(TokenType::LPAREN)) {
        ParamGroup group = parseParamGroup();
        groupSizes.push_back(group.size());
        for (size_t i = 0; i < group.size(); ++i) {
            allParams.push_back(std::move(const_cast<ParamPtr&>(group[i])));
        }
    }
    
    auto paramsBuilder = arena_.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramsBuilder.push_back(std::move(p));
    funcType->sig.allParams = paramsBuilder.build();
    
    auto gsBuilder = arena_.makeBuilder<size_t>();
    for (auto& sz : groupSizes) gsBuilder.push_back(sz);
    funcType->sig.groupSizes = gsBuilder.build();

    // Return types
    if (ts_.match(TokenType::ARROW)) {
        funcType->sig.returnTypes = parseReturnList();
    }

    return funcType;
}