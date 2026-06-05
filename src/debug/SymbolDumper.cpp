#include "SymbolDumper.hpp"
#include "DebugUtils.hpp"
#include "debug/DebugMacros.hpp"
#include "semantic/SemanticSymbol.hpp"
#include "ast/TypeAST.hpp"
#include "ast/support/StringPool.hpp"
#include <sstream>

namespace LucDebug {

namespace {

std::string getIndent(int level) {
    std::string result;
    for (int i = 0; i < level; ++i) result += "  ";
    return result;
}

std::string symbolKindToString(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::Module:      return "module";
        case SymbolKind::Var:         return "var";
        case SymbolKind::Func:        return "func";
        case SymbolKind::ExternFunc:  return "extern_func";
        case SymbolKind::Struct:      return "struct";
        case SymbolKind::Enum:        return "enum";
        case SymbolKind::Trait:       return "trait";
        case SymbolKind::TypeAlias:   return "typealias";
        case SymbolKind::Param:       return "param";
        case SymbolKind::Field:       return "field";
        case SymbolKind::Method:      return "method";
        case SymbolKind::EnumVariant: return "enum_variant";
        case SymbolKind::Casting:     return "casting";
        case SymbolKind::Impl:        return "impl";
        default:                      return "unknown";
    }
}

std::string visibilityToString(Visibility vis) {
    switch (vis) {
        case Visibility::Private: return "private";
        case Visibility::Package: return "pub";
        case Visibility::Export:  return "export";
        default:                  return "";
    }
}

// Simplified type printer (avoids full TypeAST formatting)
std::string typeToString(const TypeAST* type, const StringPool& pool) {
    if (!type) return "<none>";
    // You can reuse formatType from ASTDumper or implement a simple one here.
    // For brevity, we just show the kind.
    return kindToString(type->kind);  // requires including DebugUtils.hpp
}

std::string locationToString(SourceLocation loc) {
    return "line " + std::to_string(loc.line()) + ", col " + std::to_string(loc.column());
}

void dumpScope(std::stringstream& ss, const std::unordered_map<uint32_t, Symbol>& scope,
               int depth, const StringPool& pool) {
    if (scope.empty()) return;

    for (const auto& [id, sym] : scope) {
        std::string_view name = pool.lookup(sym.name);
        ss << getIndent(depth) << "- " << symbolKindToString(sym.kind) << " '" << name << "'";
        if (sym.type) {
            ss << " : " << typeToString(sym.type, pool);
        }
        if (sym.visibility != Visibility::Private) {
            ss << " (" << visibilityToString(sym.visibility) << ")";
        }
        if (sym.isExtern) {
            ss << " [extern";
            if (sym.externSymbol.isValid()) {
                ss << " symbol=" << pool.lookup(sym.externSymbol);
            }
            ss << "]";
        }
        ss << " at " << locationToString(sym.loc) << "\n";
    }
}

} // anonymous namespace

std::string dumpSymbolTable(const SymbolTable& table, const StringPool& pool, int /*verbosity*/) {
    std::stringstream ss;

    // Access internal scopes – we need a const view. If SymbolTable doesn't provide
    // a const getter, add a method `const std::deque<...>& getScopes() const` or
    // implement iteration logic inside SymbolTable.
    // For now, assuming we add a friend or a const accessor.

    const auto& scopes = table.getScopes();
    ss << "=== Symbol Table (depth=" << scopes.size() << ") ===\n";

    for (size_t i = 0; i < scopes.size(); ++i) {
        ss << getIndent(i) << "Scope[" << i << "] (" << scopes[i].size() << " symbols):\n";
        dumpScope(ss, scopes[i], i + 1, pool);
    }
    ss << "=== End Symbol Table ===\n";
    return ss.str();
}

} // namespace LucDebug