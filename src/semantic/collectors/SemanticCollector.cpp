/**
 * @file SemanticCollector.cpp
 * @responsibility Implements Phase 1 of semantic analysis: collecting top‑level declarations into the symbol table.
 *
 * This file contains the SemanticCollector implementation, which now uses
 * switch‑case dispatch on ASTKind instead of the visitor pattern. All state
 * is passed via SemanticContext.
 */

#include "SemanticCollector.hpp"
#include "diagnostics/Diagnostic.hpp"
#include "debug/DebugMacros.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/helpers/NameMangler.hpp"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// collectProgram  —  Main entry point: collects all top‑level symbols.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::collectProgram(ProgramAST& program, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC("SemanticCollector::collectProgram: file=" << ctx.pool.lookup(program.filePath));

    // Ensure global scope exists
    if (ctx.symbols->currentDepth() == 0) {
        ctx.symbols->pushScope();
    }

    for (auto& decl : program.decls) {
        if (!decl) continue;

        switch (decl->kind) {
            case ASTKind::UseDecl:
                collectUseDecl(*decl->as<UseDeclAST>(), ctx);
                break;
            case ASTKind::VarDecl:
                collectVarDecl(*decl->as<VarDeclAST>(), ctx);
                break;
            case ASTKind::FuncDecl:
                collectFuncDecl(*decl->as<FuncDeclAST>(), ctx);
                break;
            case ASTKind::StructDecl:
                collectStructDecl(*decl->as<StructDeclAST>(), ctx);
                break;
            case ASTKind::EnumDecl:
                collectEnumDecl(*decl->as<EnumDeclAST>(), ctx);
                break;
            case ASTKind::TraitDecl:
                collectTraitDecl(*decl->as<TraitDeclAST>(), ctx);
                break;
            case ASTKind::ImplDecl:
                collectImplDecl(*decl->as<ImplDeclAST>(), ctx);
                break;
            case ASTKind::FromDecl:
                collectFromDecl(*decl->as<FromDeclAST>(), ctx);
                break;
            case ASTKind::TypeAliasDecl:
                collectTypeAliasDecl(*decl->as<TypeAliasDeclAST>(), ctx);
                break;
            default:
                // Unknown or error recovery node – ignore
                break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Symbol Management Helpers
// ─────────────────────────────────────────────────────────────────────────────

void SemanticCollector::declareSymbol(const Symbol& sym, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_VERBOSE("declareSymbol: name=" << ctx.pool.lookup(sym.name)
                             << " kind=" << SymbolUtils::kindToString(sym.kind));

    if (ctx.symbols->lookupLocal(sym.name)) {
        std::string_view nameStr = ctx.pool.lookup(sym.name);
        ctx.error(sym.loc, DiagCode::E2005,
                  {"duplicate declaration of '" + std::string(nameStr) + "'"});
        return;
    }

    if (!ctx.symbols->declare(sym)) {
        std::string_view nameStr = ctx.pool.lookup(sym.name);
        ctx.error(sym.loc, DiagCode::E2005,
                  {"failed to declare symbol '" + std::string(nameStr) + "'"});
    }
}

void SemanticCollector::extractExternMetadata(const ArenaSpan<AttributePtr>& attrs, Symbol& sym, SemanticContext& ctx) {
    for (const auto& attr : attrs) {
        if (!attr) continue;
        std::string_view attrName = ctx.pool.lookup(attr->name);
        if (attrName == "extern") {
            sym.isExtern = true;
            if (!attr->args.empty() && attr->args[0] && attr->args[0]->kind == AttributeArgKind::StringLit) {
                sym.externSymbol = attr->args[0]->value;
            }
            if (attr->args.size() >= 2 && attr->args[1] && attr->args[1]->kind == AttributeArgKind::TypeIdent) {
                sym.callingConv = attr->args[1]->value;
            }
            LUC_LOG_SEMANTIC_VERBOSE("extractExternMetadata: extern symbol=" << ctx.pool.lookup(sym.externSymbol));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Declaration Collectors (each corresponds to a specific ASTKind)
// ─────────────────────────────────────────────────────────────────────────────

void SemanticCollector::collectUseDecl(UseDeclAST& node, SemanticContext& ctx) {
    // Build full path for mangling (optional, but kept for consistency)
    std::string fullPath;
    for (size_t i = 0; i < node.path.size(); ++i) {
        if (i > 0) fullPath += ".";
        fullPath += ctx.pool.lookup(node.path[i]);
    }
    InternedString symName = node.alias.value_or(node.path.back());

    Symbol sym;
    sym.name = symName;
    sym.kind = SymbolKind::Module;
    sym.visibility = node.visibility;
    sym.decl = &node;
    sym.loc = node.loc;
    sym.type = nullptr;
    declareSymbol(sym, ctx);
}

void SemanticCollector::collectVarDecl(VarDeclAST& node, SemanticContext& ctx) {
    Symbol sym;
    sym.name = node.name;
    sym.kind = SymbolKind::Var;
    sym.declKw = node.keyword;
    sym.visibility = node.visibility;
    sym.type = nullptr;
    sym.decl = &node;
    sym.loc = node.loc;
    extractExternMetadata(node.attributes, sym, ctx);
    declareSymbol(sym, ctx);
}

void SemanticCollector::collectFuncDecl(FuncDeclAST& node, SemanticContext& ctx) {
    Symbol sym;
    sym.name = node.name;
    sym.kind = SymbolKind::Func;
    sym.declKw = node.keyword;
    sym.visibility = node.visibility;
    sym.type = nullptr;
    sym.decl = &node;
    sym.loc = node.loc;
    extractExternMetadata(node.attributes, sym, ctx);
    if (sym.isExtern) sym.kind = SymbolKind::ExternFunc;
    declareSymbol(sym, ctx);
}

void SemanticCollector::collectStructDecl(StructDeclAST& node, SemanticContext& ctx) {
    Symbol sym;
    sym.name = node.name;
    sym.kind = SymbolKind::Struct;
    sym.visibility = node.visibility;
    sym.type = nullptr;
    sym.decl = &node;
    sym.loc = node.loc;
    declareSymbol(sym, ctx);
}

void SemanticCollector::collectEnumDecl(EnumDeclAST& node, SemanticContext& ctx) {
    Symbol sym;
    sym.name = node.name;
    sym.kind = SymbolKind::Enum;
    sym.visibility = node.visibility;
    sym.type = nullptr;
    sym.decl = &node;
    sym.loc = node.loc;
    declareSymbol(sym, ctx);

    // Enum variants
    std::string_view enumName = ctx.pool.lookup(node.name);
    for (const auto& variant : node.variants) {
        if (!variant) continue;
        std::string mangled = NameMangler::mangleEnumVariant(enumName, ctx.pool.lookup(variant->name));
        InternedString mangledInterned = ctx.pool.intern(mangled);

        Symbol vsym;
        vsym.name = mangledInterned;
        vsym.kind = SymbolKind::EnumVariant;
        vsym.declKw = DeclKeyword::Const;
        vsym.visibility = node.visibility;
        vsym.type = nullptr;
        vsym.decl = variant.get();
        vsym.loc = variant->loc;
        declareSymbol(vsym, ctx);
    }
}

void SemanticCollector::collectTraitDecl(TraitDeclAST& node, SemanticContext& ctx) {
    Symbol sym;
    sym.name = node.name;
    sym.kind = SymbolKind::Trait;
    sym.visibility = node.visibility;
    sym.type = nullptr;
    sym.decl = &node;
    sym.loc = node.loc;
    declareSymbol(sym, ctx);

    // Trait methods
    std::string_view traitName = ctx.pool.lookup(node.name);
    for (const auto& method : node.methods) {
        if (!method) continue;
        std::string mangled = NameMangler::mangleMethod(traitName, ctx.pool.lookup(method->name));
        InternedString mangledInterned = ctx.pool.intern(mangled);

        Symbol msym;
        msym.name = mangledInterned;
        msym.kind = SymbolKind::Method;
        msym.visibility = node.visibility;
        msym.type = nullptr;
        msym.decl = method.get();
        msym.loc = method->loc;
        declareSymbol(msym, ctx);
    }
}

void SemanticCollector::collectImplDecl(ImplDeclAST& node, SemanticContext& ctx) {
    // Extract struct name from target type (must be NamedTypeAST)
    InternedString structName;
    if (node.targetType && node.targetType->isa<NamedTypeAST>()) {
        structName = node.targetType->as<NamedTypeAST>()->name;
    } else {
        ctx.error(node.loc, DiagCode::E2016, {"impl target must be a named type (struct, enum, or type alias)"});
        return;
    }

    // Record trait conformance for later use (e.g., in type resolution)
    if (node.traitRef) {
        structTraits_[structName].push_back(node.traitRef->name);
    }

    // Methods
    std::string_view structNameStr = ctx.pool.lookup(structName);
    for (const auto& method : node.methods) {
        if (!method) continue;
        std::string mangled = NameMangler::mangleMethod(structNameStr, ctx.pool.lookup(method->name));
        InternedString mangledInterned = ctx.pool.intern(mangled);

        Symbol msym;
        msym.name = mangledInterned;
        msym.kind = SymbolKind::Method;
        msym.visibility = node.visibility;
        msym.type = nullptr;
        msym.decl = method.get();
        msym.loc = method->loc;
        declareSymbol(msym, ctx);
    }
}

void SemanticCollector::collectFromDecl(FromDeclAST& node, SemanticContext& ctx) {
    // Extract target type name
    InternedString targetName;
    if (node.targetType && node.targetType->isa<NamedTypeAST>()) {
        targetName = node.targetType->as<NamedTypeAST>()->name;
    } else {
        ctx.error(node.loc, DiagCode::E2016, {"from target must be a named type (struct, enum, or type alias)"});
        return;
    }

    std::string_view targetStr = ctx.pool.lookup(targetName);
    for (const auto& entry : node.entries) {
        if (!entry) continue;

        // Get the type of the first parameter (source type)
        TypeAST* firstParamType = nullptr;
        if (entry->sig.totalParamCount() > 0 && entry->sig.groupCount() > 0) {
            auto group = entry->sig.getGroup(0);
            if (!group.empty() && group[0]) {
                firstParamType = group[0]->type.get();
            }
        }

        std::string mangled = NameMangler::mangleFrom(targetStr, firstParamType, ctx.pool);
        InternedString mangledInterned = ctx.pool.intern(mangled);

        Symbol esym;
        esym.name = mangledInterned;
        esym.kind = SymbolKind::Casting;
        esym.visibility = node.visibility;
        esym.type = nullptr;
        esym.decl = entry.get();
        esym.loc = entry->loc;
        declareSymbol(esym, ctx);
    }
}

void SemanticCollector::collectTypeAliasDecl(TypeAliasDeclAST& node, SemanticContext& ctx) {
    Symbol sym;
    sym.name = node.name;
    sym.kind = SymbolKind::TypeAlias;
    sym.visibility = node.visibility;
    sym.type = nullptr;
    sym.decl = &node;
    sym.loc = node.loc;
    declareSymbol(sym, ctx);
}