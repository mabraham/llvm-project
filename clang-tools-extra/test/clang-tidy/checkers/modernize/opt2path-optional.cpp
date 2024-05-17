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

int *stderr;
void fprintf(int *, const char *, const char *);

void processFileInner(const char *filename);
// CHECK-MESSAGES: :[[@LINE-1]]:23: warning: Change function parameter to const std::optional<std::filesystem::path>& [modernize-opt2path-optional]

void processOptionalFile(int a, const char* in_trajfile)
// CHECK-MESSAGES: :[[@LINE-1]]:33: warning: Change function parameter to const std::optional<std::filesystem::path>& [modernize-opt2path-optional]
// CHECK-FIXES: void processOptionalFile(int a, const std::optional<std::filesystem::path>& in_trajfile)
{
    fprintf(stderr, "Working on file %s\n", in_trajfile);
    // CHECK-MESSAGES: :[[@LINE-1]]:45: warning: Get C string from std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-FIXES: fprintf(stderr, "Working on file %s\n", in_trajfile.value().string().c_str());

    processFileInner(in_trajfile);
}

void handleFile(const char* file, const char* message)
// CHECK-MESSAGES: :[[@LINE-1]]:17: warning: Change function parameter to const std::filesystem::path& [modernize-opt2path-optional]
// CHECK-FIXES: void handleFile(const std::filesystem::path& file, const char* message)
{
    // TODO I don't know how to handle the case where the optional and
    // non-optional paths both call the same inner function. Maybe it
    // depends on a preliminary refactoring to remove the ambiguity?

    // Note that this constructs a new optional<path> from
    // a path, which is an inefficiency that could warrant later refactoring
    //processFileInner(file);
}

const char *opt2fn_null(int a, int b);

void f()
{
    // Trigger the check
    const char* in_trajfile;
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Don't declare const char* variable that won't be used [modernize-opt2path-optional]
    // CHECK-FIXES: ;
    in_trajfile = opt2fn_null(1, 2);
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Use std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-MESSAGES: :[[@LINE-2]]:19: warning: Use opt2path_optional instead of opt2fn_null [modernize-opt2path-optional]
    // CHECK-FIXES: std::optional<std::filesystem::path> in_trajfile = opt2path_optional(1, 2);

    // Code that does not trigger the check
    const char* other;

    fprintf(stderr, "Writing to file %s\n", in_trajfile);
    // CHECK-MESSAGES: :[[@LINE-1]]:45: warning: Get C string from std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-FIXES: fprintf(stderr, "Writing to file %s\n", in_trajfile.value().string().c_str());

    // TODO also check multiple calls to fprintf

    // Check that calls to functions are also refactored
    processOptionalFile(1, in_trajfile);
    
    processOptionalFile(3, opt2fn_null(2, 4));
    // CHECK-MESSAGES: :[[@LINE-1]]:28: warning: Use opt2path_optional instead of opt2fn_null [modernize-opt2path-optional]
    // CHECK-FIXES: processOptionalFile(3, opt2path_optional(2, 4));

    if (in_trajfile)
    {
        handleFile(in_trajfile, "test");
        // CHECK-MESSAGES: :[[@LINE-1]]:20: warning: Extract std::filesystem::path from std::optional<std::filesystem::path> [modernize-opt2path-optional]
        // CHECK-FIXES: handleFile(in_trajfile.value(), "test");
    }

    if (in_trajfile != nullptr)
    {
        handleFile(in_trajfile, "test");
        // CHECK-MESSAGES: :[[@LINE-1]]:20: warning: Extract std::filesystem::path from std::optional<std::filesystem::path> [modernize-opt2path-optional]
        // CHECK-FIXES: handleFile(in_trajfile.value(), "test");
    }
}

// Add things that don't trigger the check here.
int *irrelevant_file;
std::filesystem::path correct_in_trajfile, correct_other;
std::optional<std::filesystem::path> correct_optional_file, correct_other_optional;
