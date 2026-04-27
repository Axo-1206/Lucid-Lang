# Luc Compiler

Luc is a modern systems programming language designed for performance, clarity, and graphics programming (Vulkan). It combines functional and procedural paradigms with a strong focus on composition and explicit semantics.

## Table of Contents
- [Project Overview](#project-overview)
    - [Project Identity](#project-identity)
    - [Design Philosophy](#design-philosophy)
- [Language Grammar](docs/LUC_GRAMMAR.md)

---

## Project Overview

### Project Identity

| Field | Value |
|---|---|
| Language name | `luc` |
| Compiler written in | C++ |
| Compiler backend | LLVM 18.1.6 |
| Execution model | JIT (Just-In-Time), cross-platform and AOT |
| Primary use case | Systems + graphics programming (Vulkan) |
| Build system | CMake + vcpkg (`x64-windows`, 2024-04-23) |

### Design Philosophy

Luc follows a **functional / procedural / composite / module** paradigm.

- **No classes or inheritance** — OOP-style hierarchies are intentionally absent and rejected at the semantic level.
- **Struct-Impl** — Go-inspired primary data and component structure; acts as typed composites (struct + behavior, no class semantics).
- **Module system** — Go-inspired file layout; central to code organization; modules resolve at semantic time.
- **First-class functions** — Composition over inheritance throughout.
- **Vulkan-programming** — Designed with graphics programming as a primary target.
- **Game-development** — This language is used for Lucid game engine project.

---

## Language Grammar

Detailed information about the language syntax, types, and rules can be found in the [LUC_GRAMMAR.md](docs/LUC_GRAMMAR.md) file.
