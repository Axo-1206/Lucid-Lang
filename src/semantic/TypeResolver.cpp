/**
 * @file TypeResolver.cpp
 *
 * @nutshell Crawls through basic type strings and turns them into explicit system bindings.
 *
 * @reason A string parsing 'std.vector<int>' is useless until bound. This logic processes those strings structurally and maps them against the SymbolTable.
 *
 * @responsibility Implementation of the Phase 2a semantic pass (resolving primitive and complex type ASTs).
 *
 * @logic Validates that named types exist, checks generics, validates nullable configurations, and enforces array semantics.
 *
 * @related TypeResolver.hpp, SemanticAnalyzer.cpp
 */

#include "TypeResolver.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// TypeResolver (constructor)  — Initializes the TypeResolver with core dependencies
//
// Binds the active symbol table to allow looking up global type names and the
// diagnostic engine to log error messages directly during resolution.
// ─────────────────────────────────────────────────────────────────────────────
TypeResolver::TypeResolver(SymbolTable& symbols, DiagnosticEngine& dc)
    : symbols_(symbols), dc_(dc) {}

// ─────────────────────────────────────────────────────────────────────────────
// resolveType  — Main entry point to validate and bind a type node
//
// Accepts a raw type node parsed from the AST, dispatches it to the correct 
// visitor implementation, and returns the resolved node if successful. 
// If resolution fails (e.g., undeclared identifier), it returns nullptr.
// ─────────────────────────────────────────────────────────────────────────────
TypeAST* TypeResolver::resolveType(TypeAST* typeNode) {
    if (!typeNode) return nullptr;
    resolved_ = nullptr;
    typeNode->accept(*this);
    // Return the same node pointer if resolved, else nullptr.
    return resolved_;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(PrimitiveTypeAST)  — Resolves primitive language types
//
// Primitives (int, float, bool) are inherently valid built-in language targets and 
// do not require verification against a symbol map. They resolve immediately.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(PrimitiveTypeAST& node) {
    // Primitive types are inherently valid strings in the language.
    resolved_ = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(NamedTypeAST)  — Resolves custom user-defined types (Struct, Enum, etc)
//
// Verifies that the string name provided genuinely maps to a recognizable type 
// entity within the SymbolTable. Refuses aliases mapped to variables/functions 
// and recurses correctly into verifying any generic inner arguments included.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(NamedTypeAST& node) {
    // CHECK 1: Is this name a generic type parameter (e.g., T in Container<T>)?
    // Generic parameters are only valid within their containing declaration's scope.
    // They take precedence over global type lookups to allow `T` to resolve without
    // requiring a global symbol entry.
    if (genericParams_) {
        for (auto& gp : *genericParams_) {
            if (gp && gp->name == node.name) {
                // This is a valid generic type parameter. Resolve it as a NamedTypeAST.
                // Stamp isGenericParam so codegen Pass 0 can distinguish abstract uses
                // (T, K, V) from concrete struct/enum types (Circle, int) without
                // repeating the symbol table lookup.
                node.isGenericParam = true;
                resolved_ = &node;
                return;
            }
        }
    }

    // If we reach here, this name is NOT a generic parameter.
    // Explicitly set false — important when the same NamedTypeAST node is
    // re-resolved in a different generic context (e.g. multiple checkFuncDecl
    // calls that each set different genericParams_ on the resolver).
    node.isGenericParam = false;

    // CHECK 2: Lookup the identifier in the global symbol table.
    // Lookup the identifier natively defined by the programmer in the symbol table.
    Symbol* sym = symbols_.lookup(node.name);
    if (!sym) {
        dc_.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                  "type '" + node.name + "' is not declared");
        resolved_ = nullptr;
        return;
    }
    
    // Strict typing: We can only resolve identifiers that actually represent a valid structural Type.
    if (sym->kind != SymbolKind::Struct && sym->kind != SymbolKind::Enum &&
        sym->kind != SymbolKind::Trait && sym->kind != SymbolKind::TypeAlias) {
        dc_.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                  "'" + node.name + "' is a value, not a type");
        resolved_ = nullptr;
        return;
    }
    
    // Transparently unwrap TypeAlias types.
    if (sym->kind == SymbolKind::TypeAlias) {
        // Resolve the underlying target type referenced by the alias.
        TypeAST* resolvedAlias = resolveType(sym->type);
        if (resolvedAlias) {
            resolved_ = resolvedAlias;
        } else {
            resolved_ = nullptr;
        }
        return;
    }

    // Drill down recursively into generic sub-arguments (<int, string>) ensuring their types exist.
    for (auto& arg : node.genericArgs) {
        resolveType(arg.get());
    }
    
    resolved_ = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(NullableTypeAST)  — Safely verifies a nullable constraint wrapper
//
// Merely ensures that whatever inner type constraint is being requested as 
// nullable maps securely to a legitimate definition.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(NullableTypeAST& node) {
    if (node.inner) resolveType(node.inner.get());
    resolved_ = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(FixedArrayTypeAST)  — Validates standard sized array declarations
//
// Array definitions map structurally correctly as long as their inner element 
// defines properly. Asserts that an array structurally cannot dictate a 
// capacity of strictly 0 items.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(FixedArrayTypeAST& node) {
    if (node.size == 0) {
        dc_.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                  "fixed array size must be greater than zero");
    }
    if (node.element) resolveType(node.element.get());
    resolved_ = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(SliceTypeAST)  — Validates dynamic fat-pointer views
//
// Merely requires the internal element it presents a viewing lens over is mapped 
// realistically.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(SliceTypeAST& node) {
    if (node.element) resolveType(node.element.get());
    resolved_ = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(DynamicArrayTypeAST)  — Validates heap-owned growable arrays
//
// Delegates resolution requirements straight down to its individual member element.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(DynamicArrayTypeAST& node) {
    if (node.element) resolveType(node.element.get());
    resolved_ = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(RefTypeAST)  — Maps safe memory reference boundaries
//
// Assures the internal struct/primitive resolving to this safe view accurately maps.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(RefTypeAST& node) {
    if (node.inner) resolveType(node.inner.get());
    resolved_ = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(PtrTypeAST)  — Validates raw C pointers in @extern contexts only
//
// The language forbids raw pointer types (*T) everywhere except declarations
// carrying the @extern attribute. The TypeResolver's insideExtern_ flag is
// set by checkFuncDecl / checkVarDecl when they detect @extern on the decl.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(PtrTypeAST& node) {
    // *T is only valid in @extern-decorated declaration contexts.
    if (!insideExtern_) {
        dc_.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                  "raw pointers (*T) are only allowed on '@extern'-decorated declarations; "
                  "use '@bitcast(T, x)' for bit reinterpretation in expression position");
        resolved_ = nullptr;
        return;
    }
    if (node.inner) resolveType(node.inner.get());
    resolved_ = &node;
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(FuncTypeAST)  — Resolves dynamic callable functional configurations
//
// Safely iterates every incoming execution argument type, confirming mappings,
// before resolving any potential outgoing returned type mappings.
// ─────────────────────────────────────────────────────────────────────────────
void TypeResolver::visit(FuncTypeAST& node) {
    for (auto& param : node.params) {
        resolveType(param.get());
    }
    if (node.returnType) {
        resolveType(node.returnType.get());
    }
    resolved_ = &node;
}