/**
 * @file ConstantPool.hpp
 *
 * @responsibility Interned constants for one Bytecode. Every constant
 *                 the code references, deduplicated, with stable
 *                 indices.
 *
 * ─── Design: the compiler translates, it does not fold ────────────────────
 * Sema already folded every constant expression (ConstEvaluator in
 * sema/const_eval/). By the time the compiler sees an ExprAST, its
 * isConst is true and its constValue is populated. This file's job is to
 * translate that value into a serializable form and intern it. It does
 * not evaluate anything.
 *
 * ─── Design: function constants are indices, not pointers ─────────────────
 * A constant of Kind::Function holds a FunctionProto index, not an
 * FnDeclAST*. The compiler resolves the FnDeclAST* to a FunctionProto
 * index at emit time.
 */

#pragma once

#include "TypeDescriptor.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lucid::bytecode {

/// @brief One constant.
struct Constant {
    enum class Kind : uint8_t {
        Bool,
        Int,
        Float,
        String,
        Char,
        Nil,
        Array,      ///< a fixed-size array of constants
        Function,   ///< a code address; payload is a FunctionProto index
    };

    Kind              kind = Kind::Nil;
    TypeDescriptor    type;                    ///< for typed constants
    std::variant<
        bool,
        int64_t,
        double,
        std::string,
        std::vector<Constant>,   ///< Array
        uint32_t                 ///< Function: FunctionProto index
    > value;
};

/// @brief The pool.
class ConstantPool {
public:
    ConstantPool() = default;

    /// @brief Intern a constant. Returns its index.
    /// Deduplicates equal constants; two calls with the same value
    /// return the same index.
    uint32_t add(Constant c);

    /// @brief The constant at an index. Panics on out-of-range.
    const Constant& at(uint32_t index) const;

    size_t size() const noexcept { return m_constants.size(); }
    const std::vector<Constant>& all() const noexcept { return m_constants; }

    /// @brief Invariants. A violation is a compiler bug.
    ///   - every Function-kind constant's index is < functionCount
    ///     (checked by Bytecode, not here, because the pool does not
    ///     know the function count)
    ///   - every Array-kind constant's elements are themselves well-formed
    ///   - every String-kind constant's payload is a well-formed UTF-8 string
    void checkInvariants() const;

private:
    std::vector<Constant> m_constants;
    // Dedup map keyed by a canonical serialization of the constant.
    // The exact key type is an implementation detail; see the .cpp.
};

} // namespace lucid::bytecode