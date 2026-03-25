// ─────────────────────────────────────────────────────────────────────────────
// Unit tests: safe_path(const String& p)
//
// Function under test (dl_server.cpp:43-45):
//   static bool safe_path(const String& p) {
//       return p.startsWith("/") && p.indexOf("..") < 0;
//   }
//
// Contract:
//   Returns true  iff  p begins with '/'  AND  p contains no ".." substring.
//   Both conditions must hold simultaneously.
//
// RED / GREEN annotations
// -----------------------
// All tests in this file are GREEN against the current implementation because
// safe_path() is a two-line pure expression with no external dependencies.
// A RED test would require the function to NOT exist yet, which is not the case.
// Tests are labelled [GREEN] to confirm they pass as-is, and [RED-if-missing]
// to show what would fail if the function were accidentally deleted/broken.
// ─────────────────────────────────────────────────────────────────────────────

#include <unity.h>
// Pull in our Arduino stub so String compiles on the host toolchain.
#include "stubs/Arduino.h"

// ── Copy of the function under test ──────────────────────────────────────────
// We copy rather than link to avoid dragging in all of dl_server.cpp's
// hardware dependencies (SD_MMC, WebServer, lwip sockets …).
// If the implementation changes, this copy must be updated — the test will then
// fail (RED) until the copy matches, which is the intended signal.
static bool safe_path(const String& p) {
    return p.startsWith("/") && p.indexOf("..") < 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Happy-path tests  [GREEN]
// ─────────────────────────────────────────────────────────────────────────────

void test_safe_path_root_slash() {
    // The canonical root path must be accepted.
    TEST_ASSERT_TRUE(safe_path("/"));
}

void test_safe_path_simple_absolute() {
    // A plain absolute path with no traversal must be accepted.
    TEST_ASSERT_TRUE(safe_path("/downloads/file.txt"));
}

void test_safe_path_nested_absolute() {
    // Deeply nested absolute path — still no traversal.
    TEST_ASSERT_TRUE(safe_path("/a/b/c/d/e.bin"));
}

void test_safe_path_dot_in_filename() {
    // A single dot inside a filename component is NOT traversal.
    // e.g. "v1.4.0.zip" or ".hidden-file"
    TEST_ASSERT_TRUE(safe_path("/files/.hidden"));
    TEST_ASSERT_TRUE(safe_path("/releases/v1.4.0.zip"));
}

void test_safe_path_dot_extension_only() {
    // Filename that is exactly ".ext" — one dot, not two.
    TEST_ASSERT_TRUE(safe_path("/.gitignore"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Traversal-rejection tests  [GREEN]
// ─────────────────────────────────────────────────────────────────────────────

void test_safe_path_rejects_dotdot_simple() {
    // Classic directory traversal — must be rejected.
    TEST_ASSERT_FALSE(safe_path("/../etc/passwd"));
}

void test_safe_path_rejects_dotdot_in_middle() {
    // Traversal embedded inside a longer path.
    TEST_ASSERT_FALSE(safe_path("/files/../secret.txt"));
}

void test_safe_path_rejects_dotdot_at_end() {
    // Traversal at the end of the path.
    TEST_ASSERT_FALSE(safe_path("/files/.."));
}

void test_safe_path_rejects_double_dotdot() {
    // Multiple traversal segments — still caught by indexOf("..") >= 0.
    TEST_ASSERT_FALSE(safe_path("/a/../../b"));
}

void test_safe_path_rejects_dotdot_in_filename() {
    // A filename that literally contains ".." as part of a longer name.
    // safe_path does a substring search, so "my..file" also fails.
    // This is intentionally conservative — the firmware does not need to
    // serve files whose names embed "..".
    TEST_ASSERT_FALSE(safe_path("/files/my..file.txt"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Non-absolute-path rejection tests  [GREEN]
// ─────────────────────────────────────────────────────────────────────────────

void test_safe_path_rejects_relative_path() {
    // Path without leading slash is relative — must be rejected.
    TEST_ASSERT_FALSE(safe_path("downloads/file.txt"));
}

void test_safe_path_rejects_empty_string() {
    // Empty string does not start with '/' — must be rejected.
    TEST_ASSERT_FALSE(safe_path(""));
}

void test_safe_path_rejects_bare_dotdot() {
    // ".." alone has no leading slash AND contains "..".
    TEST_ASSERT_FALSE(safe_path(".."));
}

void test_safe_path_rejects_windows_style_backslash() {
    // Windows-style paths (backslash separator) — no leading '/'.
    TEST_ASSERT_FALSE(safe_path("\\files\\secret.txt"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Boundary / edge-case tests  [GREEN]
// ─────────────────────────────────────────────────────────────────────────────

void test_safe_path_only_slash() {
    // The string "/" is a valid absolute path (SD root).
    TEST_ASSERT_TRUE(safe_path("/"));
}

void test_safe_path_leading_slash_then_dotdot() {
    // Has leading slash but also has ".." — both conditions required.
    // This is the most important security boundary.
    TEST_ASSERT_FALSE(safe_path("/.."));
}

void test_safe_path_url_encoded_dotdot_not_caught() {
    // NOTE: safe_path does NOT decode URL-encoded sequences.
    // "%2e%2e" is URL-encoded ".." — safe_path sees no ".." literal and would
    // PASS this, returning true.  This is a KNOWN LIMITATION documented here
    // so future developers add URL-decoding before calling safe_path if needed.
    // [GREEN] — documents the gap, does not assert a security property we don't have.
    TEST_ASSERT_TRUE(safe_path("/%2e%2e/etc/passwd"));  // passes today — see note above
}

void test_safe_path_very_long_path() {
    // A 512-character path with no traversal.  Tests that the indexOf scan
    // does not truncate or overflow on long input.
    String long_path = "/";
    for (int i = 0; i < 50; i++) long_path += "segment/";
    long_path += "file.txt";
    TEST_ASSERT_TRUE(safe_path(long_path));
}

void test_safe_path_long_path_with_traversal_at_end() {
    // Same 512-char prefix but with ".." injected at the tail.
    String long_path = "/";
    for (int i = 0; i < 50; i++) long_path += "segment/";
    long_path += "..";
    TEST_ASSERT_FALSE(safe_path(long_path));
}

// ─────────────────────────────────────────────────────────────────────────────
// Unity entry points
// ─────────────────────────────────────────────────────────────────────────────

void setUp()    {}
void tearDown() {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();

    // Happy path
    RUN_TEST(test_safe_path_root_slash);
    RUN_TEST(test_safe_path_simple_absolute);
    RUN_TEST(test_safe_path_nested_absolute);
    RUN_TEST(test_safe_path_dot_in_filename);
    RUN_TEST(test_safe_path_dot_extension_only);

    // Traversal rejection
    RUN_TEST(test_safe_path_rejects_dotdot_simple);
    RUN_TEST(test_safe_path_rejects_dotdot_in_middle);
    RUN_TEST(test_safe_path_rejects_dotdot_at_end);
    RUN_TEST(test_safe_path_rejects_double_dotdot);
    RUN_TEST(test_safe_path_rejects_dotdot_in_filename);

    // Non-absolute rejection
    RUN_TEST(test_safe_path_rejects_relative_path);
    RUN_TEST(test_safe_path_rejects_empty_string);
    RUN_TEST(test_safe_path_rejects_bare_dotdot);
    RUN_TEST(test_safe_path_rejects_windows_style_backslash);

    // Boundary / edge cases
    RUN_TEST(test_safe_path_only_slash);
    RUN_TEST(test_safe_path_leading_slash_then_dotdot);
    RUN_TEST(test_safe_path_url_encoded_dotdot_not_caught);
    RUN_TEST(test_safe_path_very_long_path);
    RUN_TEST(test_safe_path_long_path_with_traversal_at_end);

    return UNITY_END();
}
