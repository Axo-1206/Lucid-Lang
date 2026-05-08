# Luc — Project Overview

> **Scope of this file:** Project identity, architecture, pipeline status, and build environment.
> Grammar, syntax, and code examples are documented separately in `LUC_GRAMMAR.md`, `LUC_EXAMPLES.md` and standard library like `LUC_IO.md` and `LUC_ERROR.md`.

---

## Project Identity

| Field | Value |
|---|---|
| Language name | `luc` |
| Compiler written in | C++ |
| Compiler backend | LLVM 18.1.6 |
| Execution model | JIT (Just-In-Time), cross-platform and AOT |
| Primary use case | Systems + graphics programming (Vulkan) |
| Build system | CMake + vcpkg (`x64-windows`, 2024-04-23) |

---

## Design Philosophy

Luc follows a **functional / procedural / composite / module** paradigm.

- **No classes or inheritance** — OOP-style hierarchies are intentionally absent and rejected at the semantic level
- **Struct-Impl** (Go-inspired) — primary data and component structure; act as typed composites (struct + behavior, no class semantics)
- **Module system** (Go-inspired file layout) — central to code organization; modules resolve at semantic time
- **First-class functions** — composition over inheritance throughout
- **Vulkan-programming** — the language is designed with graphics programming as a primary target
- **Game-development** — the language support an io library that support binding event to system input(keycode, mouse, touch, ...) and math library for game development

---

## Codebase Structure

```
luc/
├── .agents/
├── .github/
├── .vscode/
├── build/
├── src/
│   ├── main.cpp                # compiler entry point
│   ├── Tokens.hpp              # all token definitions
│   ├── lexer/
│   │   ├── Lexer.hpp
│   │   └── Lexer.cpp
│   ├── ast/
│   │   ├── support/
│   │   │   ├── ASTArena.hpp
│   │   │   ├── InternedString.hpp
│   │   │   ├── StringPool.hpp
│   │   │   └── StringPool.cpp
│   │   ├── BaseAST.hpp         # foundation + ASTVisitor
│   │   ├── TypeAST.hpp
│   │   ├── DeclAST.hpp
│   │   ├── ExprAST.hpp
│   │   └── StmtAST.hpp
│   ├── registry/
│   │   ├── AttributeRegistry.hpp/cpp
│   │   ├── QualifierRegistry.hpp
│   │   ├── BuiltinMethodRegistry.hpp/cpp
│   │   └── IntrinsicRegistry.hpp
│   ├── diagnostics/
│   │   ├── Diagnostic.hpp
│   │   ├── DiagnosticCodes.hpp
│   │   ├── DiagnosticEngine.cpp
│   │   └── DiagnosticEngine.hpp
│   ├── parser/
│   │   ├── Parser.hpp
│   │   ├── Parser.cpp
│   │   ├── ParserType.cpp
│   │   ├── ParserDecl.cpp
│   │   ├── ParserExpr.cpp
│   │   └── ParserStmt.cpp
│   ├── semantic/               # phase 1-4 implementation
│   │   ├── SemanticAnalyzer.hpp/cpp    # driver
│   │   ├── SymbolTable.hpp/cpp         # scope management
│   │   ├── SemanticCollector.hpp/cpp   # phase 1 & 2
│   │   ├── SemanticDecl.cpp            # phase 3 (declarations)
│   │   ├── SemanticExpr.cpp            # phase 3 (expressions)
│   │   ├── SemanticStmt.cpp            # phase 3 (statements)
│   │   ├── TypeResolver.hpp/cpp        # type resolution
│   │   ├── TypeChecker.hpp/cpp         # type compatibility
│   │   ├── Annotator.cpp               # phase 4 (annotations)
│   │   ├── BuiltinMethodRegistry.hpp/cpp
│   │   ├── Intrinsicregistry.hpp
│   │   ├── SemanticHelpers.hpp
│   │   └── SemanticSymbol.hpp
│   ├── codegen/
│   │   ├── CodeGen.hpp
│   │   ├── CodeGen.cpp
│   │   ├── CodeGenDecl.cpp
│   │   ├── CodeGenExpr.cpp
│   │   ├── CodeGenStmt.cpp
│   │   ├── ValueEnv.hpp
│   │   └── luc_runtime.c
│   └── debug/
│       ├── ASTDumper.hpp/cpp
│       ├── DebugMacros.hpp
│       └── DebugUtils.hpp
├── docs/
│   ├── LUC_PROJECT_OVERVIEW.md      ← identity + architecture (this file)
│   ├── LUC_GRAMMAR.md               ← syntax + grammar rules
│   ├── LUC_DIAGNOSTIC_CODES.md      ← diagnostic code definitions
│   ├── TASK_LOG.md                 ← project task tracking
│   ├── phases/                      ← detailed implementation docs
│   │   ├── ASTNODE.md
│   │   ├── LEXER.md
│   │   ├── PARSER.md
│   │   ├── SEMANTIC.md
│   │   └── CODEGEN.md
│   ├── std_libraries/               ← standard library docs
│   │   ├── LUC_ERROR.md
│   │   ├── LUC_IO.md
│   │   └── LUC_REGEX.md
│   └── examples/                    ← code examples
├── language_support/               # IDE extensions/syntax highlighting
├── tests/                          # test suite
└── CMakeLists.txt
```

---

## Compiler Pipeline

```
Source (.luc)
    │
    ▼
[ Lexer ]           
    │
    ▼
[ AST ]             
    │
    ▼
[ Parser ]          
    │
    ▼
[ Semantic ]            
    │
    ▼
[ IR / LLVM ]           
    │
    ▼
[ JIT / Codegen ]   
```