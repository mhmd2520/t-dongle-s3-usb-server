// ─────────────────────────────────────────────────────────────────────────────
// Unit tests: is_system_entry(const String& name)
//
// Function under test (dl_server.cpp:48-59):
//   static bool is_system_entry(const String& name) {
//       if (name.isEmpty()) return true;
//       if (name[0] == '$') return true;
//       if (name.equalsIgnoreCase("System Volume Information")) return true;
//       if (name.startsWith("._"))  return true;
//       if (name.equalsIgnoreCase(".Trashes"))        return true;
//       if (name.equalsIgnoreCase(".Spotlight-V100")) return true;
//       if (name.equalsIgnoreCase(".fseventsd"))      return true;
//       if (name == "_dl_tmp.zip")   return true;
//       if (name == "_fileman.html") return true;
//       return false;
//   }
//
// Contract:
//   Returns true for names that must never be shown to users or included in
//   ZIP archives.  Returns false for all normal user files.
//
// RED / GREEN annotations
// -----------------------
// [GREEN] — passes against current implementation (logic already exists).
// [RED]   — would FAIL before implementation because the guard is absent.
//           Marked [RED] here to show what a pre-implementation run would see.
//           In practice every [RED] test below would fail with "Expected TRUE
//           Was FALSE" (for hidden entries) or the reverse.
// ─────────────────────────────────────────────────────────────────────────────

#include <unity.h>
#include "stubs/Arduino.h"

// ── Copy of the function under test ──────────────────────────────────────────
static bool is_system_entry(const String& name) {
    if (name.isEmpty()) return true;
    if (name[0] == '$') return true;
    if (name.equalsIgnoreCase("System Volume Information")) return true;
    if (name.startsWith("._"))  return true;
    if (name.equalsIgnoreCase(".Trashes"))        return true;
    if (name.equalsIgnoreCase(".Spotlight-V100")) return true;
    if (name.equalsIgnoreCase(".fseventsd"))      return true;
    if (name == "_dl_tmp.zip")   return true;
    if (name == "_fileman.html") return true;
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Normal user files — must NOT be filtered  [GREEN]
// ─────────────────────────────────────────────────────────────────────────────

void test_normal_filename_not_filtered() {
    TEST_ASSERT_FALSE(is_system_entry("document.pdf"));
}

void test_normal_zip_not_filtered() {
    // A user ZIP that does not match the internal temp-file name.
    TEST_ASSERT_FALSE(is_system_entry("backup.zip"));
}

void test_normal_html_not_filtered() {
    // A user HTML file that does not match the internal cache name.
    TEST_ASSERT_FALSE(is_system_entry("index.html"));
}

void test_normal_underscore_prefix_not_filtered() {
    // Leading underscore is fine as long as the full name is not a reserved one.
    TEST_ASSERT_FALSE(is_system_entry("_readme.txt"));
    TEST_ASSERT_FALSE(is_system_entry("_data.csv"));
}

void test_normal_dot_prefix_not_filtered() {
    // A single leading dot (not "._") is a hidden file but NOT a macOS resource fork.
    TEST_ASSERT_FALSE(is_system_entry(".gitignore"));
    TEST_ASSERT_FALSE(is_system_entry(".env"));
}

void test_normal_mixed_case_filename() {
    TEST_ASSERT_FALSE(is_system_entry("MyFile.TXT"));
    TEST_ASSERT_FALSE(is_system_entry("Photo_001.JPG"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Empty string  [GREEN — was RED before empty-check was added]
// ─────────────────────────────────────────────────────────────────────────────

void test_empty_string_is_system() {
    // An empty name is invalid — must be filtered.
    TEST_ASSERT_TRUE(is_system_entry(""));
}

// ─────────────────────────────────────────────────────────────────────────────
// Dollar-sign prefix (Windows system entries)  [GREEN — RED before '$' guard]
// ─────────────────────────────────────────────────────────────────────────────

void test_dollar_recycle_bin() {
    TEST_ASSERT_TRUE(is_system_entry("$RECYCLE.BIN"));
}

void test_dollar_mft() {
    TEST_ASSERT_TRUE(is_system_entry("$MFT"));
}

void test_dollar_single_char() {
    // Any name starting with '$' is filtered, even a 1-character one.
    TEST_ASSERT_TRUE(is_system_entry("$"));
}

void test_dollar_lowercase() {
    // The '$' check uses name[0] == '$' — case-sensitive on the prefix.
    // '$' is not a letter so there is no lower-case variant; this confirms
    // the char comparison works correctly.
    TEST_ASSERT_TRUE(is_system_entry("$extend"));
}

// ─────────────────────────────────────────────────────────────────────────────
// "System Volume Information" (case-insensitive)  [GREEN — RED before this guard]
// ─────────────────────────────────────────────────────────────────────────────

void test_system_volume_information_exact() {
    TEST_ASSERT_TRUE(is_system_entry("System Volume Information"));
}

void test_system_volume_information_upper() {
    TEST_ASSERT_TRUE(is_system_entry("SYSTEM VOLUME INFORMATION"));
}

void test_system_volume_information_lower() {
    TEST_ASSERT_TRUE(is_system_entry("system volume information"));
}

void test_system_volume_information_mixed() {
    TEST_ASSERT_TRUE(is_system_entry("System volume Information"));
}

void test_system_volume_information_partial_not_filtered() {
    // A name that contains but does not equal "System Volume Information"
    // must NOT be filtered — equalsIgnoreCase tests the whole name.
    TEST_ASSERT_FALSE(is_system_entry("System Volume Information Extra"));
    TEST_ASSERT_FALSE(is_system_entry("XSystem Volume Information"));
}

// ─────────────────────────────────────────────────────────────────────────────
// "._" macOS resource-fork prefix  [GREEN — RED before startsWith("._") guard]
// ─────────────────────────────────────────────────────────────────────────────

void test_macos_resource_fork_typical() {
    TEST_ASSERT_TRUE(is_system_entry("._myfile.pdf"));
}

void test_macos_resource_fork_short() {
    // Shortest possible: exactly "._"
    TEST_ASSERT_TRUE(is_system_entry("._"));
}

void test_macos_resource_fork_underscore_only() {
    // "._" is the prefix — any trailing characters also match.
    TEST_ASSERT_TRUE(is_system_entry("._DS_Store_variant"));
}

void test_dot_alone_not_resource_fork() {
    // A single dot is not "._".
    TEST_ASSERT_FALSE(is_system_entry("."));
}

void test_dot_then_x_not_resource_fork() {
    // ".x" does not start with "._".
    TEST_ASSERT_FALSE(is_system_entry(".x"));
}

// ─────────────────────────────────────────────────────────────────────────────
// ".Trashes"  [GREEN — RED before .Trashes guard]
// ─────────────────────────────────────────────────────────────────────────────

void test_trashes_exact() {
    TEST_ASSERT_TRUE(is_system_entry(".Trashes"));
}

void test_trashes_all_lower() {
    TEST_ASSERT_TRUE(is_system_entry(".trashes"));
}

void test_trashes_all_upper() {
    TEST_ASSERT_TRUE(is_system_entry(".TRASHES"));
}

void test_trashes_partial_not_filtered() {
    // Must be an exact (case-insensitive) match — not a prefix match.
    TEST_ASSERT_FALSE(is_system_entry(".Trashes_old"));
    TEST_ASSERT_FALSE(is_system_entry("X.Trashes"));
}

// ─────────────────────────────────────────────────────────────────────────────
// ".Spotlight-V100"  [GREEN — RED before .Spotlight-V100 guard]
// ─────────────────────────────────────────────────────────────────────────────

void test_spotlight_exact() {
    TEST_ASSERT_TRUE(is_system_entry(".Spotlight-V100"));
}

void test_spotlight_lower() {
    TEST_ASSERT_TRUE(is_system_entry(".spotlight-v100"));
}

void test_spotlight_upper() {
    TEST_ASSERT_TRUE(is_system_entry(".SPOTLIGHT-V100"));
}

void test_spotlight_partial_not_filtered() {
    TEST_ASSERT_FALSE(is_system_entry(".Spotlight-V100-backup"));
}

// ─────────────────────────────────────────────────────────────────────────────
// ".fseventsd"  [GREEN — RED before .fseventsd guard]
// ─────────────────────────────────────────────────────────────────────────────

void test_fseventsd_exact() {
    TEST_ASSERT_TRUE(is_system_entry(".fseventsd"));
}

void test_fseventsd_upper() {
    TEST_ASSERT_TRUE(is_system_entry(".FSEVENTSD"));
}

void test_fseventsd_mixed() {
    TEST_ASSERT_TRUE(is_system_entry(".Fseventsd"));
}

void test_fseventsd_partial_not_filtered() {
    TEST_ASSERT_FALSE(is_system_entry(".fseventsd_old"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal temp/cache files — exact case match  [GREEN — RED before these guards]
// ─────────────────────────────────────────────────────────────────────────────

void test_dl_tmp_zip_filtered() {
    // The ZIP temp file used during folder download must never appear to users.
    TEST_ASSERT_TRUE(is_system_entry("_dl_tmp.zip"));
}

void test_dl_tmp_zip_case_mismatch_not_filtered() {
    // The check is == (case-sensitive), matching the exact constant.
    // "_DL_TMP.ZIP" is NOT the temp file name — it would pass through.
    // This is intentional: FAT32 preserves case on ESP32 IDF 5.x.
    TEST_ASSERT_FALSE(is_system_entry("_DL_TMP.ZIP"));
    TEST_ASSERT_FALSE(is_system_entry("_dl_tmp.ZIP"));
}

void test_fileman_html_filtered() {
    // The SD-cached file manager HTML must not appear in listings.
    TEST_ASSERT_TRUE(is_system_entry("_fileman.html"));
}

void test_fileman_html_case_mismatch_not_filtered() {
    // Case-sensitive check — uppercase variant is NOT the reserved name.
    TEST_ASSERT_FALSE(is_system_entry("_FILEMAN.HTML"));
    TEST_ASSERT_FALSE(is_system_entry("_fileman.HTML"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Boundary / stress tests  [GREEN]
// ─────────────────────────────────────────────────────────────────────────────

void test_single_char_normal_name() {
    // A one-character name that is not '$' — must NOT be filtered.
    TEST_ASSERT_FALSE(is_system_entry("a"));
    TEST_ASSERT_FALSE(is_system_entry("Z"));
    TEST_ASSERT_FALSE(is_system_entry("1"));
}

void test_very_long_normal_name() {
    // 200-character filename with no special prefix — must pass through.
    String long_name = "a";
    for (int i = 0; i < 199; i++) long_name += "b";
    TEST_ASSERT_FALSE(is_system_entry(long_name));
}

void test_dotdot_not_filtered_by_is_system_entry() {
    // ".." is not in the system-entry list — safe_path() handles traversal.
    // is_system_entry should NOT filter it; that would mask the traversal guard.
    // [GREEN] — confirms the responsibilities are correctly separated.
    TEST_ASSERT_FALSE(is_system_entry(".."));
}

void test_dot_not_filtered() {
    // "." (current dir) is filtered by the caller (handle_list checks
    // name == "." explicitly) — is_system_entry itself does not filter it.
    TEST_ASSERT_FALSE(is_system_entry("."));
}

// ─────────────────────────────────────────────────────────────────────────────
// Unity entry points
// ─────────────────────────────────────────────────────────────────────────────

void setUp()    {}
void tearDown() {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();

    // Normal files
    RUN_TEST(test_normal_filename_not_filtered);
    RUN_TEST(test_normal_zip_not_filtered);
    RUN_TEST(test_normal_html_not_filtered);
    RUN_TEST(test_normal_underscore_prefix_not_filtered);
    RUN_TEST(test_normal_dot_prefix_not_filtered);
    RUN_TEST(test_normal_mixed_case_filename);

    // Empty string
    RUN_TEST(test_empty_string_is_system);

    // '$' prefix
    RUN_TEST(test_dollar_recycle_bin);
    RUN_TEST(test_dollar_mft);
    RUN_TEST(test_dollar_single_char);
    RUN_TEST(test_dollar_lowercase);

    // System Volume Information
    RUN_TEST(test_system_volume_information_exact);
    RUN_TEST(test_system_volume_information_upper);
    RUN_TEST(test_system_volume_information_lower);
    RUN_TEST(test_system_volume_information_mixed);
    RUN_TEST(test_system_volume_information_partial_not_filtered);

    // "._" prefix
    RUN_TEST(test_macos_resource_fork_typical);
    RUN_TEST(test_macos_resource_fork_short);
    RUN_TEST(test_macos_resource_fork_underscore_only);
    RUN_TEST(test_dot_alone_not_resource_fork);
    RUN_TEST(test_dot_then_x_not_resource_fork);

    // .Trashes
    RUN_TEST(test_trashes_exact);
    RUN_TEST(test_trashes_all_lower);
    RUN_TEST(test_trashes_all_upper);
    RUN_TEST(test_trashes_partial_not_filtered);

    // .Spotlight-V100
    RUN_TEST(test_spotlight_exact);
    RUN_TEST(test_spotlight_lower);
    RUN_TEST(test_spotlight_upper);
    RUN_TEST(test_spotlight_partial_not_filtered);

    // .fseventsd
    RUN_TEST(test_fseventsd_exact);
    RUN_TEST(test_fseventsd_upper);
    RUN_TEST(test_fseventsd_mixed);
    RUN_TEST(test_fseventsd_partial_not_filtered);

    // Internal temp/cache files
    RUN_TEST(test_dl_tmp_zip_filtered);
    RUN_TEST(test_dl_tmp_zip_case_mismatch_not_filtered);
    RUN_TEST(test_fileman_html_filtered);
    RUN_TEST(test_fileman_html_case_mismatch_not_filtered);

    // Boundary
    RUN_TEST(test_single_char_normal_name);
    RUN_TEST(test_very_long_normal_name);
    RUN_TEST(test_dotdot_not_filtered_by_is_system_entry);
    RUN_TEST(test_dot_not_filtered);

    return UNITY_END();
}
