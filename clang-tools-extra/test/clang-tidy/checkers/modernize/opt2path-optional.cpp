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

void processFileInner(const char *filename);
// CHECK-MESSAGES: :[[@LINE-1]]:23: warning: Change function parameter to const std::optional<std::filesystem::path>& [modernize-opt2path-optional]

// Check that function declarations are changed
void functionTakingOptionalFile(int a, const char* in_trajfile)
// CHECK-MESSAGES: :[[@LINE-1]]:40: warning: Change function parameter to const std::optional<std::filesystem::path>& [modernize-opt2path-optional]
// CHECK-FIXES: void functionTakingOptionalFile(int a, const std::optional<std::filesystem::path>& in_trajfile)
{
    fprintf(stderr, "Working on file %s\n", in_trajfile);
    // CHECK-MESSAGES: :[[@LINE-1]]:45: warning: Get C string from std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-FIXES: fprintf(stderr, "Working on file %s\n", in_trajfile.value().c_str());
}

void functionCallingNestedFunction(int a, const char* in_trajfile)
// CHECK-MESSAGES: :[[@LINE-1]]:43: warning: Change function parameter to const std::optional<std::filesystem::path>& [modernize-opt2path-optional]
// CHECK-FIXES: void functionCallingNestedFunction(int a, const std::optional<std::filesystem::path>& in_trajfile)
{
    processFileInner(in_trajfile);
}

void functionWithCStringParameterTypeNotChanged(const char* fn, const char* message)
// CHECK-MESSAGES: :[[@LINE-1]]:49: warning: Change function parameter to const std::optional<std::filesystem::path>& [modernize-opt2path-optional]
// CHECK-FIXES: void functionWithCStringParameterTypeNotChanged(const std::optional<std::filesystem::path>& fn, const char* message)
{
    processFileInner(fn);
}

void handleNonOptionalFile(const char* file)
// CHECK-MESSAGES: :[[@LINE-1]]:28: warning: Change function parameter to const std::filesystem::path& [modernize-opt2path-optional]
// CHECK-FIXES: void handleNonOptionalFile(const std::filesystem::path& file)
{
    fprintf(stderr, "Working on file %s\n", file);
    // CHECK-MESSAGES: :[[@LINE-1]]:45: warning: Get C string from std::filesystem::path [modernize-opt2path-optional]
    // CHECK-FIXES: fprintf(stderr, "Working on file %s\n", file.c_str());

    // TODO I don't know how to handle the case where the optional and
    // non-optional paths both call the same inner function. Maybe it
    // depends on a preliminary refactoring to remove the ambiguity?
    // Or an assertion?

    // Note that this constructs a new optional<path> from
    // a path, which is an inefficiency that could warrant later refactoring
    //processFileInner(file);
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

    // Check that ftp2fn_null is handled
    const char* ftpfile;
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Don't declare const char* variable that won't be used [modernize-opt2path-optional]
    // CHECK-FIXES: ;
    ftpfile = ftp2fn_null(3, 5);
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Use std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-MESSAGES: :[[@LINE-2]]:15: warning: Use ftp2path_optional instead of ftp2fn_null [modernize-opt2path-optional]
    // CHECK-FIXES: std::optional<std::filesystem::path> ftpfile = ftp2path_optional(3, 5);

    // Code that does not trigger the check
    const char* other;

    // Check fprintf and friends work, assuming that existing logic
    // ensures that the optional path is valid.
    fprintf(stderr, "Writing to file %s\n", in_trajfile);
    // CHECK-MESSAGES: :[[@LINE-1]]:45: warning: Get C string from std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-FIXES: fprintf(stderr, "Writing to file %s\n", in_trajfile.value().c_str());

    printf("Writing to file %s\n", in_trajfile);
    // CHECK-MESSAGES: :[[@LINE-1]]:36: warning: Get C string from std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-FIXES: printf("Writing to file %s\n", in_trajfile.value().c_str());

    // TODO also check multiple calls to fprintf

    // Check that calls to functions are also refactored
    functionTakingOptionalFile(1, in_trajfile);
     
    // Check that calls to functions taking nullptr arguments are refactored
    functionTakingOptionalFile(1, nullptr);
    // CHECK-MESSAGES: :[[@LINE-1]]:35: warning: Use std::nullopt instead of nullptr [modernize-opt2path-optional]
    // CHECK-FIXES: functionTakingOptionalFile(1, std::nullopt);

    // Check that functions that call local functions have the whole chain refactored
    functionCallingNestedFunction(3, ftpfile);

    // Check that calls to function taking arguments that are returned
    // from builder functions have the builder function calls
    // refactored. Also check that additional parmeters with C-string
    // types are unaffected.
    functionWithCStringParameterTypeNotChanged(opt2fn_null(2, 4), "hello world");
    // CHECK-MESSAGES: :[[@LINE-1]]:48: warning: Use opt2path_optional instead of opt2fn_null [modernize-opt2path-optional]
    // CHECK-FIXES: functionWithCStringParameterTypeNotChanged(opt2path_optional(2, 4), "hello world");

    // Check that usages of optional paths are generally not refactored
    functionWithCStringParameterTypeNotChanged(in_trajfile, "test");

    // Check that usages of optional paths behind checks are refactored
    if (ftpfile)
    {
        handleNonOptionalFile(ftpfile);
        // CHECK-MESSAGES: :[[@LINE-1]]:31: warning: Extract std::filesystem::path from std::optional<std::filesystem::path> [modernize-opt2path-optional]
        // CHECK-FIXES: handleNonOptionalFile(ftpfile.value());
    }

    // Check that usages of optional paths behind checks are refactored
    if (in_trajfile != nullptr)
    // CHECK-MESSAGES: :[[@LINE-1]]:9: warning: Use std::optional::operator bool() rather than comparison with nullptr [modernize-opt2path-optional]
    // CHECK-FIXES: if (in_trajfile)
    {
        handleNonOptionalFile(in_trajfile);
        // CHECK-MESSAGES: :[[@LINE-1]]:31: warning: Extract std::filesystem::path from std::optional<std::filesystem::path> [modernize-opt2path-optional]
        // CHECK-FIXES: handleNonOptionalFile(in_trajfile.value());
    }

    // Check that usages of optional paths behind checks are refactored
    if (nullptr != in_trajfile)
    // CHECK-MESSAGES: :[[@LINE-1]]:9: warning: Use std::optional::operator bool() rather than comparison with nullptr [modernize-opt2path-optional]
    // CHECK-FIXES: if (in_trajfile)
    {
        handleNonOptionalFile(in_trajfile);
        // CHECK-MESSAGES: :[[@LINE-1]]:31: warning: Extract std::filesystem::path from std::optional<std::filesystem::path> [modernize-opt2path-optional]
        // CHECK-FIXES: handleNonOptionalFile(in_trajfile.value());
    }
    
    // Check that usages of optional paths behind checks are refactored
    if (in_trajfile && true)
    {
        handleNonOptionalFile(in_trajfile);
        // CHECK-MESSAGES: :[[@LINE-1]]:31: warning: Extract std::filesystem::path from std::optional<std::filesystem::path> [modernize-opt2path-optional]
        // CHECK-FIXES: handleNonOptionalFile(in_trajfile.value());
    }

    // Check that usages of optional paths behind nested conditional checks are refactored
    if (in_trajfile)
    {
        if (true)
        {
            handleNonOptionalFile(in_trajfile);
            // CHECK-MESSAGES: :[[@LINE-1]]:35: warning: Extract std::filesystem::path from std::optional<std::filesystem::path> [modernize-opt2path-optional]
            // CHECK-FIXES: handleNonOptionalFile(in_trajfile.value());
        }
    }

    // Check that construction of complex boolean variables is refactored
    const bool someBool = (in_trajfile != nullptr) || (nullptr != ftpfile);
    // CHECK-MESSAGES: :[[@LINE-1]]:27: warning: Use std::optional::operator bool() rather than comparison with nullptr [modernize-opt2path-optional]
    // CHECK-MESSAGES: :[[@LINE-2]]:55: warning: Use std::optional::operator bool() rather than comparison with nullptr [modernize-opt2path-optional]
    // CHECK-FIXES: const bool someBool = in_trajfile || ftpfile;
}

struct StructWithConstructorTakingPath
{
        // TODO should this get refactored?
        StructWithConstructorTakingPath(const char *path)
        {
            processFileInner(path);
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

void functionNeedingFile(const char* file);

void functionUsingAssert()
{
    // Model of existing declarations of path variables
    const char* in_trajfile;
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Don't declare const char* variable that won't be used [modernize-opt2path-optional]
    // CHECK-FIXES: ;
    in_trajfile = opt2fn_null(1, 2);
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Use std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-MESSAGES: :[[@LINE-2]]:19: warning: Use opt2path_optional instead of opt2fn_null [modernize-opt2path-optional]
    // CHECK-FIXES: std::optional<std::filesystem::path> in_trajfile = opt2path_optional(1, 2);

    assert(in_trajfile);
    
    functionNeedingFile(in_trajfile);
}
