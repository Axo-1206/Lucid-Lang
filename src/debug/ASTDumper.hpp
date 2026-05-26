#pragma once

#include "ast/BaseAST.hpp"
#include "ast/support/StringPool.hpp"
#include <string>

namespace LucDebug {

/**
 * @brief Dump an AST node to a human-readable string.
 * 
 * Returns a formatted string representation of the entire AST.
 * The output is indented and shows node types, kinds, and source locations.
 * 
 * @param node The root AST node to dump (can be null).
 * @param pool StringPool for looking up interned strings.
 * @param verbosity Currently unused, reserved for future detail levels.
 * @return A multi-line string containing the AST dump.
 */
std::string dumpAST(const BaseAST* node, const StringPool& pool, int verbosity = 0);

} // namespace LucDebug