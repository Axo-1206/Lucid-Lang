/// @file SemaContext.cpp
/// @brief Implementation of SemaContext and its RAII guards.
///
/// ─── Insertion ────────────────────────────────────────────────────────────
/// There are four insertion methods, one per form a name can take:
/// `insertTable`, `insertFunction`, `insertVariable`, `insertLocal`, plus
/// `insertType` for the type-namespace half of a table.
///
/// `insertTable` writes the table's name into both the `tables` and the
/// `types` maps. The other three write to one map each.
///
/// ─── Lookup ───────────────────────────────────────────────────────────────
/// Three specific lookups (`lookupTable`, `lookupFunction`,
/// `lookupVariable`) return the concrete declaration pointer or null.
/// `lookupValue` is the polymorphic form: it walks the scope chain, then
/// the module's `variables`, `functions`, and `tables` maps, and reports
/// which one held the name.

#include "SemaContext.hpp"

#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"

#include <cassert>

using namespace lucid::diag;

namespace lucid::sema {

// ─── Construction ─────────────────────────────────────────────────────────

SemaContext::SemaContext(StringPool& p, ASTArena& a, DiagnosticEngine& d)
    : pool(p)
    , arena(a)
    , diagnostics(d) {}

// ─── Module management ────────────────────────────────────────────────────

void SemaContext::addModule(ModuleAST* module) {
    if (!module) return;
    modules.push_back(module);
    if (module->filePath.isValid()) {
        modulesByPath[module->filePath] = module;
    }
}

void SemaContext::enterModule(ModuleAST* module) {
    currentModule      = module;
    currentModuleTable = &getOrCreateModuleTable(module);
}

ModuleTable& SemaContext::getOrCreateModuleTable(ModuleAST* module) {
    auto it = moduleTables.find(module);
    if (it != moduleTables.end()) return it->second;

    ModuleTable& table = moduleTables[module];
    table.module = module;
    return table;
}

ModuleTable* SemaContext::findModuleTable(ModuleAST* module) {
    auto it = moduleTables.find(module);
    return it != moduleTables.end() ? &it->second : nullptr;
}

ModuleAST* SemaContext::findModuleByPath(InternedString path) const {
    auto it = modulesByPath.find(path);
    return it != modulesByPath.end() ? it->second : nullptr;
}

// ─── Scope management ─────────────────────────────────────────────────────

bool SemaContext::isAtModuleLevel() const {
    return scopes.empty();
}

void SemaContext::pushScope() {
    scopes.emplace_back();
}

void SemaContext::popScope() {
    if (!scopes.empty()) scopes.pop_back();
}

Scope& SemaContext::currentScope() {
    assert(!scopes.empty() && "no scope open");
    return scopes.back();
}

const Scope& SemaContext::currentScope() const {
    assert(!scopes.empty() && "no scope open");
    return scopes.back();
}

// ─── Scope queries ────────────────────────────────────────────────────────

bool SemaContext::isInCurrentScope(InternedString name) const {
    if (scopes.empty()) return false;
    return currentScope().values.find(name) != currentScope().values.end();
}

bool SemaContext::isModuleMember(InternedString name) const {
    if (!currentModuleTable) return false;
    return currentModuleTable->variables.count(name)
        || currentModuleTable->functions.count(name)
        || currentModuleTable->tables.count(name)
        || currentModuleTable->types.count(name);
}

// ─── Symbol insertion ─────────────────────────────────────────────────────

bool SemaContext::insertTable(TableDeclAST* decl) {
    if (!decl) return false;

    AST_ASSERT_MSG(isAtModuleLevel(),
                   "insertTable() called from a non-module scope; "
                   "tables are module-level only");

    if (!currentModuleTable) return false;

    if (currentModuleTable->tables.count(decl->name)
        || currentModuleTable->types.count(decl->name)
        || currentModuleTable->functions.count(decl->name)
        || currentModuleTable->variables.count(decl->name)) {
        diagnostics.error(DiagCode::Name_Redeclaration, decl,
                          "redeclaration of '", pool.lookup(decl->name),
                          "' in module '",
                          pool.lookup(currentModule->filePath), "'");
        return false;
    }

    currentModuleTable->tables[decl->name] = decl;
    currentModuleTable->types[decl->name]  = decl;
    return true;
}

bool SemaContext::insertFunction(FnDeclAST* decl) {
    if (!decl) return false;

    AST_ASSERT_MSG(isAtModuleLevel(),
                   "insertFunction() called from a non-module scope; "
                   "functions are module-level only");

    if (!currentModuleTable) return false;

    if (currentModuleTable->functions.count(decl->name)
        || currentModuleTable->variables.count(decl->name)
        || currentModuleTable->tables.count(decl->name)
        || currentModuleTable->types.count(decl->name)) {
        diagnostics.error(DiagCode::Name_Redeclaration, decl,
                          "redeclaration of '", pool.lookup(decl->name),
                          "' in module '",
                          pool.lookup(currentModule->filePath), "'");
        return false;
    }

    currentModuleTable->functions[decl->name] = decl;
    return true;
}

bool SemaContext::insertVariable(VarDeclAST* decl) {
    if (!decl) return false;

    AST_ASSERT_MSG(isAtModuleLevel(),
                   "insertVariable() called from a non-module scope; "
                   "a top-level variable is a module declaration — a "
                   "local let uses insertLocal()");

    if (!currentModuleTable) return false;

    if (currentModuleTable->variables.count(decl->name)
        || currentModuleTable->functions.count(decl->name)
        || currentModuleTable->tables.count(decl->name)
        || currentModuleTable->types.count(decl->name)) {
        diagnostics.error(DiagCode::Name_Redeclaration, decl,
                          "redeclaration of '", pool.lookup(decl->name),
                          "' in module '",
                          pool.lookup(currentModule->filePath), "'");
        return false;
    }

    currentModuleTable->variables[decl->name] = decl;
    return true;
}

bool SemaContext::insertLocal(ValueDeclAST* decl) {
    if (!decl) return false;

    AST_ASSERT_MSG(!isAtModuleLevel(),
                   "insertLocal() called at module level; a top-level "
                   "declaration uses insertVariable() or "
                   "insertFunction()");

    if (currentScope().values.count(decl->name)) {
        diagnostics.error(DiagCode::Name_Redeclaration, decl,
                          "redeclaration of '", pool.lookup(decl->name),
                          "' in the same scope");
        return false;
    }

    currentScope().values[decl->name] = decl;
    return true;
}

bool SemaContext::insertType(TypeDeclAST* decl) {
    if (!decl) return false;

    AST_ASSERT_MSG(isAtModuleLevel(),
                   "insertType() called from a non-module scope; "
                   "types are module-level only");

    if (!currentModuleTable) return false;

    if (currentModuleTable->types.count(decl->name)) {
        diagnostics.error(DiagCode::Name_Redeclaration, decl,
                          "redeclaration of type '", pool.lookup(decl->name),
                          "' in module '",
                          pool.lookup(currentModule->filePath), "'");
        return false;
    }

    currentModuleTable->types[decl->name] = decl;

    // A TypeDeclAST in the new grammar is always a TableDeclAST. Keep
    // the two maps in sync: if this is a table, it also belongs in the
    // `tables` map so a value lookup finds it. (insertTable is the
    // preferred entry point; insertType is the type-namespace-only
    // path and is called by passes that only want the type half.)
    if (decl->isa<TableDeclAST>()) {
        currentModuleTable->tables[decl->name] = decl->as<TableDeclAST>();
    }
    return true;
}

bool SemaContext::addImportAlias(InternedString alias, ModuleAST* module,
                                 BaseAST* node) {
    if (!currentModuleTable) return false;
    if (currentModuleTable->importAliases.count(alias)) {
        diagnostics.error(DiagCode::Name_ImportAliasRedeclaration,
                          node ? node : module,
                          "redeclaration of import alias '",
                          pool.lookup(alias), "'");
        return false;
    }
    currentModuleTable->importAliases[alias] = module;
    return true;
}

// ─── Symbol lookup — polymorphic ──────────────────────────────────────────

ValueLookup SemaContext::lookupValue(InternedString name) const {
    // ─── 1. Walk the lexical scope chain ────────────────────────────────
    //
    // A local scope holds `ValueDeclAST*` — a `let`/`const` variable or
    // a parameter. The two are told apart by `isa<ParamAST>()`.
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto found = it->values.find(name);
        if (found != it->values.end()) {
            ValueDeclAST* decl = found->second;
            if (decl->isa<ParamAST>()) {
                return ValueLookup::from(decl->as<ParamAST>());
            }
            return ValueLookup::from(decl->as<VarDeclAST>());
        }
    }

    if (!currentModuleTable) return ValueLookup::none();

    // ─── 2. Module's variables ──────────────────────────────────────────
    if (auto found = currentModuleTable->variables.find(name);
        found != currentModuleTable->variables.end()) {
        return ValueLookup::from(found->second);
    }

    // ─── 3. Module's functions ──────────────────────────────────────────
    if (auto found = currentModuleTable->functions.find(name);
        found != currentModuleTable->functions.end()) {
        return ValueLookup::from(found->second);
    }

    // ─── 4. Module's tables (a table used as a value) ───────────────────
    if (auto found = currentModuleTable->tables.find(name);
        found != currentModuleTable->tables.end()) {
        return ValueLookup::from(found->second);
    }

    return ValueLookup::none();
}

// ─── Symbol lookup — per form ─────────────────────────────────────────────

FnDeclAST* SemaContext::lookupFunction(InternedString name) const {
    // A local scope never holds a function; skip it.
    if (!currentModuleTable) return nullptr;
    auto found = currentModuleTable->functions.find(name);
    return found != currentModuleTable->functions.end() ? found->second : nullptr;
}

TableDeclAST* SemaContext::lookupTable(InternedString name) const {
    // A local scope never holds a table; skip it.
    if (!currentModuleTable) return nullptr;
    auto found = currentModuleTable->tables.find(name);
    return found != currentModuleTable->tables.end() ? found->second : nullptr;
}

VarDeclAST* SemaContext::lookupVariable(InternedString name) const {
    // ─── 1. Walk the lexical scope chain ────────────────────────────────
    //
    // A local scope can hold both variables and parameters. A parameter
    // is not a variable; skip it here so the per-form lookup is exactly
    // "a `let`/`const` variable".
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto found = it->values.find(name);
        if (found != it->values.end()) {
            ValueDeclAST* decl = found->second;
            if (decl->isa<VarDeclAST>()) {
                return decl->as<VarDeclAST>();
            }
            // A parameter shadows anything further out. A `lookupVariable`
            // that reaches a parameter's name returns null: the name is
            // taken, and it is not a variable.
            return nullptr;
        }
    }

    if (!currentModuleTable) return nullptr;
    auto found = currentModuleTable->variables.find(name);
    return found != currentModuleTable->variables.end() ? found->second : nullptr;
}

TypeDeclAST* SemaContext::lookupType(InternedString name) const {
    // Types are not lexically scoped — local type declarations are
    // forbidden — so only the module table is consulted.
    if (!currentModuleTable) return nullptr;
    auto found = currentModuleTable->types.find(name);
    return found != currentModuleTable->types.end() ? found->second : nullptr;
}

// ─── Import and cross-module lookup ───────────────────────────────────────

ModuleAST* SemaContext::lookupImport(InternedString alias) const {
    if (!currentModuleTable) return nullptr;
    auto it = currentModuleTable->importAliases.find(alias);
    return it != currentModuleTable->importAliases.end() ? it->second : nullptr;
}

ValueDeclAST* SemaContext::lookupModuleValueMember(ModuleAST* module,
                                                   InternedString memberName) const {
    if (!module) return nullptr;
    auto it = moduleTables.find(module);
    if (it == moduleTables.end()) return nullptr;

    const ModuleTable& table = it->second;
    if (auto found = table.variables.find(memberName);
        found != table.variables.end()) {
        return found->second;
    }
    if (auto found = table.functions.find(memberName);
        found != table.functions.end()) {
        return found->second;
    }
    // A table member is not a `ValueDeclAST`, so it cannot be returned
    // by this function. The caller checks `lookupModuleTableMember`
    // separately when it needs one.
    return nullptr;
}

TableDeclAST* SemaContext::lookupModuleTableMember(ModuleAST* module,
                                                   InternedString memberName) const {
    if (!module) return nullptr;
    auto it = moduleTables.find(module);
    if (it == moduleTables.end()) return nullptr;
    auto found = it->second.tables.find(memberName);
    return found != it->second.tables.end() ? found->second : nullptr;
}

TypeDeclAST* SemaContext::lookupModuleTypeMember(ModuleAST* module,
                                                 InternedString memberName) const {
    if (!module) return nullptr;
    auto it = moduleTables.find(module);
    if (it == moduleTables.end()) return nullptr;
    auto found = it->second.types.find(memberName);
    return found != it->second.types.end() ? found->second : nullptr;
}

ValueDeclAST* SemaContext::lookupImportedValue(InternedString alias,
                                               InternedString member) const {
    ModuleAST* module = lookupImport(alias);
    if (!module) return nullptr;
    return lookupModuleValueMember(module, member);
}

TypeDeclAST* SemaContext::lookupImportedType(InternedString alias,
                                             InternedString member) const {
    ModuleAST* module = lookupImport(alias);
    if (!module) return nullptr;
    return lookupModuleTypeMember(module, member);
}

// ─── Type canonicalization ────────────────────────────────────────────────

PrimitiveTypeAST* SemaContext::getPrimitiveType(PrimitiveKind kind) {
    auto it = typeCache.primitives.find(kind);
    if (it != typeCache.primitives.end()) return it->second;

    PrimitiveTypeAST* t = arena.make<PrimitiveTypeAST>(kind);
    typeCache.primitives[kind] = t;
    return t;
}

UnknownTypeAST* SemaContext::getUnknownType() {
    if (!typeCache.unknownType) {
        typeCache.unknownType = arena.make<UnknownTypeAST>();
    }
    return typeCache.unknownType;
}

NamedTypeAST* SemaContext::getNamedType(InternedString name) {
    TypeCache::NamedTypeKey key{name};
    auto it = typeCache.namedTypes.find(key);
    if (it != typeCache.namedTypes.end()) return it->second;

    NamedTypeAST* t = arena.make<NamedTypeAST>(name);
    typeCache.namedTypes[key] = t;
    return t;
}

ArrayTypeAST* SemaContext::getArrayType(ArrayKind kind, uint64_t size,
                                        TypeAST* element) {
    TypeCache::ArrayTypeKey key{kind, size, element};
    auto it = typeCache.arrayTypes.find(key);
    if (it != typeCache.arrayTypes.end()) return it->second;

    ArrayTypeAST* t = arena.make<ArrayTypeAST>(kind, size, element);
    typeCache.arrayTypes[key] = t;
    return t;
}

RowRefTypeAST* SemaContext::getRowRefType(TypeAST* table) {
    if (!table) return nullptr;

    TypeCache::RowRefTypeKey key{table};
    auto it = typeCache.rowRefTypes.find(key);
    if (it != typeCache.rowRefTypes.end()) return it->second;

    RowRefTypeAST* t = arena.make<RowRefTypeAST>(table);
    typeCache.rowRefTypes[key] = t;
    return t;
}

NullableTypeAST* SemaContext::getNullableType(TypeAST* inner) {
    if (!inner) return nullptr;

    TypeCache::NullableTypeKey key{inner};
    auto it = typeCache.nullableTypes.find(key);
    if (it != typeCache.nullableTypes.end()) return it->second;

    NullableTypeAST* t = arena.make<NullableTypeAST>(inner);
    typeCache.nullableTypes[key] = t;
    return t;
}

FunctionTypeAST* SemaContext::getFunctionType(ArenaSpan<TypeAST*> params,
                                              TypeAST* returnType) {
    TypeCache::FunctionTypeKey key{params, returnType};
    auto it = typeCache.functionTypes.find(key);
    if (it != typeCache.functionTypes.end()) return it->second;

    FunctionTypeAST* t = arena.make<FunctionTypeAST>();
    t->params     = params;
    t->returnType = returnType;
    typeCache.functionTypes[key] = t;
    return t;
}

// ─── Named primitive accessors ────────────────────────────────────────────

PrimitiveTypeAST* SemaContext::getBoolType()    { return getPrimitiveType(PrimitiveKind::Bool); }
PrimitiveTypeAST* SemaContext::getIntType()     { return getPrimitiveType(PrimitiveKind::Int32); }
PrimitiveTypeAST* SemaContext::getUint32Type()  { return getPrimitiveType(PrimitiveKind::Uint32); }
PrimitiveTypeAST* SemaContext::getUint64Type()  { return getPrimitiveType(PrimitiveKind::Uint64); }
PrimitiveTypeAST* SemaContext::getFloatType()   { return getPrimitiveType(PrimitiveKind::Float32); }
PrimitiveTypeAST* SemaContext::getCharType()    { return getPrimitiveType(PrimitiveKind::Char); }
PrimitiveTypeAST* SemaContext::getStringType()  { return getPrimitiveType(PrimitiveKind::String); }
PrimitiveTypeAST* SemaContext::getUnitType()    { return getPrimitiveType(PrimitiveKind::Unit); }

// ─── RAII guards ──────────────────────────────────────────────────────────

ScopedContext::ScopedContext(SemaContext& ctx, ContextKind kind, BaseAST* node)
    : ctx_(ctx) {
    ctx_.stack.push(kind, node);
}

ScopedContext::~ScopedContext() {
    ctx_.stack.pop();
}

// ─────────────────────────────────────────────────────────────────────────

ScopedIfCondition::ScopedIfCondition(SemaContext& ctx, bool hasElse)
    : ctx_(ctx) {
    ctx_.stack.setIfConditionCtx(true);
    ctx_.stack.setHasElse(hasElse);
    ctx_.stack.clearPendingNarrowing();
}

ScopedIfCondition::~ScopedIfCondition() {
    ctx_.stack.setIfConditionCtx(false);
}

// ─────────────────────────────────────────────────────────────────────────

SymbolScope::SymbolScope(SemaContext& ctx)
    : ctx_(ctx) {
    ctx_.pushScope();
}

SymbolScope::~SymbolScope() {
    ctx_.popScope();
}

// ─────────────────────────────────────────────────────────────────────────

ScopedNarrowing::ScopedNarrowing(SemaContext& ctx, InternedString varName,
                                 TypeAST* narrowedType, bool isInverse)
    : ctx_(ctx) {
    ctx_.stack.pushNarrowingLevel(isInverse);
    ctx_.stack.narrowVariable(varName, narrowedType);
}

ScopedNarrowing::ScopedNarrowing(
    SemaContext& ctx,
    const std::unordered_map<InternedString, TypeAST*>& narrowings,
    bool isInverse)
    : ctx_(ctx) {
    ctx_.stack.pushNarrowingLevel(isInverse);
    for (const auto& [name, type] : narrowings) {
        ctx_.stack.narrowVariable(name, type);
    }
}

ScopedNarrowing::~ScopedNarrowing() {
    ctx_.stack.popNarrowingLevel();
}

// ─────────────────────────────────────────────────────────────────────────

ScopedFunction::ScopedFunction(SemaContext& ctx, FnDeclAST* decl)
    : ctx_(ctx)
    , paramScope_(ctx) {
    ContextKind kind = decl->isSequence ? ContextKind::SequenceBody
                                        : ContextKind::FuncBody;
    ctx_.stack.pushFunction(decl, kind, decl->returnType);
}

ScopedFunction::~ScopedFunction() {
    ctx_.stack.pop();
    // paramScope_ destructor pops the parameter scope.
}

} // namespace lucid::sema