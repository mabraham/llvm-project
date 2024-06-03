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

// When a variable or parameter of type const char * is encountered,
// it might represent a path (if it cannot be nullptr) or an optional
// path (if it can be nullptr) or something else. That is referred to
// as a "potential optional path" below.
//
// When a call to a builder function is known to return either a path
// or nullptr, we call that an "optional builder." We know that when
// the return values from such builders are assigned to variables or
// accepted as function arguments that those variables or function
// parameters represent optional paths. This information can be used
// to decide whether a potential optional path is actually an optional
// path, and have a type refactoring accordingly.
//
// Conditionals or assertions on optional path variables can make it
// safe to assume that the optional has a value in the following
// scope. A function parameter receiving such a value should be
// refactored to take a path.
//
// The matching takes place in source order and are purely based on
// language syntax. For example, at the point we match a variable
// declaration with the right type, we don't yet know whether it will
// have the return value from an optional builder assigned to it. So
// in many cases the bound nodes from the matches are recorded for
// later use when we find out which matches are truly relevant.
void Opt2pathOptionalCheck::registerMatchers(MatchFinder *Finder) {
    // Match any of the set of optional path builder functions.
    auto callExprToOptionalBuilder =
        callExpr(hasDeclaration(functionDecl(anyOf(hasName("opt2fn_null"),
                                                   hasName("ftp2fn_null"))
                                             ).bind("declaration of optional builder function")));
    // Match declarations that receive an optional by assignment.
    Finder->addMatcher
        (binaryOperator
         (isAssignmentOperator(),
          hasOperands
          (declRefExpr(to(varDecl().bind("declaration of optional path"))).bind("declaration of variable assigned an optional path"),
           callExprToOptionalBuilder)),
         this);
    // Match calls to optional builders, which might happen e.g. in
    // assignment operations or as direct call expressions passed as
    // function arguments.
    Finder->addMatcher(callExprToOptionalBuilder.bind("call of optional builder"), this);
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
                                                        to(varDecl().bind("declaration of potential optional path")))))))
                        ).bind("parenthesized expression to replace"),
                       this);

    // Match compound statements that include assertions on const
    // char* variables, so that uses of such variables later in the
    // scope of that statement can know it is valid to use .value()
    //    
    // We'd like to match on the assertion directly, but it's a macro
    // and the AST only sees the code after expansion. This matcher is
    // a crude model of an assertion, but it only has to work well
    // enough with the libc used by the clang-tidy version in use. We
    // are also not analyzing the sense in which the variable is used
    // in the assertion, assuming that the only relevant cases are
    // testing whether an optional path has a value.
    auto assertionExpr = parenExpr
        (anyOf(hasDescendant(declRefExpr(to(functionDecl(hasName("__assert_fail"))))),
               hasDescendant(declRefExpr(to(functionDecl(hasName("assertHandler")))))),
         hasDescendant(declRefExpr(to(varDecl(hasType(isPointerToConstChar)).bind("declaration of variable referenced in assertion"))))
         ).bind("asssertion parenthesis expression");
    // Match each assertion within a compound statement
    Finder->addMatcher
        (compoundStmt(forEachDescendant(assertionExpr)
                      ).bind("compound statement enclosing assertion"),
         this);
    // Match when an expression refers to a variable whose declaration
    // was bound to ID "declaration of potential optional path" in
    // this matcher.
    const auto declRefExprToPotentialOptionalPath = declRefExpr(to(varDecl(equalsBoundNode("declaration of potential optional path"))));
    // Match when an expression that refers to a potential optional
    // path (bound to "declaration of potential optional path") is
    // found in the body of an if statement, when the condition of
    // that if statement refers to the same potential optional path e.g.
    //
    // if (thePath)
    // {
    //   someFunction(thePath)
    // }
    //
    // This match permits the checker to understand that thePath has a
    // value.
    const auto inBodyOfIfStmt =
        traverse
        (TK_IgnoreUnlessSpelledInSource,
         hasAncestor
         (compoundStmt // Make sure the declRefExpr match above is not the one in the if-statement condition matched below
          (hasAncestor
           (ifStmt
            (hasCondition
             (anyOf
              // Match 'if(optionalPath)'.
              (declRefExprToPotentialOptionalPath,
               // Match 'if(optionalPath != nullptr)'.
               binaryOperator(hasOperatorName("!="),
                              hasOperands
                              (declRefExprToPotentialOptionalPath,
                               cxxNullPtrLiteralExpr()
                               )).bind("possible binary operator to refactor"),
               // Match 'if(optionalPath && somethingTrue && somethingElseTrue)'.
               //
               // This is very flawed, but if the existing code uses
               // 'if (optionalPath && somethingTrue || somethingElse)' and then does an unchecked
               // access to optionalPath, then there's already bigger problems with the code.
               //
               // TODO this is also quite brittle and quite repetitive
               hasDescendant(binaryOperator(hasOperatorName("&&"),
                                            hasOperands
                                            (expr(),
                                             declRefExprToPotentialOptionalPath
                                             ))),
               binaryOperator(hasOperatorName("&&"),
                              hasOperands
                              (expr(),
                               declRefExprToPotentialOptionalPath
                               ))
               ))).bind("possible if condition means optional has value")
            ))));
    const auto argRefersToOptionalPathOptionallyWithinAContext =
        // The most frequent cases to match are when a function is
        // passed a reference to a variable whose type is const char
        // *.  We also need to match on calls to functions already
        // taking std::filesystem::path, which triggers an implicit
        // call to a std::string constructor when "passed" a const
        // char *. The simplest approach is to configure the AST
        // traversal to ignore all implicit nodes, like casts and
        // implicit constructor calls.
        traverse
        (TK_IgnoreUnlessSpelledInSource,
         declRefExpr(hasType(isPointerToConstChar),
                     to(varDecl().bind("declaration of potential optional path")),
                     optionally
                     // Used to find assertions that mean that the
                     // optional is known to have a value.
                     (hasAncestor(compoundStmt().bind("optional ancestor compound statement"))),
                     // Used to find whether an if statement
                     // condition means the optional is known to
                     // have a value.
                     optionally(inBodyOfIfStmt)
                     ).bind("use of potential optional path"));
    // Match each argument of a function call that is a variable that
    // is a potential optional path. Explore the surrounding context
    // for clues about whether the variable is known to be
    // valid. These can be used later when we learn that the variable
    // actually is an optional path.
    Finder->addMatcher
        (callExpr
         (forEachArgumentWithParam
          (argRefersToOptionalPathOptionallyWithinAContext,
           parmVarDecl().bind("possible function parameter receiving optional path"))
          ).bind("expression using potential optional path"),
         this);
    // Match each argument of a constructor call that is a variable that
    // is a potential optional path. Explore the surrounding context
    // for clues about whether the variable is known to be
    // valid. These can be used later when we learn that the variable
    // actually is an optional path.
    Finder->addMatcher
        (cxxConstructExpr
         (forEachArgumentWithParam
          (argRefersToOptionalPathOptionallyWithinAContext,
           parmVarDecl().bind("possible function parameter receiving optional path"))
          ).bind("expression using potential optional path"),
         this);
    // Match code like 'if (thePath == nullptr) { return; } so that we
    // can understand if a reference to thePath in the following scope
    // is known to be valid. In practice, this works the same as an
    // assertion.
    Finder->addMatcher
        (compoundStmt
         (hasDescendant
          (ifStmt
           (hasCondition
            (anyOf
             // We want to refactor 'thePath == nullptr' to '!thePath'
             (binaryOperator
              (hasOperatorName("=="),
               hasOperands
               (ignoringImplicit(cxxNullPtrLiteralExpr()),
                ignoringImplicit(declRefExpr(hasType(isPointerToConstChar),
                                             to(varDecl().bind("declaration of potential optional path")))))
               ).bind("check for invalid path to refactor"),
              // No need to refactor if it's already '!thePath'
              unaryOperator
              (hasOperatorName("!"),
               hasUnaryOperand
               (ignoringImplicit(declRefExpr(hasType(isPointerToConstChar),
                                             to(varDecl().bind("declaration of potential optional path")))))))),
            hasThen
            (anyOf(returnStmt(),
                   hasDescendant(returnStmt())))
            ).bind("possible if statement means optional has value in following scope")
           )).bind("compound statement enclosing fast return"),
         this);
    // Match calls to printf-style functions that take potential
    // optional paths as arguments. These don't match above because
    // they are variadic functions, so don't have parameter
    // declarations. But fortunately we don't need them because we
    // aren't going to refactor into these functions.
    Finder->addMatcher
        (callExpr
         (hasAnyArgument
          (ignoringImplicit(declRefExpr(hasType(isPointerToConstChar),
                                        to(varDecl().bind("declaration of potential optional path"))
                                        ).bind("use of potential optional path as argument to printf-style function"))),
          hasDeclaration(functionDecl(anyOf(hasName("printf"),
                                            hasName("fprintf"),
                                            hasName("gmx_fatal")),
                                      isVariadic()))
          ).bind("call expression to printf-style function using potential optional path"),
         this);
    // Match each argument of a function call that is the return value
    // from one of the optional-builder functions.
    Finder->addMatcher
        (callExpr
         (forEachArgumentWithParam
          (callExprToOptionalBuilder,
           parmVarDecl().bind("possible function parameter receiving optional path"))
          ).bind("call expression using optional path from builder"),
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
           parmVarDecl(hasType(isPointerToConstChar)).bind("potential path parameter taking nullptr")
           )),
         this);
    // Match uses like 'std::filesystem::path var = somePath;' where
    // somePath is either a path or a possible optional path known to
    // have a value from a conditional or assertion.
    Finder->addMatcher
        (declStmt
         (has(varDecl
              (hasType(asString("std::filesystem::path")),
               hasDescendant
               (declRefExpr(hasType(isPointerToConstChar),
                            to(varDecl().bind("declaration of potential optional path"))
                            ).bind("use of potential optional path in declaration of path")),
               optionally
               // Used to find assertions that mean that the
               // optional is known to have a value.
               (hasAncestor(compoundStmt().bind("optional ancestor compound statement"))),
               // Used to find whether an if statement
               // condition means the optional is known to
               // have a value.
               optionally(inBodyOfIfStmt)
               ).bind("declaration of path assigned value from potential optional path"))),
         this);
}

// Records details about an assertion that might be needed later.
struct Opt2pathOptionalCheck::Assertion
{
        SourceLocation endOfAssertionParenExpr_;
        const VarDecl* declarationOfVariableReferencedInAssertion_;
};

// Records details about the use of a variable that might be an
// optional path and might be needed later.
struct Opt2pathOptionalCheck::PossibleUseOfOptionalPath
{
        bool convertToPath_;
        const DeclRefExpr* declRefExpr_;
        const CompoundStmt* optionalCompoundStmt_;
        const ParmVarDecl* optionalParmVarDeclToChange_;
        const Expr* expr_; // Only used for debug printf
        const CallExpr* callExpr_; // Used when the use of a variable really was in a call expression
        const BinaryOperator* possibleBinaryOperatorToRefactor_;
};

Opt2pathOptionalCheck::~Opt2pathOptionalCheck() = default;

Opt2pathOptionalCheck::Opt2pathOptionalCheck(StringRef Name,
                                             ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context) {}

// Helper function to pretty-print any AST node
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

bool Opt2pathOptionalCheck::optionalPathUsedAsValue(const bool convertToPath,
                                                    const DeclRefExpr *declRefExpr, const VarDecl* varDecl, const CompoundStmt* optionalCompoundStmt, ASTContext *context)
{
    if (convertToPath)
    {
        return true;
    }
    if (!optionalCompoundStmt)
    {
        return false;
    }
    // Does the compound statement include an assertion?
    if (const auto assertionIt = assertionsByEnclosingCompoundStmt_.find(optionalCompoundStmt);
        assertionIt != assertionsByEnclosingCompoundStmt_.end())
    {
        for(const auto& assertion : assertionIt->second)
        {
            // Does the assertion refer to the same variable we matched?
            if (assertion.declarationOfVariableReferencedInAssertion_ == varDecl)
            {
                // Does the assertion precede the variable reference within the context of the match?
                BeforeThanCompare<SourceLocation> isBefore(context->getSourceManager());
                if (isBefore(assertion.endOfAssertionParenExpr_, declRefExpr->getBeginLoc()))
                {
                    // Assume the assertion makes it safe to extract the value from the optional path.
                    return true;
                }
            }
        }
    }
    return false;
}

void Opt2pathOptionalCheck::refactorUseOfOptionalPathInPrintfStyleFunctionCall(const DeclRefExpr *declRefExpr,
                                                                               const bool convertToPath)
{
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

void Opt2pathOptionalCheck::refactorUseOfOptionalPath(const DeclRefExpr *declRefExpr,
                                                      const bool extractFromOptional,
                                                      const BinaryOperator* possibleBinaryOperatorToRefactor)
{
    const std::string variableName = declRefExpr->getNameInfo().getAsString();
    if (extractFromOptional)
    {
        // This refactors the body of this function
        diag(declRefExpr->getBeginLoc(), "Extract std::filesystem::path from std::optional<std::filesystem::path>")
            << FixItHint::CreateReplacement(declRefExpr->getSourceRange(), variableName + ".value()");
    }
    if (possibleBinaryOperatorToRefactor)
    {
        diag(possibleBinaryOperatorToRefactor->getBeginLoc(), "Use std::optional::operator bool() rather than comparison with nullptr")
            << FixItHint::CreateReplacement(possibleBinaryOperatorToRefactor->getSourceRange(), variableName);
    }
}

// Refactors the declarations of functions *called* from this one
void Opt2pathOptionalCheck::refactorFunctionDeclReceivingPath(const bool convertToPath,
                                                              const ParmVarDecl* parmVarDeclToChange,
                                                              ASTContext *context)
{
    if (parmVarDeclToChange)
    {
        const std::string replacementParameterType = (convertToPath ? "const std::filesystem::path&" :
                                                      "const std::optional<std::filesystem::path>&");
        diag(parmVarDeclToChange->getBeginLoc(), "Change function parameter to " + replacementParameterType)
            << FixItHint::CreateReplacement(parmVarDeclToChange->getSourceRange(), replacementParameterType + " " + parmVarDeclToChange->getNameAsString());
        convertedParameters_.push_back({parmVarDeclToChange, convertToPath});
        
        // Now we know that that parameter is an (optional) path so we should check uses of that parameter and perhaps refactor
        for (const PossibleUseOfOptionalPath& useOfOptionalPath : possibleUsesOfOptionalPath_[parmVarDeclToChange])
        {
            refactorFunctionCall(useOfOptionalPath, convertToPath, parmVarDeclToChange, context);
        }
    }
}

bool isPrintfStyleFunctionCallExpr(const CallExpr* callExpr)
{
    if (callExpr)
    {
        if (const auto* functionDecl = callExpr->getDirectCallee())
        {
            const std::string functionName = functionDecl->getNameAsString();
            return (functionName == "fprintf" ||
                    functionName == "printf" ||
                    functionName == "gmx_fatal");
        }
    }
    return false;
}

void refactorBinaryOperator(const Opt2pathOptionalCheck::Comparison& comparison,
                            const VarDecl* varDecl,
                            ClangTidyCheck* check)
{
    const std::string variableName = varDecl->getNameAsString();
    const std::string replacement = (comparison.testForValuePresent_ ? variableName : ("!" + variableName));
    check->diag(comparison.checkForPath_->getBeginLoc(), "Use " + replacement + " instead of comparison with nullptr")
        << FixItHint::CreateReplacement(comparison.checkForPath_->getSourceRange(), replacement);
}

void Opt2pathOptionalCheck::refactorBinaryOperatorIfApplicable(const VarDecl* varDecl)
{
    const auto comparisonsToRefactorIt = possibleUsesOfOptionalPathInComparisons_.find(varDecl);
    if (comparisonsToRefactorIt != possibleUsesOfOptionalPathInComparisons_.end())
    {
        for (const auto comparison : comparisonsToRefactorIt->second)
        {
            refactorBinaryOperator(comparison, varDecl, this);
        }
    }
}

void Opt2pathOptionalCheck::refactorFunctionCall(const Opt2pathOptionalCheck::PossibleUseOfOptionalPath& useOfOptionalPath,
                                                 const bool convertToPath,
                                                 const VarDecl* varDeclToChange,
                                                 ASTContext *context)
{
    if (isPrintfStyleFunctionCallExpr(useOfOptionalPath.callExpr_))
    {
        refactorUseOfOptionalPathInPrintfStyleFunctionCall(useOfOptionalPath.declRefExpr_,
                                                           convertToPath);
    }
    else
    {
        // First change the points at which we use the parameter
        const bool extractFromOptional = optionalPathUsedAsValue(useOfOptionalPath.convertToPath_,
                                                                 useOfOptionalPath.declRefExpr_,
                                                                 varDeclToChange,
                                                                 useOfOptionalPath.optionalCompoundStmt_,
                                                                 context);
        refactorBinaryOperatorIfApplicable(varDeclToChange);
        refactorUseOfOptionalPath(useOfOptionalPath.declRefExpr_, extractFromOptional,
                                  useOfOptionalPath.possibleBinaryOperatorToRefactor_);

        // Then refactor function calls that receive that parameter
        refactorFunctionDeclReceivingPath(convertToPath || extractFromOptional,
                                          useOfOptionalPath.optionalParmVarDeclToChange_,
                                          context);
    }
}

void Opt2pathOptionalCheck::check(const MatchFinder::MatchResult &Result)
{
    // Note that the Result objects ar returned to this function in
    // order of traversal of the AST, and not in order of the calls to
    // finder->addMatcher(). So the pattern of storing matches to collections
    // and looking them up later works.
    //
    // Note that some matchers bind nodes with the same ID even when
    // identified from different kinds of AST fragments, so that the
    // same diagnostics can be given for them.
    if (const auto *match = Result.Nodes.getNodeAs<VarDecl>("declaration of optional path"))
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
    if (const auto *match = Result.Nodes.getNodeAs<CallExpr>("call of optional builder"))
    {
        const Expr* callee = match->getCallee();
        const auto* functionDecl = Result.Nodes.getNodeAs<FunctionDecl>("declaration of optional builder function");
        std::string functionName = functionDecl->getNameAsString();
        std::string replacementName = (functionName == "opt2fn_null") ? "opt2path_optional" : "ftp2path_optional";
        diag(callee->getBeginLoc(), "Use " + replacementName + " instead of " + functionName)
            << FixItHint::CreateReplacement(SourceRange(callee->getBeginLoc(), callee->getEndLoc()), replacementName);
    }
    if (const auto *match = Result.Nodes.getNodeAs<DeclRefExpr>("declaration of variable assigned an optional path"))
    {
        diag(match->getLocation(), "Use std::optional<std::filesystem::path>")
            << FixItHint::CreateInsertion(match->getLocation(), "std::optional<std::filesystem::path> ");
    }
    if (const auto *match = Result.Nodes.getNodeAs<ParenExpr>("parenthesized expression to replace"))
    {
        const auto *varDecl = Result.Nodes.getNodeAs<VarDecl>("declaration of potential optional path");
        if (varDeclOfOptionalFilenames_.find(varDecl) != varDeclOfOptionalFilenames_.end())
        {
            diag(match->getBeginLoc(), "Use std::optional::operator bool() rather than comparison with nullptr")
                << FixItHint::CreateReplacement(match->getSourceRange(), varDecl->getNameAsString());
        }
    }
    if (const auto *compoundStmt = Result.Nodes.getNodeAs<CompoundStmt>("compound statement enclosing assertion"))
    {
        const auto *varDecl = Result.Nodes.getNodeAs<VarDecl>("declaration of variable referenced in assertion");
        const auto *assertionParenExpr = Result.Nodes.getNodeAs<ParenExpr>("asssertion parenthesis expression");
        assertionsByEnclosingCompoundStmt_[compoundStmt].push_back
            ({assertionParenExpr->getEndLoc(), varDecl});
    }
    if (const auto *compoundStmt = Result.Nodes.getNodeAs<CompoundStmt>("compound statement enclosing fast return"))
    {
        const auto *varDecl = Result.Nodes.getNodeAs<VarDecl>("declaration of potential optional path");
        const auto *ifStmt = Result.Nodes.getNodeAs<IfStmt>("possible if statement means optional has value in following scope");
        assertionsByEnclosingCompoundStmt_[compoundStmt].push_back
            ({ifStmt->getEndLoc(), varDecl});

        if (const auto* checkForInvalidPath = Result.Nodes.getNodeAs<BinaryOperator>("check for invalid path to refactor"))
        {
            const bool testForValuePresent = false;
            if (varDeclOfOptionalFilenames_.find(varDecl) != varDeclOfOptionalFilenames_.end())
            {
                refactorBinaryOperator({checkForInvalidPath, testForValuePresent}, varDecl, this);
            }
            else
            {
                possibleUsesOfOptionalPathInComparisons_[varDecl].push_back({checkForInvalidPath, testForValuePresent});
            }
        }
    }
    if (const auto *declRefExpr = Result.Nodes.getNodeAs<DeclRefExpr>("use of potential optional path"))
    {
        const auto *varDecl = Result.Nodes.getNodeAs<VarDecl>("declaration of potential optional path");
        const bool convertToPath = Result.Nodes.getNodeAs<IfStmt>("possible if condition means optional has value");
        // The match could be e.g. from a constructor call, which is
        // not a CallExpr. If so, avoid refactoring the declaration of
        // the parameter.
        const auto *callExpr = Result.Nodes.getNodeAs<CallExpr>("expression using potential optional path");
        const ParmVarDecl *parmVarDecl = callExpr ? Result.Nodes.getNodeAs<ParmVarDecl>("possible function parameter receiving optional path") : nullptr;
        PossibleUseOfOptionalPath possibleUseOfOptionalPath{
            convertToPath,
            declRefExpr,
            Result.Nodes.getNodeAs<CompoundStmt>("optional ancestor compound statement"),
            parmVarDecl,
            Result.Nodes.getNodeAs<Expr>("expression using potential optional path"),
            callExpr,
            Result.Nodes.getNodeAs<BinaryOperator>("possible binary operator to refactor")
        };
        // If we find that the declaration of this variable is one of
        // the known optional filenames (e.g. because it was assigned
        // a value that was the return from an optional-builder
        // function), then refactor the function that receives the
        // variable as an argument.
        if (varDeclOfOptionalFilenames_.find(varDecl) != varDeclOfOptionalFilenames_.end())
        {
            refactorFunctionCall(possibleUseOfOptionalPath, convertToPath, varDecl, Result.Context);
        }
        else
        {
            possibleUsesOfOptionalPath_[varDecl].push_back(possibleUseOfOptionalPath);
        }
    }
    // Refactor function f that was called like f(opt2fn_null(args))
    if (Result.Nodes.getNodeAs<CallExpr>("call expression using optional path from builder"))
    {
        const auto *parmVarDeclToChange = Result.Nodes.getNodeAs<ParmVarDecl>("possible function parameter receiving optional path");
        // Then we refactor the function that is called
        const bool convertToPath = false;
        refactorFunctionDeclReceivingPath(convertToPath, parmVarDeclToChange, Result.Context);
    }
    if (const auto *nullptrExpr = Result.Nodes.getNodeAs<Expr>("nullptr to potentially replace"))
    {
        const auto *parmVarDecl = Result.Nodes.getNodeAs<ParmVarDecl>("potential path parameter taking nullptr");
        paramDeclsReceivingNullptr_[parmVarDecl].push_back(nullptrExpr);
    }
    if (const auto *callExpr = Result.Nodes.getNodeAs<CallExpr>("call expression to printf-style function using potential optional path"))
    {
        const bool convertToPath = false;
        const auto* varDecl = Result.Nodes.getNodeAs<VarDecl>("declaration of potential optional path");
        // Most of these fields won't matter when the call expr is to a printf-style function
        PossibleUseOfOptionalPath possibleUseOfOptionalPath{
            convertToPath,
            Result.Nodes.getNodeAs<DeclRefExpr>("use of potential optional path as argument to printf-style function"),
            nullptr,
            nullptr,
            callExpr,
            callExpr,
            nullptr
        };
        // If we find that the declaration of this variable is one of
        // the known optional filenames (e.g. because it was assigned
        // a value that was the return from an optional-builder
        // function), then refactor the function call.
        if (varDeclOfOptionalFilenames_.find(varDecl) != varDeclOfOptionalFilenames_.end())
        {
            refactorUseOfOptionalPathInPrintfStyleFunctionCall(possibleUseOfOptionalPath.declRefExpr_,
                                                               possibleUseOfOptionalPath.convertToPath_);
        }
        else
        {
            possibleUsesOfOptionalPath_[varDecl].push_back(possibleUseOfOptionalPath);
        }
    }
    if (Result.Nodes.getNodeAs<VarDecl>("declaration of path assigned value from potential optional path"))
    {
        const auto* varDecl = Result.Nodes.getNodeAs<VarDecl>("declaration of potential optional path");
        const bool convertToPath = Result.Nodes.getNodeAs<IfStmt>("possible if condition means optional has value");
        const auto* declRefExpr = Result.Nodes.getNodeAs<DeclRefExpr>("use of potential optional path in declaration of path");
        const auto* possibleBinaryOperator = Result.Nodes.getNodeAs<BinaryOperator>("possible binary operator to refactor");
        PossibleUseOfOptionalPath possibleUseOfOptionalPath{
            convertToPath,
            declRefExpr,
            Result.Nodes.getNodeAs<CompoundStmt>("optional ancestor compound statement"),
            nullptr,
            nullptr,
            nullptr,
            possibleBinaryOperator
        };
        if (varDeclOfOptionalFilenames_.find(varDecl) != varDeclOfOptionalFilenames_.end())
        {
            refactorFunctionCall(possibleUseOfOptionalPath, convertToPath, varDecl, Result.Context);
        }
        else
        {
            possibleUsesOfOptionalPath_[varDecl].push_back(possibleUseOfOptionalPath);
        }
    }
}
 
void Opt2pathOptionalCheck::onEndOfTranslationUnit()
{
    // Now that we know we have seen all the function parameters that
    // might have changed type from const char* to either
    // optional<path> or path, we can go back and convert callers that
    // were passing nullptr constants to instead use a more
    // appropriate expression.
    const auto theEnd = paramDeclsReceivingNullptr_.end();
    for (const ConvertedParameter& convertedParameter : convertedParameters_)
    {
        const std::string replacementType = (convertedParameter.convertedToPath_ ?
                                             "std::filesystem::path{}" :
                                             "std::nullopt");
        if (const auto foundParamDeclIt = paramDeclsReceivingNullptr_.find(convertedParameter.parmVarDecl_);
            foundParamDeclIt != theEnd)
        {
            for (const Expr* nullptrExpression : foundParamDeclIt->second)
            {
                diag(nullptrExpression->getBeginLoc(), "Use " + replacementType + " instead of nullptr")
                    << FixItHint::CreateReplacement(nullptrExpression->getSourceRange(), replacementType);
            }
        }
    }
}

} // namespace clang::tidy::modernize
