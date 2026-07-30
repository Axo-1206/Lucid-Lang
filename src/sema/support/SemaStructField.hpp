/// @file SemaStructField.hpp
/// @brief Struct field analysis - two-pass registration and body analysis.
/// 
/// @architectural_note Two-Pass Analysis for Structs
///   Struct fields require a two-pass approach to support self-reference:
///     Phase 1: Register ALL field names (no type resolution)
///     Phase 2: Resolve field types and analyze function bodies
/// 
///   This is separate from the global two-pass because struct fields
///   need to be registered BEFORE their types are resolved (self-reference).

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "../context/SemaContext.hpp"

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// Struct Field Registration - Phase 1 (Names Only)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Register a struct field name only (no type resolution).
/// 
/// This is Phase 1 of struct analysis. It registers the field name so that
/// self-reference is possible in Phase 2.
/// 
/// @param field The field to register.
/// @param currentStruct The struct currently being defined.
/// @param ctx The semantic context.
void registerStructFieldName(const FieldDeclAST* field,
                              const StructDeclAST* currentStruct,
                              SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Struct Field Resolution - Phase 2 (Types and Bodies)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Resolve all fields in a struct declaration.
/// 
/// This is the main entry point for struct field resolution.
/// Resolves all field types, validates self-reference, const, etc.
/// 
/// @param decl The struct declaration.
/// @param ctx The semantic context.
void resolveStructFields(const StructDeclAST* decl, SemaContext& ctx);

/// @brief Analyze a struct function field's body.
/// 
/// Called after all fields are resolved. Analyzes the function body
/// with the `self` parameter already available.
/// 
/// @param field The function field to analyze.
/// @param currentStruct The struct currently being defined.
/// @param ctx The semantic context.
void analyzeFunctionFieldBody(const FieldDeclAST* field,
                               const StructDeclAST* currentStruct,
                               SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Self-Reference Detection
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Information about a self-reference in a struct field.
struct SelfReferenceInfo {
    bool isSelfReference = false;
    bool isPointer = false;      // true if it's *Node<T> (PtrTypeAST)
    bool isNullable = false;     // true if it's T? or *T?
    bool isNonNullable = false;  // true if it's T (no ?)
    const NamedTypeAST* namedType = nullptr;
};

/// @brief Check if a field type is a self-reference to the current struct.
/// 
/// A self-reference occurs when a field is of the same type as the struct
/// being defined.
/// 
/// @param fieldType The field's type (already resolved).
/// @param currentStruct The struct currently being defined.
/// @param ctx The semantic context.
/// @return SelfReferenceInfo describing the self-reference nature.
SelfReferenceInfo checkSelfReference(const TypeAST* fieldType,
                                      const StructDeclAST* currentStruct,
                                      SemaContext& ctx);

} // namespace sema