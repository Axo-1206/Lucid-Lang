#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

// ============================================================================
// Function Type
// ============================================================================
// 
// parseFuncType() parses a function type annotation with full currying support.
// 
// Grammar: [ qualifier_list ] param_group { param_group } [ '->' return_list ]
// 
// Examples:
//   (x int) -> int
//   ~async (url string) -> string
//   (a int)(b int) -> int                    (curried → nested FuncTypeAST)
//   (src string) -> (int, string)            (multiple returns)
//   () -> int                                (zero parameters)
// 
// ─── Behavior ──────────────────────────────────────────────────────────────
//   - If there are multiple parameter groups, the function recurses to parse
//     the remaining groups as the return type.
//   - Returns the root FuncTypeAST (outermost group).
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at '(' or '~' (qualifier)
// On exit:  positioned after return list (or after last param group if void)
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
//   - Missing '(' after qualifiers: reports error, returns UnknownTypeAST
//   - Empty parameter list `()` is allowed (zero parameters)
//   - Return list errors reported by parseReturnList()
// ============================================================================

TypePtr Parser::parseFuncType() {
    LUC_LOG_TYPE_VERBOSE("parseFuncType: entering at line " << ts_.currentLoc().line()
                         << ", col " << ts_.currentLoc().column());
    
    SourceLocation loc = ts_.currentLoc();
    auto funcType = arena_.make<FuncTypeAST>();
    funcType->loc = loc;

    // Parse raw qualifiers and build bitmask
    QualifierSet quals = parseQualifiers();
    
    auto qBuilder = arena_.makeBuilder<InternedString>();
    for (auto& q : quals.raw) qBuilder.push_back(q);
    funcType->rawQualifiers = qBuilder.build();
    funcType->qualifiers = quals.bitmask;

    // Parse first parameter group (required)
    if (!ts_.check(TokenType::LPAREN)) {
        LUC_LOG_TYPE("parseFuncType: ERROR - expected '(' for function type parameters, got '" 
                     << ts_.peek().value << "'");
        errorAt(DiagCode::E1001, "expected '(' for function type parameters");
        return arena_.make<UnknownTypeAST>();
    }
    
    LUC_LOG_TYPE_EXTREME("parseFuncType: parsing first parameter group");
    
    // Parse '(' param_list ')'
    ts_.consume(TokenType::LPAREN, "expected '(' to start parameter group");
    std::vector<ParamPtr> params = parseParamList();
    ts_.consume(TokenType::RPAREN, "expected ')' to close parameter group");
    
    LUC_LOG_TYPE_EXTREME("parseFuncType: first group has " << params.size() << " parameters");
    
    // Store parameters for this group
    auto paramsBuilder = arena_.makeBuilder<ParamPtr>();
    for (auto* p : params) paramsBuilder.push_back(p);
    funcType->params = paramsBuilder.build();

    // Check for additional parameter groups (currying)
    // If another '(' is found, parse the rest recursively as the return type
    if (ts_.check(TokenType::LPAREN)) {
        LUC_LOG_TYPE_EXTREME("parseFuncType: found additional parameter group - currying");
        // Parse the curried function type (remaining groups + return types)
        TypePtr curriedType = parseFuncType();
        if (curriedType && !curriedType->isa<UnknownTypeAST>()) {
            // The return type becomes the curried function type
            auto retBuilder = arena_.makeBuilder<TypePtr>();
            retBuilder.push_back(curriedType);
            funcType->returnTypes = retBuilder.build();
        }
        LUC_LOG_TYPE_VERBOSE("parseFuncType: curried function with " 
                             << funcType->params.size() << " parameters");
        return funcType;
    }

    // No currying - parse return types if present
    if (ts_.match(TokenType::ARROW)) {
        LUC_LOG_TYPE_EXTREME("parseFuncType: found '->'");
        funcType->returnTypes = parseReturnList();
        LUC_LOG_TYPE_VERBOSE("parseFuncType: found " << funcType->returnTypes.size() 
                             << " return type(s)");
    } else {
        LUC_LOG_TYPE_EXTREME("parseFuncType: no return types (void)");
        // Void function - returnTypes remains empty
    }

    LUC_LOG_TYPE_VERBOSE("parseFuncType: success");
    return funcType;
}