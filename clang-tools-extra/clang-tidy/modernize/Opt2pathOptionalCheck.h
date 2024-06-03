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

        struct Comparison
        {
                // The binary operator expression to refactor (typically == or !=)
                const BinaryOperator* checkForPath_;
                // Whether the refactored code should test whether the
                // value is present (otherwise test for not present).
                bool testForValuePresent_;
        };
    private:
        struct Assertion;
        struct PossibleUseOfOptionalPath;
        struct ConvertedParameter
        {
                // The declaration converted
                const ParmVarDecl* parmVarDecl_;
                // Whether it was converted to plain path (otherwise optional<path>)
                bool convertedToPath_;
        };
        std::unordered_set<const VarDecl*> varDeclOfOptionalFilenames_;
        std::unordered_map<const CompoundStmt*, std::vector<Assertion>> assertionsByEnclosingCompoundStmt_;
        std::unordered_map<const VarDecl*, std::vector<PossibleUseOfOptionalPath>> possibleUsesOfOptionalPath_;
        std::unordered_map<const VarDecl*, std::vector<Comparison>> possibleUsesOfOptionalPathInComparisons_;
        // Collection of function parameters whose type was converted
        // to optional<path> or path.
        std::vector<ConvertedParameter> convertedParameters_;
        // Collection of nullptr expressions used as arguments to
        // functions whose parameters might be potential optional
        // paths.
        std::unordered_map<const ParmVarDecl*, std::vector<const Expr*>> paramDeclsReceivingNullptr_;

        void refactorFunctionCall(const PossibleUseOfOptionalPath& useOfOptionalPath,
                                  bool convertToPath,
                                  const VarDecl* parmVarDeclToChange,
                                  ASTContext *context);
        bool optionalPathUsedAsValue(bool convertToPath,
                                     const DeclRefExpr *declRefExpr, const VarDecl* varDecl, const CompoundStmt* optionalCompoundStmt,
                                     ASTContext *context);
        void refactorUseOfOptionalPath(const DeclRefExpr *declRefExpr,
                                       const bool extractFromOptional);
        void refactorUseOfOptionalPathInBinaryOperator(const VarDecl *varDecl,
                                                       const Expr* possibleBinaryOperatorExpressionToRefactor);
        void refactorBinaryOperatorIfApplicable(const VarDecl* varDecl);
        void refactorUseOfOptionalPathInPrintfStyleFunctionCall(const DeclRefExpr *declRefExpr,
                                                        bool convertToPath);
        void refactorFunctionDeclReceivingPath(bool convertToPath,
                                               const ParmVarDecl* parmVarDeclToChange,
                                               ASTContext *context);

        void onEndOfTranslationUnit() final;
};

} // namespace clang::tidy::modernize

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MODERNIZE_OPT2PATHOPTIONALCHECK_H
