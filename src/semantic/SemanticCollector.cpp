/**
 * @file SemanticCollector.cpp
 * @responsibility Implements Phase 1 of semantic analysis: collecting top‑level declarations into the symbol table.
 *
 * This file contains the SemanticCollector class, an ASTVisitor that walks the
 * parse tree before type checking. It extracts names of structs, enums,
 * functions, traits, impls, from blocks, and type aliases, and declares them
 * in the global symbol table. This enables forward references during later
 * phases (type resolution and checking).
 *
 * The collector does not resolve types; it only records names and their kinds.
 * Type information (e.g., field types, function signatures) is left unresolved
 * until Phase 2 (TypeResolver).
 *
 * @related
 *   - SemanticCollector.hpp – class declaration
 *   - SymbolTable.hpp – stores collected symbols
 *   - SemanticAnalyzer.cpp – orchestrates the four phases
 *   - NameMangler.hpp – generates unique names for methods, variants, and conversions
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * NAVIGATION – Functions in this file (in order of appearance)
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * ██ Constructor & Entry Point
 *   SemanticCollector::SemanticCollector()   – initialises symbol table, diagnostic engine, and StringPool.
 *   collectProgram()                         – collects symbols from a single ProgramAST.
 *
 * ██ Symbol Management
 *   declareSymbol()                          – declares a symbol in the symbol table (checks duplicates).
 *   extractExternMetadata()                  – extracts @extern attribute metadata for a symbol.
 *
 * ██ Declaration Visitors
 *   visit(VarDeclAST)                        – collects variable declarations.
 *   visit(FuncDeclAST)                       – collects function declarations (handles @extern).
 *   visit(StructDeclAST)                     – collects struct declarations.
 *   visit(EnumDeclAST)                       – collects enum declarations and their variants.
 *   visit(TraitDeclAST)                      – collects trait declarations and their methods.
 *   visit(ImplDeclAST)                       – collects impl blocks and their methods.
 *   visit(FromDeclAST)                       – collects from conversion entries.
 *   visit(TypeAliasDeclAST)                  – collects type alias declarations.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * @note The collector assumes the AST has been parsed (no error nodes except
 *       UnknownDeclAST). It processes only top‑level declarations; function
 *       bodies, block scopes, and expressions are ignored. All symbols are
 *       stored with their original (or mangled) names; the symbol table uses
 *       InternedString IDs for efficient lookup.
 * ─────────────────────────────────────────────────────────────────────────────
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
// Constructor & Entry Point
// ─────────────────────────────────────────────────────────────────────────────
SemanticCollector::SemanticCollector(SymbolTable& symbols, DiagnosticEngine& dc,
                                      StringPool& pool)
    : _symbols(symbols), _dc(dc), _pool(pool) {
    LUC_LOG_SEMANTIC("SemanticCollector constructed");
}

// ─────────────────────────────────────────────────────────────────────────────
// collectProgram  —  Collects all top‑level symbols from a single ProgramAST.
//
// Walks all declarations inside the program node, dispatching to the appropriate
// visitor method for each declaration type. This is the entry point for Phase 1
// on a per‑file basis. The symbol table must have at least the global scope
// before collection begins.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Populates the symbol table with all top‑level declarations (structs, enums,
// functions, traits, impls, from blocks, type aliases, variables) from the
// current file. This establishes the name mapping needed for forward references
// during Phase 2 and Phase 3.
//
// ─── Algorithm ───────────────────────────────────────────────────────────────
//   1. If the symbol table has no scopes (currentDepth() == 0), push a new
//      global scope to ensure there is a place to store symbols.
//   2. Iterate over all declarations in program.decls.
//   3. For each non‑null declaration, call decl->accept(*this), which will
//      invoke the appropriate visit() method based on the declaration's kind.
//   4. The visit methods will call declareSymbol() to register each symbol.
//
// ─── Cases Covered ───────────────────────────────────────────────────────────
//   - First file in the package → creates the global scope.
//   - Subsequent files → reuses existing global scope (no new global scope).
//   - UnknownDeclAST (error recovery) → ignored (no symbol created).
//   - Multiple declarations of the same name in the same file → duplicate
//     detection happens inside declareSymbol (error reported, second ignored).
//
// ─── What is NOT covered ─────────────────────────────────────────────────────
//   - Does NOT collect symbols from local scopes (inside functions, blocks).
//     Local symbols are collected later during Phase 3 (checking) when the
//     function body is traversed.
//   - Does NOT resolve types or check type validity – that is Phase 2.
//   - Does NOT generate code or perform semantic checks.
//
// ─── Note ────────────────────────────────────────────────────────────────────
//   This function is called once per ProgramAST (i.e., per source file) by
//   SemanticAnalyzer::collectSymbols().
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::collectProgram(ProgramAST& program) {
    LUC_LOG_SEMANTIC("SemanticCollector::collectProgram: file=" 
                     << _pool.lookup(program.filePath));
    
    // Ensure global scope exists
    if (_symbols.currentDepth() == 0) {
        _symbols.pushScope();
    }
    
    // Collect all top-level declarations
    for (auto& decl : program.decls) {
        if (decl) {
            decl->accept(*this);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Symbol Management
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// declareSymbol  —  Registers a symbol in the current scope of the symbol table.
//
// Performs the actual insertion of a Symbol into the symbol table after basic
// validation. Checks for duplicate declarations in the same scope, reports
// errors, and updates the symbol table if the declaration is valid.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Centralises the logic for adding a symbol to the symbol table, ensuring that
// duplicates are caught early and that error messages are consistent. Called
// by every visit() method for declarations that create a new symbol.
//
// ─── Algorithm ───────────────────────────────────────────────────────────────
//   1. Log the symbol name and kind (debugging).
//   2. Check if a symbol with the same name already exists in the current
//      scope using symbols_.lookupLocal(sym.name).
//   3. If duplicate exists:
//        - Report an error (duplicate declaration) and return.
//   4. Call symbols_.declare(sym) to insert the symbol.
//   5. If declare() returns false (should not happen after duplicate check,
//      but defensive), report a generic failure error.
//
// ─── Cases Covered (successful declaration) ──────────────────────────────────
//   - First declaration of a name in the current scope → symbol added.
//   - Same name declared in an outer scope (allowed) → symbol added in
//     current scope (shadows outer).
//   - Symbols with mangled names (methods, variants, conversions) → added
//     without conflict because mangled names are unique.
//
// ─── Cases Covered (error, no declaration) ───────────────────────────────────
//   - Duplicate declaration in the same scope → error reported, symbol not added.
//   - Internal symbol table error (e.g., out of memory) → error reported.
//
// ─── What is NOT covered ─────────────────────────────────────────────────────
//   - Type resolution or checking – symbols are added with type = nullptr.
//   - Visibility checking – that happens later (Phase 3).
//   - Cross‑scope duplicates (same name in different scopes) – allowed.
//
// ─── Note ────────────────────────────────────────────────────────────────────
//   The function uses symbols_.lookupLocal() only, not lookup(), because
//   duplicates are only prohibited in the same scope. Shadowing outer symbols
//   is permitted and is handled naturally by the symbol table's scope stack.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::declareSymbol(const Symbol& sym) {
    LUC_LOG_SEMANTIC_VERBOSE("declareSymbol: name=" << _pool.lookup(sym.name) 
                             << " kind=" << SymbolUtils::kindToString(sym.kind));
    
    // Check for duplicate in current scope
    if (_symbols.lookupLocal(sym.name)) {
        std::string_view nameStr = _pool.lookup(sym.name);
        _dc.error(DiagnosticCategory::Semantic, sym.loc, DiagCode::E3005,
                  "duplicate declaration of '" + std::string(nameStr) + "'");
        return;
    }
    
    if (!_symbols.declare(sym)) {
        std::string_view nameStr = _pool.lookup(sym.name);
        _dc.error(DiagnosticCategory::Semantic, sym.loc, DiagCode::E3005,
                  "failed to declare symbol '" + std::string(nameStr) + "'");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// extractExternMetadata  —  Extracts `@extern` attribute information from a
//                           declaration's attributes and stores it in the Symbol.
//
// Scans the attribute list of a declaration (e.g., function or variable) for
// the `@extern` attribute. If found, it extracts the external symbol name
// (first argument, a string literal) and optionally the calling convention
// (second argument, a type identifier) and stores them in the Symbol's
// `isExtern`, `externSymbol`, and `callingConv` fields.
//
// ─── Purpose ─────────────────────────────────────────────────────────────────
// Enables the semantic analyser and code generator to treat `@extern`‑marked
// declarations as foreign functions or variables that are resolved at link time
// rather than implemented in Luc. The metadata is attached to the Symbol for
// later use during code generation (LLVM external declarations).
//
// ─── Algorithm ───────────────────────────────────────────────────────────────
//   1. Iterate over each AttributePtr in attrs.
//   2. For each attribute, look up its name via pool_.lookup(attr->name).
//   3. If the name is "extern":
//        - Set sym.isExtern = true.
//        - If there is at least one argument and it is a StringLit,
//          assign sym.externSymbol = arg->value (the C/OS symbol name).
//        - If there is a second argument and it is a TypeIdent,
//          assign sym.callingConv = arg->value (e.g., "C", "stdcall").
//   4. Other attributes are ignored (no action).
//
// ─── Cases Covered ───────────────────────────────────────────────────────────
//   - `@extern("malloc")` → isExtern = true, externSymbol = "malloc".
//   - `@extern("printf", C)` → callingConv = "C".
//   - Multiple `@extern` attributes on the same declaration → last one
//     overwrites (not typical, but harmless).
//   - No `@extern` attribute → sym remains unchanged (default false).
//
// ─── What is NOT covered ─────────────────────────────────────────────────────
//   - Validates that the attribute arguments are of the correct type – the
//     parser guarantees StringLit and TypeIdent for the expected positions.
//   - Checks that the declaration is a function or variable – the caller must
//     only call this on declarations that can be `@extern` (e.g., FuncDeclAST,
//     VarDeclAST). Other declarations (struct, enum) ignore `@extern`.
//   - Semantically validates the extern symbol name (e.g., that it is a
//     valid C identifier) – that is left to the code generator.
//   - Handles default calling convention – if no second argument, callingConv
//     remains the default (InternedString() which maps to empty string, codegen
//     defaults to "C").
//
// ─── Note ────────────────────────────────────────────────────────────────────
//   This function is called from visit(FuncDeclAST) and visit(VarDeclAST) before
//   declareSymbol(), so the extern metadata is part of the symbol from the start.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::extractExternMetadata(const std::vector<AttributePtr>& attrs, 
                                               Symbol& sym) {
    for (const auto& attr : attrs) {
        if (!attr) continue;
        
        std::string_view attrName = _pool.lookup(attr->name);
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
                                     << _pool.lookup(sym.externSymbol));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(UseDeclAST)
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(UseDeclAST& node) {
    // Build the full module path as a string (for mangling)
    std::string fullPath;
    for (size_t i = 0; i < node.path.size(); ++i) {
        if (i > 0) fullPath += ".";
        fullPath += _pool.lookup(node.path[i]);
    }
    InternedString pathStr = _pool.intern(fullPath);

    // Determine the symbol name: alias if present, otherwise the last path segment
    InternedString symName = node.alias.value_or(node.path.back());

    Symbol sym;
    sym.name = symName;
    sym.kind = SymbolKind::Module;
    sym.visibility = node.visibility;
    sym.decl = &node;
    sym.loc = node.loc;
    sym.type = nullptr; // modules have no type

    declareSymbol(sym);
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(VarDeclAST)
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(VarDeclAST& node) {
    LUC_LOG_SEMANTIC("SemanticCollector::visit(VarDeclAST): name=" 
                     << _pool.lookup(node.name));
    
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
                     << _pool.lookup(node.name));
    
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
                     << _pool.lookup(node.name));
    
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
                     << _pool.lookup(node.name));
    
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
        std::string mangledName = NameMangler::mangleEnumVariant(_pool.lookup(node.name), _pool.lookup(variant->name));
        InternedString mangledInterned = _pool.intern(mangledName);

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
                     << _pool.lookup(node.name));
    
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
        std::string mangledName = NameMangler::mangleMethod(_pool.lookup(node.name), _pool.lookup(method->name));
        InternedString mangledInterned = _pool.intern(mangledName);
        
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
                     << _pool.lookup(node.structName) 
                     << ", methods=" << node.methods.size());

    if (node.traitRef) {
        _structTraits[node.structName].push_back(node.traitRef->name);
    }
    
    // Impl blocks themselves don't create symbols, but their methods do.
    // Methods are stored with mangled names: StructName.methodName
    for (const auto& method : node.methods) {
        if (!method) continue;
        
        // Create mangled name for the method
        // pool_.lookup returns a string_view; convert to std::string before concatenation
        std::string mangledName = NameMangler::mangleMethod(_pool.lookup(node.structName), _pool.lookup(method->name));
        InternedString mangledInterned = _pool.intern(mangledName);
        
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
                     << _pool.lookup(node.targetTypeName)
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
        std::string mangledName = NameMangler::mangleFrom(_pool.lookup(node.targetTypeName), firstParamType, _pool);
        InternedString mangledInterned = _pool.intern(mangledName);
        
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
                     << _pool.lookup(node.name));
    
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