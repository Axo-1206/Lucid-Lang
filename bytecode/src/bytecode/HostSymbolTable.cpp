/// @file bytecode/HostSymbolTable.cpp
/// @brief The deduplicated name list for host(...) references.

#include "bytecode/HostSymbolTable.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

namespace lucid::bytecode {

// ─────────────────────────────────────────────────────────────────────────────
// Key encoding
// ─────────────────────────────────────────────────────────────────────────────
//
// The dedup map is keyed by a string that combines the kind byte and
// the name. The kind byte is placed first so a function and a type
// with the same name are two distinct entries. The separator byte
// (0x01) is chosen because it cannot appear in a Lucid identifier and
// is extremely unlikely in a host name, so the concatenation is
// unambiguous in practice. (A host name with a literal 0x01 in it
// would be pathological; if it ever happens, the entry is still
// deduplicated correctly because the kind byte is a fixed prefix.)

namespace {

std::string makeKey(HostSymbol::Kind kind, std::string_view name) {
    std::string key;
    key.reserve(1 + name.size());
    key.push_back(static_cast<char>(kind));
    key.append(name.data(), name.size());
    return key;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

uint32_t HostSymbolTable::add(HostSymbol sym) {
    AST_ASSERT_MSG(!sym.name.empty(),
        "HostSymbolTable::add: host symbol name is empty — "
        "the compiler emitted a host reference with no name");

    const std::string key = makeKey(sym.kind, sym.name);

    auto it = m_index.find(key);
    if (it != m_index.end()) {
        return it->second;
    }

    const uint32_t index = static_cast<uint32_t>(m_symbols.size());
    m_symbols.push_back(std::move(sym));
    m_index.emplace(std::move(key), index);
    return index;
}

const HostSymbol& HostSymbolTable::at(uint32_t index) const {
    AST_ASSERT_MSG(index < m_symbols.size(),
        "HostSymbolTable::at: index out of range — "
        "the caller assumed a symbol index that the table does not have");
    return m_symbols[index];
}

std::optional<uint32_t> HostSymbolTable::find(HostSymbol::Kind kind,
                                              std::string_view name) const {
    const std::string key = makeKey(kind, name);
    auto it = m_index.find(key);
    if (it == m_index.end()) return std::nullopt;
    return it->second;
}

void HostSymbolTable::checkInvariants() const {
    AST_ASSERT_MSG(m_symbols.size() == m_index.size(),
        "HostSymbolTable::checkInvariants: symbol count and index count "
        "disagree — the table was mutated without going through add()");

    for (size_t i = 0; i < m_symbols.size(); ++i) {
        const HostSymbol& sym = m_symbols[i];
        AST_ASSERT_MSG(!sym.name.empty(),
            "HostSymbolTable::checkInvariants: an entry has an empty name");

        const std::string key = makeKey(sym.kind, sym.name);
        auto it = m_index.find(key);
        AST_ASSERT_MSG(it != m_index.end(),
            "HostSymbolTable::checkInvariants: an entry is not in the index");
        AST_ASSERT_MSG(it->second == i,
            "HostSymbolTable::checkInvariants: an entry's index does not "
            "match its position in the symbol list");
    }
}

} // namespace lucid::bytecode