/**
 * @file contract/Signature.hpp
 *
 * @responsibility The serializable form of a function signature.
 *                 Extracted from TypeDescriptor.hpp so the concept has
 *                 a name.
 *
 * ─── Why this is its own header ───────────────────────────────────────────
 * A function signature is the shape of a function: its parameter types
 * and its return type. It appears in three places:
 *   - FunctionProto::signature  (the compiled function's shape)
 *   - HostRegistry::registerFunction  (a host function's shape)
 *   - FunctionRef::signatureIndex  (a function value's shape)
 *
 * Every consumer reads TypeDescriptor transitively, but naming the
 * signature separately makes the dependency explicit: a host registry
 * that stores a signature is depending on "the artifact's function
 * signature shape," not "the whole type descriptor module."
 */

#pragma once

#include "TypeDescriptor.hpp"

#include <vector>

namespace lucid::contract {

/// @brief A serializable function signature. Matches FunctionTypeAST.
struct FunctionSignature {
    std::vector<TypeDescriptor> params;
    TypeDescriptor              returnType;
};

} // namespace lucid::contract