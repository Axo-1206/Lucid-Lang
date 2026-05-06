/**
 * @file SemanticCollector.cpp
 *
 * @nutshell Implements the logic to scoop all top-level symbols directly into the scope manager.
 *
 * @reason Acts as the concrete implementation of the AST traversal to capture definitions without touching complex nested bodies or loops.
 *
 * @responsibility Implementation of the Phase 1 semantic pass (top-level symbol collection).
 *
 * @logic Traverses AST nodes for top-level declarations and populates the SymbolTable for cross-referencing.
 *
 * @related SemanticCollector.hpp, SemanticAnalyzer.cpp
 */

#include "SemanticCollector.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "diagnostics/Diagnostic.hpp"
#include "debug/DebugMacros.hpp"

SemanticCollector::SemanticCollector(SymbolTable& symbols, DiagnosticEngine& dc)
    : symbols_(symbols), dc_(dc) {
    LUC_LOG_SEMANTIC("SemanticCollector constructed");
}

// ─────────────────────────────────────────────────────────────────────────────
// extractExternAttr  — Scans an attribute list for @extern and extracts metadata
//
// Looks for an AttributeAST named "extern" in the attribute list.
// If found, fills outSym (C symbol name) and outConv (calling convention).
// Returns true when @extern is present.
// This is a Phase 1 fast-path that reads only the literal args — no resolution.
// ─────────────────────────────────────────────────────────────────────────────
static bool extractExternAttr(const std::vector<AttributePtr>& attributes,
                               std::string& outSym,
                               std::string& outConv) {
    for (const auto& attr : attributes) {
        if (attr->name != "extern") continue;
        // Arg 0: symbol name (string literal).
        if (!attr->args.empty() &&
            attr->args[0].argKind == AttributeArgAST::ArgKind::StringLit) {
            outSym = attr->args[0].value;
        }
        // Arg 1: calling convention (string literal), default "C".
        outConv = "C";
        if (attr->args.size() >= 2 &&
            attr->args[1].argKind == AttributeArgAST::ArgKind::StringLit) {
            outConv = attr->args[1].value;
        }
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// collectProgram  — Entry point to index a parsed file
//
// Passes each top-level statement through the AST visitor mechanics to process
// and register its definitions.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::collectProgram(ProgramAST& program) {
    LUC_LOG_SEMANTIC_VERBOSE("collectProgram: processing file: " << program.filePath);
    int declCount = 0;
    for (auto& decl : program.decls) {
        declCount++;
        LUC_LOG_SEMANTIC_EXTREME("\tprocessing declaration #" << declCount);
        decl->accept(*this);
    }
    LUC_LOG_SEMANTIC_VERBOSE("collectProgram: processed " << declCount << " declarations");
}

// ─────────────────────────────────────────────────────────────────────────────
// declareSymbol  — Safely registers the structured semantic tracking info
//
// Tries to push the Symbol into the SymbolTable. Failure designates a pre-existing 
// identifier in this exact scope, raising `DiagCode::E3005` safely without crashing.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::declareSymbol(const Symbol& sym) {
    LUC_LOG_SEMANTIC_VERBOSE("declareSymbol: name='" << sym.name 
                           << "', kind=" << static_cast<int>(sym.kind)
                           << ", isExtern=" << (sym.isExtern ? "true" : "false"));
    
    if (!symbols_.declare(sym)) {
        LUC_LOG_SEMANTIC("\tERROR: symbol '" << sym.name << "' already declared in this scope");
        dc_.error(DiagnosticCategory::Semantic, sym.loc, DiagCode::E3005,
                  "symbol '" + sym.name + "' is already declared in this scope");
    } else {
        LUC_LOG_SEMANTIC_EXTREME("\tsymbol declared successfully");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(VarDeclAST)  — Simple top-level global variable/constant registration
//
// Inserts the top-level let or const definition name into the global map.
// If @extern is present, the symbol is tagged as linker-resolved.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(VarDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(VarDeclAST): name='" << node.name 
                   << "', keyword=" << (node.keyword == DeclKeyword::Const ? "const" : "let"));
    
    std::string externSym, callingConv;
    bool isExtern = extractExternAttr(node.attributes, externSym, callingConv);
    
    if (isExtern) {
        LUC_LOG_SEMANTIC_VERBOSE("\t@extern detected: sym='" << externSym 
                               << "', conv='" << callingConv << "'");
    }

    Symbol sym;
    sym.name         = node.name;
    sym.kind         = isExtern ? SymbolKind::ExternFunc : SymbolKind::Var;
    sym.declKw       = node.keyword;
    sym.visibility   = node.visibility;
    sym.type         = nullptr;  // Phase 2 will set this
    sym.decl         = &node;
    sym.loc          = node.loc;
    sym.isExtern     = isExtern;
    sym.externSymbol = externSym;
    sym.callingConv  = callingConv;
    declareSymbol(sym);
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(FuncDeclAST)  — Collects top-level functions and checks parameter clashes
//
// Registers the function identifier in the main scope. Temporarily pushes
// an inner scope to quickly process arguments, confirming no two parameters 
// share the same name locally, then instantly discards the parameter bindings.
// If @extern("sym") is present, the symbol is tagged as linker-resolved and
// SymbolKind::ExternFunc is used so codegen emits an external declaration.
//
// UPDATED: Now uses node.type (FuncTypeAST) as the unified signature.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(FuncDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(FuncDeclAST): name='" << node.name 
                   << "', keyword=" << (node.keyword == DeclKeyword::Const ? "const" : "let")
                   << ", paramGroups=" << node.type.paramGroups.size());
    
    // Detect @extern attribute on this function
    std::string externSym, callingConv;
    bool isExtern = extractExternAttr(node.attributes, externSym, callingConv);
    
    if (isExtern) {
        LUC_LOG_SEMANTIC_VERBOSE("\t@extern detected: sym='" << externSym 
                               << "', conv='" << callingConv << "'");
    }

    Symbol sym;
    sym.name         = node.name;
    sym.kind         = isExtern ? SymbolKind::ExternFunc : SymbolKind::Func;
    sym.declKw       = node.keyword;
    sym.visibility   = node.visibility;
    sym.type         = &node.type;  // Point to the FuncTypeAST (Phase 2 will resolve)
    sym.decl         = &node;
    sym.loc          = node.loc;
    sym.isExtern     = isExtern;
    sym.externSymbol = externSym;
    sym.callingConv  = callingConv;
    declareSymbol(sym);

    // Register params to check for duplicates
    LUC_LOG_SEMANTIC_EXTREME("\tregistering parameters");
    symbols_.pushScope();
    for (const auto& group : node.type.paramGroups) {
        for (const auto& param : group) {
            LUC_LOG_SEMANTIC_EXTREME("\t\tparam: " << param.name);
            declareSymbol({
                param.name,
                SymbolKind::Param,
                DeclKeyword::Let,
                Visibility::Private,
                param.type.get(),
                nullptr,  // ParamInfo is not a BaseAST, so no back pointer
                param.loc
            });
        }
    }
    symbols_.popScope();
    LUC_LOG_SEMANTIC_VERBOSE("\tfunction registered successfully");
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(StructDeclAST)  — Maps structures and cross-checks internal field shapes
//
// Like functions, maps the struct globally. Pushes a mock localized scope to
// iterate through the struct's definition, asserting no duplicate field aliases
// are used before popping the ephemeral scope.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(StructDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(StructDeclAST): name='" << node.name 
                   << "', fields=" << node.fields.size());
    
    // Lazy-initialize the struct's self-type representation.
    if (!node.selfType) {
        LUC_LOG_SEMANTIC_VERBOSE("\tcreating selfType for struct: " << node.name);
        node.selfType = std::make_unique<NamedTypeAST>(node.name);
        node.selfType->loc = node.loc;
    }

    declareSymbol({
        node.name,
        SymbolKind::Struct,
        DeclKeyword::Let,
        node.visibility,
        nullptr,  // Phase 2 will set this
        &node,
        node.loc
    });

    // Register fields to check for duplicates
    LUC_LOG_SEMANTIC_EXTREME("\tregistering " << node.fields.size() << " fields");
    symbols_.pushScope();
    for (const auto& field : node.fields) {
        LUC_LOG_SEMANTIC_EXTREME("\t\tfield: " << field->name);
        declareSymbol({
            field->name,
            SymbolKind::Field,
            DeclKeyword::Let,
            Visibility::Private,
            field->type.get(),
            field.get(),
            field->loc
        });
    }
    symbols_.popScope();
    LUC_LOG_SEMANTIC_VERBOSE("\tstruct registered successfully");
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(EnumDeclAST)  — Registers enumerations and uniqueness of their choices
//
// Submits the enum label to the main table, pushing a localized scope to enforce 
// uniquely labelled variant flags.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(EnumDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(EnumDeclAST): name='" << node.name 
                   << "', variants=" << node.variants.size());
    
    declareSymbol({
        node.name,
        SymbolKind::Enum,
        DeclKeyword::Let,
        node.visibility,
        nullptr,
        &node,
        node.loc
    });

    LUC_LOG_SEMANTIC_EXTREME("\tregistering " << node.variants.size() << " variants");
    symbols_.pushScope();
    for (const auto& variant : node.variants) {
        LUC_LOG_SEMANTIC_EXTREME("\t\tvariant: " << variant->name);
        declareSymbol({
            variant->name,
            SymbolKind::EnumVariant,
            DeclKeyword::Let,
            Visibility::Private,
            nullptr,
            variant.get(),
            variant->loc
        });
    }
    symbols_.popScope();
    LUC_LOG_SEMANTIC_VERBOSE("\tenum registered successfully");
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(TraitDeclAST)  — Connects method contract names into semantic awareness
//
// Adds the trait name itself, validating inside an ephemeral scope that no
// internal method signatures possess exactly duplicate naming.
//
// UPDATED: Now uses method->type (FuncTypeAST) as the unified signature.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(TraitDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(TraitDeclAST): name='" << node.name 
                   << "', methods=" << node.methods.size());
    
    declareSymbol({
        node.name,
        SymbolKind::Trait,
        DeclKeyword::Let,
        node.visibility,
        nullptr,
        &node,
        node.loc
    });

    for (const auto& method : node.methods) {
        LUC_LOG_SEMANTIC_VERBOSE("\tprocessing trait method: " << method->name);
        
        // The signature is already in method->type (FuncTypeAST)
        // No need to build it again - just register the method
        
        std::string mangledName = node.name + "." + method->name;
        LUC_LOG_SEMANTIC_EXTREME("\t\tmangled name: " << mangledName);
        
        declareSymbol({
            mangledName,
            SymbolKind::Method,
            DeclKeyword::Let,
            Visibility::Export,
            &method->type,  // Point to the FuncTypeAST
            method.get(),
            method->loc
        });
    }
    LUC_LOG_SEMANTIC_VERBOSE("\ttrait registered successfully");
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(ImplDeclAST)  — Validates Implementation blocks structure bindings
//
// Struct instance actions aren't directly available without instance traversal.
// To map them, we synthesize artificial `StructName.methodName` tags on the
// global scope index. It catches multi-impl blocks conflicting via same names.
//
// UPDATED: Now uses method->type (FuncTypeAST) as the unified signature.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(ImplDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(ImplDeclAST): structName='" << node.structName 
                   << "', methods=" << node.methods.size()
                   << ", visibility=" << (node.visibility == Visibility::Export ? "export" :
                                          node.visibility == Visibility::Package ? "pub" : "private"));
    
    for (const auto& method : node.methods) {
        LUC_LOG_SEMANTIC_VERBOSE("\tprocessing impl method: " << method->name);
        
        // The signature is already in method->type (FuncTypeAST)
        // Built during parsing with NO self parameter
        
        std::string mangledName = node.structName + "." + method->name;
        LUC_LOG_SEMANTIC_EXTREME("\t\tmangled name: " << mangledName);
        
        declareSymbol({
            mangledName,
            SymbolKind::Method,
            DeclKeyword::Let,
            node.visibility,
            &method->type,  // Point to the FuncTypeAST
            method.get(),
            method->loc
        });
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("\timpl registered successfully");
}
 
// ─────────────────────────────────────────────────────────────────────────────
// visit(FromDeclAST)  — Registers custom type casting for Type(source) calls
//
// Like methods, castings are indexed on the target type's namespace.
// Because the language supports curried casting overloads, and Phase 1 runs
// before type resolution, we assign them a unique address-based mangled name here.
// True duplicate signature checking is deferred to Phase 3 (SemanticDecl).
//
// UPDATED: Now uses entry->paramGroups (ParamGroup) instead of ParamPtr.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(FromDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(FromDeclAST): targetType='" << node.targetTypeName 
                   << "', entries=" << node.entries.size()
                   << ", visibility=" << (node.visibility == Visibility::Export ? "export" :
                                          node.visibility == Visibility::Package ? "pub" : "private"));
    
    for (auto& entry : node.entries) {
        if (!entry) continue;

        // Use pointer address as a phase 1 unique identifier
        std::string mangledName = node.targetTypeName + ".from." + 
            std::to_string(reinterpret_cast<std::uintptr_t>(entry.get()));
        
        LUC_LOG_SEMANTIC_EXTREME("\tregistering from-entry: " << mangledName);
        declareSymbol({
            mangledName,
            SymbolKind::Casting,
            DeclKeyword::Let,
            node.visibility,
            nullptr,  // Type will be resolved in Phase 2
            entry.get(),
            entry->loc
        });
    }
    LUC_LOG_SEMANTIC_VERBOSE("\tfrom-block registered successfully");
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(TypeAliasDeclAST)  — Assigns proxy labels to underlying complex shapes
//
// Stores the 'type XYZ = int' alias safely on the central scope.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(TypeAliasDeclAST& node) {
    LUC_LOG_SEMANTIC("visit(TypeAliasDeclAST): name='" << node.name 
                   << "', genericParams=" << node.genericParams.size());
    
    declareSymbol({
        node.name,
        SymbolKind::TypeAlias,
        DeclKeyword::Let,
        node.visibility,
        nullptr,  // Phase 2 will set this
        &node,
        node.loc
    });
    LUC_LOG_SEMANTIC_VERBOSE("\ttype alias registered successfully");
}