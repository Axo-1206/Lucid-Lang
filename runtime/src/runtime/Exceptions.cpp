/**
 * @file runtime/Exceptions.cpp
 *
 * @responsibility The out-of-line definitions for the runtime's
 *                 exception types. The exception hierarchy has a
 *                 vtable anchor here so its typeinfo is emitted in
 *                 exactly one translation unit (some linkers require
 *                 this for RTTI to work across shared libraries).
 *
 * ─── Design: no members are out-of-line ───────────────────────────────────
 * All the exceptions' members are defined inline in the header. This
 * file exists to give the base class a single key function (a virtual
 * member with a non-inline definition) — the destructor, defined
 * out-of-line here — so the vtable is emitted once.
 */

#include "runtime/Exceptions.hpp"

namespace lucid::runtime {

// Force the vtable and typeinfo of RuntimeException to be emitted in
// this translation unit. A virtual destructor is the conventional key
// function; the compiler places the vtable wherever the key function
// is defined.
RuntimeException::~RuntimeException() = default;

} // namespace lucid::runtime