/**
 * @file GenericParamParser.cpp
 * @brief Parses generic type parameter references (e.g., `T` in `struct Box<T>`).
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements parsing of generic type parameter references.
 * These are distinct from NamedTypeAST because they refer to a type variable
 * declared in the current scope, not a concrete user-defined type.
 * 
 * ## When is GenericParamTypeAST used?
 * 
 *   - In function signatures: `let identity<T> (v T) -> T`
 *   - In struct fields: `struct Box<T> { value T }`
 *   - In method signatures inside impl blocks: `impl Box<T> { get () -> T }`
 *   - In from entries: `from Wrapper<T> { (val T) -> Wrapper<T> }`
 *   - In type alias bodies: `type List<T> = [_, T]`
 * 
 * ## Grammar
 * 
 *   generic_param_type := IDENTIFIER
 * 
 * There are no generic arguments on a GenericParamTypeAST because it represents
 * the type parameter itself, not an instantiation of a generic type.
 * 
 * @see NamedParser.cpp for concrete named types
 * @see GenericParamParser.cpp (declaration side) for GenericParamAST
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

// ============================================================================
// Generic Parameter Type (Type Variable Reference)
// ============================================================================
// 
// parseGenericParamType() parses a reference to a generic type parameter.
// 
// Grammar: IDENTIFIER
// 
// Examples:
//   T
//   K
//   V
//   U
// 
// ─── Context Requirements ──────────────────────────────────────────────────
//   - The identifier must resolve to a GenericParamAST in the current scope
//   - This is checked during semantic analysis, not parsing
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at IDENTIFIER (parameter name)
// On exit:  positioned after the identifier
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
// - Missing identifier: returns UnknownTypeAST
// ============================================================================

TypePtr Parser::parseGenericParamType() {
    LUC_LOG_TYPE_VERBOSE("parseGenericParamType: entering at line " << ts_.currentLoc().line()
                         << ", col " << ts_.currentLoc().column());
    
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_TYPE("parseGenericParamType: ERROR - expected identifier");
        errorAt(DiagCode::E1003);
        return arena_.make<UnknownTypeAST>();
    }

    SourceLocation loc = ts_.currentLoc();
    Token nameToken = ts_.advance();
    LUC_LOG_TYPE("parseGenericParamType: consumed parameter name '" << nameToken.value 
                 << "' at line " << nameToken.line << ", col " << nameToken.column);
    
    auto* type = arena_.make<GenericParamTypeAST>(pool_.intern(nameToken.value));
    type->loc = loc;
    
    // Note: The declaration pointer and isPhantom flag will be set during semantic analysis
    // when the type parameter is resolved to its declaration.
    
    return type;
}