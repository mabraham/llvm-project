//===--- Opt2pathOptionalCheck.cpp - clang-tidy ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Opt2pathOptionalCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

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

void Opt2pathOptionalCheck::check(const MatchFinder::MatchResult &Result) {
  fprintf(stderr, "Got a match\n");
  // Any fast exits go here
  /*
  if (!MatchedAssignment->getIdentifier() || MatchedAssignment->getName().startswith("awesome_"))
    return;
  */
  {
      const auto *match = Result.Nodes.getNodeAs<CallExpr>("func call")->getCallee();
      diag(match->getBeginLoc(), "Use opt2path_optional instead of opt2fn_null")
          << FixItHint::CreateReplacement(SourceRange(match->getBeginLoc(), match->getEndLoc()), "opt2path_optional");
  }
  {
      const auto *match = Result.Nodes.getNodeAs<DeclRefExpr>("variable name");
      diag(match->getLocation(), "Use std::optional<std::filesystem::path>")
      << FixItHint::CreateInsertion(match->getLocation(), "std::optional<std::filesystem::path> ");
  }
}

} // namespace clang::tidy::modernize
