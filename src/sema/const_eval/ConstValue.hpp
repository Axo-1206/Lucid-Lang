#pragma once

#include "core/ast/BaseAST.hpp"

namespace sema {

struct ConstantValue {
    enum class Kind : uint8_t {
        Unknown,    ///< Not yet evaluated
        Error,      ///< Evaluation failed
        Void,       ///< No value (void function)
        Bool,       ///< true/false
        Int,        ///< Integer (any size)
        Float,      ///< Floating point (any precision)
        String,     ///< String literal
        Char,       ///< Character literal
        Enum,       ///< Enum variant
        Struct,     ///< Struct value
        Array,      ///< Array value
        Function,   ///< Const function pointer (for later calls)
        Nil,        ///< nil sentinel
        Err,        ///< err sentinel
    };

    Kind kind = Kind::Unknown;
    TypeAST* type = nullptr;

    // ─── Value Storage ────────────────────────────────────────────────
    // Using variant to store different value types efficiently
    std::variant<
        std::monostate,                                              // Unknown, Error, Void
        bool,                                                        // Bool
        int64_t,                                                     // Int
        double,                                                      // Float
        InternedString,                                              // String, Char, Enum
        std::vector<ConstantValue>,                                  // Array
        std::unordered_map<InternedString, ConstantValue>,           // Struct
        FuncDeclAST*                                          // Function
    > value;

    // ─── Constructors ──────────────────────────────────────────────────

    ConstantValue() : kind(Kind::Unknown) {}

    explicit ConstantValue(bool v) : kind(Kind::Bool), value(v) {}

    explicit ConstantValue(int64_t v) : kind(Kind::Int), value(v) {}

    explicit ConstantValue(double v) : kind(Kind::Float), value(v) {}

    explicit ConstantValue(InternedString v) : kind(Kind::String), value(v) {}

    explicit ConstantValue(FuncDeclAST* f) : kind(Kind::Function), value(f) {}

    // ─── Factory Methods ──────────────────────────────────────────────

    static ConstantValue nil() {
        ConstantValue v;
        v.kind = Kind::Nil;
        return v;
    }

    static ConstantValue err() {
        ConstantValue v;
        v.kind = Kind::Err;
        return v;
    }

    static ConstantValue error() {
        ConstantValue v;
        v.kind = Kind::Error;
        return v;
    }

    static ConstantValue voidValue() {
        ConstantValue v;
        v.kind = Kind::Void;
        return v;
    }

    static ConstantValue unknown() {
        return ConstantValue();
    }

    // ─── Predicates ────────────────────────────────────────────────────

    bool isEvaluated() const {
        return kind != Kind::Unknown && kind != Kind::Error;
    }

    bool isError() const {
        return kind == Kind::Error;
    }

    bool isUnknown() const {
        return kind == Kind::Unknown;
    }

    bool isBool() const { return kind == Kind::Bool; }
    bool isInt() const { return kind == Kind::Int; }
    bool isFloat() const { return kind == Kind::Float; }
    bool isString() const { return kind == Kind::String; }
    bool isChar() const { return kind == Kind::Char; }
    bool isVoid() const { return kind == Kind::Void; }
    bool isFunction() const { return kind == Kind::Function; }
    bool isNil() const { return kind == Kind::Nil; }
    bool isErr() const { return kind == Kind::Err; }
    bool isStruct() const { return kind == Kind::Struct; }
    bool isArray() const { return kind == Kind::Array; }
    bool isEnum() const { return kind == Kind::Enum; }

    // ─── Accessors ────────────────────────────────────────────────────

    bool asBool() const {
        return std::get<bool>(value);
    }

    int64_t asInt() const {
        return std::get<int64_t>(value);
    }

    double asFloat() const {
        return std::get<double>(value);
    }

    InternedString asString() const {
        return std::get<InternedString>(value);
    }

    FuncDeclAST* asFunction() const {
        return std::get<FuncDeclAST*>(value);
    }

    const std::vector<ConstantValue>& asArray() const {
        return std::get<std::vector<ConstantValue>>(value);
    }

    const std::unordered_map<InternedString, ConstantValue>& asStruct() const {
        return std::get<std::unordered_map<InternedString, ConstantValue>>(value);
    }

    // ─── Mutating Accessors ──────────────────────────────────────────

    std::vector<ConstantValue>& asArrayMut() {
        return std::get<std::vector<ConstantValue>>(value);
    }

    std::unordered_map<InternedString, ConstantValue>& asStructMut() {
        return std::get<std::unordered_map<InternedString, ConstantValue>>(value);
    }

    // ─── Comparison ───────────────────────────────────────────────────

    bool operator==(const ConstantValue& other) const {
        if (kind != other.kind) return false;
        if (type != other.type) return false;
        return value == other.value;
    }

    bool operator!=(const ConstantValue& other) const {
        return !(*this == other);
    }
};

} // namespace sema