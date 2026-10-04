/**
 * @file runtime/Panic.cpp
 *
 * @responsibility Panic has no non-inline functions of its own; the
 *                 constructor and destructor are implicit. This file
 *                 exists to give the runtime library a translation
 *                 unit for the Panic module and to host any future
 *                 out-of-line helpers (a formatting function, a
 *                 serialization function).
 */

#include "runtime/Panic.hpp"

namespace lucid::runtime {

// Intentionally empty. Panic is a plain aggregate; its construction
// and destruction are the implicit ones. This file is here so the
// build has a translation unit for the header, and so future
// out-of-line functions (formatting, comparisons) have a home.

} // namespace lucid::runtime