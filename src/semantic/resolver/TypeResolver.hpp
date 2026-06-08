/**
 * @file TypeResolver.hpp
 * @brief Resolves type annotations and handles type alias expansion.
 * 
 * ============================================================================
 * PHASE 2: TYPE RESOLUTION
 * ============================================================================
 * 
 * This pass resolves all type annotations in declarations and caches the
 * results on the AST nodes themselves.
 * 
 * ─── Resolution Strategy ───────────────────────────────────────────────────
 * 
 *   1. Primitive types: return as-is
 *   2. Named types: lookup in type namespace
 *      - If found and is a type alias: recursively resolve to underlying type
 *      - If found and is a struct/enum/trait: return selfType
 *      - If not found: error, return nullptr
 *   3. Composite types (nullable, result, array, ref, ptr): recursively resolve inner types
 *   4. Function types: resolve parameter and return types
 * 
 * ─── Caching ───────────────────────────────────────────────────────────────
 * 
 *   - Type aliases: resolved type stored in TypeAliasDeclAST::resolvedType
 *   - Struct/Enum/Trait: selfType cached for when type appears as value
 *   - Function return type: cached in FuncDeclAST::resolvedReturnType
 *   - Expression types: stored in ExprAST::resolvedType (during checking)
 * 
 * ─── Dependencies ──────────────────────────────────────────────────────────
 * 
 *   - Requires ScopeStack for type lookup (Phase 1 must have completed)
 *   - Called from SemanticAnalyzer::resolveTypes() after declaration collection
 * 
 * @see ScopeStack for type name lookup
 * @see SemanticAnalyzer::resolveTypes() for integration
 */

#pragma once

#include "ast/TypeAST.hpp"
#include "ast/DeclAST.hpp"
#include "semantic/scope/ScopeStack.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "ast/support/StringPool.hpp"

namespace luc {

/**
 * @brief Resolves type annotations and handles type alias expansion.
 * 
 * This class is responsible for resolving all type annotations after
 * declarations have been collected. It caches resolved types directly
 * on AST nodes.
 * 
 * ─── Usage ─────────────────────────────────────────────────────────────────
 * 
 *   TypeResolver resolver(ctx);
 *   
 *   // Resolve a type annotation
 *   TypeAST* resolved = resolver.resolve(varDecl->type);
 *   varDecl->valueType = resolved;  // Cache on ValueDeclAST
 *   
 *   // Resolve a type alias chain
 *   resolver.resolveTypeAlias(aliasDecl);
 *   
 *   // Resolve struct field types
 *   resolver.resolveStructFields(structDecl);
 */
class TypeResolver {
public:
    explicit TypeResolver(SemanticContext& ctx);
    
    // ========================================================================
    // Core Resolution
    // ========================================================================
    
    /**
     * @brief Resolves a type AST node to its semantic type.
     * 
     * This is the main entry point for type resolution.
     * 
     * @param type The type AST to resolve (may contain aliases)
     * @return TypeAST* The resolved type, or nullptr on error
     */
    TypeAST* resolve(TypeAST* type);
    
    /**
     * @brief Resolves a type alias to its ultimate underlying type.
     * 
     * Unwraps chains of type aliases (A = B, B = C, C = int) and caches
     * the result in TypeAliasDeclAST::resolvedType.
     * 
     * @param alias The type alias declaration
     * @return TypeAST* The ultimate underlying type, or nullptr on error
     */
    TypeAST* resolveTypeAlias(TypeAliasDeclAST* alias);
    
    // ========================================================================
    // Declaration Resolution
    // ========================================================================
    
    /**
     * @brief Resolves a variable's declared type.
     * 
     * Sets VarDeclAST::valueType to the resolved type.
     * 
     * @param var The variable declaration
     * @return TypeAST* The resolved type, or nullptr on error
     */
    TypeAST* resolveVarType(VarDeclAST* var);
    
    /**
     * @brief Resolves a function's signature.
     * 
     * Resolves all parameter types and return types.
     * Also caches the first return type in FuncDeclAST::resolvedReturnType.
     * 
     * @param func The function declaration
     * @return bool true on success
     */
    bool resolveFunctionSignature(FuncDeclAST* func);
    
    /**
     * @brief Resolves struct field types.
     * 
     * Resolves each field's type and sets FieldDeclAST::valueType.
     * Also creates selfType if not already created.
     * 
     * @param structDecl The struct declaration
     * @return bool true on success
     */
    bool resolveStructFields(StructDeclAST* structDecl);
    
    /**
     * @brief Resolves an enum (creates selfType, resolves nothing else).
     * 
     * @param enumDecl The enum declaration
     * @return bool true on success
     */
    bool resolveEnum(EnumDeclAST* enumDecl);
    
    /**
     * @brief Resolves a trait (creates selfType, resolves nothing else).
     * 
     * @param traitDecl The trait declaration
     * @return bool true on success
     */
    bool resolveTrait(TraitDeclAST* traitDecl);
    
    /**
     * @brief Resolves an impl block's target type and method signatures.
     * 
     * @param impl The impl declaration
     * @return bool true on success
     */
    bool resolveImpl(ImplDeclAST* impl);
    
    /**
     * @brief Resolves a from block's conversion entries.
     * 
     * @param from The from declaration
     * @return bool true on success
     */
    bool resolveFrom(FromDeclAST* from);
    
    // ========================================================================
    // Type Utilities
    // ========================================================================
    
    /**
     * @brief Checks if two types are structurally equal.
     * 
     * Resolves aliases before comparison.
     * 
     * @param a First type
     * @param b Second type
     * @return true if types are equal
     */
    bool typesEqual(TypeAST* a, TypeAST* b);
    
    /**
     * @brief Unwraps a type alias chain completely.
     * 
     * Follows NamedTypeAST references until a non-alias type is found.
     * 
     * @param type The starting type (may be NamedTypeAST)
     * @return TypeAST* The unwrapped type, or the original if not an alias
     */
    TypeAST* unwrapAlias(TypeAST* type);

private:
    SemanticContext& ctx_;
    
    // ========================================================================
    // Type Resolution Helpers
    // ========================================================================
    
    TypeAST* resolvePrimitive(PrimitiveTypeAST* prim);
    TypeAST* resolveNamed(NamedTypeAST* named);
    TypeAST* resolveNullable(NullableTypeAST* nullable);
    TypeAST* resolveResult(ResultTypeAST* result);
    TypeAST* resolveArray(ArrayTypeAST* array);
    TypeAST* resolveRef(RefTypeAST* ref);
    TypeAST* resolvePtr(PtrTypeAST* ptr);
    TypeAST* resolveFunc(FuncTypeAST* func);
    
    // ========================================================================
    // Parameter/Return Resolution
    // ========================================================================
    
    /**
     * @brief Resolves a parameter's type.
     * 
     * Sets ParamAST::valueType to the resolved type.
     * 
     * @param param The parameter
     * @return bool true on success
     */
    bool resolveParam(ParamAST* param);
    
    /**
     * @brief Resolves a list of return types.
     * 
     * @param returnTypes Span of return types to resolve (in-place)
     * @return bool true if all resolved successfully
     */
    bool resolveReturnTypes(ArenaSpan<TypeAST*>& returnTypes);
    
    // ========================================================================
    // Helpers
    // ========================================================================
    
    /**
     * @brief Ensures a type declaration has a selfType.
     */
    void ensureSelfType(TypeDeclAST* typeDecl);
};

} // namespace luc