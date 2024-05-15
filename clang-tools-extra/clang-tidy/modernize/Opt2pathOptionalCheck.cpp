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
         callExpr(hasDeclaration(functionDecl(hasName("opt2fn_null")))).bind("func call"))).bind("assignment"),
       this);
}

class Opt2pathOptionalCheck::IndexerVisitor
    : public RecursiveASTVisitor<IndexerVisitor> {
public:
  IndexerVisitor(ASTContext &Ctx) { TraverseAST(Ctx); }

  const std::unordered_set<const CallExpr *> &
  getFnCalls(const FunctionDecl *Fn) {
    return index_[Fn->getCanonicalDecl()].calls_;
  }

  const std::unordered_set<const DeclRefExpr *> &
  getOtherRefs(const FunctionDecl *Fn) {
    return index_[Fn->getCanonicalDecl()].otherRefs_;
  }

  bool shouldTraversePostOrder() const { return true; }

        /*
  bool WalkUpFromDeclRefExpr(DeclRefExpr *DeclRef) {
    if (const auto *Fn = dyn_cast<FunctionDecl>(DeclRef->getDecl())) {
      Fn = Fn->getCanonicalDecl();
      Index[Fn].OtherRefs.insert(DeclRef);
    }
    return true;
  }
        */
  bool WalkUpFromCallExpr(CallExpr *Call)
        {
    if (const auto *function =
            dyn_cast_or_null<FunctionDecl>(Call->getCalleeDecl()))
    {
        //const std::string functionName = function->getNameInfo().getAsString();
        //        fprintf(stderr, "Found function named %s\n", functionName.c_str());
        index_[function].calls_.insert(Call);
    }
    return true;
  }

        std::vector<const Expr*> fixCalls(const ast_matchers::MatchFinder::MatchResult &Result,
                                          const std::string& functionNameToFix,
                                          const std::string& Function);

    private:
  struct IndexEntry {
    std::unordered_set<const CallExpr *> calls_;
    std::unordered_set<const DeclRefExpr *> otherRefs_;
  };

  std::unordered_map<const FunctionDecl *, IndexEntry> index_;
};

Opt2pathOptionalCheck::~Opt2pathOptionalCheck() = default;

Opt2pathOptionalCheck::Opt2pathOptionalCheck(StringRef Name,
                                             ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context) {}

std::vector<const Expr*> Opt2pathOptionalCheck::IndexerVisitor::fixCalls(const ast_matchers::MatchFinder::MatchResult &Result,
                                                                         const std::string& functionNameToFix,
                                                                         const std::string& variableName)
{
    std::vector<const Expr*> exprsToFix;
    clang::LangOptions langOpts;
    langOpts.CPlusPlus = true;
    clang::PrintingPolicy policy(langOpts);
    for (const auto& [functionDecl, indexEntry] : index_)
    {
        // TODO can this comparison be done directly on the functionDecl?
        const std::string functionName = functionDecl->getNameInfo().getAsString();
        if (functionName != functionNameToFix)
        {
            continue;
        }
        for (const CallExpr* call : indexEntry.calls_)
        {
            const int numArgs = call->getNumArgs();
            for(int i=0; i < numArgs; i++)
            {
                std::string TypeS;
                llvm::raw_string_ostream s(TypeS);
                const Expr* argExpr = call->getArg(i);
                argExpr->printPretty(s, 0, policy);
                const std::string argumentString = s.str();
                //                fprintf(stderr, "arg: %s\n", argumentString.c_str());
                if (argumentString == variableName)
                {
                    exprsToFix.push_back(argExpr);
                }
            }
        }
    }
    return exprsToFix;
}

void Opt2pathOptionalCheck::check(const MatchFinder::MatchResult &Result)
{
  // Any fast exits go here
  /*
  if (!MatchedAssignment->getIdentifier() || MatchedAssignment->getName().startswith("awesome_"))
    return;
  */
  if (const auto *match = Result.Nodes.getNodeAs<VarDecl>("declaration"))
  {
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
  if (const auto *match = Result.Nodes.getNodeAs<CallExpr>("func call")->getCallee())
  {
      diag(match->getBeginLoc(), "Use opt2path_optional instead of opt2fn_null")
          << FixItHint::CreateReplacement(SourceRange(match->getBeginLoc(), match->getEndLoc()), "opt2path_optional");
  }
  if (const auto *match = Result.Nodes.getNodeAs<DeclRefExpr>("variable name"))
  {
      diag(match->getLocation(), "Use std::optional<std::filesystem::path>")
      << FixItHint::CreateInsertion(match->getLocation(), "std::optional<std::filesystem::path> ");

      if (!indexer_)
      {
          indexer_ = std::make_unique<IndexerVisitor>(*Result.Context);
      }
      const std::string variableName = match->getNameInfo().getAsString();
      const auto exprsToFix = indexer_->fixCalls(Result, "fprintf", variableName);
      for (const auto& exprToFix : exprsToFix)
      {
          diag(exprToFix->getEndLoc(), "Get C string from std::optional<std::filesystem::path>")
              << FixItHint::CreateReplacement(exprToFix->getSourceRange(), variableName + ".value().string().c_str()");
      }
  }
}

} // namespace clang::tidy::modernize
