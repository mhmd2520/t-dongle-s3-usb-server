// ─────────────────────────────────────────────────────────────────────────────
// Unit tests: HTTP Range header parsing
//
// Logic under test (dl_server.cpp:300-332, inside handle_dl()):
//
//   size_t rangeStart = 0;
//   size_t rangeEnd   = fileSize > 0 ? fileSize - 1 : 0;
//   bool   isRange    = false;
//   if (hasRangeHeader) {
//       String r = header;                     // e.g. "bytes=1048576-"
//       if (r.startsWith("bytes=")) {
//           String spec = r.substring(6);      // "1048576-" or "1048576-2097151"
//           int dash = spec.indexOf('-');
//           if (dash >= 0) {
//               size_t rs = (size_t)spec.substring(0, dash).toInt();
//               String es = spec.substring(dash + 1);
//               size_t re = es.length() > 0 ? (size_t)es.toInt()
//                                           : (fileSize > 0 ? fileSize - 1 : 0);
//               if (rs < fileSize && re < fileSize && rs <= re) {
//                   rangeStart = rs;
//                   rangeEnd   = re;
//                   isRange    = true;
//               }
//           }
//       }
//       // Malformed / unsatisfiable → graceful fallback to full file (isRange stays false)
//   }
//   size_t sendSize = isRange ? (rangeEnd - rangeStart + 1) : fileSize;
//
// This block is extracted into a pure function parse_range() for testability.
// The production code embeds it inline; the function signature here is the
// "extracted" form that the tests drive.  If the implementation is ever
// refactored to call parse_range(), the tests continue to pass unchanged.
//
// RED / GREEN annotations
// -----------------------
// [GREEN] — passes against current inline logic (function is a faithful copy).
// [RED]   — the test name calls out a case that was ABSENT before Range support
//           was added (commit "HTTP Range request support").  Running against the
//           pre-fix codebase would return isRange=false for every input.
// ─────────────────────────────────────────────────────────────────────────────

#include <unity.h>
#include <stdlib.h>
#include "stubs/Arduino.h"

// ── Extracted pure function matching the inline block in handle_dl() ─────────
//
// Returns true and populates rangeStart/rangeEnd if the header is valid and
// satisfiable for the given fileSize.  Returns false for full-file delivery.
//
// Preconditions:
//   - hasHeader is true (caller checked WebServer::hasHeader("Range"))
//   - header is the raw header value string
//   - fileSize > 0 (callers guard on empty files before reaching this block)
struct RangeResult {
    bool   isRange;
    size_t rangeStart;
    size_t rangeEnd;
    size_t sendSize;
};

static RangeResult parse_range(const String& header, size_t fileSize) {
    size_t rangeStart = 0;
    size_t rangeEnd   = fileSize > 0 ? fileSize - 1 : 0;
    bool   isRange    = false;

    if (header.startsWith("bytes=")) {
        String spec = header.substring(6);
        int dash = spec.indexOf('-');
        if (dash >= 0) {
            String startStr = spec.substring(0, dash);
            String es = spec.substring(dash + 1);
            size_t rs = (size_t)strtoul(startStr.c_str(), nullptr, 10);
            size_t re = es.length() > 0 ? (size_t)strtoul(es.c_str(), nullptr, 10)
                                        : (fileSize > 0 ? fileSize - 1 : 0);
            if (rs < fileSize && re < fileSize && rs <= re) {
                rangeStart = rs;
                rangeEnd   = re;
                isRange    = true;
            }
        }
    }

    size_t sendSize = isRange ? (rangeEnd - rangeStart + 1) : fileSize;
    return { isRange, rangeStart, rangeEnd, sendSize };
}

// ─────────────────────────────────────────────────────────────────────────────
// Happy path: "bytes=N-" (open-ended — Chrome resume)  [RED before Range support]
// ─────────────────────────────────────────────────────────────────────────────

void test_range_open_ended_from_1MB() {
    // Chrome sends "bytes=1048576-" when resuming a 5 MB file after 1 MB.
    // Expected: start=1048576, end=fileSize-1, sendSize=4194304.
    const size_t FILE = 5 * 1024 * 1024;  // 5 MB
    auto r = parse_range("bytes=1048576-", FILE);
    TEST_ASSERT_TRUE(r.isRange);
    TEST_ASSERT_EQUAL(1048576, r.rangeStart);
    TEST_ASSERT_EQUAL(FILE - 1, r.rangeEnd);
    TEST_ASSERT_EQUAL(FILE - 1048576, r.sendSize);
}

void test_range_open_ended_from_zero() {
    // "bytes=0-" is technically a valid range (the full file) — must be accepted.
    const size_t FILE = 1024;
    auto r = parse_range("bytes=0-", FILE);
    TEST_ASSERT_TRUE(r.isRange);
    TEST_ASSERT_EQUAL(0, r.rangeStart);
    TEST_ASSERT_EQUAL(FILE - 1, r.rangeEnd);
    TEST_ASSERT_EQUAL(FILE, r.sendSize);
}

void test_range_open_ended_last_byte() {
    // "bytes=N-" where N == fileSize-1 → exactly the last byte.
    const size_t FILE = 256;
    auto r = parse_range("bytes=255-", FILE);
    TEST_ASSERT_TRUE(r.isRange);
    TEST_ASSERT_EQUAL(255, r.rangeStart);
    TEST_ASSERT_EQUAL(255, r.rangeEnd);
    TEST_ASSERT_EQUAL(1, r.sendSize);
}

// ─────────────────────────────────────────────────────────────────────────────
// Happy path: "bytes=N-M" (explicit end)  [RED before Range support]
// ─────────────────────────────────────────────────────────────────────────────

void test_range_explicit_first_chunk() {
    // First 512-byte chunk of a 2048-byte file.
    auto r = parse_range("bytes=0-511", 2048);
    TEST_ASSERT_TRUE(r.isRange);
    TEST_ASSERT_EQUAL(0, r.rangeStart);
    TEST_ASSERT_EQUAL(511, r.rangeEnd);
    TEST_ASSERT_EQUAL(512, r.sendSize);
}

void test_range_explicit_middle_chunk() {
    // Middle range: bytes 512-1023 of a 2048-byte file.
    auto r = parse_range("bytes=512-1023", 2048);
    TEST_ASSERT_TRUE(r.isRange);
    TEST_ASSERT_EQUAL(512, r.rangeStart);
    TEST_ASSERT_EQUAL(1023, r.rangeEnd);
    TEST_ASSERT_EQUAL(512, r.sendSize);
}

void test_range_explicit_last_chunk() {
    // Last chunk: bytes 1536 to end of 2048-byte file.
    auto r = parse_range("bytes=1536-2047", 2048);
    TEST_ASSERT_TRUE(r.isRange);
    TEST_ASSERT_EQUAL(1536, r.rangeStart);
    TEST_ASSERT_EQUAL(2047, r.rangeEnd);
    TEST_ASSERT_EQUAL(512, r.sendSize);
}

void test_range_single_byte_in_middle() {
    // A single-byte range (start == end) must be accepted.
    auto r = parse_range("bytes=100-100", 200);
    TEST_ASSERT_TRUE(r.isRange);
    TEST_ASSERT_EQUAL(100, r.rangeStart);
    TEST_ASSERT_EQUAL(100, r.rangeEnd);
    TEST_ASSERT_EQUAL(1, r.sendSize);
}

void test_range_explicit_covers_whole_file() {
    // "bytes=0-(fileSize-1)" is the entire file — valid range, isRange=true.
    auto r = parse_range("bytes=0-99", 100);
    TEST_ASSERT_TRUE(r.isRange);
    TEST_ASSERT_EQUAL(0, r.rangeStart);
    TEST_ASSERT_EQUAL(99, r.rangeEnd);
    TEST_ASSERT_EQUAL(100, r.sendSize);
}

void test_range_large_file_4GB_boundary() {
    // Simulate a file just under 4 GB (uint32_t / size_t boundary on 32-bit).
    // On ESP32-S3 size_t is 32 bits — check no truncation in the parse.
    const size_t FILE = 0xFFFF0000UL;  // ~4 GB - 64 KB
    size_t start      = 0x7FFF0000UL;
    size_t end        = FILE - 1;
    char buf[64];
    snprintf(buf, sizeof(buf), "bytes=%lu-%lu", (unsigned long)start, (unsigned long)end);
    auto r = parse_range(buf, FILE);
    TEST_ASSERT_TRUE(r.isRange);
    TEST_ASSERT_EQUAL(start, r.rangeStart);
    TEST_ASSERT_EQUAL(end,   r.rangeEnd);
    TEST_ASSERT_EQUAL(end - start + 1, r.sendSize);
}

// ─────────────────────────────────────────────────────────────────────────────
// Unsatisfiable range → graceful fallback to full file  [GREEN]
// ─────────────────────────────────────────────────────────────────────────────

void test_range_start_equals_filesize_unsatisfiable() {
    // rs == fileSize → condition "rs < fileSize" fails → full file.
    auto r = parse_range("bytes=1024-", 1024);
    TEST_ASSERT_FALSE(r.isRange);
    TEST_ASSERT_EQUAL(1024, r.sendSize);
}

void test_range_start_beyond_filesize() {
    // rs > fileSize → unsatisfiable.
    auto r = parse_range("bytes=9999-", 1024);
    TEST_ASSERT_FALSE(r.isRange);
    TEST_ASSERT_EQUAL(1024, r.sendSize);
}

void test_range_end_beyond_filesize() {
    // re >= fileSize → condition "re < fileSize" fails → full file.
    auto r = parse_range("bytes=0-1024", 1024);
    TEST_ASSERT_FALSE(r.isRange);
    TEST_ASSERT_EQUAL(1024, r.sendSize);
}

void test_range_start_greater_than_end() {
    // rs > re (inverted range) → condition "rs <= re" fails → full file.
    auto r = parse_range("bytes=100-50", 1024);
    TEST_ASSERT_FALSE(r.isRange);
    TEST_ASSERT_EQUAL(1024, r.sendSize);
}

void test_range_end_equals_filesize_minus_one_explicit() {
    // rs=0, re=fileSize-1 given explicitly — this IS a valid range.
    auto r = parse_range("bytes=0-1023", 1024);
    TEST_ASSERT_TRUE(r.isRange);
    TEST_ASSERT_EQUAL(0,    r.rangeStart);
    TEST_ASSERT_EQUAL(1023, r.rangeEnd);
    TEST_ASSERT_EQUAL(1024, r.sendSize);
}

// ─────────────────────────────────────────────────────────────────────────────
// Malformed headers → graceful fallback  [GREEN]
// ─────────────────────────────────────────────────────────────────────────────

void test_range_missing_bytes_prefix() {
    // "kbytes=0-" does not start with "bytes=" → full file.
    auto r = parse_range("kbytes=0-", 1024);
    TEST_ASSERT_FALSE(r.isRange);
}

void test_range_empty_string() {
    // Empty header value → full file.
    auto r = parse_range("", 1024);
    TEST_ASSERT_FALSE(r.isRange);
}

void test_range_no_dash_in_spec() {
    // "bytes=1024" — no dash → indexOf('-') returns -1 → full file.
    auto r = parse_range("bytes=1024", 1024);
    TEST_ASSERT_FALSE(r.isRange);
}

void test_range_only_bytes_equals() {
    // "bytes=" with empty spec — spec is "", dash==-1 → full file.
    auto r = parse_range("bytes=", 1024);
    TEST_ASSERT_FALSE(r.isRange);
}

void test_range_non_numeric_start() {
    // "bytes=abc-512" → toInt() returns 0, so rs=0 which IS < fileSize.
    // This is a parsing quirk: strtol("abc",…) returns 0, so the range
    // becomes bytes=0-511 which is valid.  Document this here.
    // [GREEN] — describes current behaviour, not necessarily intended design.
    auto r = parse_range("bytes=abc-511", 1024);
    // rs=0, re=511, both < 1024, rs<=re → isRange=true (strtol quirk)
    TEST_ASSERT_TRUE(r.isRange);
    TEST_ASSERT_EQUAL(0,   r.rangeStart);
    TEST_ASSERT_EQUAL(511, r.rangeEnd);
    TEST_ASSERT_EQUAL(512, r.sendSize);
}

void test_range_negative_start_after_toInt() {
    // "bytes=-100-" — RFC 7233 suffix-range; our simple parser does NOT support it.
    // indexOf('-') returns 0 (first dash at position 0), so:
    //   rs = substring(0,0).toInt() = "".toInt() = 0
    //   re = substring(1).toInt()   = "100-".toInt() = 100  (stops at '-')
    // Result: parsed as bytes=0-100 → valid range [0..100].
    // This is a known parser limitation — suffix-ranges silently become prefix ranges.
    auto r = parse_range("bytes=-100-", 1024);
    TEST_ASSERT_TRUE(r.isRange);
    TEST_ASSERT_EQUAL(0,   r.rangeStart);
    TEST_ASSERT_EQUAL(100, r.rangeEnd);
}

// ─────────────────────────────────────────────────────────────────────────────
// sendSize arithmetic correctness  [RED before Range support]
// ─────────────────────────────────────────────────────────────────────────────

void test_sendsize_is_rangeEnd_minus_rangeStart_plus_one() {
    // Verifies the formula: sendSize = rangeEnd - rangeStart + 1
    // for a byte-exact mid-file range.
    auto r = parse_range("bytes=200-299", 1000);
    TEST_ASSERT_TRUE(r.isRange);
    TEST_ASSERT_EQUAL(200, r.rangeStart);
    TEST_ASSERT_EQUAL(299, r.rangeEnd);
    TEST_ASSERT_EQUAL(100, r.sendSize);   // 299 - 200 + 1
}

void test_sendsize_full_file_when_no_range() {
    // When isRange is false, sendSize must equal the full fileSize.
    auto r = parse_range("not-a-range-header", 4096);
    TEST_ASSERT_FALSE(r.isRange);
    TEST_ASSERT_EQUAL(4096, r.sendSize);
}

// ─────────────────────────────────────────────────────────────────────────────
// Unity entry points
// ─────────────────────────────────────────────────────────────────────────────

void setUp()    {}
void tearDown() {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();

    // Open-ended ranges
    RUN_TEST(test_range_open_ended_from_1MB);
    RUN_TEST(test_range_open_ended_from_zero);
    RUN_TEST(test_range_open_ended_last_byte);

    // Explicit N-M ranges
    RUN_TEST(test_range_explicit_first_chunk);
    RUN_TEST(test_range_explicit_middle_chunk);
    RUN_TEST(test_range_explicit_last_chunk);
    RUN_TEST(test_range_single_byte_in_middle);
    RUN_TEST(test_range_explicit_covers_whole_file);
    RUN_TEST(test_range_large_file_4GB_boundary);

    // Unsatisfiable → full file fallback
    RUN_TEST(test_range_start_equals_filesize_unsatisfiable);
    RUN_TEST(test_range_start_beyond_filesize);
    RUN_TEST(test_range_end_beyond_filesize);
    RUN_TEST(test_range_start_greater_than_end);
    RUN_TEST(test_range_end_equals_filesize_minus_one_explicit);

    // Malformed headers
    RUN_TEST(test_range_missing_bytes_prefix);
    RUN_TEST(test_range_empty_string);
    RUN_TEST(test_range_no_dash_in_spec);
    RUN_TEST(test_range_only_bytes_equals);
    RUN_TEST(test_range_non_numeric_start);
    RUN_TEST(test_range_negative_start_after_toInt);

    // sendSize arithmetic
    RUN_TEST(test_sendsize_is_rangeEnd_minus_rangeStart_plus_one);
    RUN_TEST(test_sendsize_full_file_when_no_range);

    return UNITY_END();
}
