//===--- CStringToStdFilenameCheck.cpp - clang-tidy -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CStringToStdFilenameCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Tooling/Transformer/Stencil.h"

using namespace clang::ast_matchers;

namespace clang::tidy::modernize {

using ::clang::ast_matchers::forEachArgumentWithParam;
using ::clang::ast_matchers::hasName;
using ::clang::ast_matchers::hasType;
using ::clang::ast_matchers::matchesName;
using ::clang::ast_matchers::parmVarDecl;
using ::clang::transformer::addInclude;
using ::clang::transformer::applyFirst;
using ::clang::transformer::callArgs;
using ::clang::transformer::cat;
using ::clang::transformer::changeTo;
using ::clang::transformer::describe;
using ::clang::transformer::makeRule;
using ::clang::transformer::name;
using ::clang::transformer::node;
using ::clang::transformer::noopEdit;
using ::clang::transformer::remove;
using ::clang::transformer::RewriteRuleWith;

AST_MATCHER(Type, isCharType) { return Node.isCharType(); }

std::string variableNameEndingWithFile = "endsWithFile";
//llvm::StringRef variableNameEndingWithFile = "endsWithFile";

auto isPointerToConstChar = pointerType(pointee(isAnyCharacter(), isConstQualified()));
/*
static transformer::RewriteRuleWith<std::string>
myRewriteRule() {
  const auto constPtrStrLiteralDecl = varDecl(
                                              isDefinition(),
                                              hasType(pointerType(pointee(isAnyCharacter(), isConstQualified()))),
                                              matchesName(".*file"));
  return makeRule(constPtrStrLiteralDecl.bind(variableNameEndingWithFile),
                    //noopEdit(node(variableNameEndingWithFile)),
                    remove(node(variableNameEndingWithFile)),
                    cat("Found C-string file variable declaration to remove"));
}
*/

static transformer::RewriteRuleWith<std::string>
secondRewriteRule() {
  return
      applyFirst({
              makeRule(
                       binaryOperator(isAssignmentOperator(),
                                      hasOperands(declRefExpr(to(varDecl().bind("declaration"))).bind("variable name"),
                                                  callExpr(hasDeclaration(functionDecl(hasName("opt2fn_null")))).bind("func call"))),
                       {
                           changeTo(cat("std::filesystem::path ",
                                        node("variable name"),
                                        " = opt2path_optional(",
                                        callArgs("func call"),
                                        cat(")"))),
                           remove(node("declaration"))
                       },
                       cat("Found opt2fn_null usage to change")
                       ),
              makeRule(
                       // Find parameters with types declared as optional<path> passed to function as arguement with type const char* and change that.
                       callExpr( forEachArgumentWithParam( declRefExpr(hasType(isPointerToConstChar)).bind("parm"),
                                                           parmVarDecl(hasType(isPointerToConstChar)).bind("arg"))).bind("call"),
                       
                       //isPointerToConstChar
                       noopEdit(node("parm")),
                       cat("Found call taking const char*")
                       ),
          });
}

CStringToStdFilenameCheck::CStringToStdFilenameCheck(StringRef Name, ClangTidyContext *Context)
    : TransformerClangTidyCheck(secondRewriteRule(), Name, Context) {}

/*
void CStringToStdFilenameCheck::registerMatchers(MatchFinder *Finder) {
  // FIXME: Add matchers.
  Finder->addMatcher(varDecl().bind("filenameVariable"), this);
}

void CStringToStdFilenameCheck::check(const MatchFinder::MatchResult &Result) {
  // FIXME: Add callback implementation.
  const auto *MatchedDecl = Result.Nodes.getNodeAs<FunctionDecl>("filenameVariable");
  if (!MatchedDecl->getIdentifier() || MatchedDecl->getName().startswith("awesome_"))
    return;
  diag(MatchedDecl->getLocation(), "function %0 is insufficiently awesome")
      << MatchedDecl
      << FixItHint::CreateInsertion(MatchedDecl->getLocation(), "awesome_");
  diag(MatchedDecl->getLocation(), "insert 'awesome'", DiagnosticIDs::Note);
}
*/

} // namespace clang::tidy::modernize
