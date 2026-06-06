#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

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
    LUC_LOG_TYPE_VERBOSE("parseFuncType: entering at line " << ts_.currentLoc().line()
                         << ", col " << ts_.currentLoc().column());
    
    // Log current token
    LUC_LOG_TYPE("parseFuncType: current token = '" << ts_.peek().value 
                 << "' (type=" << static_cast<int>(ts_.peek().type) 
                 << ") at line " << ts_.peek().line << ", col " << ts_.peek().column);
    
    SourceLocation loc = ts_.currentLoc();
    auto funcType = arena_.make<FuncTypeAST>();
    funcType->loc = loc;

    // Parse raw qualifiers and build bitmask
    std::vector<InternedString> rawQuals;
    uint32_t qualMask = 0;
    int qualifierCount = 0;
    while (ts_.check(TokenType::TILDE)) {
        LUC_LOG_TYPE_EXTREME("parseFuncType: found '~' at line " << ts_.peek().line 
                             << ", col " << ts_.peek().column);
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            LUC_LOG_TYPE("parseFuncType: ERROR - expected qualifier name after '~'");
            errorAt(DiagCode::E1003, "expected qualifier name after '~'");
            break;
        }
        InternedString q = pool_.intern(ts_.advance().value);
        rawQuals.push_back(q);
        std::string_view qstr = pool_.lookup(q);
        LUC_LOG_TYPE_EXTREME("parseFuncType: found qualifier '~" << qstr << "'");
        
        if (qstr == "async") qualMask |= QualifierBits::Async;
        else if (qstr == "nullable") qualMask |= QualifierBits::Nullable;
        else if (qstr == "parallel") qualMask |= QualifierBits::Parallel;
        qualifierCount++;
    }
    
    if (qualifierCount > 0) {
        LUC_LOG_TYPE_EXTREME("parseFuncType: found " << qualifierCount << " qualifiers");
    }
    
    auto qBuilder = arena_.makeBuilder<InternedString>();
    for (auto& q : rawQuals) qBuilder.push_back(std::move(q));
    funcType->rawQualifiers = qBuilder.build();
    funcType->qualifiers = qualMask;

    // Parameter groups: flat accumulation
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    int groupCount = 0;
    
    LUC_LOG_TYPE("parseFuncType: checking for '(' at line " << ts_.peek().line 
                 << ", col " << ts_.peek().column);
    
    if (!ts_.check(TokenType::LPAREN)) {
        LUC_LOG_TYPE("parseFuncType: ERROR - expected '(' for function type parameters, got '" 
                     << ts_.peek().value << "'");
        errorAt(DiagCode::E1001, "expected '(' for function type parameters");
        return arena_.make<UnknownTypeAST>();
    }
    
    while (ts_.check(TokenType::LPAREN)) {
        LUC_LOG_TYPE_EXTREME("parseFuncType: parsing parameter group " << groupCount + 1 
                             << " at line " << ts_.peek().line << ", col " << ts_.peek().column);
        ParamGroup group = parseParamGroup();
        groupSizes.push_back(group.size());
        LUC_LOG_TYPE_EXTREME("parseFuncType: group " << groupCount << " has " << group.size() << " parameters");
        
        for (size_t i = 0; i < group.size(); ++i) {
            allParams.push_back(std::move(const_cast<ParamPtr&>(group[i])));
        }
        groupCount++;
    }
    
    LUC_LOG_TYPE_VERBOSE("parseFuncType: parsed " << groupCount << " parameter groups with " 
                         << allParams.size() << " total parameters");

    // Log after parameter groups
    LUC_LOG_TYPE("parseFuncType: after parameter groups, next token: '" << ts_.peek().value 
                 << "' (type=" << static_cast<int>(ts_.peek().type)
                 << ") at line " << ts_.peek().line << ", col " << ts_.peek().column);
    
    auto paramsBuilder = arena_.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramsBuilder.push_back(std::move(p));
    funcType->sig.allParams = paramsBuilder.build();
    
    auto gsBuilder = arena_.makeBuilder<size_t>();
    for (auto& sz : groupSizes) gsBuilder.push_back(sz);
    funcType->sig.groupSizes = gsBuilder.build();

    // Return types
    if (ts_.match(TokenType::ARROW)) {
        LUC_LOG_TYPE_EXTREME("parseFuncType: found '->' at line " << ts_.peek().line
                             << ", col " << ts_.peek().column);
        funcType->sig.returnTypes = parseReturnList();
        LUC_LOG_TYPE_VERBOSE("parseFuncType: found " << funcType->sig.returnTypes.size() 
                             << " return type(s)");
    } else {
        LUC_LOG_TYPE_EXTREME("parseFuncType: no return types (void)");
    }

    LUC_LOG_TYPE_VERBOSE("parseFuncType: success");
    return funcType;
}