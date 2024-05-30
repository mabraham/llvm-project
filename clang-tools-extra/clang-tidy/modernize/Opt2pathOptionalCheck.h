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
        bool optionalPathUsedAsValue(bool convertToPath,
                                     const DeclRefExpr *declRefExpr, const VarDecl* varDecl, const CompoundStmt* optionalCompoundStmt,
                                     ASTContext *context);
        void refactorUseOfPath(const DeclRefExpr *declRefExpr,
                               const bool extractFromOptional,
                               const BinaryOperator* binaryOperatorToRefactor);
        void refactorUseOfPathInPrintfStyleFunctionCall(const DeclRefExpr *declRefExpr,
                                                        bool convertToPath);
        void refactorFunctionDeclReceivingPath(bool convertToPath,
                                               const ParmVarDecl* parmVarDeclToChange,
                                               const CallExpr* callExpr,
                                               ASTContext *context);

        void onEndOfTranslationUnit() final;
    private:
        struct Assertion;
        struct PossibleUseOfOptionalPath;
        std::unordered_set<const VarDecl*> varDeclOfOptionalFilenames_;
        std::unordered_map<const CompoundStmt*, std::vector<Assertion>> assertionsByEnclosingCompoundStmt_;
        std::unordered_map<const VarDecl*, std::vector<PossibleUseOfOptionalPath>> possibleUsesOfOptionalPath_;
        std::vector<const ParmVarDecl*> parametersConvertedToOptionalPath_;
        std::vector<const ParmVarDecl*> parametersConvertedToPath_;
        std::unordered_map<const ParmVarDecl*, std::vector<const Expr*>> paramDeclsWithTypeConstCharPointersReceivingNullptr_;
};

} // namespace clang::tidy::modernize

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MODERNIZE_OPT2PATHOPTIONALCHECK_H
