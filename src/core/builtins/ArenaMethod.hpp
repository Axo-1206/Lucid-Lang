/// @file core/builtins/ArenaMethod.hpp
/// @brief Arena method enumeration - minimal built-in type definition.
///
/// This is the only remnant of the old BuiltinTypes module.
/// All type predicates have been moved to SemaType.hpp/cpp.

#pragma once

#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"
#include <optional>
#include <string_view>

namespace builtins {

/// @brief All valid Arena method names.
enum class ArenaMethodKind {
    Create,     // static: Arena::create(size) -> Arena!
    Empty,      // static: Arena::empty() -> Arena
    Alloc,      // instance: arena::alloc<T>(count) -> [_]T
    Reset,      // instance: arena::reset() -> ()
    Descriptor, // instance: arena::descriptor() -> ArenaDescriptor
    Capacity,   // instance: arena::capacity() -> uint64
    Remaining,  // instance: arena::remaining() -> uint64
    IsEmpty,    // instance: arena::isEmpty() -> bool
    Space,      // instance: arena::space<T>() -> uint64
    CanFit,     // instance: arena::canFit<T>(n) -> bool
};

/// @brief Parse an arena method name from an interned string.
inline std::optional<ArenaMethodKind> parseArenaMethod(InternedString name, StringPool& pool) {
    std::string_view sv = pool.lookupView(name);
    
    if (sv == "create")    return ArenaMethodKind::Create;
    if (sv == "empty")     return ArenaMethodKind::Empty;
    if (sv == "alloc")     return ArenaMethodKind::Alloc;
    if (sv == "reset")     return ArenaMethodKind::Reset;
    if (sv == "descriptor") return ArenaMethodKind::Descriptor;
    if (sv == "capacity")  return ArenaMethodKind::Capacity;
    if (sv == "remaining") return ArenaMethodKind::Remaining;
    if (sv == "isEmpty")   return ArenaMethodKind::IsEmpty;
    if (sv == "space")     return ArenaMethodKind::Space;
    if (sv == "canFit")    return ArenaMethodKind::CanFit;
    
    return std::nullopt;
}

/// @brief Check if an Arena method requires a generic argument.
inline bool arenaMethodRequiresGenericArg(ArenaMethodKind method) {
    return method == ArenaMethodKind::Alloc ||
           method == ArenaMethodKind::Space ||
           method == ArenaMethodKind::CanFit;
}

/// @brief Check if an Arena method is static.
inline bool isArenaMethodStatic(ArenaMethodKind method) {
    return method == ArenaMethodKind::Create ||
           method == ArenaMethodKind::Empty;
}

/// @brief Check if an Arena method returns void.
inline bool isArenaMethodVoid(ArenaMethodKind method) {
    return method == ArenaMethodKind::Reset;
}

} // namespace builtins