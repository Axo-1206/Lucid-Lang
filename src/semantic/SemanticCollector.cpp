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

SemanticCollector::SemanticCollector(SymbolTable& symbols, DiagnosticEngine& dc)
    : symbols_(symbols), dc_(dc) {}

// ─────────────────────────────────────────────────────────────────────────────
// collectProgram  — Entry point to index a parsed file
//
// Passes each top-level statement through the AST visitor mechanics to process
// and register its definitions.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::collectProgram(ProgramAST& program) {
    for (auto& decl : program.decls) {
        decl->accept(*this);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// declareSymbol  — Safely registers the structured semantic tracking info
//
// Tries to push the Symbol into the SymbolTable. Failure designates a pre-existing 
// identifier in this exact scope, raising `DiagCode::E3005` safely without crashing.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::declareSymbol(const Symbol& sym) {
    if (!symbols_.declare(sym)) {
        // Find existing to report properly, though dc_.error is enough here
        dc_.error(DiagnosticCategory::Semantic, sym.loc, DiagCode::E3005,
                  "symbol '" + sym.name + "' is already declared in this scope");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(VarDeclAST)  — Simple top-level global variable/constant registration
//
// Inserts the top-level let, imt, or val definition name into the global map.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(VarDeclAST& node) {
    declareSymbol({
        node.name,
        SymbolKind::Var,
        node.keyword,
        node.visibility,
        node.type.get(),
        &node,
        false,
        node.loc
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(FuncDeclAST)  — Collects top-level functions and checks parameter clashes
//
// Registers the function identifier in the main scope. Temporarily pushes
// an inner scope to quickly process arguments, confirming no two parameters 
// share the same name locally, then instantly discards the parameter bindings.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(FuncDeclAST& node) {
    declareSymbol({
        node.name,
        SymbolKind::Func,
        node.keyword,
        node.visibility,
        node.returnType.get(),
        &node,
        node.isAsync,
        node.loc
    });

    // Register params to check for duplicates
    symbols_.pushScope();
    for (const auto& group : node.paramGroups) {
        for (const auto& param : group) {
            declareSymbol({
                param->name,
                SymbolKind::Param,
                DeclKeyword::Let,
                Visibility::Private,
                param->type.get(),
                param.get(),
                false,
                param->loc
            });
        }
    }
    symbols_.popScope();
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(StructDeclAST)  — Maps structures and cross-checks internal field shapes
//
// Like functions, maps the struct globally. Pushes a mock localized scope to
// iterate through the struct's definition, asserting no duplicate field aliases
// are used before popping the ephemeral scope.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(StructDeclAST& node) {
    declareSymbol({
        node.name,
        SymbolKind::Struct,
        DeclKeyword::Let, // N/A
        node.visibility,
        nullptr,
        &node,
        false,
        node.loc
    });

    // Register fields to check for duplicates
    symbols_.pushScope();
    for (const auto& field : node.fields) {
        declareSymbol({
            field->name,
            SymbolKind::Field,
            DeclKeyword::Let,
            Visibility::Private,
            field->type.get(),
            field.get(),
            false,
            field->loc
        });
    }
    symbols_.popScope();
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(EnumDeclAST)  — Registers enumerations and uniqueness of their choices
//
// Submits the enum label to the main table, pushing a localized scope to enforce 
// uniquely labelled variant flags.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(EnumDeclAST& node) {
    declareSymbol({
        node.name,
        SymbolKind::Enum,
        DeclKeyword::Let,
        node.visibility,
        nullptr,
        &node,
        false,
        node.loc
    });

    symbols_.pushScope();
    for (const auto& variant : node.variants) {
        declareSymbol({
            variant->name,
            SymbolKind::EnumVariant,
            DeclKeyword::Let,
            Visibility::Private,
            nullptr,
            variant.get(),
            false,
            variant->loc
        });
    }
    symbols_.popScope();
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(TraitDeclAST)  — Connects method contract names into semantic awareness
//
// Adds the trait name itself, validating inside an ephemeral scope that no
// internal method signatures possess exactly duplicate naming.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(TraitDeclAST& node) {
    declareSymbol({
        node.name,
        SymbolKind::Trait,
        DeclKeyword::Let,
        node.visibility,
        nullptr,
        &node,
        false,
        node.loc
    });

    symbols_.pushScope();
    for (const auto& method : node.methods) {
        declareSymbol({
            method->name,
            SymbolKind::Method,
            DeclKeyword::Let,
            Visibility::Private,
            method->returnType.get(),
            method.get(),
            false,
            method->loc
        });
    }
    symbols_.popScope();
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(ImplDeclAST)  — Validates Implementation blocks structure bindings
//
// Struct instance actions aren't directly available without instance traversal.
// To map them, we synthesize artificial `StructName.methodName` tags on the
// global scope index. It catches multi-impl blocks conflicting via same names.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(ImplDeclAST& node) {
    // Merge methods onto the struct's namespace by mangling their names.
    // E.g., StructName.methodName in the global scope.
    for (const auto& method : node.methods) {
        std::string mangledName = node.structName + "." + method->name;
        declareSymbol({
            mangledName,
            SymbolKind::Method,
            DeclKeyword::Let,
            node.visibility, // Inherit impl visibility conceptually
            method->returnType.get(),
            method.get(),
            method->isAsync,
            method->loc
        });
    }

    for (const auto& fromDecl : node.fromDecls) {
        // Name can be structName.from.srcParamType (if we had the name of the type stringified).
        // Since from blocks are also duplicates if same srcParamType, we need a way to track them.
        // We'll record them under StructName.from for now overloads might need distinct names based on types
        // in a more advanced pass.
        std::string mangledName = node.structName + ".from." + fromDecl->srcParamName;
        declareSymbol({
            mangledName,
            SymbolKind::Method,
            DeclKeyword::Let,
            node.visibility,
            nullptr, // we don't know the exact type AST easily yet
            fromDecl.get(),
            false,
            fromDecl->loc
        });
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(TypeAliasDeclAST)  — Assigns proxy labels to underlying complex shapes
//
// Stores the 'type XYZ = int' alias safely on the central scope.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(TypeAliasDeclAST& node) {
    declareSymbol({
        node.name,
        SymbolKind::TypeAlias,
        DeclKeyword::Let,
        node.visibility,
        node.aliasedType.get(),
        &node,
        false,
        node.loc
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// visit(ExternDeclAST)  — Collects external bindings for LLVM linkers
//
// Treat identically to standard `FuncDeclAST` nodes, indexing the base extern
// function and temporarily asserting parameter label uniqueness.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticCollector::visit(ExternDeclAST& node) {
    declareSymbol({
        node.name,
        SymbolKind::Func, // Extern funcs are treated similarly
        DeclKeyword::Let,
        Visibility::Private,
        node.returnType.get(),
        &node,
        false,
        node.loc
    });

    symbols_.pushScope();
    for (const auto& param : node.params) {
        declareSymbol({
            param->name,
            SymbolKind::Param,
            DeclKeyword::Let,
            Visibility::Private,
            param->type.get(),
            param.get(),
            false,
            param->loc
        });
    }
    symbols_.popScope();
}
