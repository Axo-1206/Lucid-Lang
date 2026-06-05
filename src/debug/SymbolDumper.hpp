#pragma once

#include "ast/support/StringPool.hpp"
#include "semantic/SymbolTable.hpp"
#include <string>

namespace LucDebug {

/**
 * @brief Dump the entire symbol table to a string in a tree-like format.
 *
 * Produces output similar to ASTDumper: each scope is indented, symbols
 * are shown with their kind, name, type, visibility, and source location.
 *
 * @param table The symbol table to dump.
 * @param pool  String pool used to resolve interned names.
 * @param verbosity Currently unused (reserved).
 * @return A multi-line string containing the dump.
 */
std::string dumpSymbolTable(const SymbolTable& table, const StringPool& pool, int verbosity = 0);

} // namespace LucDebug