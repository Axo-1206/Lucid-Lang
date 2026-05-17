/**
 * @file TypeResolver.cpp
 * @responsibility Implements Phase 2a of semantic analysis: resolving type annotations.
 *
 * This file contains the TypeResolver class implementation, an ASTVisitor that
 * walks type AST nodes and validates that every referenced type name exists
 * in the symbol table. It handles:
 *   - Primitive types (int, float, bool, etc.) – trivially resolved.
 *   - Named types (structs, enums, traits, type aliases) – symbol table lookup.
 *   - Nullable, array, reference, pointer, and function types – recursive resolution.
 *   - Generic parameters – detection via a stack of generic parameter lists.
 *   - Substitution of concrete types for generic parameters during instantiation.
 *
 * The resolver maintains two stacks:
 *   - genericParamsStack_ – lists of generic parameters from enclosing declarations.
 *   - substitutionMapStack_ – maps from generic parameter names to concrete types.
 *
 * @related
 *   - TypeResolver.hpp – class declaration
 *   - SymbolTable.hpp – for name lookup
 *   - SemanticAnalyzer.cpp – orchestrates the resolution phase
 *   - NameMangler.hpp – for generating mangled names (for from‑entries)
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * NAVIGATION – Functions in this file (in order of appearance)
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * ██ Constructor & Entry Point
 *   TypeResolver::TypeResolver()      – initialises symbol table, diagnostic engine,
 *                                       StringPool, and ASTArena.
 *   resolveType()                     – main entry point for resolving a type node.
 *
 * ██ Generic Parameter & Substitution Stacks
 *   pushGenericParams()               – pushes a generic parameter list onto the stack.
 *   popGenericParams()                – pops the topmost generic parameter list.
 *   isGenericParam()                  – checks if a name is a generic parameter.
 *   pushSubstitutionMap()             – pushes a substitution map onto the stack.
 *   popSubstitutionMap()              – pops the topmost substitution map.
 *   lookupSubstitution()              – looks up a name in the substitution stack.
 *
 * ██ Helper Methods
 *   resolveStructFields()             – resolves all field types in a struct.
 *   resolveFunctionType()             – resolves all types inside a function signature.
 *   getFunctionReturnTypes()          – returns a vector of resolved return types.
 *   getFunctionReturnType()           – convenience for single return type.
 *
 * ██ Type Node Visitors
 *   visit(PrimitiveTypeAST)           – resolves primitive types (no lookup).
 *   visit(NamedTypeAST)               – resolves user‑defined types (struct, enum, etc.).
 *   visit(NullableTypeAST)            – resolves inner type.
 *   visit(FixedArrayTypeAST)          – resolves element type, checks size > 0.
 *   visit(SliceTypeAST)               – resolves element type.
 *   visit(DynamicArrayTypeAST)        – resolves element type.
 *   visit(RefTypeAST)                 – resolves inner type.
 *   visit(PtrTypeAST)                 – resolves inner type, checks @extern context.
 *   visit(FuncTypeAST)                – resolves qualifiers, parameter and return types.
 *
 * ██ Declaration Node Visitors
 *   visit(FuncDeclAST)                – resolves function signature, updates symbol.
 *   visit(VarDeclAST)                 – resolves variable type annotation.
 *   visit(StructDeclAST)              – resolves fields, creates selfType.
 *   visit(ImplDeclAST)                – resolves methods, builds substitution map.
 *   visit(MethodDeclAST)              – resolves method signature.
 *   visit(FromDeclAST)                – resolves conversion entries, registers symbols.
 *   visit(TypeAliasDeclAST)           – resolves aliased type.
 *   visit(TraitDeclAST)               – resolves trait methods.
 *   visit(TraitMethodAST)             – resolves trait method signature.
 *   visit(TraitRefAST)                – resolves trait reference and its generic args.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * @note The resolver assumes that Phase 1 (SemanticCollector) has already
 *       populated the symbol table with all declarations. If a type name is
 *       not found, an error is reported and the resolution returns nullptr.
 *       Generic parameters are resolved only when the current generic context
 *       (via pushGenericParams) includes them; otherwise they are treated as
 *       ordinary (and likely undeclared) names.
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "header/TypeResolver.hpp"
#include "ast/support/StringPool.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "header/SymbolTable.hpp"
#include "registry/QualifierRegistry.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"
#include "header/NameMangler.hpp"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// TypeResolver (constructor)  — Initializes the TypeResolver with core dependencies
//
// Binds the active symbol table to allow looking up global type names and the
// diagnostic engine to log error messages directly during resolution.
// ─────────────────────────────────────────────────────────────────────────────
TypeResolver::TypeResolver(SymbolTable& symbols, DiagnosticEngine& dc, StringPool& pool, ASTArena& arena)
    : _symbols(symbols), _dc(dc), _pool(pool), _arena(arena) {
    LUC_LOG_SEMANTIC("TypeResolver constructed");
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveType  —  Main entry point for type resolution.
//
// Accepts a raw TypeAST node (from the parser) and resolves it to a validated
// TypeAST. Resolution includes:
//   - Checking that named types exist in the symbol table.
//   - Resolving generic arguments recursively.
//   - Substituting generic parameters with concrete types when a substitution
//     map is active.
//   - Unwrapping type aliases to their underlying types.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Called during Phase 2a (resolveTypes) for every type annotation in the AST.
// Ensures that before type checking (Phase 3), every type node has been
// verified and its resolvedType field is set (or the node itself is the
// resolved type).
//
// ─── Algorithm ───────────────────────────────────────────────────────────────
//   1. If typeNode is nullptr → return nullptr.
//   2. Set _resolved = nullptr.
//   3. Invoke typeNode->accept(*this) to dispatch to the appropriate visitor.
//   4. After visitation, _resolved holds the resolved node (or nullptr on error).
//   5. Log success/failure and return _resolved.
//
// ─── Cases Covered ───────────────────────────────────────────────────────────
//   - Primitive types (int, float, bool, etc.) → returns the node unchanged.
//   - Named types (struct, enum, trait, type alias) → resolves symbol,
//     unwraps aliases, resolves generic arguments.
//   - Nullable types → resolves inner type.
//   - Array types (fixed, slice, dynamic) → resolves element type.
//   - Reference (&T) and pointer (*T) → resolves inner type.
//   - Function types → resolves parameter and return types.
//   - All other TypeAST subclasses via their visit methods.
//
// ─── What is NOT covered ─────────────────────────────────────────────────────
//   - Type checking (compatibility, assignability) – handled by TypeChecker.
//   - Semantic restrictions on where a type can appear (e.g., raw pointers
//     only in @extern) – enforced in Phase 3.
//   - Generic parameter substitution for declarations (e.g., struct Box<T>
//     where T is a generic param) – handled by marking isGenericParam.
//
// ─── Return Value ───────────────────────────────────────────────────────────
//   Returns the resolved TypeAST (usually the same node, but may be a different
//   node for type aliases or substitutions). Returns nullptr if resolution
//   fails (error already reported via DiagnosticEngine).
// ─────────────────────────────────────────────────────────────────────────────
TypeAST* TypeResolver::resolveType(TypeAST* typeNode) {
    LUC_LOG_SEMANTIC_VERBOSE("resolveType: starting with kind=" 
                        << (typeNode ? LucDebug::kindToString(typeNode->kind) : "null"));
    
    if (!typeNode) {
        LUC_LOG_SEMANTIC("resolveType: null type node -> nullptr");
        return nullptr;
    }
    
    _resolved = nullptr;
    typeNode->accept(*this);
    
    bool success = (_resolved != nullptr);
    LUC_LOG_SEMANTIC_VERBOSE("resolveType: " << (success ? "success" : "failed"));
    
    return _resolved;
}

// ─────────────────────────────────────────────────────────────────────────────
// Generic Parameter & Substitution Stacks
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// pushGenericParams  —  Pushes a list of generic parameters onto the stack.
//
// Called when entering a generic declaration (struct, trait, impl, function,
// or type alias) that introduces new type parameters. The resolver uses the
// stack to determine whether a NamedTypeAST refers to a generic parameter
// rather than a concrete type.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Allows nested generic contexts (e.g., a generic struct inside a generic
// function) to correctly resolve names like `T` that may refer to different
// parameters at different nesting levels. The innermost (most recent) list
// is searched first.
//
// ─── Parameters ─────────────────────────────────────────────────────────────
//   params – Pointer to a vector of GenericParamAST owned by the AST node.
//            Must remain valid for the duration the stack entry is active.
//            May be nullptr (in which case a null entry is pushed, which
//            is ignored during lookup).
//
// ─── Stack Behaviour ─────────────────────────────────────────────────────────
//   - A new entry is added to the back of the vector (the "top" of the stack).
//   - isGenericParam searches from the back (innermost) to front (outermost).
//   - Each call to pushGenericParams must be matched with a call to
//     popGenericParams when leaving the generic context.
//
// ─── Usage Pattern ───────────────────────────────────────────────────────────
//   void visit(FuncDeclAST& node) {
//       pushGenericParams(&node.genericParams);
//       // ... resolve types inside the function ...
//       popGenericParams();
//   }
//
// ─── Error Handling ─────────────────────────────────────────────────────────
//   No errors; the stack is unbounded (limited only by recursion depth of
//   generic nesting).
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::pushGenericParams(const std::vector<GenericParamPtr>* params) {
    genericParamsStack_.push_back(params);
    LUC_LOG_SEMANTIC_EXTREME("pushGenericParams: depth=" << genericParamsStack_.size()
                            << ", params=" << (params ? std::to_string(params->size()) : "null"));
}

// ─────────────────────────────────────────────────────────────────────────────
// popGenericParams  —  Removes the topmost generic parameter list from the stack.
//
// Called when exiting a generic declaration. Must be called exactly once for
// each pushGenericParams. If the stack is empty, logs an error (developer bug).
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Maintains the correct stack depth for nested generic contexts. Forgetting
// to pop would cause subsequent type resolution to incorrectly see stale
// generic parameters.
//
// ─── Stack Behaviour ─────────────────────────────────────────────────────────
//   - Removes the last element of genericParamsStack_.
//   - Does not deallocate the pointed‑to vector (owned by the AST node).
//
// ─── Error Handling ─────────────────────────────────────────────────────────
//   - If the stack is already empty, logs an error but does nothing else.
//     This indicates a mismatch in push/pop calls, which is a programming
//     error in the resolver.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::popGenericParams() {
    if (!genericParamsStack_.empty()) {
        genericParamsStack_.pop_back();
        LUC_LOG_SEMANTIC_EXTREME("popGenericParams: depth=" << genericParamsStack_.size());
    } else {
        LUC_LOG_SEMANTIC("popGenericParams: ERROR - stack empty");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// isGenericParam  —  Determines whether a name is a generic type parameter in
//                    the current context.
//
// Searches the generic parameter stack (innermost first) for a parameter whose
// name matches the given InternedString. Used by visit(NamedTypeAST) to decide
// whether to treat the name as a generic parameter or to look it up in the
// symbol table.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Enables the resolver to distinguish between:
//   - Generic type parameters (e.g., `T` in `struct Box<T>`)
//   - Concrete type names (e.g., `int`, `Circle`, `Vec2`)
//   - Type aliases (unwrapped after resolution)
//
// ─── Algorithm ───────────────────────────────────────────────────────────────
//   1. Iterate over genericParamsStack_ from the back (innermost) to front.
//   2. For each entry (a vector of GenericParamPtr), check each parameter.
//   3. If a parameter with the given name exists, return true.
//   4. If none found, return false.
//
// ─── Cases Covered (returns true) ────────────────────────────────────────────
//   - The name matches a generic parameter declared in the current function.
//   - The name matches a generic parameter declared in an enclosing generic
//     declaration (e.g., outer struct when inside an inner generic function).
//
// ─── What is NOT covered (returns false) ─────────────────────────────────────
//   - The name is a concrete type (struct, enum, primitive).
//   - The name is a type alias (resolved by unwrapping, not as generic param).
//   - No active generic context (stack is empty).
//
// ─── Relation with substitution ──────────────────────────────────────────────
//   Even if isGenericParam returns true, the name may be replaced by a concrete
//   type via lookupSubstitution (when the generic parameter is instantiated).
//   The caller (visit(NamedTypeAST)) checks substitution first, then falls back
//   to marking the node as a generic parameter.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeResolver::isGenericParam(InternedString name) const {
    // Search from innermost (back) to outermost (front)
    for (auto it = genericParamsStack_.rbegin(); it != genericParamsStack_.rend(); ++it) {
        const auto* params = *it;
        if (!params) continue;
        for (const auto& gp : *params) {
            if (gp && gp->name == name) {
                LUC_LOG_SEMANTIC_EXTREME("isGenericParam: found " << _pool.lookup(name) << " at depth "
                                         << (genericParamsStack_.rend() - it - 1));
                return true;
            }
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveGenericParamConstraints  —  Validates that trait names in generic
//                                    parameter constraints exist in the symbol table.
//
// Called during resolution of a generic declaration (struct, trait, function,
// impl, or type alias) before the generic parameters are pushed onto the stack.
// This function only checks that each constraint name resolves to a declared
// trait; it does NOT check that concrete type arguments satisfy those constraints
// (that is done later in satisfiesConstraints).
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Ensures that constraint trait names are valid and have been declared.
// Prevents later errors when checking that a concrete type implements an
// undeclared trait.
//
// ─── Algorithm ───────────────────────────────────────────────────────────────
//   1. For each trait name in gp.constraints:
//        - Look up the name in the symbol table.
//        - If not found or the symbol is not a trait, report an error.
//   2. No storage; the trait names remain in the GenericParamAST for later use.
//
// ─── Error Reporting ─────────────────────────────────────────────────────────
//   Reports an error for any constraint trait that does not exist or is not a trait.
//   The error uses the generic parameter's source location.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::resolveGenericParamConstraints(GenericParamAST& gp) {
    for (auto traitName : gp.constraints) {
        Symbol* sym = _symbols.lookup(traitName);
        if (!sym || sym->kind != SymbolKind::Trait) {
            std::string_view traitStr = _pool.lookup(traitName);
            _dc.error(DiagnosticCategory::Semantic, gp.loc, DiagCode::E3001,
                      "trait '" + std::string(traitStr) + "' not found");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// satisfiesConstraints  —  Checks whether a concrete type argument implements
//                          all required traits listed in a generic constraint.
//
// Called during instantiation of a generic struct (e.g., `Box<int>`) to verify
// that each concrete type argument satisfies the constraints of the corresponding
// generic parameter. Uses the `_structTraits` map built by SemanticCollector,
// which records which traits each struct implements via `impl` blocks.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Enforces generic constraints as defined in the grammar (e.g., `T : Drawable`).
// Without this check, a generic struct could be instantiated with a type that
// does not implement the required methods, leading to uncheckable method calls.
//
// ─── Algorithm ───────────────────────────────────────────────────────────────
//   1. If requiredTraits is empty → return true (no constraints).
//   2. If the concrete type is not a named struct → return false (only named
//      structs can implement traits).
//   3. Look up the struct's name in the `_structTraits` map.
//   4. For each required trait name, check that it appears in the struct's
//      implemented traits list.
//   5. Return true if all required traits are found, false otherwise.
//
// ─── Cases Covered (returns true) ────────────────────────────────────────────
//   - No constraints.
//   - Concrete type is a struct that implements all required traits.
//
// ─── What is NOT covered (returns false) ─────────────────────────────────────
//   - Concrete type is not a named struct (e.g., primitive, array, function).
//   - Struct does not implement one or more required traits.
//   - `_structTraits` map is not provided (nullptr) → returns false.
//
// ─── Dependencies ────────────────────────────────────────────────────────────
//   Requires `_structTraits` to be populated via setStructTraits() before
//   any constraint checking is performed.
// ─────────────────────────────────────────────────────────────────────────────
bool TypeResolver::satisfiesConstraints(TypeAST* type, const std::vector<InternedString>& requiredTraits) const {
    if (requiredTraits.empty()) return true;
    if (!type->isa<NamedTypeAST>()) return false; // only named structs can implement traits
    auto* named = type->as<NamedTypeAST>();
    if (!_structTraits) return false;
    auto it = _structTraits->find(named->name);
    if (it == _structTraits->end()) return false;
    const auto& implemented = it->second;
    for (InternedString req : requiredTraits) {
        bool found = false;
        for (InternedString impl : implemented) {
            if (impl == req) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// pushSubstitutionMap  —  Pushes a substitution map onto the stack.
//
// When instantiating a generic type with concrete arguments (e.g., `Box<int>`),
// the resolver builds a mapping from generic parameter names (e.g., `"T"`) to
// the concrete types provided (e.g., `int`). This mapping is pushed onto a stack
// so that while resolving the body of the generic declaration, any reference
// to the parameter name is replaced by the concrete type.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Enables nested generic instantiations (e.g., `Box<Pair<int, float>>`) by
// maintaining a stack of substitution maps. The innermost map (most recent
// push) takes precedence, allowing outer maps to be visible but overridden
// by inner instantiations for the same parameter name if necessary.
//
// ─── Parameters ─────────────────────────────────────────────────────────────
//   map – Pointer to an unordered_map from InternedString (generic param name)
//         to the concrete TypeAST. The map is owned by the caller (usually a
//         local variable in visit(ImplDeclAST) or similar). Must remain valid
//         for the duration the stack entry is active.
//
// ─── Stack Behaviour ─────────────────────────────────────────────────────────
//   - The map is added to the back of substitutionMapStack_ (the "top").
//   - lookupSubstitution searches from back (innermost) to front (outermost).
//   - Each pushSubstitutionMap must be matched with a call to popSubstitutionMap.
//
// ─── Usage Pattern ───────────────────────────────────────────────────────────
//   void visit(ImplDeclAST& node) {
//       std::unordered_map<InternedString, TypeAST*> localSubstitutionMap;
//       // ... build map ...
//       pushSubstitutionMap(&localSubstitutionMap);
//       // ... resolve method signatures that may refer to T ...
//       popSubstitutionMap();
//   }
//
// ─── Error Handling ─────────────────────────────────────────────────────────
//   No errors; the stack is unbounded (limited by nesting depth of generic
//   instantiations). The caller is responsible for ensuring the map outlives
//   the stack entry.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::pushSubstitutionMap(const std::unordered_map<InternedString, TypeAST*>* map) {
    substitutionMapStack_.push_back(map);
    LUC_LOG_SEMANTIC_EXTREME("pushSubstitutionMap: depth=" << substitutionMapStack_.size()
                            << ", map=" << (map ? std::to_string(map->size()) : "null"));
}

// ─────────────────────────────────────────────────────────────────────────────
// popSubstitutionMap  —  Removes the topmost substitution map from the stack.
//
// Called after finishing resolution of a generic instantiation context.
// Must be called exactly once for each pushSubstitutionMap. If the stack is
// empty, logs an error (developer bug).
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Restores the previous substitution context, ensuring that outer generic
// parameters are visible again after exiting an inner instantiation.
//
// ─── Stack Behaviour ─────────────────────────────────────────────────────────
//   - Removes the last element of substitutionMapStack_.
//   - Does not deallocate the pointed‑to map (owned by the caller).
//
// ─── Error Handling ─────────────────────────────────────────────────────────
//   - If the stack is already empty, logs an error but does nothing else.
//     This indicates a mismatch in push/pop calls, which is a programming
//     error in the resolver.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::popSubstitutionMap() {
    if (!substitutionMapStack_.empty()) {
        substitutionMapStack_.pop_back();
        LUC_LOG_SEMANTIC_EXTREME("popSubstitutionMap: depth=" << substitutionMapStack_.size());
    } else {
        LUC_LOG_SEMANTIC("popSubstitutionMap: ERROR - stack empty");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// lookupSubstitution  —  Looks up a name in the substitution map stack.
//
// Given a generic parameter name (e.g., `"T"`), searches the substitution map
// stack from innermost to outermost for a concrete type binding. If found,
// returns the concrete TypeAST; otherwise returns nullptr.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Used during resolution of NamedTypeAST when the name is recognised as a
// generic parameter (isGenericParam returns true). The concrete type from the
// innermost matching substitution overrides the generic parameter, effectively
// instantiating the generic with the provided type argument.
//
// ─── Algorithm ───────────────────────────────────────────────────────────────
//   1. Iterate over substitutionMapStack_ from the back (innermost) to front.
//   2. For each map entry, perform a lookup using the given name.
//   3. If found, return the associated TypeAST*.
//   4. If no map contains the name, return nullptr.
//
// ─── Cases Covered (returns non‑null) ────────────────────────────────────────
//   - The name is a generic parameter of the current instantiation and a
//     concrete type has been provided (e.g., `T` → `int` in `Box<int>`).
//   - The name is a generic parameter of an outer instantiation and the inner
//     instantiation does not override it.
//
// ─── What is NOT covered (returns nullptr) ───────────────────────────────────
//   - No substitution map is active (stack empty).
//   - The name is not found in any map on the stack.
//   - The name is a generic parameter but no substitution was provided
//     (e.g., inside the generic definition itself, where T remains abstract).
//
// ─── Return Value ───────────────────────────────────────────────────────────
//   Returns a pointer to the concrete TypeAST (owned by the AST arena).
//   The caller must not delete the returned node.
// ─────────────────────────────────────────────────────────────────────────────
TypeAST* TypeResolver::lookupSubstitution(InternedString name) const {
    // Search from innermost (back) to outermost (front)
    for (auto it = substitutionMapStack_.rbegin(); it != substitutionMapStack_.rend(); ++it) {
        const auto* map = *it;
        if (!map) continue;
        auto found = map->find(name);
        if (found != map->end()) {
            LUC_LOG_SEMANTIC_EXTREME("lookupSubstitution: found " << _pool.lookup(name) 
                                     << " at depth " << (substitutionMapStack_.rend() - it - 1));
            return found->second;
        }
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper Methods
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// resolveStructFields  —  Resolves all field types inside a struct declaration.
//
// Walks the list of fields of a StructDeclAST and resolves the type annotation
// of each field. This must be done after the struct's generic parameters are
// known, because field types may refer to those parameters (e.g., `value T`).
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Ensures that every field type in a struct is a valid, fully resolved type.
// Called during Phase 2a from visit(StructDeclAST) before the struct's selfType
// is registered in the symbol table. Also used when resolving struct fields
// in other contexts (e.g., re‑resolution after generic substitution – though
// currently only called once per struct).
//
// ─── Algorithm ───────────────────────────────────────────────────────────────
//   1. Push the struct's own generic parameters onto the generic parameter stack
//      (so that `T` inside field types is recognised as a generic param).
//   2. For each field in node.fields:
//        - If the field has a type annotation, call resolveType() on it.
//        - If resolution fails, log an error (but continue resolving others).
//   3. Pop the generic parameters stack to restore the previous context.
//
// ─── Cases Covered ───────────────────────────────────────────────────────────
//   - Non‑generic struct: field types are resolved normally (no generic stack).
//   - Generic struct: field types that refer to generic parameters (e.g., `T`)
//     are marked as `isGenericParam = true` and not looked up in the symbol table.
//   - Nested generic structs (struct inside generic function) – the generic
//     parameters stack already contains outer parameters; this function pushes
//     the struct's own parameters on top, allowing both to be visible.
//   - Field types that are type aliases or other named types – resolved recursively.
//
// ─── What is NOT covered ─────────────────────────────────────────────────────
//   - Default values of fields (expressions) – handled in Phase 3.
//   - Semantic checks (duplicate field names, field visibility) – handled
//     by the semantic pass (Phase 3), not by type resolution.
//   - Substitution of concrete types for generic parameters during instantiation
//     (handled by ImplDeclAST resolution with substitution maps).
//
// ─── Note ────────────────────────────────────────────────────────────────────
//   This function does not resolve the struct's selfType; that is done by the
//   caller (visit(StructDeclAST)) after field resolution, because selfType is
//   a NamedTypeAST that refers to the struct itself, not to its generic parameters.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::resolveStructFields(StructDeclAST& node) {
    pushGenericParams(&node.genericParams);
    
    for (auto& field : node.fields) {
        if (field && field->type) {
            TypeAST* resolvedFieldType = resolveType(field->type.get());
            if (!resolvedFieldType) {
                LUC_LOG_SEMANTIC("\tERROR: failed to resolve field type for '" 
                               << _pool.lookup(field->name) << "' in struct '" 
                               << _pool.lookup(node.name) << "'");
            }
        }
    }
    
    popGenericParams();
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveFunctionType  —  Resolves all parameter and return types inside a
//                         function type.
//
// Given a FuncTypeAST, walks its parameter groups and return types, resolving
// each type annotation. This is the core routine for validating function
// signatures, whether they appear in a declaration, a function type annotation,
// a trait method, or an impl method.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Ensures that every type within a function signature (parameter types and
// return types) is a valid, fully resolved type. Called during Phase 2a from
// visit(FuncTypeAST), and also used by helper functions when synthesising
// temporary FuncTypeAST nodes.
//
// ─── Algorithm ───────────────────────────────────────────────────────────────
//   1. For each parameter group (curry level):
//        - For each parameter in the group:
//            - If the parameter has a type annotation, call resolveType() on it.
//   2. For each return type in the returnTypes vector:
//        - If the return type is non‑null, call resolveType() on it.
//
// ─── Cases Covered ───────────────────────────────────────────────────────────
//   - Regular function: one parameter group, optional return type.
//   - Curried function: multiple parameter groups.
//   - Void function: empty returnTypes vector.
//   - Multiple return values: returnTypes vector with more than one element.
//   - Parameters with generic types (e.g., `(x T)`) – resolved within the
//     current generic context (genericParams stack already set by caller).
//   - Function types nested inside other types (e.g., `let callback (int) string`).
//
// ─── What is NOT covered ─────────────────────────────────────────────────────
//   - Qualifier resolution (~async, ~nullable, ~parallel) – handled by the
//     visit(FuncTypeAST) method before calling this function.
//   - Semantic checks (e.g., duplicate parameter names) – handled in Phase 3.
//   - Default parameter values – Luc does not have them.
//   - Variadic parameters – the type is already resolved; this function treats
//     them like any other parameter type.
//
// ─── Note ────────────────────────────────────────────────────────────────────
//   This function does not modify the generic parameter stack. The caller is
//   responsible for ensuring the correct generic context is active before
//   calling resolveFunctionType.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::resolveFunctionType(const FuncTypeAST& type) {
    for (auto& group : type.sig.paramGroups) {
        for (auto& param : group) {
            if (param && param->type) {
                resolveType(param->type.get());
            }
        }
    }
    for (auto& retType : type.sig.returnTypes) {
        if (retType) {
            resolveType(retType.get());
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// getFunctionReturnTypes  —  Returns a vector of resolved return types for a
//                            function type.
//
// Given a FuncTypeAST, resolves each return type (if not already resolved) and
// returns a vector of the resulting TypeAST pointers. The order of the vector
// matches the order in the signature.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Provides a convenient way to obtain the fully resolved return types of a
// function for use in semantic checking (e.g., matching return statements,
// checking call expressions, unifying branches). The vector may be empty for
// void functions.
//
// ─── Algorithm ───────────────────────────────────────────────────────────────
//   1. Create an empty vector<TypeAST*>.
//   2. For each return type in type.sig.returnTypes:
//        - Call resolveType() on the return type node.
//        - Push the resolved pointer onto the result vector.
//   3. Return the vector.
//
// ─── Cases Covered ───────────────────────────────────────────────────────────
//   - Void function → returns empty vector.
//   - Single return type → vector with one element.
//   - Multiple return types → vector with elements in source order.
//   - Return types that are themselves function types or generic instantiations
//     → resolved recursively.
//
// ─── What is NOT covered ─────────────────────────────────────────────────────
//   - Returns a vector of pointers to the resolved TypeAST nodes; these nodes
//     are owned by the AST arena (or the original node). The caller must not
//     delete them.
//   - Does not validate that the return types are valid in the current context
//     (e.g., that a generic type is fully instantiated) – that is the caller's
//     responsibility.
//   - Does not handle the case where a return type is unresolved (resolveType
//     returns nullptr); in that case the vector will contain a nullptr entry.
//     The caller should check for null and report an error if needed.
//
// ─── Note ────────────────────────────────────────────────────────────────────
//   This function is often used in conjunction with getFunctionReturnType
//   (single return convenience wrapper). Use this function when you need to
//   inspect all return types, e.g., for checking multi‑return assignments.
// ─────────────────────────────────────────────────────────────────────────────

std::vector<TypeAST*> TypeResolver::getFunctionReturnTypes(const FuncTypeAST& type) {
    std::vector<TypeAST*> result;
    for (auto& retType : type.sig.returnTypes) {
        if (retType) {
            result.push_back(resolveType(retType.get()));
        }
    }
    return result;
}
TypeAST* TypeResolver::getFunctionReturnType(const FuncTypeAST& type, const SourceLocation* loc) {
    if (type.sig.returnTypes.empty()) {
        return nullptr;
    }
    if (type.sig.returnTypes.size() > 1) {
        if (loc) {
            _dc.error(DiagnosticCategory::Semantic, *loc, DiagCode::E3002,
                      "function has multiple return types but single return expected");
        }
        return nullptr;
    }
    return resolveType(type.sig.returnTypes[0].get());
}

// ─────────────────────────────────────────────────────────────────────────────
// Type Node Visitors
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// visit(PrimitiveTypeAST)  — Resolves primitive language types
//
// Primitives (int, float, bool) are inherently valid built-in language targets and 
// do not require verification against a symbol map. They resolve immediately.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(PrimitiveTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("visit(PrimitiveTypeAST): kind=" 
                        << static_cast<int>(node.primitiveKind));
    _resolved = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(NamedTypeAST)  — Resolves custom user-defined types (Struct, Enum, etc)
//
// Verifies that the string name provided genuinely maps to a recognizable type 
// entity within the SymbolTable. Refuses aliases mapped to variables/functions 
// and recurses correctly into verifying any generic inner arguments included.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(NamedTypeAST& node) {
    LUC_LOG_SEMANTIC("visit(NamedTypeAST): name='" << std::string(_pool.lookup(node.name)) << "'");
    LUC_LOG_SEMANTIC_VERBOSE("\tgenericArgs count=" << node.genericArgs.size());
    
    // CHECK 1: Is this name a generic type parameter?
    if (isGenericParam(node.name)) {
        LUC_LOG_SEMANTIC_VERBOSE("\tfound as generic param: '" << _pool.lookup(node.name) << "'");
             
        TypeAST* subst = lookupSubstitution(node.name);
        if (subst) {
            LUC_LOG_SEMANTIC("\tsubstituting '" << _pool.lookup(node.name) << "' with concrete type");
            _resolved = subst;
            return;
        }
        
        node.isGenericParam = true;
        LUC_LOG_SEMANTIC("\tresolved as generic parameter (isGenericParam=true)");
        _resolved = &node;
        return;
    }

    // If we reach here, this name is NOT a generic parameter.
    // Explicitly set false — important when the same NamedTypeAST node is
    // re-resolved in a different generic context (e.g. multiple checkFuncDecl
    // calls that each set different genericParams_ on the resolver).
    node.isGenericParam = false;
    LUC_LOG_SEMANTIC_VERBOSE("\tnot a generic param, looking up in symbol table");

    // CHECK 2: Lookup the identifier in the global symbol table.
    // Lookup the identifier natively defined by the programmer in the symbol table.
    Symbol* sym = _symbols.lookup(node.name);
    if (!sym) {
        LUC_LOG_SEMANTIC("\tERROR: type '" << std::string(_pool.lookup(node.name)) << "' not declared");
        _dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                  "type '" + std::string(_pool.lookup(node.name)) + "' is not declared");
        _resolved = nullptr;
        return;
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("\tfound symbol: kind=" << static_cast<int>(sym->kind));
    
    // Strict typing: We can only resolve identifiers that actually represent a type.
    if (sym->kind != SymbolKind::Struct && sym->kind != SymbolKind::Enum &&
        sym->kind != SymbolKind::Trait && sym->kind != SymbolKind::TypeAlias) {
        LUC_LOG_SEMANTIC("\tERROR: '" << std::string(_pool.lookup(node.name)) << "' is a value, not a type");
        _dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                  "'" + std::string(_pool.lookup(node.name)) + "' is a value, not a type");
        _resolved = nullptr;
        return;
    }
    
    // Transparently unwrap TypeAlias types.
    if (sym->kind == SymbolKind::TypeAlias) {
        LUC_LOG_SEMANTIC("\tunwrapping type alias '" << std::string(_pool.lookup(node.name)) << "'");
        TypeAST* resolvedAlias = resolveType(sym->type);
        if (resolvedAlias) {
            LUC_LOG_SEMANTIC("\talias resolved to: " << LucDebug::kindToString(resolvedAlias->kind));
            _resolved = resolvedAlias;
        } else {
            LUC_LOG_SEMANTIC("\tERROR: failed to resolve alias");
            _resolved = nullptr;
        }
        return;
    }

    // Drill down recursively into generic sub-arguments.
    LUC_LOG_SEMANTIC_VERBOSE("\tresolving " << node.genericArgs.size() << " generic arguments");
    for (size_t i = 0; i < node.genericArgs.size(); ++i) {
        LUC_LOG_SEMANTIC_EXTREME("\t\tresolving arg " << i);
        resolveType(node.genericArgs[i].get());
    }

    // Generic constraint checking (only for struct instantiations) 
    if (sym->kind == SymbolKind::Struct && !node.genericArgs.empty()) {
        auto* structDecl = sym->decl->as<StructDeclAST>();
        if (structDecl->genericParams.size() != node.genericArgs.size()) {
            _dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                      "struct '" + std::string(_pool.lookup(node.name)) +
                      "' expects " + std::to_string(structDecl->genericParams.size()) +
                      " generic arguments, got " + std::to_string(node.genericArgs.size()));
            _resolved = nullptr;
            return;
        }
        for (size_t i = 0; i < structDecl->genericParams.size(); ++i) {
            GenericParamAST* gp = structDecl->genericParams[i].get();
            TypeAST* argType = node.genericArgs[i].get();
            if (!satisfiesConstraints(argType, gp->constraints)) {
                std::string_view argName = _pool.lookup(
                    argType->kind == ASTKind::NamedType
                        ? argType->as<NamedTypeAST>()->name
                        : InternedString());
                _dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                          "type '" + std::string(argName) +
                          "' does not satisfy constraints for generic parameter '" +
                          std::string(_pool.lookup(gp->name)) + "'");
                _resolved = nullptr;
                return;
            }
        }
    }
    
    LUC_LOG_SEMANTIC("\tresolved NamedTypeAST: '" << std::string(_pool.lookup(node.name)) << "'");
    _resolved = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(NullableTypeAST)  — Safely verifies a nullable constraint wrapper
//
// Merely ensures that whatever inner type constraint is being requested as 
// nullable maps securely to a legitimate definition.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(NullableTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("visit(NullableTypeAST)");
    if (node.inner) {
        LUC_LOG_SEMANTIC_EXTREME("\tresolving inner type");
        resolveType(node.inner.get());
    }
    _resolved = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(FixedArrayTypeAST)  — Validates standard sized array declarations
//
// Array definitions map structurally correctly as long as their inner element 
// defines properly. Asserts that an array structurally cannot dictate a 
// capacity of strictly 0 items.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(FixedArrayTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("visit(FixedArrayTypeAST): size=" << node.size);
    
    if (node.size == 0) {
        LUC_LOG_SEMANTIC("\tERROR: array size must be > 0");
        _dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                  "fixed array size must be greater than zero");
    }
    if (node.element) {
        LUC_LOG_SEMANTIC_EXTREME("\tresolving element type");
        resolveType(node.element.get());
    }
    _resolved = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(SliceTypeAST)  — Validates dynamic fat-pointer views
//
// Merely requires the internal element it presents a viewing lens over is mapped 
// realistically.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(SliceTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("visit(SliceTypeAST)");
    if (node.element) {
        LUC_LOG_SEMANTIC_EXTREME("\tresolving element type");
        resolveType(node.element.get());
    }
    _resolved = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(DynamicArrayTypeAST)  — Validates heap-owned growable arrays
//
// Delegates resolution requirements straight down to its individual member element.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(DynamicArrayTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("visit(DynamicArrayTypeAST)");
    if (node.element) {
        LUC_LOG_SEMANTIC_EXTREME("\tresolving element type");
        resolveType(node.element.get());
    }
    _resolved = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(RefTypeAST)  — Maps safe memory reference boundaries
//
// Assures the internal struct/primitive resolving to this safe view accurately maps.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(RefTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("visit(RefTypeAST)");
    if (node.inner) {
        LUC_LOG_SEMANTIC_EXTREME("\tresolving inner type");
        resolveType(node.inner.get());
    }
    _resolved = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(PtrTypeAST)  — Validates raw C pointers in @extern contexts only
//
// The language forbids raw pointer types (*T) everywhere except declarations
// carrying the @extern attribute. The TypeResolver's _insideExtern flag is
// set by checkFuncDecl / checkVarDecl when they detect @extern on the decl.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(PtrTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("visit(PtrTypeAST)");
    LUC_LOG_SEMANTIC_VERBOSE("\t_insideExtern=" << (_insideExtern ? "true" : "false"));
    
    // Raw pointers are now allowed anywhere (just storage).
    // Operation restrictions are enforced in checkBinaryExpr, checkIndexExpr, etc.
    if (node.inner) {
        LUC_LOG_SEMANTIC_EXTREME("\tresolving inner type");
        resolveType(node.inner.get());
    }
    _resolved = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(FuncTypeAST)  — Resolves dynamic callable functional configurations
//
// Safely iterates every incoming execution argument type, confirming mappings,
// before resolving any potential outgoing returned type mappings.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(FuncTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("visit(FuncTypeAST): paramGroups=" << node.sig.paramGroups.size() 
                        << ", returnTypes=" << node.sig.returnTypes.size());
    
    // ── Resolve type qualifiers (Phase 2) ────────────────────────────────────
    for (const auto& qualName : node.sig.rawQualifiers) {
        uint32_t bit = QualifierRegistry::instance().getBit(qualName);
        if (bit == 0) {
            _dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E2010,
                      "unknown type qualifier '~" + std::string(_pool.lookup(qualName)) + "'; " +
                      "known qualifiers: " + QualifierRegistry::instance().allNames());
        } else {
            node.sig.qualifiers |= bit;
            LUC_LOG_SEMANTIC_EXTREME("\tadded qualifier '~" << std::string(_pool.lookup(qualName)) 
                                    << "' bit=0x" << std::hex << bit);
        }
    }
    
    // Clear raw qualifiers to free memory
    node.sig.rawQualifiers.clear();
    
    // ── Resolve parameter types in all curry groups ──────────────────────────
    for (auto& group : node.sig.paramGroups) {
        for (auto& param : group) {
            if (param->type) {
                LUC_LOG_SEMANTIC_EXTREME("\tresolving param: " << std::string(_pool.lookup(param->name)));
                resolveType(param->type.get());
            }
        }
    }
    
    // ── Resolve return types ─────────────────────────────────────────────────
    for (auto& retType : node.sig.returnTypes) {
        if (retType) {
            LUC_LOG_SEMANTIC_EXTREME("\tresolving return type");
            resolveType(retType.get());
        }
    }
    
    _resolved = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// Declaration Node Visitors
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// visit(FuncDeclAST)  — Resolves function parameter and return types
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(FuncDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(FuncDeclAST): name='" << _pool.lookup(node.name) << "'");
    
    pushGenericParams(&node.genericParams);
    
    auto funcType = _arena.make<FuncTypeAST>();
    funcType->sig = node.sig;
    funcType->loc = node.loc;
    TypeAST* resolvedType = resolveType(funcType.get());
    
    popGenericParams();
    
    Symbol* sym = _symbols.lookup(node.name);
    if (sym && resolvedType) {
        sym->type = resolvedType;
        LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << _pool.lookup(node.name) << "' type");
    }
    
    _resolved = resolvedType;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(VarDeclAST)  — Resolves variable type annotations
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(VarDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(VarDeclAST): name='" << std::string(_pool.lookup(node.name)) << "'");
    
    TypeAST* resolvedType = nullptr;
    if (node.type) {
        resolvedType = resolveType(node.type.get());
        if (resolvedType) {
            node.resolvedType = resolvedType;
        }
    }
    
    // Point directly to resolved type
    Symbol* sym = _symbols.lookup(node.name);
    if (sym && resolvedType) {
        sym->type = resolvedType;
        LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << std::string(_pool.lookup(node.name)) 
                               << "' type to resolved type");
    }
    
    _resolved = resolvedType;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(StructDeclAST)  — Resolves struct field types
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(StructDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(StructDeclAST): name='" << std::string(_pool.lookup(node.name)) 
                   << "', fields=" << node.fields.size());
    
    resolveStructFields(node);
    
    // Ensure selfType exists – now allocate via arena
    if (!node.selfType) {
        node.selfType = _arena.make<NamedTypeAST>(node.name);
        node.selfType->loc = node.loc;
    }
    
    Symbol* sym = _symbols.lookup(node.name);
    if (sym) {
        sym->type = node.selfType.get();
        LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << std::string(_pool.lookup(node.name)) 
                               << "' type to selfType");
    }
    
    _resolved = node.selfType.get();
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(ImplDeclAST)  — Resolves method signatures in impl blocks
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(ImplDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(ImplDeclAST): structName='" << _pool.lookup(node.structName)
                   << "', methods=" << node.methods.size());
    
    pushGenericParams(&node.genericParams);
    
    // Build substitution map for concrete generic args and push onto stack
    std::unordered_map<InternedString, TypeAST*> localSubstitutionMap;
    if (!node.structGenericArgs.empty()) {
        Symbol* structSym = _symbols.lookup(node.structName);
        if (structSym && structSym->kind == SymbolKind::Struct) {
            auto* structDecl = structSym->decl->as<StructDeclAST>();
            if (structDecl && structDecl->genericParams.size() == node.structGenericArgs.size()) {
                for (size_t i = 0; i < structDecl->genericParams.size(); ++i) {
                    if (i < node.structGenericArgs.size() && node.structGenericArgs[i]) {
                        TypeAST* concreteType = resolveType(node.structGenericArgs[i].get());
                        if (concreteType && structDecl->genericParams[i]) {
                            localSubstitutionMap[structDecl->genericParams[i]->name] = concreteType;
                        }
                    }
                }
            }
        }
    }
    
    // Push the substitution map (even if empty) so inner resolutions see it
    pushSubstitutionMap(&localSubstitutionMap);
    
    // Resolve each method's type
    for (auto& method : node.methods) {
        if (!method) continue;
        
        auto funcType = _arena.make<FuncTypeAST>();
        funcType->sig = method->sig;
        funcType->loc = method->loc;
        TypeAST* resolvedType = resolveType(funcType.get());
        
        std::string mangledName = NameMangler::mangleMethod(_pool.lookup(node.structName), _pool.lookup(method->name));
        Symbol* sym = _symbols.lookup(_pool.intern(mangledName));
        if (sym && resolvedType) {
            sym->type = resolvedType;
            LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << mangledName 
                                   << "' type to resolved method signature");
        }
    }
    
    popSubstitutionMap();
    popGenericParams();
    _resolved = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(MethodDeclAST)  — Resolves methods
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(MethodDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(MethodDeclAST): name='" << std::string(_pool.lookup(node.name)) << "'");
    
    auto funcType = _arena.make<FuncTypeAST>();
    funcType->sig = node.sig;
    funcType->loc = node.loc;
    
    TypeAST* resolvedType = resolveType(funcType.get());
    _resolved = resolvedType;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(FromDeclAST)  —  Resolves from-entry parameter and return types.
//
// A `from` block defines implicit conversions from a source type (described by
// the parameter groups) to a target struct type. This function:
//   1. Resolves any generic parameters on the `from` block (e.g., `from Wrapper<T>`)
//      by pushing them onto the generic parameter stack. This makes `T` inside
//      entry signatures recognisable as a generic parameter.
//   2. Resolves the target struct type (for type checking).
//   3. For each entry, resolves all parameter and return types.
//   4. Registers a symbol for the conversion (mangled name) so that `TypeChecker`
//      can find it during implicit conversion lookup.
//
// ─── Generic Parameter Handling ──────────────────────────────────────────────
//   Generic parameters on the `from` block (e.g., `from Wrapper<T>`) are pushed
//   onto the stack so that references to `T` inside entry signatures are resolved
//   as generic parameters (isGenericParam = true). This ensures that the types
//   like `val T` are correctly recognised.
//
//   The actual substitution of concrete types for generic parameters during
//   instantiation is deferred to Phase 3 (when the conversion is used). At that
//   point, the TypeChecker will build a substitution map and instantiate the
//   entry signature before generating a call.
//
// ─── Error Handling ─────────────────────────────────────────────────────────
//   - Reports errors for unresolved target types or invalid parameter/return types.
//   - Generic parameter constraint validation is performed via resolveGenericParamConstraints.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(FromDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(FromDeclAST): targetType='" << std::string(_pool.lookup(node.targetTypeName))
                   << "', entries=" << node.entries.size());
    
    // Resolve generic parameter constraints (trait names) if any.
    for (auto& gp : node.genericParams) {
        if (gp) resolveGenericParamConstraints(*gp);
    }
    
    // Push generic parameters onto stack so that T inside entries is recognised.
    pushGenericParams(&node.genericParams);
    
    // Resolve the target type (NamedType) for type checking
    // Create a temporary NamedTypeAST to resolve
    NamedTypeAST targetType(node.targetTypeName);
    targetType.loc = node.loc;
    resolveType(&targetType);
    
    // Resolve each casting entry's parameter types and return types
    for (auto& entry : node.entries) {
        if (!entry) continue;
        
        // Resolve all parameter types in all curry groups
        for (auto& group : entry->sig.paramGroups) {
            for (auto& param : group) {
                if (param->type) {
                    resolveType(param->type.get());
                }
            }
        }
        
        // Resolve return type
        if (entry->returnType) {
            resolveType(entry->returnType.get());
        }

        // Update the symbol table so the type checker can find this conversion
        TypeAST* firstParamType = nullptr;
        if (!entry->sig.paramGroups.empty() && !entry->sig.paramGroups[0].empty() && entry->sig.paramGroups[0][0]) {
            firstParamType = entry->sig.paramGroups[0][0]->type.get();
        }
        std::string mangledName = NameMangler::mangleFrom(_pool.lookup(node.targetTypeName), firstParamType, _pool);
        Symbol* sym = _symbols.lookup(_pool.intern(mangledName));
        
        if (sym) {
            auto funcType = _arena.make<FuncTypeAST>();
            funcType->sig = entry->sig;
            funcType->loc = entry->loc;
            if (entry->returnType) {
                funcType->sig.returnTypes.push_back(entry->returnType);
            }
            sym->type = resolveType(funcType.get());
            LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << mangledName << "' type");
        }
    }
    
    popGenericParams();
    _resolved = nullptr;  // From blocks don't produce a type
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(TypeAliasDeclAST)  — Properly resolves type alias
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(TypeAliasDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(TypeAliasDeclAST): name='" << _pool.lookup(node.name) << "'");
    
    pushGenericParams(&node.genericParams);
    
    if (node.aliasedType) {
        TypeAST* resolved = resolveType(node.aliasedType.get());
        if (resolved) {
            node.resolvedType = resolved;
        }
    }
    
    popGenericParams();
    _resolved = node.resolvedType 
            ? static_cast<TypeAST*>(node.resolvedType) 
            : (node.aliasedType ? node.aliasedType.get() : nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(TraitDeclAST)  — Resolves trait method signatures
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(TraitDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(TraitDeclAST): name='" << _pool.lookup(node.name) 
                   << "', methods=" << node.methods.size());
    
    pushGenericParams(&node.genericParams);
    
    for (auto& method : node.methods) {
        if (!method) continue;
        
        auto funcType = _arena.make<FuncTypeAST>();
        funcType->sig = method->sig;
        funcType->loc = method->loc;
        TypeAST* resolvedType = resolveType(funcType.get());
        
        std::string mangledName = NameMangler::mangleMethod(_pool.lookup(node.name), _pool.lookup(method->name));
        Symbol* sym = _symbols.lookup(_pool.intern(mangledName));
        if (sym && resolvedType) {
            sym->type = resolvedType;
            LUC_LOG_SEMANTIC_VERBOSE("\tupdated symbol '" << mangledName 
                                   << "' type to resolved trait method signature");
        }
    }
    
    popGenericParams();
    _resolved = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(TraitMethodAST)  — Resolves trait method signatures
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(TraitMethodAST& node) {
    LUC_LOG_SEMANTIC("visit(TraitMethodAST): name='" << std::string(_pool.lookup(node.name)) << "'");
    
    auto funcType = _arena.make<FuncTypeAST>();
    funcType->sig = node.sig;
    funcType->loc = node.loc;
    
    TypeAST* resolvedType = resolveType(funcType.get());
    _resolved = resolvedType;
}

void TypeResolver::visit(TraitRefAST& node) {
    // Resolve the trait name
    Symbol* sym = _symbols.lookup(node.name);
    if (!sym || sym->kind != SymbolKind::Trait) {
        _dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                  "trait '" + std::string(_pool.lookup(node.name)) + "' not found");
        _resolved = nullptr;
        return;
    }
    // Resolve generic arguments
    for (auto& arg : node.genericArgs) {
        resolveType(arg.get());
    }
    _resolved = nullptr; // TraitRef is not a type; just a reference
}