---
trigger: manual
---

# Luc Master Implementation Rules

## [Section: Core Philosophy]
- **No Classes/Inheritance**: If you see a class-like structure, refactor to `struct` + `impl`.
- **Composition**: Use `+>` for component-based logic. 
- **Visibility**: Only `pub` and `module` (for internal package sharing) are valid.

## [Section: Variable Triplets]
- `let`: Mutable variable (default).
- `imt`: Immutable constant (cannot be reassigned).
- `val`: Value-type/inline (compiler optimization hint).

## [Section: Syntax Delimiters]
- **Separators**: `,` and `;` are OPTIONAL. 
- **Constraint**: Never error out if a comma or semicolon is missing between statements or struct fields if a newline exists.

## [Section: Advanced Functional Features]
- **Currying**: Chained groups ONLY. `let f (a int) (b int)` is curried; `let f (a int, b int)` is NOT.
- **Pipelines**: 
  - `->` is for standard data flow.
  - `+>` is for composition/instantiation flow.
- **Match Statements**: Support `if` guards and `..` ranges. Refer to Section 11 of `@docs/LUC_GRAMMAR.md` for pattern nesting.

## [Section: Agent Behavior]
- **Ref Check**: Always mention which Section of `@docs/LUC_GRAMMAR.md` you are referencing in your Plan.
- **Model Switch**: Start Plan with Gemini 3 Pro. Execute Code with Gemini 3 Flash.