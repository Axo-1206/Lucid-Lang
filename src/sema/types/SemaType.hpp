/// @file SemaType.hpp
/// @brief Unified header for type-related semantic functions.
/// 
/// This file includes all sub-headers for convenience.
/// Use this when you need type resolution, comparison, or validation.
/// 
/// # File Organization
/// 
/// | File             | Purpose                                             |
/// | ---------------- | --------------------------------------------------- |
/// | SemaResolve.hpp  | Type resolution - convert type AST to semantic type |
/// | SemaCompare.hpp  | Type comparison and assignability                   |
/// | SemaValidate.hpp | Semantic validation rules                           |
/// 
/// # Quick Reference
/// 
/// | What you need                  | Where to look | Function                        |
/// | ------------------------------ | ------------- | ------------------------------- |
/// | Resolve a type annotation      | SemaResolve   | `resolveType()`                 |
/// | Resolve a trait reference      | SemaResolve   | `resolveTraitRef()`             |
/// | Compare two types              | SemaCompare   | `typesEqual()`                  |
/// | Check assignability            | SemaCompare   | `isAssignable()`                |
/// | Check if type is nullable      | SemaCompare   | `isNullableType()`              |
/// | Validate trait implementation  | SemaValidate  | `validateTraitImplementation()` |
/// | Validate generic arguments     | SemaValidate  | `validateGenericArguments()`    |
/// | Validate const field type      | SemaValidate  | `validateConstFieldType()`      |

#pragma once

#include "SemaResolve.hpp"
#include "SemaCompare.hpp"
#include "SemaValidate.hpp"