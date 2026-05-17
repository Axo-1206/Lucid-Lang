/**
 * @file TypeResolver.hpp
 * @responsibility Phase 2a of semantic analysis: resolves type names to semantic type nodes.
 *
 * TypeResolver is an ASTVisitor that walks type annotations (PrimitiveTypeAST,
 * NamedTypeAST, FuncTypeAST, etc.) and validates that every name refers to a
 * declared type (struct, enum, trait, type alias, or primitive). It also handles
 * generic parameters: when a name matches a generic parameter of the enclosing
 * declaration (e.g., `T` in `struct Box<T>`), it marks the NamedTypeAST as
 * a generic parameter rather than performing a symbol lookup.
 *
 * The resolver uses a stack of generic parameter lists and a stack of substitution
 * maps to correctly resolve types inside nested generic contexts (e.g., a generic
 * struct inside a generic function).
 *
 * @related
 *   - SymbolTable.hpp – for looking up named types
 *   - TypeChecker.hpp – for type compatibility checks (Phase 2b)
 *   - SemanticAnalyzer.hpp – orchestrates the four phases
 *   - TypeAST.hpp – the AST nodes being resolved
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Why a separate type resolution phase is necessary
 * ─────────────────────────────────────────────────────────────────────────────
 * - The parser builds raw TypeAST nodes using only syntax (e.g., `Vec2`, `int`,
 *   `Buffer<int>`). These nodes contain unverified names and unresolved generics.
 * - Resolving types requires symbol table lookups, generic parameter matching,
 *   and substitution of concrete type arguments – all of which are semantic
 *   concerns, not syntactic.
 * - Factoring resolution into a dedicated pass keeps the parser simple and
 *   allows forward references (types can be used before they are defined).
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Design decisions
 * ─────────────────────────────────────────────────────────────────────────────
 * - **Visitor‑based** – TypeResolver inherits from ASTVisitor and dispatches to
 *   specific visit methods for each type node kind. This keeps the logic
 *   organised and extensible.
 * - **Stack of generic parameters** – because generic declarations can nest,
 *   a single pointer would be insufficient. The resolver maintains a stack
 *   (`genericParamsStack_`) that is pushed when entering a generic declaration
 *   and popped when leaving.
 * - **Stack of substitution maps** – when instantiating a generic type with
 *   concrete arguments (e.g., `Scene<Circle>`), the resolver pushes a map from
 *   generic parameter names to concrete types. This allows recursive resolution
 *   of nested generic types.
 * - **Arena allocation** – temporary FuncTypeAST nodes and struct selfType are
 *   allocated in the same ASTArena as the rest of the AST, ensuring consistent
 *   lifetime and no memory leaks.
 * - **Explicit StringPool dependency** – for diagnostics and name display,
 *   the resolver holds a reference to StringPool to convert InternedString
 *   names to readable strings.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Usage example
 * ─────────────────────────────────────────────────────────────────────────────
 * @code
 * // During Phase 2 (resolveTypes)
 * TypeResolver resolver(symbols, dc, pool, arena);
 *
 * for (auto* decl : program->decls) {
 *     if (auto* typeAlias = decl->as<TypeAliasDeclAST>()) {
 *         resolver.visit(*typeAlias);   // resolves the aliased type
 *     }
 *     if (auto* structDecl = decl->as<StructDeclAST>()) {
 *         resolver.visit(*structDecl);  // resolves field types
 *     }
 * }
 * @endcode
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * @note The resolver assumes that the symbol table already contains all
 *       declarations (Phase 1 has completed). It does not declare new symbols;
 *       it only reads existing ones and updates their `type` field.
 * ─────────────────────────────────────────────────────────────────────────────
 */
#pragma once

#include "ast/TypeAST.hpp"
#include "ast/DeclAST.hpp"
#include "SymbolTable.hpp"
#include "ast/support/StringPool.hpp"
#include "ast/support/ASTArena.hpp"

#include <unordered_map>
#include <string>

class DiagnosticEngine;

class TypeResolver : public ASTVisitor {
public:
    explicit TypeResolver(SymbolTable& symbols, DiagnosticEngine& dc, StringPool& pool, ASTArena& arena);

    TypeAST* resolveType(TypeAST* typeNode);

    // ── Type Nodes Visitor Overrides ──
    void visit(PrimitiveTypeAST& node)      override;
    void visit(NamedTypeAST& node)          override;
    void visit(NullableTypeAST& node)       override;
    void visit(FixedArrayTypeAST& node)     override;
    void visit(SliceTypeAST& node)          override;
    void visit(DynamicArrayTypeAST& node)   override;
    void visit(RefTypeAST& node)            override;
    void visit(PtrTypeAST& node)            override;
    void visit(FuncTypeAST& node)           override;

    // ── Declaration nodes Visitor Overrides ──
    void visit(FuncDeclAST& node)           override;
    void visit(VarDeclAST& node)            override;
    void visit(StructDeclAST& node)         override;
    void visit(ImplDeclAST& node)           override;
    void visit(MethodDeclAST& node)         override;
    void visit(FromDeclAST& node)           override;
    void visit(TypeAliasDeclAST& node)      override;
    void visit(TraitDeclAST& node)          override;
    void visit(TraitMethodAST& node)        override;
    void visit(TraitRefAST& node)           override;

    // ── Helper methods for resolving composite structures ──
    void resolveStructFields(StructDeclAST& node);
    void resolveFunctionType(const FuncTypeAST& type);
    std::vector<TypeAST*> getFunctionReturnTypes(const FuncTypeAST& type);
    TypeAST* getFunctionReturnType(const FuncTypeAST& typeconst, const SourceLocation* loc = nullptr);

    // Call this before resolving types in an @extern-declared declaration
    // so that *T raw pointer types are permitted in that context.
    void setInsideExtern(bool val) { _insideExtern = val; }

    void setStructTraits(const std::unordered_map<InternedString, std::vector<InternedString>>* map) {
        _structTraits = map;
    }

private:
    SymbolTable& _symbols;
    DiagnosticEngine& _dc;
    StringPool& _pool;
    ASTArena& _arena;
    TypeAST* _resolved = nullptr;
    bool _insideExtern = false;
    const std::unordered_map<InternedString, std::vector<InternedString>>* _structTraits = nullptr;

    // Stack for nested generic parameters
    std::vector<const std::vector<GenericParamPtr>*> genericParamsStack_;
    
    // Stack for nested substitution maps (concrete type arguments)
    std::vector<const std::unordered_map<InternedString, TypeAST*>*> substitutionMapStack_;

    void pushGenericParams(const std::vector<GenericParamPtr>* params);
    void popGenericParams();
    bool isGenericParam(InternedString name) const;
    
    void resolveGenericParamConstraints(GenericParamAST& gp);
    bool satisfiesConstraints(TypeAST* type, const std::vector<InternedString>& requiredTraits) const;

    void pushSubstitutionMap(const std::unordered_map<InternedString, TypeAST*>* map);
    void popSubstitutionMap();
    TypeAST* lookupSubstitution(InternedString name) const;
};