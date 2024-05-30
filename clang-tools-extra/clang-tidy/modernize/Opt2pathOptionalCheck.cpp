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
                                                   hasName("ftp2fn_null")))));
    // Match declarations that receive an optional by assignment.
    Finder->addMatcher
        (binaryOperator
         (isAssignmentOperator(),
          hasOperands
          (declRefExpr(to(varDecl().bind("declaration"))).bind("declaration of variable assigned an optional path"),
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
        (hasDescendant(declRefExpr(to(functionDecl(hasName("__assert_fail"))))),
         hasDescendant(declRefExpr(to(varDecl(hasType(isPointerToConstChar)).bind("declaration of variable referenced in assertion"))))
         ).bind("asssertion parenthesis expression");
    // Match each assertion within a compound statement
    Finder->addMatcher
        (compoundStmt(forEachDescendant(assertionExpr)
                      ).bind("compound statement enclosing assertion"),
         this);
    // Match each argument of a function call that is a variable that
    // is a potential optional path. Explore the surrounding context
    // for clues about whether the variable is known to be
    // valid. These can be used later when we learn that the variable
    // actually is an optional path.
    Finder->addMatcher
        (callExpr
         (forEachArgumentWithParam
          // The most frequent cases to match are when a function is
          // passed a reference to a variable whose type is const char
          // *.  We also need to match on calls to functions already
          // taking std::filesystem::path, which triggers an implicit
          // call to a std::string constructor when "passed" a const
          // char *. The simplest approach is to configure the AST
          // traversal to ignore all implicit nodes, like casts and
          // implicit constructor calls.
          (traverse
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
                        optionally
                        (hasAncestor
                         (compoundStmt // Make sure the declRefExpr match above is not the one in the if-statement condition matched below
                          (hasAncestor
                           (ifStmt
                            (hasCondition
                             (anyOf
                              // Match 'if(optionalPath)'.
                              (ignoringImplicit(declRefExpr(to(varDecl(equalsBoundNode("declaration of potential optional path"))))),
                               // Match 'if(optionalPath != nullptr)'.
                               binaryOperator(hasOperatorName("!="),
                                              hasOperands
                                              (ignoringImplicit(declRefExpr(to(varDecl(equalsBoundNode("declaration of potential optional path"))))),
                                               ignoringImplicit(cxxNullPtrLiteralExpr())
                                               )).bind("possible binary operator to refactor"),
                               // Match 'if(optionalPath && somethingTrue && somethingElseTrue)'.
                               //
                               // This is very flawed, but if the existing code uses
                               // 'if (optionalPath && somethingTrue || somethingElse)' and then does an unchecked
                               // access to optionalPath, then there's already bigger problems with the code.
                               hasDescendant(binaryOperator(hasOperatorName("&&"),
                                                            hasOperands
                                                            (expr(),
                                                             ignoringImplicit(declRefExpr(to(varDecl(equalsBoundNode("declaration of potential optional path")))))
                                                             )))
                               ))).bind("possible if condition means optional has value")
                            ))))
                        ).bind("use of potential optional path as function argument")),
           parmVarDecl().bind("possible function parameter receiving optional path"))
          ).bind("call expression using potential optional path"),
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
        // TODO only used in debug
        const CallExpr* callExpr_;
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
    //fprintf(stderr, "Matched variable named '%s' used compound statement '%s'\n",
    //        varDecl->getNameAsString().c_str(),
    //        optionalCompoundStmt ? prettyPrint(optionalCompoundStmt).c_str() : "missing");
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

void Opt2pathOptionalCheck::refactorUseOfPathInPrintfStyleFunctionCall(const DeclRefExpr *declRefExpr,
                                                                        const bool convertToPath)
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

void Opt2pathOptionalCheck::refactorUseOfPath(const DeclRefExpr *declRefExpr,
                                              const bool extractFromOptional,
                                              const BinaryOperator* possibleBinaryOperatorToRefactor)
{
    const std::string variableName = declRefExpr->getNameInfo().getAsString();
    fprintf(stderr, "Converting DeclRefExpr usage of %s to %s\n", variableName.c_str(), extractFromOptional ? "path" : "optional path");
    if (extractFromOptional)
    {
        // This refactors the body of this function
        fprintf(stderr, "Extracting .value() on expression '%s'\n", prettyPrint(declRefExpr).c_str());
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
                                                              const CallExpr* callExpr,
                                                              ASTContext *context)
{
    fprintf(stderr, "Refactoring function '%s' taking (optional) path, convertToPath is %s.\n", prettyPrint(callExpr).c_str(), convertToPath ? "true" : "false");
    // TODO remove
    {
        if (parmVarDeclToChange)
        {
            fprintf(stderr, "Found function parameter '%s' to change to %s\n", parmVarDeclToChange->getNameAsString().c_str(), convertToPath ? "path" : "optional");
            if (convertToPath)
            {
                // This refactors the declaration of a function called from this function
                const std::string replacementParameterType = "const std::filesystem::path&";
                diag(parmVarDeclToChange->getBeginLoc(), "Change function parameter to " + replacementParameterType)
                    << FixItHint::CreateReplacement(parmVarDeclToChange->getSourceRange(), replacementParameterType + " " + parmVarDeclToChange->getNameAsString());
                parametersConvertedToPath_.push_back(parmVarDeclToChange);
            }
            else
            {
                // This refactors the declaration of a function called from this function
                const std::string replacementParameterType = "const std::optional<std::filesystem::path>&";
                diag(parmVarDeclToChange->getBeginLoc(), "Change function parameter to " + replacementParameterType)
                    << FixItHint::CreateReplacement(parmVarDeclToChange->getSourceRange(), replacementParameterType + " " + parmVarDeclToChange->getNameAsString());
                parametersConvertedToOptionalPath_.push_back(parmVarDeclToChange);
            }

            // Now we know that that parameter is an (optional) path so we should check uses of that parameter and perhaps refactor
            for (const PossibleUseOfOptionalPath& useOfOptionalPath : possibleUsesOfOptionalPath_[parmVarDeclToChange])
            {
                refactorFunctionCall(useOfOptionalPath, convertToPath, parmVarDeclToChange, context);
            }
        }
    }
}

void Opt2pathOptionalCheck::refactorFunctionCall(const Opt2pathOptionalCheck::PossibleUseOfOptionalPath& useOfOptionalPath,
                                                 const bool convertToPath,
                                                 const VarDecl* parmVarDeclToChange,
                                                 ASTContext *context)
{
    // First change the points at which we use the parameter
    if (isPrintfStyleFunctionCallExpr(useOfOptionalPath.callExpr_))
    {
        refactorUseOfPathInPrintfStyleFunctionCall(useOfOptionalPath.declRefExpr_,
                                                   convertToPath);
    }
    else
    {
        const bool extractFromOptional = optionalPathUsedAsValue(useOfOptionalPath.convertToPath_,
                                                                 useOfOptionalPath.declRefExpr_,
                                                                 parmVarDeclToChange,
                                                                 useOfOptionalPath.optionalCompoundStmt_,
                                                                 context);
        refactorUseOfPath(useOfOptionalPath.declRefExpr_, extractFromOptional,
                          useOfOptionalPath.possibleBinaryOperatorToRefactor_);
        
        // Then refactor function calls that receive that parameter
        refactorFunctionDeclReceivingPath(convertToPath || extractFromOptional,
                                          useOfOptionalPath.optionalParmVarDeclToChange_,
                                          useOfOptionalPath.callExpr_,
                                          context);
    }
}

void Opt2pathOptionalCheck::check(const MatchFinder::MatchResult &Result)
{
    // Note that the Result objects seem to be returned to this
    // function in order of traversal of the AST, and not in order of
    // the calls to finder->addMatcher().
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
    if (const auto *match = Result.Nodes.getNodeAs<CallExpr>("call of optional builder"))
    {
        const Expr* callee = match->getCallee();
        std::string functionName = prettyPrint(callee);
        std::string replacementName = (functionName == "opt2fn_null") ? "opt2path_optional" : "ftp2path_optional";
        diag(callee->getBeginLoc(), "Use " + replacementName + " instead of " + functionName)
            << FixItHint::CreateReplacement(SourceRange(callee->getBeginLoc(), callee->getEndLoc()), replacementName);
    }
    if (const auto *match = Result.Nodes.getNodeAs<DeclRefExpr>("declaration of variable assigned an optional path"))
    {
        diag(match->getLocation(), "Use std::optional<std::filesystem::path>")
            << FixItHint::CreateInsertion(match->getLocation(), "std::optional<std::filesystem::path> ");
    }
    // TODO could this logic be re-used with if statement condition expressions?
    if (const auto *match = Result.Nodes.getNodeAs<ParenExpr>("parenthesized expression to replace"))
    {
        const auto *varDecl = Result.Nodes.getNodeAs<VarDecl>("declaration of potential optional path");
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
    if (const auto *matchingDeclRefExpr = Result.Nodes.getNodeAs<DeclRefExpr>("use of potential optional path as function argument"))
    {
        const auto *varDecl = Result.Nodes.getNodeAs<VarDecl>("declaration of potential optional path");
        const bool convertToPath = Result.Nodes.getNodeAs<IfStmt>("possible if condition means optional has value");
        PossibleUseOfOptionalPath possibleUseOfOptionalPath{
            convertToPath,
            matchingDeclRefExpr,
            /* optionalCompoundStmt */ Result.Nodes.getNodeAs<CompoundStmt>("optional ancestor compound statement"),
            /* optionalParmVarDeclToChange */ Result.Nodes.getNodeAs<ParmVarDecl>("possible function parameter receiving optional path"),
            /* callExpr */ Result.Nodes.getNodeAs<CallExpr>("call expression using potential optional path"),
            /* possibleBinaryOperatorToRefactor */ Result.Nodes.getNodeAs<BinaryOperator>("possible binary operator to refactor")
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
    if (const auto *callExpr = Result.Nodes.getNodeAs<CallExpr>("call expression using optional path from builder"))
    {
        const auto *parmVarDeclToChange = Result.Nodes.getNodeAs<ParmVarDecl>("possible function parameter receiving optional path");
        fprintf(stderr, "Got extra parmVarDeclToChange\n");
        // Then we refactor the function that is called
        const bool convertToPath = false;
        refactorFunctionDeclReceivingPath(convertToPath,
                                          parmVarDeclToChange, callExpr, Result.Context);
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
