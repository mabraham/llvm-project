//===--- Opt2pathOptionalCheck.cpp - clang-tidy ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Opt2pathOptionalCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"

#include <unordered_map>
#include <vector>

using namespace clang::ast_matchers;

namespace clang::tidy::modernize {

namespace {

auto isPointerToConstChar = pointerType(pointee(isAnyCharacter(), isConstQualified()));

} // namespace

void Opt2pathOptionalCheck::registerMatchers(MatchFinder *Finder) {
    Finder->addMatcher
        (binaryOperator
         (isAssignmentOperator(),
          hasOperands
          (declRefExpr(to(varDecl().bind("declaration"))).bind("variable name"),
           callExpr(hasDeclaration(functionDecl(anyOf(hasName("opt2fn_null"),
                                                      hasName("ftp2fn_null"))))).bind("call of opt2fn_null"))),
         this);
    // Note that this matcher must follow the previous one, which
    // matches the same fragment in a more specific way. It seems to be
    // OK that the binding uses the same string ID as the above match.
    Finder->addMatcher(callExpr
                       (forEachArgumentWithParam
                        (callExpr(hasDeclaration(functionDecl(anyOf(hasName("opt2fn_null"),
                                                                    hasName("ftp2fn_null"))))).bind("call of opt2fn_null"),
                         parmVarDecl().bind("function parameter bound to optional")),
                        hasDeclaration(functionDecl().bind("declaration of function receiving optional"))),
                       this);
    // Find uses like
    //
    //   bool var = (filename != nullptr) || (nullptr != otherFilename)
    //
    // to refactor them to use std::optional properly
    Finder->addMatcher(parenExpr
                       (has
                        (binaryOperator
                         (hasOperatorName("!="),
                          hasOperands
                          (ignoringImplicit(cxxNullPtrLiteralExpr()),
                           ignoringImplicit(declRefExpr(hasType(isPointerToConstChar),
                                                        to(varDecl().bind("declaration of possible optional path")))))))
                        ).bind("parenthesized expression to replace"),
                       this);

    // We'd like to match on the assertion directly, but it's a macro
    // and the AST only sees the post-expansion version. This matcher
    // is a crude model of an assertion, but it only has to work well
    // enough with the libc used by the clang-tidy version in use. We
    // are also not analyzing the sense in which the variable is used
    // in the assertion, assuming that the only relevant cases are
    // testing whether an optional path has a value.
    auto assertionExpr = parenExpr
        (hasDescendant(declRefExpr(to(functionDecl(hasName("__assert_fail"))))),
         hasDescendant(declRefExpr(to(varDecl().bind("declaration of variable referenced in assertion"))))
         ).bind("asssertion parenthesis expression");
    Finder->addMatcher
        (compoundStmt(unless(hasDescendant(compoundStmt())),
                      forEachDescendant(assertionExpr)
                      ).bind("compound statement enclosing assertion"),
         this);
    // Match function calls taking references to possible optional paths
    Finder->addMatcher
        (callExpr
         (forEachArgumentWithParam
          (declRefExpr(hasType(isPointerToConstChar),
                       to(varDecl().bind("declaration of possible optional path")),
                       optionally(hasAncestor(compoundStmt().bind("optional ancestor compound statement")))
                       ).bind("use of possible optional path in call expression"),
           parmVarDecl().bind("possible function parameter receiving optional path"))).bind("call expression using possible optional path"),
         this);
    // Match someFunction(1, nullptr, b) where the second parameter
    // has type const char *, so that later when we are sure whether
    // this should take an optional path, a path, or neither, we can
    // refactor to use std::nullopt or std::filesystem::path{} as
    // appropriate.
    Finder->addMatcher
        (callExpr
         (forEachArgumentWithParam
          (expr(ignoringImplicit(ignoringParens(cxxNullPtrLiteralExpr()))).bind("nullptr to potentially replace"),
           parmVarDecl(hasType(isPointerToConstChar)).bind("potential path parameter taking nullptr"))
          ),
         this);
}

// Describes an argument to fix for a function all
struct ArgExprToFix
{
        // The declaration of the function (not its call)
        const FunctionDecl* functionDecl_;
        // The expression that is the argument to fix
        const Expr* argExpr_;
        // The index of this argument within the set of parameters to functionDecl_
        const size_t parameterIndex_;
};

class Opt2pathOptionalCheck::IndexerVisitor
    : public RecursiveASTVisitor<IndexerVisitor>
{
    public:
        IndexerVisitor(ASTContext &Ctx) { TraverseAST(Ctx); }

        bool shouldTraversePostOrder() const { return true; }

        bool WalkUpFromCallExpr(CallExpr *call)
        {
            if (const auto *function =
                dyn_cast_or_null<FunctionDecl>(call->getCalleeDecl()))
            {
                //const std::string functionName = function->getNameInfo().getAsString();
                //        fprintf(stderr, "Found function named %s\n", functionName.c_str());
                index_[function].calls_.insert(call);
            }
            return true;
        }

        bool WalkUpFromDeclRefExpr(DeclRefExpr *declRefExpr)
        {
            const std::string variableName = declRefExpr->getNameInfo().getAsString();
            declRefExprs_[variableName].push_back(declRefExpr);
            return true;
        }

        std::vector<ArgExprToFix> argExprsToFix(const ast_matchers::MatchFinder::MatchResult &Result,
                                                const std::string& variableName,
                                                const FunctionDecl* enclosingFunctionDecl);

        void fixNullptrArguments(const ast_matchers::MatchFinder::MatchResult &Result,
                                 const FunctionDecl* enclosingFunctionDecl,
                                 const FunctionDecl* functionDeclUpdated,
                                 const std::vector<size_t>& argumentIndicesToUpdate,
                                 ClangTidyCheck *check);

        // TODO the names used as keys is risky if the same name is
        // used declarations in different scopes
        std::unordered_map<std::string, std::vector<const DeclRefExpr*>> declRefExprs_;
    private:
        struct IndexEntry {
            std::unordered_set<const CallExpr *> calls_;
        };

        std::unordered_map<const FunctionDecl *, IndexEntry> index_;
};

struct Opt2pathOptionalCheck::Assertion
{
        SourceLocation endOfAssertionParenExpr_;
        const VarDecl* declarationOfVariableReferencedInAssertion_;
};

struct Opt2pathOptionalCheck::PossibleUseOfOptionalPath
{
        const DeclRefExpr* declRefExpr_;
        const CompoundStmt* optionalCompoundStmt_;
        const ParmVarDecl* optionalParmVarDeclToChange_;
        const CallExpr* callExpr_;
};

Opt2pathOptionalCheck::~Opt2pathOptionalCheck() = default;

Opt2pathOptionalCheck::Opt2pathOptionalCheck(StringRef Name,
                                             ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context) {}

template <typename NodeT>
std::string prettyPrint(const NodeT* node)
{
    static clang::LangOptions langOpts;
    langOpts.CPlusPlus = true;
    static clang::PrintingPolicy policy(langOpts);

    std::string TypeS;
    llvm::raw_string_ostream s(TypeS);
    node->printPretty(s, 0, policy);
    return s.str();
}

template <typename NodeT>
const FunctionDecl* findEnclosingFuncDecl(const ast_matchers::MatchFinder::MatchResult &Result,
                                              const NodeT* node)
{
    // Get its parent nodes. The docs do not really explain why there can
    // be multiple parents.
    // TODO check node for nullptr?
    clang::DynTypedNodeList nodeList = Result.Context->getParents(*node);
    while (!nodeList.empty())
    {
        // Get the first parent.
        clang::DynTypedNode parentNode = nodeList[0];
        
        // You can dump the parent like this to inspect it.
        //parentNode.dump(llvm::outs(), *(Result.Context));
        
        // Is the parent a FunctionDecl?
        if (const FunctionDecl *parent = parentNode.get<FunctionDecl>()) {
            /*
              llvm::outs() << "Found ancestor FunctionDecl: "
              << (void const*)parent << '\n';
              llvm::outs() << "FunctionDecl name: "
              << parent->getNameAsString() << '\n';
            */
            return parent;
        }
        
        // It was not a FunctionDecl.  Keep going up.
        nodeList = Result.Context->getParents(parentNode);
    }
    return nullptr;
}

// TODO this does not only find grandparents, limit it?
template <typename NodeT>
const NodeT* findGrandparentExpr(const ast_matchers::MatchFinder::MatchResult &Result,
                                 const DeclRefExpr* declRefExpr)
{
    // Get its parent nodes. The docs do not really explain why there can
    // be multiple parents.
    // TODO check node for nullptr?
    clang::DynTypedNodeList nodeList = Result.Context->getParents(*declRefExpr);
    while (!nodeList.empty())
    {
        // Get the first parent.
        clang::DynTypedNode parentNode = nodeList[0];
        
        // You can dump the parent like this to inspect it.
        //parentNode.dump(llvm::outs(), *(Result.Context));
        
        // Is the parent a NodeT?
        if (const NodeT *parent = parentNode.get<NodeT>()) {
            /*
              llvm::outs() << "Found ancestor FunctionDecl: "
              << (void const*)parent << '\n';
              llvm::outs() << "FunctionDecl name: "
              << parent->getNameAsString() << '\n';
            */
            return parent;
        }
        
        // It was not a FunctionDecl.  Keep going up.
        nodeList = Result.Context->getParents(parentNode);
    }
    return nullptr;
}

template <typename NodeT>
const IfStmt* findEnclosingIfStatementWithinFunction(const ast_matchers::MatchFinder::MatchResult &Result,
                                                     const NodeT* node)
{
    // Get its parent nodes. The docs do not really explain why there can
    // be multiple parents.
    // TODO check node for nullptr?
    clang::DynTypedNodeList nodeList = Result.Context->getParents(*node);
    while (!nodeList.empty()) {
        // Get the first parent.
        clang::DynTypedNode parentNode = nodeList[0];
        
        // You can dump the parent like this to inspect it.
        //parentNode.dump(llvm::outs(), *(Result.Context));
        
        // Is the parent a FunctionDecl?
        if (const FunctionDecl *parent = parentNode.get<FunctionDecl>())
        {
            // We haven't found an enclosing if statement within the function within which node was found
            return nullptr;
        }
        // Is the parent a IfStmt?
        if (const IfStmt *parent = parentNode.get<IfStmt>())
        {
            return parent;
        }
        // It was not a relevant node. Keep going up.
        nodeList = Result.Context->getParents(parentNode);
    }
    return nullptr;
}

std::vector<ArgExprToFix>
Opt2pathOptionalCheck::IndexerVisitor::argExprsToFix(const ast_matchers::MatchFinder::MatchResult &Result,
                                                     const std::string& variableName,
                                                     const FunctionDecl* enclosingFunctionDecl)
{
    std::vector<ArgExprToFix> argExprsToFix;
    //fprintf(stderr, "starting to search for %s in %s\n", variableName.c_str(), enclosingFunctionDecl->getNameInfo().getAsString().c_str());
    for (const auto& [functionDecl, indexEntry] : index_)
    {
        for (const CallExpr* call : indexEntry.calls_)
        {
            // Does this call come within the function declared by enclosingFunctionDecl?
            if (findEnclosingFuncDecl(Result, call) != enclosingFunctionDecl)
            {
                continue;
            }
            //fprintf(stderr, "Found call to %s within scope of %s\n", prettyPrint(call).c_str(), enclosingFunctionDecl->getNameInfo().getAsString().c_str());
            const size_t numArgs = call->getNumArgs();
            for(size_t i=0; i < numArgs; i++)
            {
                const Expr* argExpr = call->getArg(i);
                if (prettyPrint(argExpr) == variableName)
                {
                    argExprsToFix.push_back({functionDecl, argExpr, i});
                }
            }
        }
    }
    return argExprsToFix;
}

void Opt2pathOptionalCheck::IndexerVisitor::fixNullptrArguments(const ast_matchers::MatchFinder::MatchResult &Result,
                                                                const FunctionDecl* enclosingFunctionDecl,
                                                                const FunctionDecl* functionDeclUpdated,
                                                                const std::vector<size_t>& argumentIndicesToUpdate,
                                                                ClangTidyCheck *check)
{
    //fprintf(stderr, "trying to fix nullptr args to function %s\n", functionDeclUpdated->getNameAsString().c_str());
    for (const auto& [functionDecl, indexEntry] : index_)
    {
        const std::string functionName = functionDecl->getNameAsString();
        if (functionDecl != functionDeclUpdated)
        {
            //fprintf(stderr, "Not considering calls to function %s\n", functionName.c_str());
            continue;
        }
        for (const CallExpr* call : indexEntry.calls_)
        {
            // Does this call come within the function declared by enclosingFunctionDecl?
            if (findEnclosingFuncDecl(Result, call) != enclosingFunctionDecl)
            {
                //fprintf(stderr, "Not considering call to function %s because not in scope of %s\n", functionName.c_str(), enclosingFunctionDecl->getNameAsString().c_str());
                continue;
            }
            //fprintf(stderr, "Considering call to function %s because in scope of %s\n", functionName.c_str(), enclosingFunctionDecl->getNameAsString().c_str());
            const size_t numArgs = call->getNumArgs();
            for (const size_t argIndex : argumentIndicesToUpdate)
            {
                //fprintf(stderr, "argIndex %zu numArgs %zu\n", argIndex, numArgs);
                if (argIndex < numArgs)
                {
                    const Expr* argExpr = call->getArg(argIndex);
                    //fprintf(stderr, "argIndex describes arg '%s'\n", prettyPrint(argExpr).c_str());
                    if (prettyPrint(argExpr) == "nullptr")
                    {
                        check->diag(argExpr->getBeginLoc(), "Use std::nullopt instead of nullptr")
                            << FixItHint::CreateReplacement(argExpr->getSourceRange(), "std::nullopt");
                    }
                }
            }
        }
    }
    //fprintf(stderr, "done trying to fix nullptr args\n");
}

bool optionalCheckedToHaveValue(const Expr* enclosingIfStatementCondition,
                                const std::string& variableName,
                                ClangTidyCheck* check)
{
    const std::string ifConditionString = prettyPrint(enclosingIfStatementCondition);

    //fprintf(stderr, "Checking if condition: %s\n", ifConditionString.c_str());
    if (ifConditionString == variableName)
    {
        return true;
    }
    if ((ifConditionString == variableName + " != nullptr") ||
        (ifConditionString == "nullptr != " + variableName))
    {
        check->diag(enclosingIfStatementCondition->getBeginLoc(), "Use std::optional::operator bool() rather than comparison with nullptr")
            << FixItHint::CreateReplacement(enclosingIfStatementCondition->getSourceRange(), variableName);
        return true;
    }
    // This is very much a hack, but it probably covers all the relevant cases
    if ((ifConditionString.find(variableName + " &&") != std::string::npos) ||
        (ifConditionString.find("&& " + variableName) != std::string::npos))
    {
        return true;
    }
    return false;
}

bool enclosingIfStatementEnsuresOptionalHasValue(const ast_matchers::MatchFinder::MatchResult &Result,
                                                 const Expr* argExpr,
                                                 const std::string& variableName,
                                                 ClangTidyCheck* check)
{
    const IfStmt* enclosingIfStatement = findEnclosingIfStatementWithinFunction<Expr>(Result, argExpr);
    while (enclosingIfStatement)
    {
        if (optionalCheckedToHaveValue(enclosingIfStatement->getCond(), variableName, check))
        {
            return true;
        }
        enclosingIfStatement = findEnclosingIfStatementWithinFunction<IfStmt>(Result, enclosingIfStatement);
    }
    return false;
}

void Opt2pathOptionalCheck::updateFunctionDeclaration(const ast_matchers::MatchFinder::MatchResult &Result,
                                                      const FunctionDecl* functionDeclToChange,
                                                      const ParmVarDecl* parmVarDeclToChange,
                                                      const bool toOptionalPath)
{
    const std::string parameterName = parmVarDeclToChange->getNameAsString();
    const std::string functionName = functionDeclToChange->getNameAsString();
    //fprintf(stderr, "Changing type of parameter named %s of function %s, changing %sto optional\n", parameterName.c_str(), functionName.c_str(), toOptionalPath ? "" : "not ");
    const std::string replacementParameterType = toOptionalPath ? "const std::optional<std::filesystem::path>&" : "const std::filesystem::path&";
    diag(parmVarDeclToChange->getBeginLoc(), "Change function parameter to " + replacementParameterType)
        << FixItHint::CreateReplacement(parmVarDeclToChange->getSourceRange(), replacementParameterType + " " + parameterName);
    // Recurse on updating the definition of that function only if its definition is visible
    if (functionDeclToChange->isDefined())
    {
        //fprintf(stderr, "Recursing on parameter named %s in definition of function %s, changing %sto optional\n", parameterName.c_str(), functionName.c_str(), toOptionalPath ? "" : "not ");
        updateVariableWithinFunction(Result, parameterName, functionDeclToChange, toOptionalPath);
    }
    //fprintf(stderr, "Done updating function declaration\n");
}

void Opt2pathOptionalCheck::updateVariableWithinFunction(const ast_matchers::MatchFinder::MatchResult &Result,
                                                         const std::string& variableName,
                                                         const FunctionDecl* enclosingFunctionDecl,
                                                         const bool toOptionalPath)
{
    // Find all places where variableName is used as an argument to a
    // function and update the function call according to
    // toOptionalPath. fprintf/printf/gmx_fatal are handled as special
    // cases where we need to extract the C-string properly. Then
    // proceed to modify the function declaration consistently.

    std::unordered_map<const FunctionDecl*, std::vector<size_t>> functionCallsTakingOptionalToUpdate;
    //fprintf(stderr, "Updating variable named %s when used within function %s, changing %sto optional\n", variableName.c_str(), enclosingFunctionDecl->getNameAsString().c_str(), toOptionalPath ? "" : "not ");
    const std::vector<ArgExprToFix> argExprsToFix = indexer_->argExprsToFix(Result, variableName, enclosingFunctionDecl);
    for (const auto& argExprToFix : argExprsToFix)
    {
        // TODO can this comparison be done directly on the functionDecl?
        const std::string functionName = argExprToFix.functionDecl_->getNameInfo().getAsString();
        //fprintf(stderr, "Handling ArgExpr '%s' in call to function %s\n", prettyPrint(argExprToFix.argExpr_).c_str(), functionName.c_str());
        if (functionName == "fprintf" || functionName == "printf" || functionName == "gmx_fatal")
        {
            if (toOptionalPath)
            {
                diag(argExprToFix.argExpr_->getEndLoc(), "Get C string from std::optional<std::filesystem::path>")
                    << FixItHint::CreateReplacement(argExprToFix.argExpr_->getSourceRange(), variableName + ".value().c_str()");
            }
            else
            {
                diag(argExprToFix.argExpr_->getEndLoc(), "Get C string from std::filesystem::path")
                    << FixItHint::CreateReplacement(argExprToFix.argExpr_->getSourceRange(), variableName + ".c_str()");
            }
        }
        else
        {
            // Does the enclosing function contain logic to check that
            // the std::optional has a value? If so, the callee should
            // receive a std::filesystem::path via a call to
            // .value(). Otherwise, leave it alone.
            if (toOptionalPath && enclosingIfStatementEnsuresOptionalHasValue(Result, argExprToFix.argExpr_, variableName, this))
            {
                diag(argExprToFix.argExpr_->getBeginLoc(), "Extract std::filesystem::path from std::optional<std::filesystem::path>")
                    << FixItHint::CreateReplacement(argExprToFix.argExpr_->getSourceRange(), variableName + ".value()");
                updateFunctionDeclaration(Result,
                                          argExprToFix.functionDecl_,
                                          argExprToFix.functionDecl_->getParamDecl(argExprToFix.parameterIndex_),
                                          false);
            }
            else
            {
                if (toOptionalPath)
                {
                    //fprintf(stderr, "Preparing nullptr check for function %s parameter index %zu in optional case\n", argExprToFix.functionDecl_->getNameAsString().c_str(), argExprToFix.parameterIndex_);
                    functionCallsTakingOptionalToUpdate[argExprToFix.functionDecl_].push_back(argExprToFix.parameterIndex_);
                }
                //fprintf(stderr, "Updating declaration of function %s in non-optional case\n", argExprToFix.functionDecl_->getNameAsString().c_str());
                updateFunctionDeclaration(Result,
                                          argExprToFix.functionDecl_,
                                          argExprToFix.functionDecl_->getParamDecl(argExprToFix.parameterIndex_),
                                          toOptionalPath);
            }
            // TODO handle case where an assertion ensures the optional is valid
        }
    }

    // Find all calls to this function in this scope and update any
    // matching nullptr arguments to std::nullopt.
    for (const auto& [functionDecl, argumentIndicesToUpdate] : functionCallsTakingOptionalToUpdate)
    {
        indexer_->fixNullptrArguments(Result, enclosingFunctionDecl, functionDecl, argumentIndicesToUpdate, this);
    }
        
    // Find all constructor calls taking variableName and refactor them as well
    for (const DeclRefExpr* declRefExpr : indexer_->declRefExprs_[variableName])
    {
        if (const auto* cxxConstructExpr = findGrandparentExpr<CXXConstructExpr>(Result, declRefExpr))
        {
            if (findEnclosingFuncDecl(Result, cxxConstructExpr) == enclosingFunctionDecl)
            {
                //fprintf(stderr, "Found reference to %s passed to constructor call in %s\n", variableName.c_str(), enclosingFunctionDecl->getNameAsString().c_str());
                if (const IfStmt* enclosingIfStatement =
                    findEnclosingIfStatementWithinFunction(Result, cxxConstructExpr);
                    toOptionalPath && enclosingIfStatement &&
                    optionalCheckedToHaveValue(enclosingIfStatement->getCond(), variableName, this))
                {
                    diag(declRefExpr->getBeginLoc(), "Extract std::filesystem::path from std::optional<std::filesystem::path>")
                        << FixItHint::CreateReplacement(declRefExpr->getSourceRange(), variableName + ".value()");
                }
                // TODO does it make sense to try to update C++ constructor declarations like we do function declarations?
            }
        }
    }
    
    //fprintf(stderr, "Done updating variable %s\n", variableName.c_str());
}

bool Opt2pathOptionalCheck::optionalPathUsedAsValue(const DeclRefExpr *declRefExpr, const VarDecl* varDecl, const CompoundStmt* optionalCompoundStmt, ASTContext *context)
{
    if (!optionalCompoundStmt)
    {
        return false;
    }
    //fprintf(stderr, "Matched variable named '%s' used compound statement '%s'\n",
    //        varDecl->getNameAsString().c_str(),
    //        prettyPrint(compoundStmt).c_str());
    // Does the compound statement include an assertion?
    if (const auto assertionIt = assertionsByEnclosingCompoundStmt_.find(optionalCompoundStmt);
        assertionIt != assertionsByEnclosingCompoundStmt_.end())
    {
        //fprintf(stderr, "Compound statement includes an assertion\n");
        for(const auto& assertion : assertionIt->second)
        {
            // Does the assertion refer to the same variable we matched?
            if (assertion.declarationOfVariableReferencedInAssertion_ == varDecl)
            {
                //fprintf(stderr, "Matched assertion within compound statement '%s' on variable named '%s'\n",
                //        prettyPrint(optionalCompoundStmt).c_str(),
                //        varDecl->getNameAsString().c_str());
                // Does the assertion precede the variable reference within the context of the match?
                BeforeThanCompare<SourceLocation> isBefore(context->getSourceManager());
                if (isBefore(assertion.endOfAssertionParenExpr_, declRefExpr->getBeginLoc()))
                {
                    //fprintf(stderr, "Assertion precedes use\n");
                    // Assume the assertion makes it safe to extract the value from the optional path.
                    return true;
                }
            }
        }
    }
    return false;
}

bool isPrintfStyleFunctionCallExpr(const CallExpr* callExpr)
{
    if (const auto* functionDecl = callExpr->getDirectCallee())
    {
        const std::string functionName = functionDecl->getNameAsString();
        return (functionName == "fprintf" ||
                functionName == "printf" ||
                functionName == "gmx_fatal");
    }
    return false;
}

void Opt2pathOptionalCheck::refactorUseOfPathInFunctionCall(const DeclRefExpr *declRefExpr,
                                                            const VarDecl* varDecl,
                                                            const bool convertToPath,
                                                            const CompoundStmt* optionalCompoundStmt,
                                                            const ParmVarDecl* parmVarDeclToChange,
                                                            const bool printfStyleFunctionCallExpr,
                                                            const CallExpr* callExpr,
                                                            ASTContext *context)
{
    if (printfStyleFunctionCallExpr)
    {
        fprintf(stderr, "Converting DeclRefExpr usage in printf-style function to %s\n", convertToPath ? "path" : "optional path");
        // special handling for fprintf and similar
        if (!convertToPath)
        {
            diag(declRefExpr->getEndLoc(), "Get C string from std::optional<std::filesystem::path>")
                << FixItHint::CreateReplacement(declRefExpr->getSourceRange(), declRefExpr->getNameInfo().getAsString() + ".value().c_str()");
        }
        else
        {
            diag(declRefExpr->getEndLoc(), "Get C string from std::filesystem::path")
                << FixItHint::CreateReplacement(declRefExpr->getSourceRange(), declRefExpr->getNameInfo().getAsString() + ".c_str()");
        }
    }
    else
    {
        fprintf(stderr, "Converting DeclRefExpr usage in regular function to %s\n", convertToPath ? "path" : "optional path");
        const bool extractFromOptional = optionalPathUsedAsValue(declRefExpr, varDecl, optionalCompoundStmt, context);
        if (extractFromOptional)
        {
            fprintf(stderr, "Extracting .value()\n");
            diag(declRefExpr->getBeginLoc(), "Extract std::filesystem::path from std::optional<std::filesystem::path>")
                << FixItHint::CreateReplacement(declRefExpr->getSourceRange(), varDecl->getNameAsString() + ".value()");
        }
        if (parmVarDeclToChange)
        {
            fprintf(stderr, "Found function parameter '%s' to change to %s\n", parmVarDeclToChange->getNameAsString().c_str(), (convertToPath || extractFromOptional) ? "path" : "optional");
            if (convertToPath || extractFromOptional)
            {
                const std::string replacementParameterType = "const std::filesystem::path&";
                diag(parmVarDeclToChange->getBeginLoc(), "Change function parameter to " + replacementParameterType)
                    << FixItHint::CreateReplacement(parmVarDeclToChange->getSourceRange(), replacementParameterType + " " + parmVarDeclToChange->getNameAsString());
                parametersConvertedToPath_.push_back(parmVarDeclToChange);
            }
            else
            {
                const std::string replacementParameterType = "const std::optional<std::filesystem::path>&";
                diag(parmVarDeclToChange->getBeginLoc(), "Change function parameter to " + replacementParameterType)
                    << FixItHint::CreateReplacement(parmVarDeclToChange->getSourceRange(), replacementParameterType + " " + parmVarDeclToChange->getNameAsString());
                parametersConvertedToOptionalPath_.push_back(parmVarDeclToChange);
            }

        // Now we now that that parameter is an (optional) path so we should check uses of that parameter and perhaps refactor
            for (const PossibleUseOfOptionalPath& useOfOptionalPath : possibleUsesOfOptionalPath_[parmVarDeclToChange])
            {
                fprintf(stderr, "Reconsidering match on variable '%s' %sin compound statement %sassociated with parameter declaration, associated with %s-style function in call expression '%s'\n",
                        prettyPrint(useOfOptionalPath.declRefExpr_).c_str(),
                        useOfOptionalPath.optionalCompoundStmt_ ? "" : "not",
                        useOfOptionalPath.optionalParmVarDeclToChange_ ? "" : "not",
                        printfStyleFunctionCallExpr ? "printf" : "regular",
                        prettyPrint(useOfOptionalPath.callExpr_).c_str());
                refactorUseOfPathInFunctionCall(useOfOptionalPath.declRefExpr_,
                                                parmVarDeclToChange,
                                                convertToPath || extractFromOptional,
                                                useOfOptionalPath.optionalCompoundStmt_,
                                                useOfOptionalPath.optionalParmVarDeclToChange_,
                                                isPrintfStyleFunctionCallExpr(useOfOptionalPath.callExpr_),
                                                useOfOptionalPath.callExpr_,
                                                context);
            }
        }
    }
}

void Opt2pathOptionalCheck::check(const MatchFinder::MatchResult &Result)
{
    // Note that the Result objects seem to appear in order of
    // traversal of the AST, and not in order of the calls to
    // finder->addMatcher().
    if (!indexer_)
    {
        indexer_ = std::make_unique<IndexerVisitor>(*Result.Context);
    }
  if (const auto *match = Result.Nodes.getNodeAs<VarDecl>("declaration"))
  {
      // It's probably better to leave the lines containing only a
      // semicolon so that they can be grepped away later.
      /*
      ASTContext *context = Result.Context;
      SourceLocation semicolon = Lexer::getLocForEndOfToken
          (match->getLocation(), 0, context->getSourceManager(),
           context->getLangOpts());
      */
      diag(match->getBeginLoc(), "Don't declare const char* variable that won't be used")
          << FixItHint::CreateRemoval(SourceRange(match->getBeginLoc(), match->getEndLoc()));
      //          << FixItHint::CreateRemoval(semicolon);
      varDeclOfOptionalFilenames_.insert(match);
     }
  if (const auto *match = Result.Nodes.getNodeAs<CallExpr>("call of opt2fn_null"))
  {
      {
          const Expr* callee = match->getCallee();
          std::string functionName = prettyPrint(callee);
          std::string replacementName = (functionName == "opt2fn_null") ? "opt2path_optional" : "ftp2path_optional";
          diag(callee->getBeginLoc(), "Use " + replacementName + " instead of " + functionName)
              << FixItHint::CreateReplacement(SourceRange(callee->getBeginLoc(), callee->getEndLoc()), replacementName);
      }
      {
          const auto *match = Result.Nodes.getNodeAs<DeclRefExpr>("variable name");
          diag(match->getLocation(), "Use std::optional<std::filesystem::path>")
              << FixItHint::CreateInsertion(match->getLocation(), "std::optional<std::filesystem::path> ");
      }
  }
  /*
  if (const auto *match = Result.Nodes.getNodeAs<DeclRefExpr>("variable name"))
  {
      diag(match->getLocation(), "Use std::optional<std::filesystem::path>")
      << FixItHint::CreateInsertion(match->getLocation(), "std::optional<std::filesystem::path> ");

      const std::string variableName = match->getNameInfo().getAsString();
      // TODO this is too general. It does not handle the case where
      // the same variable name is declared multiple times in the same
      // function, potentially with different types.
      const FunctionDecl* enclosingFunctionDecl = findEnclosingFuncDecl(Result, match);
      const bool toOptionalPath = true;
      updateVariableWithinFunction(Result, variableName, enclosingFunctionDecl, toOptionalPath);
  }
  */
  /*
  if (const auto *match = Result.Nodes.getNodeAs<FunctionDecl>("declaration of function receiving optional"))
  {
      const bool toOptionalPath = true;
      const auto *parmVarDecl = Result.Nodes.getNodeAs<ParmVarDecl>("function parameter bound to optional");
      assert(parmVarDecl && "Must have matching parameter declaration");
      updateFunctionDeclaration(Result, match, parmVarDecl, toOptionalPath);
  }
  */
  // TODO could this logic be re-used with if statement condition expressions?
  if (const auto *match = Result.Nodes.getNodeAs<ParenExpr>("parenthesized expression to replace"))
  {
      const auto *varDecl = Result.Nodes.getNodeAs<VarDecl>("declaration of possible optional path");
      if (varDeclOfOptionalFilenames_.find(varDecl) != varDeclOfOptionalFilenames_.end())
      {
          diag(match->getBeginLoc(), "Use std::optional::operator bool() rather than comparison with nullptr")
              << FixItHint::CreateReplacement(match->getSourceRange(), varDecl->getNameAsString());
      }
  }
    if (const auto *matchingCompoundStmt = Result.Nodes.getNodeAs<CompoundStmt>("compound statement enclosing assertion"))
    {
        const auto *varDecl = Result.Nodes.getNodeAs<VarDecl>("declaration of variable referenced in assertion");
        const auto *assertionParenExpr = Result.Nodes.getNodeAs<ParenExpr>("asssertion parenthesis expression");
        //fprintf(stderr, "adding assertion '%s' referring to %s in compound statement '%s'\n",
        //        prettyPrint(assertionParenExpr).c_str(), varDecl->getNameAsString().c_str(), prettyPrint(matchingCompoundStmt).c_str());
        assertionsByEnclosingCompoundStmt_[matchingCompoundStmt].push_back
            ({assertionParenExpr->getEndLoc(), varDecl});
    }
    if (const auto *matchingDeclRefExpr = Result.Nodes.getNodeAs<DeclRefExpr>("use of possible optional path in call expression"))
    {
        const auto *varDecl = Result.Nodes.getNodeAs<VarDecl>("declaration of possible optional path");
        const auto *optionalCompoundStmt = Result.Nodes.getNodeAs<CompoundStmt>("optional ancestor compound statement");
        const auto *optionalParmVarDeclToChange = Result.Nodes.getNodeAs<ParmVarDecl>("possible function parameter receiving optional path");
        const auto *callExpr = Result.Nodes.getNodeAs<CallExpr>("call expression using possible optional path");
        const bool isPrintfStyle = isPrintfStyleFunctionCallExpr(callExpr);
        fprintf(stderr, "Got match on variable '%s' %sin compound statement %sassociated with parameter declaration, associated with %s-style function in call expression '%s'\n",
                prettyPrint(matchingDeclRefExpr).c_str(),
                optionalCompoundStmt ? "" : "not",
                optionalParmVarDeclToChange ? "" : "not",
                isPrintfStyle ? "printf" : "regular",
                prettyPrint(callExpr).c_str());
        if (varDeclOfOptionalFilenames_.find(varDecl) != varDeclOfOptionalFilenames_.end())
        {
            fprintf(stderr, "Found DeclRefExpr to known optional filename, refactoring\n");
            if (optionalCompoundStmt)
            {
                fprintf(stderr, "Found optional compound statement\n");
            }
            // TODO consider passing Result to optionalPathUsedAsValue
            refactorUseOfPathInFunctionCall(matchingDeclRefExpr, varDecl,
                                            optionalPathUsedAsValue(matchingDeclRefExpr, varDecl, optionalCompoundStmt, Result.Context),
                                            optionalCompoundStmt,
                                            optionalParmVarDeclToChange, isPrintfStyle, callExpr, Result.Context);
            fprintf(stderr, "Done with DeclRefExpr\n");
        }
        else
        {
            fprintf(stderr, "Found DeclRefExpr to something not known to be an optional filename, storing\n");
            possibleUsesOfOptionalPath_[varDecl].push_back({matchingDeclRefExpr, optionalCompoundStmt, optionalParmVarDeclToChange, callExpr});
        }
    }
    if (const auto *matchingExpr = Result.Nodes.getNodeAs<Expr>("nullptr to potentially replace"))
    {
        const auto *parmVarDecl = Result.Nodes.getNodeAs<ParmVarDecl>("potential path parameter taking nullptr");
        paramDeclsWithTypeConstCharPointersReceivingNullptr_[parmVarDecl].push_back(matchingExpr);
    }
}

void Opt2pathOptionalCheck::onEndOfTranslationUnit()
{
    // Now that we know we have seen all the function parameters that
    // might have changed type from const char* to either
    // optional<path> or path, we can go back and convert callers that
    // were passing nullptr constants to instead use a more
    // appropriate expression.
    const auto theEnd = paramDeclsWithTypeConstCharPointersReceivingNullptr_.end();
    for (const ParmVarDecl* parmVarDecl : parametersConvertedToOptionalPath_)
    {
        if (paramDeclsWithTypeConstCharPointersReceivingNullptr_.find(parmVarDecl) != theEnd)
        {
            for (const Expr* nullptrExpression : paramDeclsWithTypeConstCharPointersReceivingNullptr_[parmVarDecl])
            {
                diag(nullptrExpression->getBeginLoc(), "Use std::nullopt instead of nullptr")
                    << FixItHint::CreateReplacement(nullptrExpression->getSourceRange(), "std::nullopt");
            }
        }
    }
    for (const ParmVarDecl* parmVarDecl : parametersConvertedToPath_)
    {
        if (paramDeclsWithTypeConstCharPointersReceivingNullptr_.find(parmVarDecl) != theEnd)
        {
            for (const Expr* nullptrExpression : paramDeclsWithTypeConstCharPointersReceivingNullptr_[parmVarDecl])
            {
                diag(nullptrExpression->getBeginLoc(), "Use std::filesystem::path{} instead of nullptr")
                    << FixItHint::CreateReplacement(nullptrExpression->getSourceRange(), "std::filesystem::path{}");
            }
        }
    }
}

} // namespace clang::tidy::modernize
