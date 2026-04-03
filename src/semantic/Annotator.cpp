/**
 * @file Annotator.cpp
 *
 * @nutshell Attaches final semantic properties to raw AST nodes.
 *
 * @reason Once the compiler has successfully completed all types and symbol resolutions, the AST itself needs to carry these contextual stamps to safely interface with the machine code generator (Codegen).
 *
 * @responsibility Phase 4 of semantic analysis: binds resolved state context back to the abstract syntax tree.
 *
 * @logic Unlocks constraints generated during semantic checks onto BaseAST elements for generator consumption (resolvedType, isConst, etc.).
 *
 * @related SemanticExpr.cpp, SemanticStmt.cpp, SemanticAnalyzer.cpp
 */
