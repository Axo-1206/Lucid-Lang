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
| Execution model | JIT (Just-In-Time), cross-platform |
| Primary use case | Systems + graphics programming (Vulkan) |
| Build system | CMake + vcpkg (`x64-windows`, 2024-04-23) |

---

## Design Philosophy

Luc follows a **functional / composite / module** paradigm.

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
│   ├── Tokens.hpp          # all token definitions
│   ├── lexer/
│   │   ├── Lexer.hpp
│   │   └── Lexer.cpp
│   ├── ast/
│   │   ├── BaseAST.hpp
│   │   ├── TypeAST.hpp
│   │   ├── DeclAST.hpp
│   │   ├── ExprAST.hpp
│   │   └── StmtAST.hpp
│   ├── diagnostics/
│   │   ├── Diagnostic.hpp
│   │   ├── DiagnosticCodes.hpp
│   │   ├── DiagnosticEngine.cpp
│   │   ├── DiagnosticEngine.hpp
│   ├── parser/
│   │   ├── Parser.hpp
│   │   ├── Parser.cpp
│   │   ├── ParserType.cpp
│   │   ├── ParserDecl.cpp
│   │   ├── ParserExpr.cpp
│   │   └── ParserStmt.cpp
│   ├── semantic/           # in progress
│   └── codegen/            # pending
├── docs/
│   ├── LUC_ERROR.md                ← error library
│   ├── LUC_IO.md                   ← io library
│   ├── LUC_EXAMPLES.md             ← example code
│   ├── LUC_GRAMMAR.md              ← syntax + grammar rules
│   └── LUC_EXAMPLES.md             ← annotated code examples
├── language_support/luc-syntax-highlighter/
├── tests/
└── CMakeLists.txt

```

---

## Compiler Pipeline

```
Source (.luc)
    │
    ▼
[ Lexer ]           ✅  Complete
    │
    ▼
[ AST ]             🔨  In progress
    │
    ▼
[ Parser ]          ✅  Grammar complete — implementation in progress
    │
    ▼
[ Semantic ]        🔧  Setting up
    │
    ▼
[ IR / LLVM ]       ⏳  Pending
    │
    ▼
[ JIT / Codegen ]   ⏳  Pending
```

---

## Semantic Phase — Goals

The semantic pass is the current focus. It must enforce:

- [ ] Symbol table construction and scope resolution
- [ ] Type checking
- [ ] Table field validation (typed composites)
- [ ] Module import resolution (must resolve at semantic time)
- [ ] Function signature checking
- [ ] Rejection of class / inheritance constructs (hard language rule)
- [ ] Nullable type enforcement (`val` forbids nil anywhere in type tree)

---

## Variable Declaration Model

| Keyword | Reassignable | Mutable in place | Nil allowed |
|---|---|---|---|
| `let` | ✅ | ✅ | ✅ |
| `imt` | ❌ | ❌ | ✅ |
| `val` | ❌ | ❌ | ❌ (entire type tree) |
