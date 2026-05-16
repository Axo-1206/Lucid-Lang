/**
 * @file SemanticCollector.cpp
 *
 * @nutshell A quick first-pass AST visitor that gathers definitions.
 *
 * @reason Modern language syntax requires types to be referenceable before they are fully checked. This establishes that baseline index mapping.
 *
 * @responsibility Phase 1 of semantic analysis: collect all top-level names into the file-scope symbol table.
 *
 * @logic First pass over the AST. Collects declarations (struct, enum, function, etc.) before type checking to enable forward references.
 *
 * @related SemanticAnalyzer.hpp, SymbolTable.hpp
 */

#include "header/SemanticCollector.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugMacros.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "header/NameMangler.hpp"
#include <string>
// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
SemanticCollector::SemanticCollector(SymbolTable& symbols, DiagnosticEngine& dc,
                                      StringPool& pool)
    : symbols_(symbols), dc_(dc), pool_(pool) {
    LUC_LOG_SEMANTIC("SemanticCollector constructed");
}

// ─────────────────────────────────────────────────────────────────────────────
// collectProgram
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::collectProgram(ProgramAST& program) {
    LUC_LOG_SEMANTIC("SemanticCollector::collectProgram: file=" 
                     << pool_.lookup(program.filePath));
    
    // Ensure global scope exists
    if (symbols_.currentDepth() == 0) {
        symbols_.pushScope();
    }
    
    // Collect all top-level declarations
    for (auto& decl : program.decls) {
        if (decl) {
            decl->accept(*this);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// declareSymbol
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::declareSymbol(const Symbol& sym) {
    LUC_LOG_SEMANTIC_VERBOSE("declareSymbol: name=" << pool_.lookup(sym.name) 
                             << " kind=" << SymbolUtils::kindToString(sym.kind));
    
    // Check for duplicate in current scope
    if (symbols_.lookupLocal(sym.name)) {
        std::string_view nameStr = pool_.lookup(sym.name);
        dc_.error(DiagnosticCategory::Semantic, sym.loc, DiagCode::E3005,
                  "duplicate declaration of '" + std::string(nameStr) + "'");
        return;
    }
    
    if (!symbols_.declare(sym)) {
        std::string_view nameStr = pool_.lookup(sym.name);
        dc_.error(DiagnosticCategory::Semantic, sym.loc, DiagCode::E3005,
                  "failed to declare symbol '" + std::string(nameStr) + "'");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// extractExternMetadata
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::extractExternMetadata(const std::vector<AttributePtr>& attrs, 
                                               Symbol& sym) {
    for (const auto& attr : attrs) {
        if (!attr) continue;
        
        std::string_view attrName = pool_.lookup(attr->name);
        if (attrName == "extern") {
            sym.isExtern = true;
            
            // Extract the symbol name from the attribute argument
            if (!attr->args.empty() && attr->args[0]) {
                auto* arg = attr->args[0].get();
                if (arg && arg->kind == AttributeArgKind::StringLit) {
                    sym.externSymbol = arg->value;
                }
            }
            
            // Extract calling convention if specified as second argument
            if (attr->args.size() >= 2 && attr->args[1]) {
                auto* arg = attr->args[1].get();
                if (arg && arg->kind == AttributeArgKind::TypeIdent) {
                    sym.callingConv = arg->value;
                }
            }
            
            LUC_LOG_SEMANTIC_VERBOSE("extractExternMetadata: extern symbol=" 
                                     << pool_.lookup(sym.externSymbol));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(VarDeclAST)
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(VarDeclAST& node) {
    LUC_LOG_SEMANTIC("SemanticCollector::visit(VarDeclAST): name=" 
                     << pool_.lookup(node.name));
    
    Symbol sym;
    sym.name = node.name;
    sym.kind = SymbolKind::Var;
    sym.declKw = node.keyword;
    sym.visibility = node.visibility;
    sym.type = nullptr;  // Will be set during type resolution
    sym.decl = &node;
    sym.loc = node.loc;
    
    // Check for @extern attribute (though vars typically don't have it)
    extractExternMetadata(node.attributes, sym);
    
    declareSymbol(sym);
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(FuncDeclAST)
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(FuncDeclAST& node) {
    LUC_LOG_SEMANTIC("SemanticCollector::visit(FuncDeclAST): name=" 
                     << pool_.lookup(node.name));
    
    Symbol sym;
    sym.name = node.name;
    sym.kind = SymbolKind::Func;  // default, may be changed to ExternFunc
    sym.declKw = node.keyword;
    sym.visibility = node.visibility;
    sym.type = nullptr;
    sym.decl = &node;
    sym.loc = node.loc;
    
    // Extract @extern metadata (sets sym.isExtern, sym.externSymbol, etc.)
    extractExternMetadata(node.attributes, sym);
    
    // If an @extern attribute was found, change the kind accordingly
    if (sym.isExtern) {
        sym.kind = SymbolKind::ExternFunc;
    }
    
    declareSymbol(sym);
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(StructDeclAST)
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(StructDeclAST& node) {
    LUC_LOG_SEMANTIC("SemanticCollector::visit(StructDeclAST): name=" 
                     << pool_.lookup(node.name));
    
    Symbol sym;
    sym.name = node.name;
    sym.kind = SymbolKind::Struct;
    sym.declKw = DeclKeyword::Let;  // Not applicable for structs
    sym.visibility = node.visibility;
    sym.type = nullptr;  // Will be set during type resolution (selfType)
    sym.decl = &node;
    sym.loc = node.loc;
    
    declareSymbol(sym);
    
    // Note: Struct fields and methods are collected separately by the
    // SemanticAnalyzer when processing the struct's body.
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(EnumDeclAST)
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(EnumDeclAST& node) {
    LUC_LOG_SEMANTIC("SemanticCollector::visit(EnumDeclAST): name=" 
                     << pool_.lookup(node.name));
    
    Symbol sym;
    sym.name = node.name;
    sym.kind = SymbolKind::Enum;
    sym.declKw = DeclKeyword::Let;  // Not applicable for enums
    sym.visibility = node.visibility;
    sym.type = nullptr;
    sym.decl = &node;
    sym.loc = node.loc;
    
    declareSymbol(sym);
    
    // Declare enum variants as symbols in the enum's scope
    // Note: Variants are accessed via EnumName.Variant syntax
    for (const auto& variant : node.variants) {
        if (!variant) continue;
        
        // Create mangled name for the variant
        std::string mangledName = NameMangler::mangleEnumVariant(pool_.lookup(node.name), pool_.lookup(variant->name));
        InternedString mangledInterned = pool_.intern(mangledName);

        Symbol variantSym;
        variantSym.name = mangledInterned;
        variantSym.kind = SymbolKind::EnumVariant;
        variantSym.declKw = DeclKeyword::Const;  // Enum variants are constants
        variantSym.visibility = node.visibility;
        variantSym.type = nullptr;  // Will be set to the enum type
        variantSym.decl = variant.get();
        variantSym.loc = variant->loc;
        
        declareSymbol(variantSym);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(TraitDeclAST)
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(TraitDeclAST& node) {
    LUC_LOG_SEMANTIC("SemanticCollector::visit(TraitDeclAST): name=" 
                     << pool_.lookup(node.name));
    
    Symbol sym;
    sym.name = node.name;
    sym.kind = SymbolKind::Trait;
    sym.declKw = DeclKeyword::Let;  // Not applicable for traits
    sym.visibility = node.visibility;
    sym.type = nullptr;
    sym.decl = &node;
    sym.loc = node.loc;
    
    declareSymbol(sym);
    
    // Declare trait methods as symbols with mangled names (TraitName.methodName)
    for (const auto& method : node.methods) {
        if (!method) continue;
        
        // Create mangled name for the trait method
        std::string mangledName = NameMangler::mangleMethod(pool_.lookup(node.name), pool_.lookup(method->name));
        InternedString mangledInterned = pool_.intern(mangledName);
        
        Symbol methodSym;
        methodSym.name = mangledInterned;
        methodSym.kind = SymbolKind::Method;
        methodSym.declKw = DeclKeyword::Let;
        methodSym.visibility = node.visibility;
        methodSym.type = nullptr;  // Will be set during type resolution
        methodSym.decl = method.get();
        methodSym.loc = method->loc;
        
        declareSymbol(methodSym);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(ImplDeclAST)
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(ImplDeclAST& node) {
    LUC_LOG_SEMANTIC("SemanticCollector::visit(ImplDeclAST): struct=" 
                     << pool_.lookup(node.structName) 
                     << ", methods=" << node.methods.size());
    
    // Impl blocks themselves don't create symbols, but their methods do.
    // Methods are stored with mangled names: StructName.methodName
    for (const auto& method : node.methods) {
        if (!method) continue;
        
        // Create mangled name for the method
        // pool_.lookup returns a string_view; convert to std::string before concatenation
        std::string mangledName = NameMangler::mangleMethod(pool_.lookup(node.structName), pool_.lookup(method->name));
        InternedString mangledInterned = pool_.intern(mangledName);
        
        Symbol methodSym;
        methodSym.name = mangledInterned;
        methodSym.kind = SymbolKind::Method;
        methodSym.declKw = DeclKeyword::Let;
        methodSym.visibility = node.visibility;
        methodSym.type = nullptr;  // Will be set during type resolution
        methodSym.decl = method.get();
        methodSym.loc = method->loc;
        
        declareSymbol(methodSym);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(FromDeclAST)
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(FromDeclAST& node) {
    LUC_LOG_SEMANTIC("SemanticCollector::visit(FromDeclAST): target=" 
                     << pool_.lookup(node.targetTypeName)
                     << ", entries=" << node.entries.size());
    
    // From blocks themselves don't create symbols, but their conversion entries do.
    // Conversion entries are stored with mangled names: From_<targetType>_<paramTypes>
    for (const auto& entry : node.entries) {
        if (!entry) continue;
        
        // Build a mangled name for the conversion
        TypeAST* firstParamType = nullptr;
        if (!entry->sig.paramGroups.empty() && !entry->sig.paramGroups[0].empty() && entry->sig.paramGroups[0][0]) {
            firstParamType = entry->sig.paramGroups[0][0]->type.get();
        }
        std::string mangledName = NameMangler::mangleFrom(pool_.lookup(node.targetTypeName), firstParamType, pool_);
        InternedString mangledInterned = pool_.intern(mangledName);
        
        Symbol entrySym;
        entrySym.name = mangledInterned;
        entrySym.kind = SymbolKind::Casting;
        entrySym.declKw = DeclKeyword::Let;
        entrySym.visibility = node.visibility;
        entrySym.type = nullptr;
        entrySym.decl = entry.get();
        entrySym.loc = entry->loc;
        
        declareSymbol(entrySym);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(TypeAliasDeclAST)
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(TypeAliasDeclAST& node) {
    LUC_LOG_SEMANTIC("SemanticCollector::visit(TypeAliasDeclAST): name=" 
                     << pool_.lookup(node.name));
    
    Symbol sym;
    sym.name = node.name;
    sym.kind = SymbolKind::TypeAlias;
    sym.declKw = DeclKeyword::Let;  // Not applicable for type aliases
    sym.visibility = node.visibility;
    sym.type = nullptr;  // Will be set during type resolution
    sym.decl = &node;
    sym.loc = node.loc;
    
    declareSymbol(sym);
}