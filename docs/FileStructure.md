# File Structure

```
lucid/                                 # the language runtime (standalone, embeddable library + CLI)
├── README.md
├── LICENSE
├── CMakeLists.txt
├── .gitignore
│
├── docs/
│   ├── grammar/
│   │   ├── LUCID_GRAMMAR.md            # the language specification
│   │   └── CORE_SCRIPTS.md             # the standard core scripts
│   ├── ARCHITECTURE.md                 # this document
│   ├── BUILD.md
│   └── examples/
│
├── temp/
└── src/
    │
    ├── main.cpp                        # the `lucid` CLI entry point
    │
    ├── core/                           # language-agnostic shared types (no runtime, no host)
    │   ├── SourceLocation.hpp
    │   ├── Tokens.hpp/cpp
    │   ├── ASTStrings.hpp
    │   ├── JSONFormatter.hpp/cpp
    │   ├── ast/                        # AST node definitions
    │   │   ├── BaseAST.hpp
    │   │   ├── DeclAST.hpp
    │   │   ├── ExprAST.hpp
    │   │   ├── StmtAST.hpp
    │   │   ├── TypeAST.hpp
    │   │   └── ResourceKind.hpp/cpp
    │   ├── memory/
    │   │   ├── ASTArena.hpp
    │   │   ├── ArenaSpan.hpp
    │   │   ├── InternedString.hpp
    │   │   └── StringPool.hpp/cpp
    │   ├── registry/
    │   │   ├── AttributeRegistry.hpp/cpp       # A lookup table for the language's built-in attribute
    │   │   └── BuiltinMethodRegistry.hpp/cpp   # A lookup table for the built-in method on a table, a column view, a row, or an array
    │   ├── diagnostics/
    │   │   ├── DiagCode.hpp
    │   │   ├── Diagnostic.hpp/cpp
    │   │   └── StackTrace.hpp/cpp
    │   └── trace/
    │       └── Trace.hpp/cpp
    │
    ├── parser/                         # frontend stage 1 — source text → AST
    │   ├── Parser.hpp/cpp
    │   ├── lexer/
    │   │   └── Lexer.hpp/cpp
    │   ├── context/
    │   │   ├── ParserContext.hpp
    │   │   └── TokenStream.hpp/cpp
    │   ├── rules/
    │   │   ├── ParseDecl.cpp
    │   │   ├── ParseExpr.cpp
    │   │   ├── ParseStmt.cpp
    │   │   └── ParseType.cpp
    │   └── support/
    │       ├── ErrorRecovery.hpp
    │       ├── Helpers.cpp
    │       ├── ParseAttr.cpp           # The attribute parsers
    │       └── GrammarPositions.cpp    # The token sets that name grammatical positions in Lucid
    │
    ├── sema/                           # frontend stage 2 — AST → validated AST
    │   ├── Sema.hpp/cpp
    │   ├── context/
    │   │   ├── SemaContext.hpp/cpp
    │   │   ├── ContextStack.hpp/cpp
    │   │   ├── Generic.hpp/cpp
    │   │   └── Instantiation.cpp
    │   ├── rules/
    │   │   ├── SemaDecl.cpp
    │   │   ├── SemaExpr.cpp
    │   │   └── SemaStmt.cpp
    │   ├── types/
    │   │   ├── SemaType.hpp
    │   │   ├── SemaResolve.cpp
    │   │   ├── SemaTypeEquality.cpp
    │   │   ├── SemaTypePredicates.cpp
    │   │   └── SemaValidate.cpp
    │   ├── const_eval/
    │   │   ├── ConstEvaluator.hpp/cpp
    │   │   ├── ConstEvalBinary.cpp
    │   │   ├── ConstEvalUnary.cpp
    │   │   ├── ConstEvalStatement.cpp
    │   │   └── ConstEvalHelpers.hpp/cpp
    │   ├── registry/
    │   │   ├── AttributeValidator.hpp/cpp
    │   │   ├── IntrinsicValidator.hpp/cpp
    │   │   └── ArgTypeValidators.hpp/cpp
    │   └── support/
    │       ├── CaptureAnalysis.hpp/cpp
    │       ├── MangledName.hpp/cpp
    │       ├── Truthiness.hpp
    │       ├── TypeNarrowHelpers.hpp/cpp
    │       └── SwitchHelpers.hpp/cpp
    │
    ├── bytecode/                       # AST → bytecode module
    │   ├── Bytecode.hpp/cpp
    │   ├── Opcode.hpp
    │   ├── FunctionProto.hpp/cpp
    │   ├── ModuleStateProto.hpp/cpp
    │   ├── Manifest.hpp
    │   ├── ConstantPool.hpp/cpp
    │   ├── HostSymbolTable.hpp/cpp
    │   ├── Serialize.hpp/cpp           # the .lucb serializer
    │   └── compile/
    │       ├── Compiler.hpp/cpp
    │       ├── EmitDecl.cpp
    │       ├── EmitStmt.cpp
    │       ├── EmitExpr.cpp
    │       ├── EmitPlace.cpp
    │       ├── Frame.hpp/cpp
    │       └── ConstantFolding.cpp
    │
    ├── interp/                         # bytecode module → execution
    │   ├── Interpreter.hpp/cpp
    │   ├── Frame.hpp/cpp
    │   ├── Value.hpp/cpp
    │   ├── Dispatch.cpp
    │   ├── Ops/
    │   │   ├── OpsLoadStore.cpp
    │   │   ├── OpsArithmetic.cpp
    │   │   ├── OpsComparison.cpp
    │   │   ├── OpsControl.cpp
    │   │   ├── OpsCall.cpp
    │   │   ├── OpsAggregate.cpp
    │   │   ├── OpsConcurrency.cpp
    │   │   └── OpsHost.cpp
    │   ├── ExecutionResult.hpp
    │   └── InterpreterError.hpp
    │
    ├── runtime-abi/                    # the ABI surface shared by the compiler, runtime, and interpreter
    │   ├── functions.def
    │   ├── lucid_abi.h
    │   └── lucid_runtime.h
    │
    ├── runtime/                        # the runtime library implementation
    │   ├── StringRuntime.cpp
    │   ├── MemoryRuntime.cpp
    │   ├── ClosureRuntime.cpp
    │   ├── ConcurrencyRuntime.hpp/cpp
    │   ├── ConcurrencyEntry.cpp
    │   ├── PanicRuntime.cpp
    │   ├── RuntimeInternal.hpp
    │   ├── RuntimeError.hpp
    │   └── exports.cpp
    │
    ├── host/                           # the embedding API
    │   ├── Registry.hpp/cpp
    │   ├── VM.hpp/cpp
    │   ├── TypeInfo.hpp
    │   ├── Signature.hpp/cpp
    │   ├── HostType.hpp
    │   └── bindings/
    │       ├── BindFunction.hpp
    │       ├── BindMethod.hpp
    │       └── BindStruct.hpp
    │
    ├── stdlib/                         # the standard library (written in Lucid)
    │   ├── core.luc
    │   ├── core.map.luc
    │   ├── core.array.luc
    │   ├── core.string.luc
    │   ├── core.math.luc
    │   ├── core.io.luc
    │   ├── core.fn.luc
    │   └── core.simd.luc
    │
    ├── cli/
    │   ├── CLIContext.hpp
    │   ├── CLIOptions.hpp
    │   ├── RunOptions.hpp
    │   ├── DependencyGraph.hpp
    │   ├── FileWatcher.hpp
    │   ├── ModuleResolver.hpp/cpp
    │   ├── commands/
    │   │   ├── run.hpp/cpp
    │   │   ├── parse.hpp/cpp
    │   │   ├── sema.hpp/cpp
    │   │   ├── compile.hpp/cpp
    │   │   └── build.hpp/cpp           # stub — NOT READY
    │   └── pipeline/
    │       ├── Pipeline.hpp/cpp
    │       └── JSONDumper.hpp/cpp
    │
    └── debug/
        ├── DebugMacros.hpp
        └── DebugUtils.hpp

tests/
```