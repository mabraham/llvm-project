// RUN: %check_clang_tidy %s modernize-opt2path-optional %t

// Add mocks for compilation to avoid running clang-tidy on the
// whole of #include <filesystem>, etc.
namespace std
{
namespace filesystem
{
class path
{
};
} // namespace filesystem
template <typename T>
class optional
{
};
} // namespace std

// Things that don't trigger messages or refactorings
int *irrelevant_file;
std::filesystem::path correct_in_trajfile, correct_other;
std::optional<std::filesystem::path> correct_optional_file, correct_other_optional;

// Stubs for symbols used in GROMACS code
int *stderr;
void printf(const char *, const char *);
void fprintf(int *, const char *, const char *);
const char *opt2fn_null(int a, int b);
const char *ftp2fn_null(int a, int b);

//#include <cassert>
void __assert_fail();
#define assert(e) ((e) ? (void)0 : __assert_fail())

void functionNeedingFileInner(const char* out);
// CHECK-MESSAGES: :[[@LINE-1]]:31: warning: Change function parameter to const std::filesystem::path& [modernize-opt2path-optional]
// CHECK-FIXES: void functionNeedingFileInner(const std::filesystem::path& out);

void functionNeedingAFile(const char* file)
// CHECK-MESSAGES: :[[@LINE-1]]:27: warning: Change function parameter to const std::filesystem::path& [modernize-opt2path-optional]
// CHECK-FIXES: void functionNeedingAFile(const std::filesystem::path& file)
{
    fprintf(stderr, "Working on file %s\n", file);
    // CHECK-MESSAGES: :[[@LINE-1]]:45: warning: Get C string from std::filesystem::path [modernize-opt2path-optional]
    // CHECK-FIXES: fprintf(stderr, "Working on file %s\n", file.c_str());

    functionNeedingFileInner(file);
}

void functionUsingAssert()
{
    // Model of existing declarations of path variables
    const char* path;
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Don't declare const char* variable that won't be used [modernize-opt2path-optional]
    // CHECK-FIXES: ;
    path = opt2fn_null(1, 2);
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Use std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-MESSAGES: :[[@LINE-2]]:12: warning: Use opt2path_optional instead of opt2fn_null [modernize-opt2path-optional]
    // CHECK-FIXES: std::optional<std::filesystem::path> path = opt2path_optional(1, 2);

    assert(path);
    
    functionNeedingAFile(path);
    // CHECK-MESSAGES: :[[@LINE-1]]:26: warning: Extract std::filesystem::path from std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-FIXES: functionNeedingAFile(path.value());

    fprintf(stderr, "Working after assertion %s\n", path);
    // CHECK-MESSAGES: :[[@LINE-1]]:53: warning: Get C string from std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-FIXES: fprintf(stderr, "Working after assertion %s\n", path.value().c_str());
}

void processOptionalFileInner(const char *filename);
// CHECK-MESSAGES: :[[@LINE-1]]:31: warning: Change function parameter to const std::optional<std::filesystem::path>& [modernize-opt2path-optional]

// Check that function declarations are changed
void functionTakingOptionalFile(int a, const char* thePath)
// CHECK-MESSAGES: :[[@LINE-1]]:40: warning: Change function parameter to const std::optional<std::filesystem::path>& [modernize-opt2path-optional]
// CHECK-FIXES: void functionTakingOptionalFile(int a, const std::optional<std::filesystem::path>& thePath)
{
    // Check that fprintf is handled correctly
    fprintf(stderr, "Working on file %s\n", thePath);
    // CHECK-MESSAGES: :[[@LINE-1]]:45: warning: Get C string from std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-FIXES: fprintf(stderr, "Working on file %s\n", thePath.value().c_str());

    // Call another function
    processOptionalFileInner(thePath);

    // Call another function taking a path after checking a condition
    if (thePath)
    {
        functionNeedingAFile(thePath);
        // CHECK-MESSAGES: :[[@LINE-1]]:30: warning: Extract std::filesystem::path from std::optional<std::filesystem::path> [modernize-opt2path-optional]
        // CHECK-FIXES: functionNeedingAFile(thePath.value());
    }

    // Call another function taking a path after checking a different condition
    if (nullptr != thePath)
    // CHECK-MESSAGES: :[[@LINE-1]]:9: warning: Use std::optional::operator bool() rather than comparison with nullptr [modernize-opt2path-optional]
    // CHECK-FIXES: if (thePath)
    {
        functionNeedingAFile(thePath);
        // CHECK-MESSAGES: :[[@LINE-1]]:30: warning: Extract std::filesystem::path from std::optional<std::filesystem::path> [modernize-opt2path-optional]
        // CHECK-FIXES: functionNeedingAFile(thePath.value());
    }
}

void functionWithCStringParameterTypeNotChanged(const char* fn, const char* message)
// CHECK-MESSAGES: :[[@LINE-1]]:49: warning: Change function parameter to const std::optional<std::filesystem::path>& [modernize-opt2path-optional]
// CHECK-FIXES: void functionWithCStringParameterTypeNotChanged(const std::optional<std::filesystem::path>& fn, const char* message)
{
    processOptionalFileInner(fn);
}

void functionUsingOptionalPath()
{
    // Model of existing declarations of path variables
    const char* in_trajfile;
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Don't declare const char* variable that won't be used [modernize-opt2path-optional]
    // CHECK-FIXES: ;
    in_trajfile = opt2fn_null(1, 2);
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Use std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-MESSAGES: :[[@LINE-2]]:19: warning: Use opt2path_optional instead of opt2fn_null [modernize-opt2path-optional]
    // CHECK-FIXES: std::optional<std::filesystem::path> in_trajfile = opt2path_optional(1, 2);

    // Check printf is handled correctly
    printf("Writing to file %s\n", in_trajfile);
    // CHECK-MESSAGES: :[[@LINE-1]]:36: warning: Get C string from std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-FIXES: printf("Writing to file %s\n", in_trajfile.value().c_str());

    functionTakingOptionalFile(1, in_trajfile);

    // Code that does not trigger the check
    const char* other;

    // Check that usages of optional paths are generally not
    // refactored. Also check that additional parmeters with C-string
    // types are unaffected.
    functionWithCStringParameterTypeNotChanged(in_trajfile, "test");

    // Check that calls to function taking arguments that are returned
    // from builder functions have the builder function calls
    // changed to the new versions.
    functionTakingOptionalFile(3, opt2fn_null(2, 4));
    // CHECK-MESSAGES: :[[@LINE-1]]:35: warning: Use opt2path_optional instead of opt2fn_null [modernize-opt2path-optional]
    // CHECK-FIXES: functionTakingOptionalFile(3, opt2path_optional(2, 4));
}

// Check that ftp2fn_null is handled
void callOfFtp2fn_nullIsRefactored()
{
    const char* ftpfile;
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Don't declare const char* variable that won't be used [modernize-opt2path-optional]
    // CHECK-FIXES: ;
    ftpfile = ftp2fn_null(3, 5);
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Use std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-MESSAGES: :[[@LINE-2]]:15: warning: Use ftp2path_optional instead of ftp2fn_null [modernize-opt2path-optional]
    // CHECK-FIXES: std::optional<std::filesystem::path> ftpfile = ftp2path_optional(3, 5);

    // Check that usages of optional paths behind checks are refactored
    if (ftpfile)
    {
        functionNeedingAFile(ftpfile);
        // CHECK-MESSAGES: :[[@LINE-1]]:30: warning: Extract std::filesystem::path from std::optional<std::filesystem::path> [modernize-opt2path-optional]
        // CHECK-FIXES: functionNeedingAFile(ftpfile.value());
    }

    // Check that usages of optional paths behind checks are refactored
    if (ftpfile != nullptr)
    // CHECK-MESSAGES: :[[@LINE-1]]:9: warning: Use std::optional::operator bool() rather than comparison with nullptr [modernize-opt2path-optional]
    // CHECK-FIXES: if (ftpfile)
    {
        functionNeedingAFile(ftpfile);
        // CHECK-MESSAGES: :[[@LINE-1]]:30: warning: Extract std::filesystem::path from std::optional<std::filesystem::path> [modernize-opt2path-optional]
        // CHECK-FIXES: functionNeedingAFile(ftpfile.value());
    }

    const bool somethingThatIsTrue = true;
    const bool somethingElseThatIsTrue = true;
    // Check that usages of optional paths behind some kinds of more complex checks are refactored
    if (ftpfile && somethingThatIsTrue && somethingElseThatIsTrue)
    {
        functionNeedingAFile(ftpfile);
        // CHECK-MESSAGES: :[[@LINE-1]]:30: warning: Extract std::filesystem::path from std::optional<std::filesystem::path> [modernize-opt2path-optional]
        // CHECK-FIXES: functionNeedingAFile(ftpfile.value());
    }

    // Check that usages of optional paths behind nested conditional checks are refactored
    if (ftpfile)
    {
        if (somethingThatIsTrue)
        {
            functionNeedingAFile(ftpfile);
            // CHECK-MESSAGES: :[[@LINE-1]]:34: warning: Extract std::filesystem::path from std::optional<std::filesystem::path> [modernize-opt2path-optional]
            // CHECK-FIXES: functionNeedingAFile(ftpfile.value());
        }
    }

    const char* in_trajfile;
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Don't declare const char* variable that won't be used [modernize-opt2path-optional]
    // CHECK-FIXES: ;
    in_trajfile = opt2fn_null(1, 2);
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Use std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-MESSAGES: :[[@LINE-2]]:19: warning: Use opt2path_optional instead of opt2fn_null [modernize-opt2path-optional]
    // CHECK-FIXES: std::optional<std::filesystem::path> in_trajfile = opt2path_optional(1, 2);

    // Check that construction of complex boolean variables is refactored
    const bool someBool = (in_trajfile != nullptr) || (nullptr != ftpfile);
    // CHECK-MESSAGES: :[[@LINE-1]]:27: warning: Use std::optional::operator bool() rather than comparison with nullptr [modernize-opt2path-optional]
    // CHECK-MESSAGES: :[[@LINE-2]]:55: warning: Use std::optional::operator bool() rather than comparison with nullptr [modernize-opt2path-optional]
    // CHECK-FIXES: const bool someBool = in_trajfile || ftpfile;
}

void callsTakingNullptrAreRefactored()
{
    // Check that calls to functions taking nullptr arguments to optional<path> arguments are refactored
    functionTakingOptionalFile(1, nullptr);
    // CHECK-MESSAGES: :[[@LINE-1]]:35: warning: Use std::nullopt instead of nullptr [modernize-opt2path-optional]
    // CHECK-FIXES: functionTakingOptionalFile(1, std::nullopt);

    // Check that calls to functions taking nullptr arguments to path arguments are refactored
    functionNeedingAFile(nullptr);
    // CHECK-MESSAGES: :[[@LINE-1]]:26: warning: Use std::filesystem::path{} instead of nullptr [modernize-opt2path-optional]
    // CHECK-FIXES: functionNeedingAFile(std::filesystem::path{});
}

struct StructWithConstructorTakingPath
{
        // TODO do we need the ability to refactor this?
        StructWithConstructorTakingPath(const char *path)
        {
            functionNeedingAFile(path);
        }
};

void functionUsingClassObject()
{
    // Model of existing declarations of path variables
    const char* thePath;
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Don't declare const char* variable that won't be used [modernize-opt2path-optional]
    // CHECK-FIXES: ;
    thePath = opt2fn_null(1, 2);
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Use std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-MESSAGES: :[[@LINE-2]]:15: warning: Use opt2path_optional instead of opt2fn_null [modernize-opt2path-optional]
    // CHECK-FIXES: std::optional<std::filesystem::path> thePath = opt2path_optional(1, 2);

    StructWithConstructorTakingPath obj(thePath);
}

// Check that function declarations are changed
void anotherFunctionTakingOptionalFile(int a, const char* thePath)
// CHECK-MESSAGES: :[[@LINE-1]]:47: warning: Change function parameter to const std::optional<std::filesystem::path>& [modernize-opt2path-optional]
// CHECK-FIXES: void anotherFunctionTakingOptionalFile(int a, const std::optional<std::filesystem::path>& thePath)
{
    // Check that fprintf is handled correctly
    fprintf(stderr, "Working on file %s\n", thePath);
    // CHECK-MESSAGES: :[[@LINE-1]]:45: warning: Get C string from std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-FIXES: fprintf(stderr, "Working on file %s\n", thePath.value().c_str());

    assert(thePath);

    functionNeedingAFile(thePath);
    // CHECK-MESSAGES: :[[@LINE-1]]:26: warning: Extract std::filesystem::path from std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-FIXES: functionNeedingAFile(thePath.value());
}

void functionUsingReturnValueFromBuilderAsArgument()
{
    // Check that calls to function taking arguments that are returned
    // from builder functions have the builder function calls
    // changed to the new versions.
    anotherFunctionTakingOptionalFile(3, opt2fn_null(2, 4));
    // CHECK-MESSAGES: :[[@LINE-1]]:42: warning: Use opt2path_optional instead of opt2fn_null [modernize-opt2path-optional]
    // CHECK-FIXES: anotherFunctionTakingOptionalFile(3, opt2path_optional(2, 4));
}
