/**
 * @file core/PrimitiveKind.hpp
 *
 * @responsibility The primitive-kind enum and its pure predicates.
 *                 Extracted from core/ast/TypeAST.hpp so that
 *                 bytecode/ can name a primitive kind without including
 *                 the AST header tree.
 *
 * ─── Why this file exists ─────────────────────────────────────────────────
 * PrimitiveKind is a fact about the language, not about the AST. It was
 * defined in TypeAST.hpp because that was the first file that needed it.
 * Both the AST and the bytecode artifact need to name primitive kinds;
 * if the definition lives only in TypeAST.hpp, bytecode/ has to include
 * the AST header to use it, and the artifact layer ends up depending on
 * the frontend. Moving the enum (and its predicates) here lets both
 * sides include one small header and nothing else.
 *
 * ArrayKind gets the same treatment in core/ArrayKind.hpp.
 */

#pragma once

#include <cstddef>
#include <cstdint>

enum class PrimitiveKind : uint8_t {
    Bool,
    Char,
    String,
    Void,

    Int8,
    Int16,
    Int32,
    Int64,

    Uint8,
    Uint16,
    Uint32,
    Uint64,

    Float32,
    Float64,
};

inline size_t primitiveBitWidth(PrimitiveKind kind) noexcept {
    switch (kind) {
        case PrimitiveKind::Int8:
        case PrimitiveKind::Uint8:
            return 8;
        case PrimitiveKind::Int16:
        case PrimitiveKind::Uint16:
            return 16;
        case PrimitiveKind::Int32:
        case PrimitiveKind::Uint32:
        case PrimitiveKind::Float32:
            return 32;
        case PrimitiveKind::Int64:
        case PrimitiveKind::Uint64:
        case PrimitiveKind::Float64:
            return 64;
        case PrimitiveKind::Bool:
        case PrimitiveKind::Char:
        case PrimitiveKind::String:
        case PrimitiveKind::Void:
            return 0;
    }
    return 0;
}

inline bool isSignedIntegerKind(PrimitiveKind kind) noexcept {
    switch (kind) {
        case PrimitiveKind::Int8:
        case PrimitiveKind::Int16:
        case PrimitiveKind::Int32:
        case PrimitiveKind::Int64:
            return true;
        default:
            return false;
    }
}

inline bool isUnsignedIntegerKind(PrimitiveKind kind) noexcept {
    switch (kind) {
        case PrimitiveKind::Uint8:
        case PrimitiveKind::Uint16:
        case PrimitiveKind::Uint32:
        case PrimitiveKind::Uint64:
            return true;
        default:
            return false;
    }
}

inline bool isIntegerKind(PrimitiveKind kind) noexcept {
    return isSignedIntegerKind(kind) || isUnsignedIntegerKind(kind);
}

inline bool isFloatKind(PrimitiveKind kind) noexcept {
    return kind == PrimitiveKind::Float32 || kind == PrimitiveKind::Float64;
}

inline bool isNumericKind(PrimitiveKind kind) noexcept {
    return isIntegerKind(kind) || isFloatKind(kind);
}
