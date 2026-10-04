/// @file ConstantPool.hpp
///
/// @responsibility Every constant value the code references, deduplicated,
///                 with stable indices. The pool answers one question:
///                 "what does this instruction reference by value?"
///
/// ─── What goes in the pool ────────────────────────────────────────────────
/// Anything a LoadConst opcode can name:
///
///   - a literal that appears in an expression (`10` in `x + 10`,
///     `"hello"` in `println("hello")`)
///   - a folded constant expression (`5` from `2 + 3`)
///   - a function address (a `Kind::Function` entry holding a
///     FunctionProto index)
///   - a constant array
///
/// ─── What does NOT go in the pool ─────────────────────────────────────────
/// The initial value of a top-level binding or a baked table cell. Those
/// are static data; see StaticData.hpp. The two containers overlap in
/// kind (both can hold a `5`) but answer different questions:
///
///   - ConstantPool: what a code instruction references.
///   - StaticData:   what a declaration starts as.
///
/// ─── Design: every constant is concrete ───────────────────────────────────
/// There is no Kind::Unknown. A Constant in the pool is always a
/// concrete value: a bool, an integer, a float, a string, a char, nil,
/// an array, or a function address. The compiler only interns values
/// that Sema has already folded, and Sema's evaluator produces only
/// concrete kinds (its Unknown and Error kinds are pre-evaluation
/// sentinels that never survive to the compiler).
///
/// If the compiler ever encounters an unevaluated or errored
/// ConstantValue, that is a Sema bug, and the compiler asserts. It does
/// not encode the sentinel into the artifact.
///
/// ─── Design: the compiler translates, it does not fold ────────────────────
/// Sema already folded every constant expression (ConstEvaluator in
/// sema/const_eval/). By the time the compiler sees an ExprAST, its
/// isConst is true and its constValue is populated with a concrete kind.
/// BakeConstant translates that value into a serializable Constant and
/// the compiler interns it here. Nothing in the compiler re-evaluates.
///
/// ─── Design: function constants are indices, not pointers ─────────────────
/// A Kind::Function constant holds a FunctionProto index, not an
/// FnDeclAST*. The compiler resolves the FnDeclAST* to an index at emit
/// time; BakeConstant receives the resolved index.

#pragma once

#include "contract/TypeDescriptor.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace lucid::bytecode {

/// @brief The payload of a RowRef constant.
///
/// Identifies a specific row of a specific table by position:
/// the table's index in StaticData::tables, and the row's index in
/// the table's rows list. The interpreter reconstructs a runtime &T
/// from this pair at load time.
struct RowRefConstant {
    uint32_t tableIndex;
    uint32_t rowIndex;
};

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
        RowRef,     ///< a &T; payload is (table index, row index)
    };

    Kind           kind = Kind::Nil;
    contract::TypeDescriptor type;

    std::variant<
        std::monostate,          // Nil
        bool,                    // Bool
        int64_t,                 // Int
        double,                  // Float
        std::string,             // String, Char
        std::vector<Constant>,   // Array
        uint32_t,                // Function: FunctionProto index
        RowRefConstant           // RowRef: (table index, row index)
    > value;
};

/// @brief The pool.
class ConstantPool {
public:
    ConstantPool() = default;

    /// @brief Intern a constant. Returns its index.
    /// Deduplicates equal constants by a canonical key.
    uint32_t add(Constant c);

    /// @brief The constant at an index. Panics on out-of-range.
    const Constant& at(uint32_t index) const;

    size_t size() const noexcept { return m_constants.size(); }
    const std::vector<Constant>& all() const noexcept { return m_constants; }

    /// @brief Invariants. A violation is a compiler bug.
    ///   - every Function-kind constant's index is < the Bytecode's
    ///     function count. Checked by Bytecode, not here (the pool does
    ///     not know the function count).
    ///   - every Array-kind constant's elements are themselves well-formed.
    ///   - every String-kind constant's payload is well-formed UTF-8.
    ///   - the variant's active alternative matches the kind tag.
    void checkInvariants() const;

private:
    std::vector<Constant> m_constants;

    /// Dedup map. Keyed by a canonical serialization of (kind, type,
    /// value); the value is the index of the first constant that
    /// produced that key. See the .cpp for the key encoding.
    std::unordered_map<std::string, uint32_t> m_dedup;
};

} // namespace lucid::bytecode