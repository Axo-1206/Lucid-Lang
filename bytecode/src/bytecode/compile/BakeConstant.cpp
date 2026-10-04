/// @file bytecode/compile/BakeConstant.cpp
/// @brief Translate a folded ConstantValue into a serializable Constant.

#include "BakeConstant.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG
#include "core/memory/StringPool.hpp"

using namespace lucid::contract;

namespace lucid::bytecode::compile {

// ─────────────────────────────────────────────────────────────────────────────
// Translation
// ─────────────────────────────────────────────────────────────────────────────
//
// This function is a pure translation: it reads a ConstantValue that
// Sema has already folded and produces a Constant that the artifact
// can hold. It does not evaluate, does not fold, does not traverse
// beyond the recursion that the value's structure requires.
//
// The function asserts on the two sentinel kinds (Unknown, Error).
// Reaching either means Sema did not do its job: a value that was not
// folded, or a value whose folding failed, arrived at the compiler.
// The compiler's contract is that it only runs on Sema-validated
// modules, and Sema does not produce these sentinels for a value whose
// expression is marked isConst. So a sentinel here is a Sema bug.

namespace {

Constant::Kind translateKind(ConstantValue::Kind k) {
    switch (k) {
        case ConstantValue::Kind::Bool:     return Constant::Kind::Bool;
        case ConstantValue::Kind::Int:      return Constant::Kind::Int;
        case ConstantValue::Kind::Float:    return Constant::Kind::Float;
        case ConstantValue::Kind::String:   return Constant::Kind::String;
        case ConstantValue::Kind::Char:     return Constant::Kind::Char;
        case ConstantValue::Kind::Nil:      return Constant::Kind::Nil;
        case ConstantValue::Kind::Array:    return Constant::Kind::Array;
        case ConstantValue::Kind::Function: return Constant::Kind::Function;

        case ConstantValue::Kind::Unknown:
        case ConstantValue::Kind::Error:
        case ConstantValue::Kind::Void:
            // A value with one of these kinds reached the compiler.
            // The asserts below name the specific case.
            break;
    }
    return Constant::Kind::Nil;   // unreachable after the asserts
}

} // namespace

Constant bakeConstant(StringPool& pool,
                      const ConstantValue& value,
                      const TypeDescriptor& type,
                      uint32_t functionIndexForFunctionKind) {
    // ─── Preconditions ─────────────────────────────────────────────────
    AST_ASSERT_MSG(value.kind != ConstantValue::Kind::Unknown,
        "BakeConstant: value is unevaluated — Sema should have folded "
        "this expression before the compiler saw it");
    AST_ASSERT_MSG(value.kind != ConstantValue::Kind::Error,
        "BakeConstant: value is an evaluation error — Sema should have "
        "reported a diagnostic and refused to compile this module");
    AST_ASSERT_MSG(value.kind != ConstantValue::Kind::Void,
        "BakeConstant: value has kind Void — a void value cannot be a "
        "constant; Sema should not have marked this expression isConst");

    // ─── Translate ─────────────────────────────────────────────────────
    Constant out;
    out.kind = translateKind(value.kind);
    out.type = type;

    switch (value.kind) {
        case ConstantValue::Kind::Bool:
            out.value = value.asBool();
            break;

        case ConstantValue::Kind::Int:
            out.value = value.asInt();
            break;

        case ConstantValue::Kind::Float:
            out.value = value.asFloat();
            break;

        case ConstantValue::Kind::String:
            out.value = pool.lookup(value.asString());
            break;

        case ConstantValue::Kind::Char:
            // A char is stored as a one-character string. The
            // ConstantValue::Kind::Char case holds an InternedString
            // that is exactly one character (the lexer guarantees it).
            out.value = pool.lookup(value.asString());
            break;

        case ConstantValue::Kind::Nil:
            out.value = std::monostate{};
            break;

        case ConstantValue::Kind::Array: {
            // Recursively translate each element. The element type is
            // the array type's component; if the array has no element
            // type (an empty array folded with no target type — which
            // Sema should not have allowed), the element type is
            // Unknown and the assert in the recursive call fires.
            std::vector<Constant> elems;
            const auto& srcElems = value.asArray();
            elems.reserve(srcElems.size());

            const TypeDescriptor* elemType = nullptr;
            if (type.isArray() && type.component) {
                elemType = type.component.get();
            }

            for (const auto& srcElem : srcElems) {
                AST_ASSERT_MSG(elemType != nullptr,
                    "BakeConstant: an array constant has elements but "
                    "its type does not carry an element type — Sema "
                    "should have resolved the array's element type");
                elems.push_back(bakeConstant(pool, srcElem, *elemType,
                                             functionIndexForFunctionKind));
            }
            out.value = std::move(elems);
            break;
        }

        case ConstantValue::Kind::Function: {
            // The value holds an FnDeclAST*. The caller resolved it to
            // a FunctionProto index and passed it in. We do not
            // re-derive the index from the AST — the compiler already
            // has it.
            //
            // A precondition: the caller must pass a valid index for a
            // Function-kind value. Passing a sentinel (such as
            // UINT32_MAX) when the value is not a function would be a
            // caller bug; the assert below catches the case where the
            // caller passed a sentinel for a value that *is* a function.
            AST_ASSERT_MSG(functionIndexForFunctionKind != UINT32_MAX,
                "BakeConstant: value is a function but the caller passed "
                "the sentinel function index — the caller must resolve "
                "the function's index before calling bakeConstant");
            out.value = functionIndexForFunctionKind;
            break;
        }

        case ConstantValue::Kind::Unknown:
        case ConstantValue::Kind::Error:
        case ConstantValue::Kind::Void:
            // Already asserted above; unreachable.
            AST_ASSERT_MSG(false,
                "BakeConstant: unreachable — the preconditions above "
                "should have fired");
            break;
    }

    return out;
}

} // namespace lucid::bytecode::compile