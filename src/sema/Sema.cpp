#include "Sema.hpp"
#include "SemaContext.hpp"
#include "NameResolver.hpp"
#include "TypeChecker.hpp"
#include "FFIValidator.hpp"

bool Sema::analyze(std::vector<ModuleAST*>& modules) {
    // for (auto* mod : modules) {
    //     if (!processModule(mod)) {
    //         return false;
    //     }
    // }
    // return !diag.hasErrors();
}

bool Sema::processModule(ModuleAST* mod) {
    // SemaContext ctx(mod, diag);
    
    // // Name resolution
    // if (!NameResolver::resolve(ctx)) {
    //     return false;
    // }
    
    // // Type checking
    // if (!TypeChecker::check(ctx)) {
    //     return false;
    // }
    
    // // FFI validation
    // if (!FFIValidator::validate(ctx)) {
    //     return false;
    // }
    
    // return true;
}