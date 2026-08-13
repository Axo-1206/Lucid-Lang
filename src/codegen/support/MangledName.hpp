/// @file codegen/support/MangledName.hpp
/// @brief Mangled name generation for declarations.
///
/// Name mangling is the process of encoding a declaration's identity
/// (name, type, module, etc.) into a unique string that can be used
/// as a symbol name in the object file.
///
/// ─── Why Name Mangling? ──────────────────────────────────────────────────────
/// 1. **Overloading**: Same name, different parameter types → different symbols
/// 2. **Module Scoping**: Same name in different modules → different symbols
/// 3. **Generics**: Different instantiations → different symbols
/// 4. **Linking**: The linker needs unique, deterministic names
/// 5. **ABI Stability**: Names must be stable across compilations
///
/// ─── Mangling Scheme ──────────────────────────────────────────────────────
/// Format: _L{module}_{name}_{params}_{return}_{generic}
///
/// Components:
///   - _L: Lucid prefix (distinguishes from C symbols)
///   - module: Module path (sanitized)
///   - name: Declaration name
///   - params: Parameter types (encoded)
///   - return: Return type (encoded)
///   - generic: Generic arguments (if any)
///
/// ─── Type Encoding ──────────────────────────────────────────────────────
/// | Type           | Code | Notes                        |
/// |----------------|------|------------------------------|
/// | void           | V    |                              |
/// | bool           | b    |                              |
/// | int8/byte      | c    | char                         |
/// | int16/short    | s    |                              |
/// | int32/int      | i    |                              |
/// | int64/long     | l    |                              |
/// | uint8/ubyte    | h    | unsigned char                |
/// | uint16/ushort  | t    | unsigned short               |
/// | uint32/uint    | u    |                              |
/// | uint64/ulong   | m    | unsigned long                |
/// | float          | f    |                              |
/// | double         | d    |                              |
/// | decimal        | D    | 128-bit decimal              |
/// | string         | S    |                              |
/// | char           | C    |                              |
/// | pointer        | P{T} | T is the pointee type        |
/// | reference      | R{T} | T is the referenced type     |
/// | nullable       | N{T} | T is the inner type          |
/// | fallible       | F{T} | T is the inner type          |
/// | combined       | X{T} | T is the inner type          |
/// | fixed array    | A{N}{T} | N is size, T is element      |
/// | slice          | A_{T} | T is element                 |
/// | dynamic array  | A*{T} | T is element                 |
/// | function       | F{params}_{return} |                          |
/// | named type     | {name} | User-defined type            |
/// | generic named  | {name}_G{args} | Generic arguments appended   |
///
/// ─── Examples ──────────────────────────────────────────────────────────────
/// add (a int)(b int) -> int
///   → _Lmath_add_P_i_i_Ri
///
/// identity<T> (v T) -> T
///   → _Lcore_identity_G_T_P_T_RT
///
/// identity<int> (v int) -> int (specialized)
///   → _Lcore_identity_G_i_P_i_Ri
///
/// process (data string) -> bool
///   → _Lapp_process_P_S_Rb

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"
#include "../context/CodeGenContext.hpp"

#include <string>
#include <vector>

namespace codegen {

// ─── Public API ─────────────────────────────────────────────────────────────

/// @brief Generate a mangled name for a function declaration.
/// @param decl The function declaration.
/// @param ctx The code generation context.
/// @return The mangled name as an InternedString.
InternedString generateMangledName(const FuncDeclAST* decl, CodeGenContext& ctx);

/// @brief Generate a mangled name for a variable declaration.
/// @param decl The variable declaration.
/// @param ctx The code generation context.
/// @return The mangled name as an InternedString.
InternedString generateMangledName(const VarDeclAST* decl, CodeGenContext& ctx);

/// @brief Generate a mangled name for a generic instantiation.
/// @param baseDecl The generic declaration (function or struct).
/// @param typeArgs The concrete type arguments.
/// @param ctx The code generation context.
/// @return The mangled name as an InternedString.
InternedString generateMangledNameForGeneric(
    const DeclAST* baseDecl,
    const std::vector<const TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

/// @brief Generate a mangled name for a struct.
/// @param decl The struct declaration.
/// @param ctx The code generation context.
/// @return The mangled name as an InternedString.
InternedString generateMangledName(const StructDeclAST* decl, CodeGenContext& ctx);

// ─── Core Encoding Functions ──────────────────────────────────────────────

/// @brief Encode a type to a mangled string.
/// @param type The type to encode.
/// @param pool The string pool for looking up names.
/// @return The encoded type string (as std::string for building).
std::string typeToMangleString(const TypeAST* type, StringPool& pool);

/// @brief Encode a type to a mangled string (context overload).
/// @param type The type to encode.
/// @param ctx The code generation context.
/// @return The encoded type string.
inline std::string typeToMangleString(const TypeAST* type, CodeGenContext& ctx) {
    return typeToMangleString(type, ctx.pool);
}

/// @brief Sanitize a string for use in a mangled name.
/// @param str The string to sanitize.
/// @return The sanitized string.
std::string sanitizeForMangledName(const std::string& str);

/// @brief Get the module path for mangling.
/// @param ctx The code generation context.
/// @return The sanitized module path.
std::string getMangledModulePath(CodeGenContext& ctx);

// ─── Primitive Type Encoding ─────────────────────────────────────────────

/// @brief Encode a primitive kind to a single character.
/// @param kind The primitive kind.
/// @return The encoded character.
char encodePrimitiveKind(PrimitiveKind kind);

/// @brief Check if a type is a primitive type.
bool isPrimitiveType(const TypeAST* type);

} // namespace codegen