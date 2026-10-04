# File Structure

```
lucid/
│
├── CMakeLists.txt
├── README.md
├── LICENSE
├── .gitignore
│
├── docs/
│   ├── grammar/LUCID_GRAMMAR.md
│   ├── ARCHITECTURE.md
│   ├── MEMORY_MODEL.md
│   └── BUILD.md
│
├── core/
│   ├── include/core/
│   │   ├── SourceLocation.hpp
│   │   ├── Tokens.hpp
│   │   ├── PrimitiveKind.hpp
│   │   ├── ArrayKind.hpp
│   │   ├── ASTStrings.hpp
│   │   ├── JSONFormatter.hpp
│   │   ├── ast/
│   │   │   ├── BaseAST.hpp
│   │   │   ├── DeclAST.hpp
│   │   │   ├── ExprAST.hpp
│   │   │   ├── StmtAST.hpp
│   │   │   └── TypeAST.hpp
│   │   ├── memory/
│   │   │   ├── ASTArena.hpp
│   │   │   ├── ArenaSpan.hpp
│   │   │   ├── InternedString.hpp
│   │   │   └── StringPool.hpp
│   │   ├── registry/
│   │   │   ├── AttributeRegistry.hpp
│   │   │   └── BuiltinMethodRegistry.hpp
│   │   ├── diagnostics/
│   │   │   ├── DiagCode.hpp
│   │   │   ├── Diagnostic.hpp
│   │   │   └── StackTrace.hpp
│   │   └── trace/
│   │       └── Trace.hpp
│   └── src/core/
│       ├── Tokens.cpp
│       ├── JSONFormatter.cpp
│       ├── memory/
│       │   └── StringPool.cpp
│       ├── registry/
│       │   ├── AttributeRegistry.cpp
│       │   └── BuiltinMethodRegistry.cpp
│       ├── diagnostics/
│       │   ├── Diagnostic.cpp
│       │   └── StackTrace.cpp
│       └── trace/
│           └── Trace.cpp
│
├── parser/
│   ├── include/parser/
│   │   └── Parser.hpp
│   └── src/parser/
│       ├── Parser.cpp
│       ├── lexer/
│       │   ├── Lexer.hpp
│       │   └── Lexer.cpp
│       ├── context/
│       │   ├── ParserContext.hpp
│       │   ├── TokenStream.hpp
│       │   └── TokenStream.cpp
│       ├── rules/
│       │   ├── ParseDecl.cpp
│       │   ├── ParseExpr.cpp
│       │   ├── ParseStmt.cpp
│       │   └── ParseType.cpp
│       └── support/
│           ├── ErrorRecovery.hpp
│           ├── Helpers.cpp
│           ├── ParseAttr.cpp
│           ├── GrammarPositions.hpp
│           └── GrammarPositions.cpp
│
├── sema/
│   ├── include/sema/
│   │   └── Sema.hpp
│   └── src/sema/
│       ├── Sema.cpp
│       ├── context/
│       │   ├── SemaContext.hpp  / .cpp
│       │   └── ContextStack.hpp / .cpp
│       ├── rules/
│       │   ├── SemaDecl.cpp
│       │   ├── SemaExpr.cpp
│       │   └── SemaStmt.cpp
│       ├── types/
│       │   ├── SemaType.hpp
│       │   ├── SemaResolve.cpp
│       │   ├── SemaTypeEquality.cpp
│       │   ├── SemaTypePredicates.cpp
│       │   └── SemaValidate.cpp
│       ├── const_eval/
│       │   ├── ConstEvaluator.hpp / .cpp
│       │   ├── ConstEvalBinary.cpp
│       │   ├── ConstEvalUnary.cpp
│       │   ├── ConstEvalLiteral.cpp
│       │   └── ConstEvalHelpers.hpp
│       ├── registry/
│       │   ├── AttributeValidator.hpp / .cpp
│       │   └── ArgTypeValidators.hpp / .cpp
│       └── support/
│           ├── MangledName.hpp / .cpp
│           ├── SequenceChecker.hpp / .cpp
│           ├── TableConstraintChecker.hpp / .cpp
│           └── TypeNarrowHelpers.hpp / .cpp
│
├── contract/
│   ├── include/contract/
│   │   ├── Format.hpp
│   │   ├── Opcode.hpp
│   │   ├── ResourcePlan.hpp
│   │   ├── RuntimeOp.hpp
│   │   ├── FuncSignature.hpp
│   │   └── TypeDescriptor.hpp
│   └── src/contract/
│       ├── Opcode.cpp
│       ├── ResourcePlan.cpp
│       ├── RuntimeOp.cpp
│       └── TypeDescriptor.cpp
│
├── runtime/
│   ├── include/runtime/
│   │   ├── Array.hpp
│   │   ├── Handle.hpp
│   │   ├── HostRegistry.hpp
│   │   ├── Panic.hpp
│   │   ├── String.hpp
│   │   ├── Value.hpp
│   │   └── ValueOps.hpp
│   └── src/runtime/
│       ├── Array.cpp
│       ├── Handle.cpp
│       ├── HostRegistry.cpp
│       ├── Panic.cpp
│       ├── String.cpp
│       └── ValueOps.cpp
│
├── bytecode/
│   ├── include/bytecode/
│   │   ├── Bytecode.hpp
│   │   ├── FunctionProto.hpp
│   │   ├── ConstantPool.hpp
│   │   ├── StaticData.hpp
│   │   ├── HostSymbolTable.hpp
│   │   ├── Manifest.hpp
│   │   ├── Serialize.hpp
│   │   └── compile/
│   │       └── Compiler.hpp
│   └── src/bytecode/
│       ├── Bytecode.cpp
│       ├── ConstantPool.cpp
│       ├── FunctionProto.cpp
│       ├── HostSymbolTable.cpp
│       ├── Manifest.cpp
│       ├── Serialize.cpp
│       ├── StaticData.cpp
│       ├── compile/
│       │   ├── ArtifactBuildState.hpp
│       │   ├── BakeConstant.hpp
│       │   ├── BakeConstant.cpp
│       │   ├── Compiler.cpp
│       │   ├── CompilerContext.hpp
│       │   ├── CompilerContext.cpp
│       │   ├── SlotAllocator.hpp
│       │   ├── SlotAllocator.cpp
│       │   ├── TypeTranslation.hpp
│       │   └── TypeTranslation.cpp
│       ├── emit/
│       │   ├── EmitDecl.hpp
│       │   ├── EmitDecl.cpp
│       │   ├── EmitExpr.hpp
│       │   ├── EmitExpr.cpp
│       │   ├── EmitPlace.hpp
│       │   ├── EmitPlace.cpp
│       │   ├── EmitStmt.hpp
│       │   └── EmitStmt.cpp
│       └── memory/
│           ├── DropSchedule.hpp
│           ├── DropSchedule.cpp
│           ├── EmitCopy.hpp
│           ├── EmitCopy.cpp
│           ├── EmitDrop.hpp
│           ├── EmitDrop.cpp
│           ├── OwnedValue.hpp
│           └── OwnedValue.cpp
│
├── interp/ (in working)
│   ├── include/interp/
│   │   ├── ExecutionResult.hpp
│   │   ├── Frame.hpp
│   │   ├── FunctionRef.hpp
│   │   ├── Interpreter.hpp
│   │   ├── InterpreterConfig.hpp
│   │   ├── LoadedProgram.hpp
│   │   ├── Loader.hpp
│   │   └── TableObject.hpp
│   └── src/interp/ (not implemented,  this is a draft)
│       ├── Interpreter.cpp
│       ├── Frame.hpp / .cpp
│       ├── Value.hpp / .cpp
│       ├── Dispatch.cpp
│       ├── ExecutionResult.hpp
│       ├── InterpreterError.hpp
│       └── Ops/
│           ├── OpsLoadStore.cpp
│           ├── OpsArithmetic.cpp
│           ├── OpsComparison.cpp
│           ├── OpsControl.cpp
│           ├── OpsCall.cpp
│           ├── OpsAggregate.cpp
│           ├── OpsConcurrency.cpp
│           └── OpsHost.cpp
│
├── vm/ (not implemented)
│   ├── include/vm/
│   │   ├── VM.hpp
│   │   ├── Registry.hpp
│   │   ├── TypeInfo.hpp
│   │   ├── Signature.hpp
│   │   ├── HostType.hpp
│   │   └── bindings/
│   │       ├── BindFunction.hpp
│   │       ├── BindMethod.hpp
│   │       └── BindStruct.hpp
│   └── src/vm/
│       ├── VM.cpp
│       ├── Registry.cpp
│       ├── Signature.cpp
│       └── ...
│
├── cli/ (deprecated - in refactoring)
│   └── src/cli/
│       ├── CLIContext.hpp
│       ├── CLIOptions.hpp
│       ├── RunOptions.hpp
│       ├── DependencyGraph.hpp
│       ├── FileWatcher.hpp
│       ├── ModuleResolver.hpp / .cpp
│       ├── commands/
│       │   ├── run.hpp / .cpp
│       │   ├── parse.hpp / .cpp
│       │   ├── sema.hpp / .cpp
│       │   ├── compile.hpp / .cpp (not implemented)
│       │   └── build.hpp / .cpp   (not implemented)
│       └── pipeline/
│           ├── Pipeline.hpp / .cpp
│           └── JSONDumper.hpp / .cpp
│
├── src/
│   └── main.cpp
│
└── tests/
    ├── core/
    ├── parser/
    ├── sema/
    ├── bytecode/
    ├── interp/
    └── vm/

tests/
```