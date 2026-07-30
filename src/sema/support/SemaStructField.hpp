/// @file SemaStructField.hpp
/// @brief Struct field analysis - two-pass registration and body analysis.
/// 
/// @architectural_note Two-Pass Analysis for Structs
///   Struct fields require a two-pass approach to support self-reference:
///     Phase 1: Register ALL field names (no type resolution)
///     Phase 2: Resolve field types and analyze function bodies
/// 
///   This allows:
///     - `self.bar` to resolve even if `bar` is declared after the function
///     - Recursive field types to be detected (`Node<T>` vs `*Node<T>`)
///     - Const fields to be validated before body analysis

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

/// @brief Resolve a struct field's type and validate it.
/// 
/// This is Phase 2 of struct analysis. Called after all field names are registered.
/// Resolves the field type, validates self-reference, const rules, etc.
/// 
/// @param field The field to resolve.
/// @param currentStruct The struct currently being defined.
/// @param ctx The semantic context.
void resolveStructField(const FieldDeclAST* field,
                         const StructDeclAST* currentStruct,
                         SemaContext& ctx);

/// @brief Resolve all fields in a struct declaration.
/// 
/// Checks for duplicate field names and resolves each field.
/// 
/// @param decl The struct declaration.
/// @param ctx The semantic context.
void resolveStructFields(const StructDeclAST* decl, SemaContext& ctx);

/// @brief Analyze a struct function field's body.
/// 
/// This is Phase 2 of struct function fields. Called after all fields are resolved.
/// Analyzes the function body with the `self` parameter already available.
/// 
/// @param field The function field to analyze.
/// @param currentStruct The struct currently being defined.
/// @param ctx The semantic context.
void analyzeFunctionFieldBody(const FieldDeclAST* field,
                               const StructDeclAST* currentStruct,
                               SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Struct Field Validation Helpers (Used by resolveStructField)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Validate a single struct field's type (self-reference, const, etc.).
/// 
/// Shared validation logic used during resolution.
/// 
/// @param field The field to validate.
/// @param currentStruct The struct currently being defined.
/// @param ctx The semantic context.
void validateStructFieldType(const FieldDeclAST* field,
                              const StructDeclAST* currentStruct,
                              SemaContext& ctx);

/// @brief Validate all fields in a struct declaration.
/// 
/// Checks for duplicate field names and validates each field.
/// 
/// @param decl The struct declaration.
/// @param ctx The semantic context.
void validateStructFields(const StructDeclAST* decl, SemaContext& ctx);

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
/// being defined. This can be:
///   - Direct: `Node<T>` (NamedTypeAST) → recursive value (deep copy)
///   - Indirect: `*Node<T>` (PtrTypeAST) → raw pointer (sealed conduit)
/// 
/// @param fieldType The field's type (already resolved).
/// @param currentStruct The struct currently being defined.
/// @param ctx The semantic context.
/// @return SelfReferenceInfo describing the self-reference nature, or empty if none.
SelfReferenceInfo checkSelfReference(const TypeAST* fieldType,
                                      const StructDeclAST* currentStruct,
                                      SemaContext& ctx);

/// @brief Check if a field type is a recursive value (deep copy semantics).
/// 
/// A recursive value is a self-reference that is NOT a pointer.
/// This means the user wrote `Node<T>` or `Node<T>?` (not `*Node<T>`).
/// 
/// @param fieldType The field's type.
/// @param currentStruct The struct currently being defined.
/// @param ctx The semantic context.
/// @return true if this is a recursive value (deep copy semantics).
bool isRecursiveValueType(const TypeAST* fieldType,
                           const StructDeclAST* currentStruct,
                           SemaContext& ctx);

/// @brief Check if a field type is a raw pointer self-reference.
/// 
/// A raw pointer self-reference means the user explicitly wrote `*Node<T>`
/// or `*Node<T>?` (sealed conduit semantics).
/// 
/// @param fieldType The field's type.
/// @param currentStruct The struct currently being defined.
/// @param ctx The semantic context.
/// @return true if this is a raw pointer self-reference.
bool isPointerSelfReference(const TypeAST* fieldType,
                             const StructDeclAST* currentStruct,
                             SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// DEPRECATED (kept for compatibility during transition)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief [DEPRECATED] Use registerStructFieldName() + resolveStructField() instead.
[[deprecated("Use registerStructFieldName() + resolveStructField() instead")]]
void registerStructField(const FieldDeclAST* field,
                          const StructDeclAST* currentStruct,
                          SemaContext& ctx);

} // namespace sema