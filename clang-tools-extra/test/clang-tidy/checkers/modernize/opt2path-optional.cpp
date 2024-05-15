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

void processFile(int a, const char *filename)
// CHECK-MESSAGES: :[[@LINE-1]]:25: warning: Change function parameter to const std::optional<std::filesystem::path>& [modernize-opt2path-optional]
// CHECK-FIXES: void processFile(int a, const std::optional<std::filesystem::path>& filename)
{
    fprintf(stderr, "Working on file %s\n", filename);
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

    processFile(1, in_trajfile);
     }

// Add things that don't trigger the check here.
int *irrelevant_file;
std::filesystem::path correct_in_trajfile, correct_other;
std::optional<std::filesystem::path> correct_optional_file, correct_other_optional;
