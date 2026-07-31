/// @file SemaStructField.hpp
/// @brief Struct field analysis - focused on function field bodies and self-reference.
/// 
/// @architectural_note Minimal Design
///   Only the functionality that isn't duplicated elsewhere is kept here.
///   Field name registration and type resolution are handled in SemaDecl.cpp.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "../context/SemaContext.hpp"

namespace sema {

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

// ─────────────────────────────────────────────────────────────────────────────
// Function Field Body Analysis
// ─────────────────────────────────────────────────────────────────────────────

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

} // namespace sema