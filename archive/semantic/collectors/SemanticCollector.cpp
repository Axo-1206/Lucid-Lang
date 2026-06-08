/**
 * @file SemanticCollector.cpp
 * @brief Phase 1: collects top‑level declarations into the symbol table.
 */

#include "SemanticCollector.hpp"
#include "diagnostics/Diagnostic.hpp"
#include "debug/DebugMacros.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/helpers/NameMangler.hpp"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// collectProgram
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::collectProgram(ProgramAST& program, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC("SemanticCollector::collectProgram: file=" << ctx.pool.lookup(program.filePath));

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
                  "duplicate declaration of '" + std::string(nameStr) + "'");
        return;
    }

    if (!ctx.symbols->declare(sym)) {
        std::string_view nameStr = ctx.pool.lookup(sym.name);
        ctx.error(sym.loc, DiagCode::E2005,
                  "failed to declare symbol '" + std::string(nameStr) + "'");
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
// Declaration Collectors
// ─────────────────────────────────────────────────────────────────────────────

void SemanticCollector::collectUseDecl(UseDeclAST& node, SemanticContext& ctx) {
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

    // Trait methods (mangled for lookup)
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
    LUC_LOG_SEMANTIC("SemanticCollector::collectImplDecl");
    
    // Phase 1: ONLY record that this impl block exists.
    // NO trait conformance recording - defer to Phase 2.
    // NO target type validation - defer to Phase 2.
    
    // Create a unique name for this impl block
    std::string implName = "__impl_" + std::to_string(implCounter_++);
    InternedString implInterned = ctx.pool.intern(implName);
    
    Symbol sym;
    sym.name = implInterned;
    sym.kind = SymbolKind::Impl;
    sym.visibility = node.visibility;
    sym.type = nullptr;
    sym.decl = &node;
    sym.loc = node.loc;
    declareSymbol(sym, ctx);
    
    // Methods will be recorded in Phase 2 when we know the fully resolved target type
    // For now, they remain only in the AST
}

void SemanticCollector::collectFromDecl(FromDeclAST& node, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC("SemanticCollector::collectFromDecl");
    
    // Phase 1: Record from entries for lookup
    // Type validation happens in Phase 2
    
    if (!node.targetType) {
        ctx.error(node.loc, DiagCode::E2016, "from target type is missing");
        return;
    }
    
    std::string targetStr = NameMangler::mangleType(node.targetType.get(), ctx.pool, ctx.symbols);
    for (const auto& entry : node.entries) {
        if (!entry) continue;

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