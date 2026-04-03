---
applyTo: "**/*.cpp", "**/*.hpp", "**/*.luc"
description: "Use when: generating code, writing functions, or implementing features in the Luc compiler project."
---

# Luc Code Generation Instructions

When generating or suggesting code for the Luc compiler:

- **Always read `./context/LUC_GRAMMAR.md` first** to understand the language syntax, rules, and constructs. Ensure generated code adheres to the grammar (e.g., no classes/inheritance, struct-impl paradigm, module system).

- For I/O-related code (e.g., input handling, file operations, event binding): Read `./context/LUC_IO.md` to align with the io library and game-development support.

- For error handling or diagnostics: Read `./context/LUC_ERROR.md` to follow error reporting standards and nullable type enforcement.

- Prioritize functional/composite/module paradigm: Use first-class functions, struct-impl, and reject OOP constructs.

- Reference `./context/LUC_PROJECT_OVERVIEW.md` for design philosophy and `./context/TASK_LOG.md` for pipeline status and tasks if needed.

- Ensure code is cross-platform, JIT-compatible, and optimized for Vulkan/graphics programming.