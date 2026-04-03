## Tasks

| Date | ID | Name | Description |
|---|---|---|---|


---

## Log

| Date | Note |
|---|---|
| 2026-02-16 | set up the llvm + project base |
| unrecorded | Major redesign I, a language with scripting style, aim to be c alternative but with lua style - (not performance, the syntax cause too much ambiguity) |
| unrecorded | Major redesign II, metatable, compisite table like lua but with improvement - (abandoned, too much inconsistent with compiler design) |
| 2026-03-10 | Migrated context from Gemini to Claude and start |
| 2026-03-11 | Major redesign III and restart to Tokenize phase, the language grow into functional programming style with go/rust syntax inspired, change the goal from being c alternate to a flexible functional style language for game development with vulkan |
| 2026-03-12 | Enhance functional programming with composition and currying |
| 2026-03-14 | Grammar: trait keyword added — explicit conformance `impl T : Trait`, structural + declared |
| 2026-03-14 | Grammar: generic constraints finalised — `T : Trait`, multiple via `T : A + B` |
| 2026-03-14 | Grammar: generics wired to func_decl, type_decl, impl_decl (was only on struct) |
| 2026-03-14 | Grammar: module manifest finalised — two-layer visibility (pub = file→package, module = package→world) |
| 2026-03-14 | Grammar: type alias = required, func types allowed on RHS, anonymous struct removed |
| 2026-03-14 | Grammar: `_` placeholder (Model A currying) removed — chained param groups only |
| 2026-03-14 | Grammar: `+>` generic rule finalised — concrete types or explicit instantiation required |
| 2026-03-14 | Grammar: pipeline `->` seed/step distinction — seed is any expr, steps must be functions |
| 2026-03-14 | Grammar: type conversion syntax — `float(x)` safe, `@float(x)` unsafe bit reinterpret |
| 2026-03-14 | Grammar: match finalised — guards, ranges, multiple values, nested struct, array option B |
| 2026-03-14 | Grammar: compound assignment — relaxed desugar model, any type where operator is defined |
| 2026-03-14 | Grammar: parallel finalised — `parallel for` (DOD) and `parallel` block (task), no await inside |
| 2026-03-14 | Grammar: doc comments finalised — three forms, attachment rules, visibility model, Markdown content |
| 2026-03-15 | Grammar: Enum — named constant set, integer-backed, `EnumName.Variant` access |
| 2026-03-15 | Grammar: Improve sugar syntax for function |
| 2026-03-15 | Grammar: all Open/TBD items resolved |
| 2026-03-22 | Grammar: adjust array and trait definition to be more explicit |
| 2026-03-22 | AST Node: complete BaseAST.hpp, TypeAST.hpp, DeclAST.hpp |
| 2026-03-23 | AST Node: complete ExprAST.hpp, StmtAST.hpp, PatternAST.hpp |
| 2026-03-25 | Parser: set up Parser.hpp structure |
| 2026-03-25 | Parser: implement basics top-level file parser and basic helper, like peak(), advance(), and much more |
| 2026-03-28 | Development: dual IDE with vscode(manual debugging) and antigravity(AI support for planning and automatic tasks) |
| 2026-03-28 | Parser: complete parsers implementation |
| 2026-03-28 | ERROR LIB: major adjustment for error library and the way luc handling errors |
| 2026-03-28 | Parser: create error systems + diagnostics, and refactor parsers |
| 2026-03-30 | Semantic: set up semantic file structure |
| 2026-03-30 | Semantic: write SemanticSymbol.hpp, SymbolTable.hpp and SymbolTable.cpp, SemanticCollector.hpp and SemanticCollector.cpp |
| 2026-03-31 | Semantic: adjust header comments and write TypeResolver.hpp and TypeResolver.cpp |
| 2026-03-31 | Semantic: write TypeChecker.hpp and TypeChecker.cpp |
| 2026-03-31 | AST Node: use node kind to void dynamic_cast |