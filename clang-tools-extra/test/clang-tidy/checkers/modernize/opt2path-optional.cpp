// RUN: %check_clang_tidy %s modernize-opt2path-optional %t

// Add a mock for compilation
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

const char *opt2fn_null(int a, int b);

void f()
{
    // Trigger the check
    const char* in_trajfile;
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Don't declare const char* variable that won't be used [modernize-opt2path-optional]
    // C HECK-FIXES: 
    in_trajfile = opt2fn_null(1, 2);
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Use std::optional<std::filesystem::path> [modernize-opt2path-optional]
    // CHECK-MESSAGES: :[[@LINE-2]]:19: warning: Use opt2path_optional instead of opt2fn_null [modernize-opt2path-optional]
    // CHECK-FIXES: std::optional<std::filesystem::path> in_trajfile = opt2path_optional(1, 2);

    // Code that does not trigger the check
    const char* other;

}

// Add things that don't trigger the check here.
int *irrelevant_file;
std::filesystem::path correct_in_trajfile, correct_other;
std::optional<std::filesystem::path> correct_optional_file, correct_other_optional;
