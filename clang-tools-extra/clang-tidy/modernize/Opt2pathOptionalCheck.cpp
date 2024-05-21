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
#include <unordered_set>
#include <vector>

using namespace clang::ast_matchers;

namespace clang::tidy::modernize {

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

        std::unordered_map<std::string, std::vector<const DeclRefExpr*>> declRefExprs_;
    private:
        struct IndexEntry {
            std::unordered_set<const CallExpr *> calls_;
        };

        std::unordered_map<const FunctionDecl *, IndexEntry> index_;
};

Opt2pathOptionalCheck::~Opt2pathOptionalCheck() = default;

Opt2pathOptionalCheck::Opt2pathOptionalCheck(StringRef Name,
                                             ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context) {}

std::string prettyPrintExpr(const Expr* expr)
{
    static clang::LangOptions langOpts;
    langOpts.CPlusPlus = true;
    static clang::PrintingPolicy policy(langOpts);

    std::string TypeS;
    llvm::raw_string_ostream s(TypeS);
    expr->printPretty(s, 0, policy);
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
            //fprintf(stderr, "Found call to %s within scope of %s\n", prettyPrintExpr(call).c_str(), enclosingFunctionDecl->getNameInfo().getAsString().c_str());
            const size_t numArgs = call->getNumArgs();
            for(size_t i=0; i < numArgs; i++)
            {
                const Expr* argExpr = call->getArg(i);
                if (prettyPrintExpr(argExpr) == variableName)
                {
                    argExprsToFix.push_back({functionDecl, argExpr, i});
                }
            }
        }
    }
    return argExprsToFix;
}

bool optionalCheckedToHaveValue(const Expr* enclosingIfStatementCondition,
                                const std::string& variableName,
                                ClangTidyCheck* check)
{
    const std::string ifConditionString = prettyPrintExpr(enclosingIfStatementCondition);

    //fprintf(stderr, "Checking if condition: %s\n", ifConditionString.c_str());
    if (ifConditionString == variableName)
    {
        return true;
    }
    if (ifConditionString == variableName + " != nullptr")
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
    // toOptionalPath. fprintf/printf is handled as a special case
    // where we need to extract the C-string properly. Then proceed to
    // modify the function declaration consistently.
    
    //fprintf(stderr, "Updating variable named %s when used within function %s, changing %sto optional\n", variableName.c_str(), enclosingFunctionDecl->getNameAsString().c_str(), toOptionalPath ? "" : "not ");
    const std::vector<ArgExprToFix> argExprsToFix = indexer_->argExprsToFix(Result, variableName, enclosingFunctionDecl);
    for (const auto& argExprToFix : argExprsToFix)
    {
        // TODO can this comparison be done directly on the functionDecl?
        const std::string functionName = argExprToFix.functionDecl_->getNameInfo().getAsString();
        //fprintf(stderr, "Handling ArgExpr '%s' in call to function %s\n", prettyPrintExpr(argExprToFix.argExpr_).c_str(), functionName.c_str());
        if (functionName == "fprintf" || functionName == "printf")
        {
            // TODO cater for toOptionalPath == false
            diag(argExprToFix.argExpr_->getEndLoc(), "Get C string from std::optional<std::filesystem::path>")
                << FixItHint::CreateReplacement(argExprToFix.argExpr_->getSourceRange(), variableName + ".value().string().c_str()");
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
                //fprintf(stderr, "Updating declaration of function %s in optional case\n", argExprToFix.functionDecl_->getNameAsString().c_str());
                updateFunctionDeclaration(Result,
                                          argExprToFix.functionDecl_,
                                          argExprToFix.functionDecl_->getParamDecl(argExprToFix.parameterIndex_),
                                          toOptionalPath);
            }
        }
    }

    // TODO write logic to find all constructor calls taking variableName and refactor them as well
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

void Opt2pathOptionalCheck::check(const MatchFinder::MatchResult &Result)
{
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
  }
  if (const auto *match = Result.Nodes.getNodeAs<CallExpr>("call of opt2fn_null")->getCallee())
  {
      std::string functionName = prettyPrintExpr(match);
      std::string replacementName = (functionName == "opt2fn_null") ? "opt2path_optional" : "ftp2path_optional";
      diag(match->getBeginLoc(), "Use " + replacementName + " instead of " + functionName)
          << FixItHint::CreateReplacement(SourceRange(match->getBeginLoc(), match->getEndLoc()), replacementName);
  }
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
  if (const auto *match = Result.Nodes.getNodeAs<FunctionDecl>("declaration of function receiving optional"))
  {
      const bool toOptionalPath = true;
      const auto *parmVarDecl = Result.Nodes.getNodeAs<ParmVarDecl>("function parameter bound to optional");
      assert(parmVarDecl && "Must have matching parameter declaration");
      updateFunctionDeclaration(Result, match, parmVarDecl, toOptionalPath);
  }
}

} // namespace clang::tidy::modernize
