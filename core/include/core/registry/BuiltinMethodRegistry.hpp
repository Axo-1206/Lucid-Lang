/// @file registry/BuiltinMethodRegistry.hpp
/// @brief Pure data registry for the built-in methods on tables and arrays.
///
/// ─── What this file is ────────────────────────────────────────────────────
/// A lookup table for the built-in operations that appear after a `.` on a
/// table, a column view, a row, or an array. Given a method's name, it
/// answers:
///
///   1. Is this a recognized built-in method?
///   2. Which kinds of receiver does it apply to?
///   3. What is the shape of its argument list?
///   4. What category of result does it produce?
///
/// ─── What this file is not ────────────────────────────────────────────────
/// It is not the method's type. The registry records that `SUM` takes no
/// arguments and produces a primitive; Sema computes *which* primitive
/// from the receiver's element type. The registry records that `FIND`
/// takes one predicate function; Sema checks the predicate's signature
/// against the receiver's row type. The registry records shapes; Sema
/// resolves types.
///
/// It is not the parser. The parser produces `FieldAccessExprAST` nodes
/// for `T.ADD`, `arr.ADD`, and `p.name`; whether the field is a method, a
/// column, or a cell is decided by Sema after resolving the receiver's
/// type, using this registry to test method names.
///
/// ─── Design: fixed names, compile-time known ──────────────────────────────
/// The set of built-in method names is closed (grammar §7.1, §7.2, §7.4,
/// §8). It is spelled out in a static table and never changes at runtime.
/// Adding a method means: add an entry here, teach Sema how to typecheck
/// and lower it, and update the grammar's tables. No runtime registration
/// path exists — the standard library cannot introduce new built-in
/// method names, only new free functions and new host-backed types.
///
/// ─── Design: ALLCAPS names ────────────────────────────────────────────────
/// The grammar's §7.1 states the naming convention: built-in methods are
/// spelled in ALLCAPS with no exceptions (`ADD`, `REMOVE`, `CLEAR`,
/// `SHRINK`, `AT`, `COUNT`, `VERSION`, `FIND`, `SORT`, `CONTAINS`, `SUM`,
/// `AVG`, `TOARRAY`). The uppercase spelling keeps built-in methods from
/// colliding with the other names reachable after a table's dot: columns
/// are lowercase or camelCase (`Person.count`), fixed-table members are
/// PascalCase (`Direction.North`), and identifiers are case-sensitive
/// (§2.3). A column or fixed-table member whose name equals a built-in
/// method name is a semantic error; the collision is detected by Sema,
/// which knows the receiver's columns and members.
///
/// Method names are matched by exact spelling. `ADD` is a method; `add`
/// is not. The registry stores the names as they appear in source and
/// matches them exactly.
///
/// ─── Design: `by<Column>` is a shape, not a name ──────────────────────────
/// `T.by<Column>(value)` is generated per `@primary` column (§4.1.5).
/// The method name is `by` plus the column's name with its first letter
/// uppercased; the set of such names is not enumerable in advance. The
/// registry exposes `isPrimaryLookupName` and `parsePrimaryLookupName` so
/// Sema can recognize the `by...` shape and delegate to the table's
/// primary column. The registry does not need an entry for every
/// possible `byXxx` name.
///
/// ─── Design: argument shape is an enum, not a list of types ───────────────
/// A method's arguments are of a small number of shapes:
///
///   - None:                `T.CLEAR()`, `arr.COUNT()`, `view.SUM()`
///   - OneIndex:            `T.REMOVE(i)`, `T.AT(i)`, `arr.REMOVE(i)`
///   - OneElement:          `arr.CONTAINS(x)`, `arr.ADD(x)`
///   - OnePredicate:        `T.FIND(pred)`
///   - AddValue:            `T.ADD(a, b, c)` — one argument per column for
///                          a table, one element for an array
///   - ZeroOrOneComparator: `arr.SORT()` or `arr.SORT(less)`
///   - PrimaryKey:          `T.by<Column>(value)`
///
/// The registry records the shape; Sema resolves the specific types from
/// the receiver.

#pragma once

#include "core/ast/BaseAST.hpp"

#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// ReceiverKind
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The category of object a method applies to.
enum class ReceiverKind : uint8_t {
    /// A table sheet (`Person.ADD(...)`, `Person.FIND(...)`).
    TableSheet,

    /// A column view (`Person.age.SUM()`, `Person.age.TOARRAY()`).
    /// A column view is not a first-class value (§5.6), so its methods
    /// appear only in the positions where a column view is legal.
    ColumnView,

    /// A dynamic array (`arr.ADD(x)`, `arr.SORT()`).
    DynamicArray,

    /// A fixed-size array (`arr.SORT()`, `arr.CONTAINS(x)`).
    FixedArray,
};

// ─────────────────────────────────────────────────────────────────────────────
// MethodArgShape
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The shape of a built-in method's argument list.
///
/// The registry records the shape; Sema resolves the specific argument
/// types from the receiver's type.
enum class MethodArgShape : uint8_t {
    /// No arguments. `T.CLEAR()`, `arr.COUNT()`, `view.SUM()`.
    None,

    /// One index argument (an unsigned integer). `T.REMOVE(i)`,
    /// `T.AT(i)`, `arr.REMOVE(i)`.
    OneIndex,

    /// One value of the receiver's element type. `arr.CONTAINS(x)`,
    /// `arr.ADD(x)` (for an array; a table's `ADD` is `AddValue`).
    OneElement,

    /// One predicate `(Elem) -> bool`. `T.FIND(pred)`.
    OnePredicate,

    /// One argument per column, in column order, for a table sheet;
    /// one argument of the element type for a dynamic array. The
    /// receiver kind determines which. `T.ADD(a, b, c)` on a table
    /// with three columns; `arr.ADD(x)` on a `[T]`.
    AddValue,

    /// Zero or one comparator. `arr.SORT()` uses the natural order;
    /// `arr.SORT(less)` takes a `(T, T) -> bool`.
    ZeroOrOneComparator,

    /// One value of the receiver's primary key type. `T.by<Column>(value)`.
    PrimaryKey,
};

// ─────────────────────────────────────────────────────────────────────────────
// MethodResultShape
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The category of value a built-in method produces.
///
/// Sema uses this to decide how to typecheck the surrounding expression.
/// The concrete type depends on the receiver; the registry records only
/// the category.
enum class MethodResultShape : uint8_t {
    /// The method returns nothing (`unit`). `T.REMOVE(i)`, `arr.SORT()`.
    Unit,

    /// The method returns a row reference into the receiver's table.
    /// `T.ADD(...)`, `T.AT(i)`, `T.by<Column>(value)`.
    RowRef,

    /// The method returns a primitive derived from the receiver's
    /// element type. `T.COUNT()`, `T.VERSION()`, `view.SUM()`,
    /// `view.AVG()`.
    Primitive,

    /// The method returns `bool`. A specialization of `Primitive` for
    /// methods whose result type is fixed. `arr.CONTAINS(x)`.
    Bool,

    /// The method returns a live view of the receiver's table.
    /// `T.FIND(pred)`.
    View,

    /// The method returns a `[T]` copy derived from the receiver.
    /// `view.TOARRAY()`.
    ArrayCopy,
};

// ─────────────────────────────────────────────────────────────────────────────
// BuiltinMethodInfo
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Information about one built-in method.
struct BuiltinMethodInfo {
    /// The method's name, as it appears after `.`. ALLCAPS by convention.
    std::string_view name;

    /// The receiver kinds the method applies to.
    std::initializer_list<ReceiverKind> receivers;

    /// The shape of the method's argument list.
    MethodArgShape argShape = MethodArgShape::None;

    /// The category of the method's result.
    MethodResultShape resultShape = MethodResultShape::Unit;
};

// ─────────────────────────────────────────────────────────────────────────────
// BuiltinMethodRegistry
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The fixed table of built-in methods on tables, column views,
///        and arrays.
class BuiltinMethodRegistry {
public:
    BuiltinMethodRegistry();

    BuiltinMethodRegistry(const BuiltinMethodRegistry&)            = default;
    BuiltinMethodRegistry& operator=(const BuiltinMethodRegistry&) = default;
    BuiltinMethodRegistry(BuiltinMethodRegistry&&)                 = default;
    BuiltinMethodRegistry& operator=(BuiltinMethodRegistry&&)      = default;

    // ─── Lookup ──────────────────────────────────────────────────────────

    /// The info for a method, or nullptr if the name is not a recognized
    /// method.
    const BuiltinMethodInfo* getInfo(std::string_view name) const;

    /// True if the name is a recognized method on any receiver.
    bool isRegistered(std::string_view name) const {
        return getInfo(name) != nullptr;
    }

    /// True if the method applies to the given receiver kind.
    bool isForReceiver(std::string_view name, ReceiverKind kind) const;

    // ─── `by<Column>` ────────────────────────────────────────────────────
    //
    // The `by<Column>` name is not enumerable: it is `by` plus a column's
    // name with its first letter uppercased. Sema checks the shape with
    // `isPrimaryLookupName`, then resolves the column against the
    // receiver's `@primary` column using the extracted suffix.

    /// True if `name` has the `by<Column>` shape — starts with `by`,
    /// followed by at least one character. Does not confirm that the
    /// receiver has a matching primary column; Sema does that.
    static bool isPrimaryLookupName(std::string_view name);

    /// Given a name of the `by<Column>` shape, returns the column name
    /// it was generated from. The suffix is `name.substr(2)`; the
    /// caller lowercases the first character to obtain the column's
    /// declared spelling.
    ///
    /// Precondition: `isPrimaryLookupName(name)` is true. The return
    /// value is a view into `name`.
    static std::string_view primaryLookupColumnName(std::string_view name);

    // ─── Enumeration ────────────────────────────────────────────────────

    /// The names of all registered methods, sorted lexicographically.
    /// Does not include generated `by<Column>` names.
    std::vector<std::string_view> getAllNames() const;

    /// The number of registered methods.
    size_t size() const noexcept { return m_methods.size(); }

private:
    /// The lookup table. Keyed by the method's spelling. The keys point
    /// into the static table's string_views, which have static storage
    /// duration.
    std::unordered_map<std::string_view, const BuiltinMethodInfo*> m_methods;
};