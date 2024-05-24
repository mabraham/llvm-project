//===--- Opt2pathOptionalCheck.h - clang-tidy -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MODERNIZE_OPT2PATHOPTIONALCHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MODERNIZE_OPT2PATHOPTIONALCHECK_H

#include "../ClangTidyCheck.h"

#include <memory>
#include <string>
#include <unordered_set>

namespace clang::tidy::modernize {

/// FIXME: Write a short description.
///
/// For the user-facing documentation see:
/// http://clang.llvm.org/extra/clang-tidy/checks/modernize/opt2path-optional.html
class Opt2pathOptionalCheck : public ClangTidyCheck {
    public:
        Opt2pathOptionalCheck(StringRef Name, ClangTidyContext *Context);
        ~Opt2pathOptionalCheck();
        void registerMatchers(ast_matchers::MatchFinder *Finder) override;
        void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
        void updateVariableWithinFunction(const ast_matchers::MatchFinder::MatchResult &Result,
                                          const std::string& variableName,
                                          const FunctionDecl* functionDecl,
                                          bool toOptionalPath);
        void updateFunctionDeclaration(const ast_matchers::MatchFinder::MatchResult &Result,
                                       const FunctionDecl* functionDeclToChange,
                                       const ParmVarDecl* parmVarDeclToChange,
                                       bool toOptionalPath);

    private:
        class IndexerVisitor;
        struct Assertion;
        std::unique_ptr<IndexerVisitor> indexer_;
        std::unordered_set<const VarDecl*> varDeclOfOptionalFilenames_;
        std::unordered_map<const CompoundStmt*, std::vector<Assertion>> assertionsByEnclosingCompoundStmt_;
};

} // namespace clang::tidy::modernize

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MODERNIZE_OPT2PATHOPTIONALCHECK_H
