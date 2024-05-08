// RUN: %check_clang_tidy -std=c++17-or-later %s modernize-C-string-to-std-filename %t

// Add a mock for compilation
namespace std
{
namespace filesystem
{
class path
{
};
} // namespace filesystem
class optional
{
};
} // namespace std

const char *opt2fn_null(int a, int b);

void f()
{
    // FIXME: Add something that triggers the check here.
    const char* in_trajfile;
    // C HECK-FIXES: 
    const char* other;
    // C HECK-MESSAGES: :[[@LINE-1]]:4: warning: Found C-string file variable declaration to remove [modernize-C-string-to-std-filename]

    // FIXME: Verify the applied fix.
    //   * Make the CHECK patterns specific enough and try to make verified lines
    //     unique to avoid incorrect matches.
    //   * Use {{}} for regular expressions.
    // example C HECK-FIXES: {{^}}std::filesystem::path in_trajfile;{{$}}

    in_trajfile = opt2fn_null(1, 2);
    // CHECK-MESSAGES: :[[@LINE-1]]:5: warning: Found opt2fn_null usage to change [modernize-C-string-to-std-filename]
    // CHECK-FIXES: std::optional<std::filesystem::path> in_trajfile = opt2path_optional(1, 2);
}

// FIXME: Add something that doesn't trigger the check here.
int *irrelevant_file;
std::filesystem::path correct_in_trajfile, correct_other;
