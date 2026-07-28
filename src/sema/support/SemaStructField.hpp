/// @file SemaStructField.hpp
/// @brief Struct field analysis - two-pass registration and body analysis.
/// 
/// @architectural_note Two-Pass Analysis for Structs
///   Struct fields require a two-pass approach to support self-reference:
///     Phase 1: Register ALL fields (names and types) without analyzing bodies
///     Phase 2: Analyze function bodies (with self parameter available)
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
// Struct Field Registration - Phase 1
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Register a struct field (name and type) without analyzing body.
/// 
/// This is Phase 1 of struct analysis. It registers the field name and
/// resolves its type, but does NOT analyze function bodies.
/// 
/// @param field The field to register.
/// @param currentStruct The struct currently being defined.
/// @param ctx The semantic context.
void registerStructField(const FieldDeclAST* field,
                          const StructDeclAST* currentStruct,
                          SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Struct Function Body Analysis - Phase 2
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Analyze a struct function field's body.
/// 
/// This is Phase 2 of struct analysis. Called after all fields are registered.
/// Analyzes the function body with the `self` parameter already available.
/// 
/// @param field The function field to analyze.
/// @param currentStruct The struct currently being defined.
/// @param ctx The semantic context.
void analyzeFunctionFieldBody(const FieldDeclAST* field,
                               const StructDeclAST* currentStruct,
                               SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Struct Field Validation Helpers
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Validate a single struct field (type, self-reference, const, etc.).
/// 
/// Shared validation logic used in both Phase 1 and Phase 2.
/// 
/// @param field The field to validate.
/// @param currentStruct The struct currently being defined.
/// @param ctx The semantic context.
void validateStructField(const FieldDeclAST* field,
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

} // namespace sema