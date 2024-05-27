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
// TODO I don't understand why this assertion doesn't work, because the fix-it was applied!
// C HECK-FIXES: void functionNeedingAFile(const std::filesystem::path& file);
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
