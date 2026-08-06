/// @file SemaType.hpp
/// @brief Type resolution and validation conventions.
/// 
/// # Return Value Conventions
/// 
/// ## nullptr - "Type Does Not Exist"
/// 
/// Return `nullptr` when the type fundamentally does not exist or cannot be
/// resolved. This is a hard error that should stop further processing of this
/// declaration.
/// 
/// Examples:
///   - `ctx.lookupType(name)` returns nullptr → type was never declared
///   - `resolveNamedType()` fails to find a type → return nullptr
///   - Generic arity mismatch → return nullptr (the type itself is invalid)
/// 
/// ## UnknownTypeAST - "Type Exists But Is Unknown/Invalid"
/// 
/// Return `ctx.getUnknownType()` when the type exists syntactically but its
/// semantic meaning is unknown or invalid. This is a soft error that allows
/// the compiler to continue analyzing the rest of the AST.
/// 
/// Examples:
///   - `resolveExprWithTarget()` fails → return UnknownTypeAST
///   - Type mismatch in an expression → return UnknownTypeAST
///   - Invalid operation on a type → return UnknownTypeAST
/// 
/// ## Why Two Different Signals?
/// 
/// This distinction allows the compiler to distinguish between:
///   1. "This type doesn't exist at all" → stop processing this declaration
///   2. "This expression has a problem" → continue analyzing surrounding code
/// 
/// For a declaration like `let x int = y + z`, if `y` is unknown, we still
/// want to check `z` and report all errors, not stop at the first one.
/// 
/// ## Rule of Thumb
/// 
/// | Situation                         | Return Value     | Reason                     |
/// | --------------------------------- | ---------------- | -------------------------- |
/// | Type lookup failed (undeclared)   | `nullptr`        | Type doesn't exist         |
/// | Expression type resolution failed | `UnknownTypeAST` | Type exists but is invalid |
/// | Generic arity mismatch            | `nullptr`        | The type itself is invalid |
/// | Type mismatch in assignment       | `UnknownTypeAST` | Expression is invalid      |
/// | Invalid operation on type         | `UnknownTypeAST` | Expression is invalid      |
/// | Const evaluation failed           | `UnknownTypeAST` | Expression is invalid      |
/// 
/// This file includes all sub-headers for convenience.
/// Use this when you need type resolution, comparison, or validation.
/// 
/// # File Organization
/// 
/// | File             | Purpose                                             |
/// | ---------------- | --------------------------------------------------- |
/// | SemaResolve.hpp  | Type resolution - convert type AST to semantic type |
/// | SemaCompare.hpp  | Type comparison and assignability                   |
/// | SemaValidate.hpp | Semantic validation rules                           |
/// 
/// # Quick Reference
/// 
/// ## Type Resolution (SemaResolve.hpp)
/// 
/// | What you need               | Function                             | Notes                                                     |
/// | --------------------------- | ------------------------------------ | --------------------------------------------------------- |
/// | Resolve any type annotation | `resolveType(type, ctx)`             | Main entry point, dispatches to specific resolvers        |
/// | Resolve a primitive type    | `resolvePrimitiveType(type, ctx)`    | Always succeeds (built-in)                                |
/// | Resolve a named type        | `resolveNamedType(type, ctx)`        | Checks generic params, looks up declaration               |
/// | Resolve a qualified type    | `resolveModuleTypeAccess(type, ctx)` | Handles `module:Type` syntax                              |
/// | Resolve an array type       | `resolveArrayType(type, ctx)`        | Validates element type, checks Downward Flow              |
/// | Resolve a nullable type     | `resolveNullableType(type, ctx)`     | Validates inner type (no arrays/functions)                |
/// | Resolve a fallible type     | `resolveFallibleType(type, ctx)`     | Validates inner type (no arrays/functions)                |
/// | Resolve a combined type     | `resolveCombinedType(type, ctx)`     | Validates inner type (no arrays/functions)                |
/// | Resolve a reference type    | `resolveRefType(type, ctx)`          | Checks Downward Flow Rule                                 |
/// | Resolve a pointer type      | `resolvePtrType(type, ctx)`          | Structural validation only (FFI checked separately)       |
/// | Resolve a function type     | `resolveFuncType(type, ctx)`         | Validates params, return type, recursion                  |
/// | Resolve a trait reference   | `resolveTraitRef(ref, ctx)`          | Returns TraitDeclAST, used in constraints                 |
/// | Resolve a function callee   | `resolveCalleeOrError(callee, ctx)`  | For function calls, handles identifiers and module access |
/// 
/// ## Type Comparison (SemaCompare.hpp)
/// 
/// | What you need                      | Function                                   | Notes                                  |
/// | ---------------------------------- | ------------------------------------------ | -------------------------------------- |
/// | Compare two types structurally     | `typesEqual(a, b)`                         | Deep equality check                    |
/// | Unwrap nullable layer              | `unwrapNullable(type)`                     | Strips `?` or `?!`, returns inner type |
/// | Unwrap fallible layer              | `unwrapFallible(type)`                     | Strips `!` or `?!`, returns inner type |
/// | Check assignability                | `isAssignable(target, source, ctx)`        | Includes widening, trait conformance   |
/// | Check if type is nullable          | `isNullableType(type)`                     | `T?` or `T?!`                          |
/// | Check if type is fallible          | `isFallibleType(type)`                     | `T!` or `T?!`                          |
/// | Check if type is a reference       | `isReferenceType(type)`                    | `&T`                                   |
/// | Check if type is a pointer         | `isPointerType(type)`                      | `*T`                                   |
/// | Check if type is primitive         | `isPrimitiveType(type)`                    | Any built-in type                      |
/// | Check if type is bool              | `isBoolType(type)`                         | `bool`                                 |
/// | Check if type is integer           | `isIntegerType(type)`                      | `int`, `uint`, `int8`, etc.            |
/// | Check if type is float             | `isFloatType(type)`                        | `float`, `double`, `decimal`           |
/// | Check if type is numeric           | `isNumericType(type)`                      | Integer or float                       |
/// | Check if type is string            | `isStringType(type)`                       | `string`                               |
/// | Check if type is char              | `isCharType(type)`                         | `char`                                 |
/// | Check if type is a struct          | `isStructType(type, ctx)`                  | Named type that resolves to StructDecl |
/// | Check if type is an enum           | `isEnumType(type, ctx)`                    | Named type that resolves to EnumDecl   |
/// | Check if type is a trait           | `isTraitType(type, ctx)`                   | Named type that resolves to TraitDecl  |
/// | Check if type is a generic param   | `isGenericParamType(type, ctx)`            | `T` from `<T>`                         |
/// | Check if type is switch-compatible | `isValidSwitchType(type, ctx)`             | Integers, bool, char, string, enums    |
/// | Get enum declaration from type     | `getEnumDeclFromType(type, ctx)`           | For enum exhaustiveness checking       |
/// | Check case value compatibility     | `isSwitchCaseCompatible(value, type, ctx)` | Literal or enum variant matching       |
/// | Check FFI compatibility            | `isValidFFIType(type, ctx)`                | Types that can cross C boundary        |
/// 
/// ## Semantic Validation (SemaValidate.hpp)
/// 
/// | What you need                | Function                                                     | Notes                                         |
/// | ---------------------------- | ------------------------------------------------------------ | --------------------------------------------- |
/// | Validate const type          | `validateConstType(type, name, kind, ctx)`                   | Must be definite (non-nullable, non-fallible) |
/// | Validate const initializer   | `validateConstInitializer(hasInit, name, kind, ctx)`         | Const must have an initializer                |
/// | Validate single trait impl   | `validateTraitImplementation(structDecl, traitDecl, ctx)`    | Checks fields match trait requirements        |
/// | Validate all trait impls     | `validateAllTraitImplementations(structDecl, ctx)`           | Checks all traits, detects conflicts          |
/// | Check trait field conflicts  | `checkTraitFieldConflicts(structDecl, ctx)`                  | Same name, different type across traits       |
/// | Validate generic arguments   | `validateGenericArguments(args, params, useSite, ctx)`       | Arity and constraint checking                 |
/// | Validate generic param usage | `validateGenericParameterUsage(params, types, useSite, ctx)` | All params must be used                       |
/// | Validate reference context   | `validateRefContext(type, ctx)`                              | Downward Flow Rule enforcement                |
/// | Validate foreign function    | `validateForeignFunction(decl, attr, ctx)`                   | ABI, FFI types, no body, no generics          |
/// 
/// ## Self-Reference Helpers (SemaResolve.hpp)
/// 
/// | What you need                  | Function                                                 | Notes                       |
/// | ------------------------------ | -------------------------------------------------------- | --------------------------- |
/// | Check let self-reference       | `checkLetSelfReference(expr, varName, ctx)`              | `let x = x` is invalid      |
/// | Validate struct self-reference | `isValidStructSelfReference(fieldType, structDecl, ctx)` | Must be nullable or pointer |
/// | Check field on generic type    | `isFieldAccessibleOnGenericType(type, fieldName, ctx)`   | Via trait constraints       |
/// | Get field type on generic type | `getFieldTypeOnGenericType(type, fieldName, ctx)`        | From trait constraints      |
/// 
/// ## Expression Resolution (SemaExpr.cpp)
/// 
/// | What you need                  | Function                                          | Notes                                       |
/// | ------------------------------ | ------------------------------------------------- | ------------------------------------------- |
/// | Resolve expression with target | `resolveExprWithTarget(expr, targetType, ctx)`    | Main entry point, validates against target  |
/// | Resolve expression (no target) | `resolveExpr(expr, ctx)`                          | Legacy wrapper, use resolveExprWithTarget   |
/// | Resolve literal                | `resolveLiteralExpr(expr, targetType, ctx)`       | Handles all literal kinds                   |
/// | Resolve identifier             | `resolveIdentifierExpr(expr, targetType, ctx)`    | Variable, function, enum variant lookup     |
/// | Resolve array literal          | `resolveArrayLiteralExpr(expr, targetType, ctx)`  | Type checks all elements                    |
/// | Resolve struct literal         | `resolveStructLiteralExpr(expr, targetType, ctx)` | Field validation, generic args              |
/// | Resolve binary expression      | `resolveBinaryExpr(expr, targetType, ctx)`        | Operator validation, narrowing detection    |
/// | Resolve unary expression       | `resolveUnaryExpr(expr, targetType, ctx)`         | Negation, logical not, bitwise not          |
/// | Resolve function call          | `resolveCallExpr(expr, targetType, ctx)`          | Callee resolution, arg validation, variadic |
/// | Resolve intrinsic call         | `resolveIntrinsicCallExpr(expr, targetType, ctx)` | Compiler built-ins                          |
/// | Resolve index expression       | `resolveIndexExpr(expr, targetType, ctx)`         | Array indexing, l-value propagation         |
/// | Resolve slice expression       | `resolveSliceExpr(expr, targetType, ctx)`         | Range slicing, returns borrowed view        |
/// | Resolve field access           | `resolveFieldAccessExpr(expr, targetType, ctx)`   | Struct/enum field access, l-value           |
/// | Resolve module access          | `resolveModuleAccessExpr(expr, targetType, ctx)`  | `module:member` access                      |
/// | Resolve null coalesce          | `resolveNullCoalesceExpr(expr, targetType, ctx)`  | `??` operator                               |
/// | Resolve assignment             | `resolveAssignExpr(expr, targetType, ctx)`        | L-value, const, compound ops                |
/// | Resolve pipeline               | `resolvePipelineExpr(expr, targetType, ctx)`      | `|>` chain validation                      |
/// | Resolve composition            | `resolveComposeExpr(expr, targetType, ctx)`       | `+>` compile-time function composition      |
/// | Resolve anonymous function     | `resolveAnonFuncExpr(expr, targetType, ctx)`      | Function literal with body                  |
/// | Resolve if expression          | `resolveIfExpr(expr, targetType, ctx)`            | Branch type compatibility                   |
/// | Resolve range expression       | `resolveRangeExpr(expr, targetType, ctx)`         | Range bounds validation                     |
/// 
/// ## Statement Resolution (SemaStmt.cpp)
/// 
/// | What you need                 | Function                                 | Notes                                |
/// | ----------------------------- | ---------------------------------------- | ------------------------------------ |
/// | Resolve a statement           | `resolveStmt(stmt, ctx)`                 | Main entry point, dispatches by kind |
/// | Resolve a block               | `resolveBlock(block, ctx)`               | Returns true if block returns        |
/// | Resolve if statement          | `resolveIfStmt(ifStmt, ctx)`             | Narrowing, type checking             |
/// | Resolve switch statement      | `resolveSwitchStmt(switchStmt, ctx)`     | Exhaustiveness, case compatibility   |
/// | Resolve for statement         | `resolveForStmt(forStmt, ctx)`           | Range vs collection iteration        |
/// | Resolve while statement       | `resolveWhileStmt(whileStmt, ctx)`       | Condition type checking              |
/// | Resolve do-while statement    | `resolveDoWhileStmt(doWhileStmt, ctx)`   | Body-first loop                      |
/// | Resolve return statement      | `resolveReturnStmt(returnStmt, ctx)`     | Type matches current return          |
/// | Resolve break statement       | `resolveBreakStmt(breakStmt, ctx)`       | Must be inside loop                  |
/// | Resolve continue statement    | `resolveContinueStmt(continueStmt, ctx)` | Must be inside loop                  |
/// | Resolve expression statement  | `resolveExprStmt(exprStmt, ctx)`         | Discard value, warn on non-void      |
/// | Resolve declaration statement | `resolveDeclStmt(declStmt, ctx)`         | Registers nested declarations        |
/// | Resolve async statement       | `resolveAsyncStmt(asyncStmt, ctx)`       | Cooperative concurrency              |
/// | Resolve await statement       | `resolveAwaitStmt(awaitStmt, ctx)`       | Wait for async operations            |
/// | Resolve spawn statement       | `resolveSpawnStmt(spawnStmt, ctx)`       | OS thread parallelism                |
/// | Resolve join statement        | `resolveJoinStmt(joinStmt, ctx)`         | Wait for spawned threads             |
/// 
/// ## Declaration Resolution (SemaDecl.cpp)
/// 
/// | What you need           | Function                                    | Notes                             |
/// | ----------------------- | ------------------------------------------- | --------------------------------- |
/// | Register import name    | `registerImportName(importDecl, ctx)`       | Phase 1: module alias             |
/// | Register variable name  | `registerVarName(varDecl, ctx)`             | Phase 1: value namespace          |
/// | Register function name  | `registerFuncName(funcDecl, ctx)`           | Phase 1: value, params, generics  |
/// | Register parameter name | `registerParamName(param, ctx)`             | Phase 1/2: value namespace        |
/// | Register generic param  | `registerGenericParamName(param, ctx)`      | Phase 1: generic namespace        |
/// | Register enum name      | `registerEnumName(enumDecl, ctx)`           | Phase 1: type + variants          |
/// | Register trait name     | `registerTraitName(traitDecl, ctx)`         | Phase 1: type + generics          |
/// | Register struct name    | `registerStructName(structDecl, ctx)`       | Phase 1: type + fields + generics |
/// | Register struct fields  | `registerStructFieldNames(structDecl, ctx)` | Phase 1: field values             |
/// | Resolve import          | `resolveImportDecl(importDecl, ctx)`        | Phase 2: module validation        |
/// | Resolve variable        | `resolveVarDecl(varDecl, ctx)`              | Phase 2: type, init, const eval   |
/// | Resolve function        | `resolveFuncDecl(funcDecl, ctx)`            | Phase 2: type, body, foreign      |
/// | Resolve parameter       | `resolveParam(param, ctx)`                  | Phase 2: type, const check        |
/// | Resolve generic param   | `resolveGenericParam(param, ctx)`           | Phase 2: constraint resolution    |
/// | Resolve enum            | `resolveEnumDecl(enumDecl, ctx)`            | Phase 2: variants, backing type   |
/// | Resolve trait           | `resolveTraitDecl(traitDecl, ctx)`          | Phase 2: fields, generics         |
/// | Resolve struct          | `resolveStructDecl(structDecl, ctx)`        | Phase 2: fields, traits, generics |
/// | Resolve struct fields   | `resolveStructFields(structDecl, ctx)`      | Phase 2: field types, defaults    |
/// 
/// ## Context Stack (ContextStack.hpp)
/// 
/// | What you need                     | Method                                | Notes                     |
/// | --------------------------------- | ------------------------------------- | ------------------------- |
/// | Are we inside a function?         | `insideFunction()`                    | Context kind check        |
/// | Are we inside a loop?             | `insideLoop()`                        | Break/continue allowed    |
/// | Are we inside a switch?           | `insideSwitch()`                      | Case/default allowed      |
/// | Are we analyzing an if condition? | `isIfConditionCtx()`                  | Narrowing detection       |
/// | What's the narrowed type of X?    | `getNarrowedType(name)`               | From if conditions        |
/// | Current expected return type      | `currentReturnType()`                 | For return validation     |
/// | Push function context             | `pushFunction(node, returnType, loc)` | With return type tracking |
/// | Push loop context                 | `pushLoop(loopStmt, loc)`             | For break/continue        |
/// | Push switch context               | `pushSwitch(switchStmt, loc)`         | For exhaustiveness        |

#pragma once

#include "SemaResolve.hpp"
#include "SemaCompare.hpp"
#include "SemaValidate.hpp"