// ─────────────────────────────────────────────────────────────────────────────
// Unit tests: crc32_feed(uint32_t state, const uint8_t* buf, size_t len)
//
// Function under test (web_server.cpp:77-92):
//   static uint32_t crc32_feed(uint32_t state, const uint8_t* buf, size_t len)
//
// Algorithm: CRC-32/ISO-HDLC (a.k.a. CRC-32/PKZIP, the ZIP standard).
//   Polynomial : 0xEDB88320 (reflected 0x04C11DB7)
//   Init       : 0xFFFFFFFF
//   XorOut     : 0xFFFFFFFF
//   RefIn/Out  : true / true
//
// Usage:
//   uint32_t state = 0xFFFFFFFF;
//   state = crc32_feed(state, data, len);
//   uint32_t crc = state ^ 0xFFFFFFFF;
//
// Key test vector from ZIP spec:
//   crc32_feed(0xFFFFFFFF, "123456789", 9) ^ 0xFFFFFFFF == 0xCBF43926
//
// RED / GREEN annotations
// -----------------------
// [GREEN] — passes against current implementation (table is correct).
// [RED]   — would fail with a wrong polynomial, missing XorOut, wrong bit order,
//           or a nibble-table (16-entry) lookup giving wrong CRC for multi-byte
//           inputs.  Each test is labelled with which failure mode it catches.
// ─────────────────────────────────────────────────────────────────────────────

#include <unity.h>
#include "stubs/Arduino.h"
#include <cstdint>
#include <cstring>

// ── Copy of crc32_feed from web_server.cpp ────────────────────────────────────
// Static local built/T arrays are fine for host testing — no threading concerns.
static uint32_t crc32_feed(uint32_t state, const uint8_t* buf, size_t len) {
    static uint32_t T[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            T[i] = c;
        }
        built = true;
    }
    for (size_t i = 0; i < len; i++) {
        state = T[(state ^ buf[i]) & 0xFF] ^ (state >> 8);
    }
    return state;
}

// ── Convenience wrapper ───────────────────────────────────────────────────────
// Computes the complete CRC-32 (init + feed + XorOut) for a byte buffer.
static uint32_t crc32(const void* data, size_t len) {
    return crc32_feed(0xFFFFFFFF, (const uint8_t*)data, len) ^ 0xFFFFFFFF;
}

// ─────────────────────────────────────────────────────────────────────────────
// Standard test vector  [RED before correct implementation]
// ─────────────────────────────────────────────────────────────────────────────

void test_crc32_standard_vector_123456789() {
    // This is the canonical check value for CRC-32/ISO-HDLC defined in the
    // PKZIP specification and used by every compliant ZIP library.
    // A wrong polynomial, wrong init, or wrong XorOut will fail here.
    const uint8_t data[] = "123456789";           // 9 bytes (no NUL)
    uint32_t result = crc32_feed(0xFFFFFFFF, data, 9) ^ 0xFFFFFFFF;
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, result);
}

void test_crc32_empty_input() {
    // CRC of 0 bytes = 0xFFFFFFFF ^ 0xFFFFFFFF = 0x00000000.
    // Catches implementations that skip the XorOut step.
    uint32_t result = crc32(nullptr, 0);
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, result);
}

// ─────────────────────────────────────────────────────────────────────────────
// Single-byte test vectors  [RED — catches wrong table entry for single bytes]
// ─────────────────────────────────────────────────────────────────────────────

void test_crc32_single_byte_0x00() {
    // CRC-32 of {0x00}: known value from reference implementation.
    uint8_t b = 0x00;
    TEST_ASSERT_EQUAL_HEX32(0xD202EF8Du, crc32(&b, 1));
}

void test_crc32_single_byte_0xFF() {
    // CRC-32 of {0xFF}: tests the high-end of the lookup table.
    uint8_t b = 0xFF;
    TEST_ASSERT_EQUAL_HEX32(0xFF000000u, crc32(&b, 1));
}

void test_crc32_single_byte_0x01() {
    uint8_t b = 0x01;
    TEST_ASSERT_EQUAL_HEX32(0xA505DF1Bu, crc32(&b, 1));
}

// ─────────────────────────────────────────────────────────────────────────────
// Multi-byte known vectors  [RED — catches nibble-table bugs, endianness errors]
// ─────────────────────────────────────────────────────────────────────────────

void test_crc32_all_zeros_4_bytes() {
    // CRC-32 of {0,0,0,0}: distinguishes byte-reflected from non-reflected.
    const uint8_t data[] = { 0x00, 0x00, 0x00, 0x00 };
    TEST_ASSERT_EQUAL_HEX32(0x2144DF1Cu, crc32(data, 4));
}

void test_crc32_all_ones_4_bytes() {
    const uint8_t data[] = { 0xFF, 0xFF, 0xFF, 0xFF };
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, crc32(data, 4));
}

void test_crc32_ascii_hello() {
    // CRC-32 of "hello" (5 bytes, no NUL).
    const uint8_t data[] = { 'h', 'e', 'l', 'l', 'o' };
    TEST_ASSERT_EQUAL_HEX32(0x3610A686u, crc32(data, 5));
}

void test_crc32_ascii_hello_world() {
    // CRC-32 of "hello world" (11 bytes, no NUL).
    // Verified: g++ reference computation 0x0D4A1185 matches Python binascii.crc32(b"hello world").
    const uint8_t data[] = "hello world";
    TEST_ASSERT_EQUAL_HEX32(0x0D4A1185u, crc32(data, 11));
}

// ─────────────────────────────────────────────────────────────────────────────
// Incremental / streaming property  [RED — catches state-carry bugs]
// ─────────────────────────────────────────────────────────────────────────────
// A correct CRC-32 implementation must be composable:
//   crc32(A+B) == crc32_feed(crc32_feed(INIT,A,|A|), B, |B|) ^ XOROUT

void test_crc32_incremental_two_halves_equal_one_pass() {
    // Split "123456789" into "1234" + "56789".
    const uint8_t full[] = "123456789";
    const uint8_t a[]    = { '1','2','3','4' };
    const uint8_t b[]    = { '5','6','7','8','9' };

    uint32_t one_pass    = crc32_feed(0xFFFFFFFF, full, 9) ^ 0xFFFFFFFF;
    uint32_t incremental = crc32_feed(
                               crc32_feed(0xFFFFFFFF, a, 4),
                               b, 5) ^ 0xFFFFFFFF;
    TEST_ASSERT_EQUAL_HEX32(one_pass, incremental);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, incremental);
}

void test_crc32_incremental_byte_by_byte() {
    // Feed "123456789" one byte at a time — must equal the bulk result.
    const uint8_t data[] = "123456789";
    uint32_t state = 0xFFFFFFFF;
    for (int i = 0; i < 9; i++) {
        state = crc32_feed(state, data + i, 1);
    }
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, state ^ 0xFFFFFFFF);
}

void test_crc32_incremental_three_chunks() {
    // Three arbitrary chunks of "hello world":
    // "hell" + "o w" + "orld"
    const uint8_t c1[] = { 'h','e','l','l' };
    const uint8_t c2[] = { 'o',' ','w' };
    const uint8_t c3[] = { 'o','r','l','d' };
    uint32_t state = 0xFFFFFFFF;
    state = crc32_feed(state, c1, 4);
    state = crc32_feed(state, c2, 3);
    state = crc32_feed(state, c3, 4);
    TEST_ASSERT_EQUAL_HEX32(0x0D4A1185u, state ^ 0xFFFFFFFF);  // same as crc32("hello world")
}

// ─────────────────────────────────────────────────────────────────────────────
// Zero-length chunks in stream  [GREEN — confirms len==0 is a no-op]
// ─────────────────────────────────────────────────────────────────────────────

void test_crc32_zero_len_chunk_is_noop() {
    // Feeding a zero-length buffer must not change state.
    uint32_t state_before = 0xDEADBEEFu;
    uint32_t state_after  = crc32_feed(state_before, nullptr, 0);
    TEST_ASSERT_EQUAL_HEX32(state_before, state_after);
}

void test_crc32_interleaved_zero_len_chunks() {
    // Zero-length feeds between real data must not corrupt the result.
    const uint8_t data[] = "123456789";
    uint8_t dummy = 0;
    uint32_t state = 0xFFFFFFFF;
    state = crc32_feed(state, &dummy, 0);    // no-op
    state = crc32_feed(state, data, 9);
    state = crc32_feed(state, &dummy, 0);    // no-op
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, state ^ 0xFFFFFFFF);
}

// ─────────────────────────────────────────────────────────────────────────────
// Table build idempotency  [GREEN — static init must happen exactly once]
// ─────────────────────────────────────────────────────────────────────────────

void test_crc32_table_build_idempotent() {
    // Call crc32_feed multiple times; the table is built on the first call
    // (static bool built).  Subsequent calls must return identical results.
    const uint8_t data[] = "123456789";
    uint32_t r1 = crc32_feed(0xFFFFFFFF, data, 9) ^ 0xFFFFFFFF;
    uint32_t r2 = crc32_feed(0xFFFFFFFF, data, 9) ^ 0xFFFFFFFF;
    uint32_t r3 = crc32_feed(0xFFFFFFFF, data, 9) ^ 0xFFFFFFFF;
    TEST_ASSERT_EQUAL_HEX32(r1, r2);
    TEST_ASSERT_EQUAL_HEX32(r1, r3);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, r1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Large buffer — performance / correctness at scale
// (16 KB matches the ZIP pass-1 read buffer size in run_deferred_zip)
// ─────────────────────────────────────────────────────────────────────────────

void test_crc32_16k_all_zeros() {
    // 16 384 bytes of 0x00.
    // Verified: g++ reference computation 0xAB54D286 (initial agent value was wrong).
    static uint8_t buf[16384];
    memset(buf, 0x00, sizeof(buf));
    TEST_ASSERT_EQUAL_HEX32(0xAB54D286u, crc32(buf, sizeof(buf)));
}

void test_crc32_16k_sequential_bytes() {
    // 16 384 bytes of 0x00..0xFF repeated 64 times.
    // Verified: g++ reference computation 0xE81722F0 (initial agent value was wrong).
    static uint8_t buf[16384];
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(i & 0xFF);
    TEST_ASSERT_EQUAL_HEX32(0xE81722F0u, crc32(buf, sizeof(buf)));
}

// ─────────────────────────────────────────────────────────────────────────────
// Polynomial correctness: specific table entries  [RED — catches wrong poly]
// ─────────────────────────────────────────────────────────────────────────────

void test_crc32_polynomial_reflected_not_normal() {
    // CRC-32/ISO-HDLC uses the REFLECTED polynomial 0xEDB88320.
    // If the normal (non-reflected) polynomial 0x04C11DB7 were used instead,
    // the single-byte CRC of 0x80 would differ.
    // Reflected CRC-32({0x80}) == 0x3FBA6CAD  (reference value)
    // Normal    CRC-32({0x80}) would be different.
    uint8_t b = 0x80;
    TEST_ASSERT_EQUAL_HEX32(0x3FBA6CADu, crc32(&b, 1));
}

// ─────────────────────────────────────────────────────────────────────────────
// Unity entry points
// ─────────────────────────────────────────────────────────────────────────────

void setUp()    {}
void tearDown() {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();

    // Standard test vector
    RUN_TEST(test_crc32_standard_vector_123456789);
    RUN_TEST(test_crc32_empty_input);

    // Single-byte table entries
    RUN_TEST(test_crc32_single_byte_0x00);
    RUN_TEST(test_crc32_single_byte_0xFF);
    RUN_TEST(test_crc32_single_byte_0x01);

    // Multi-byte known vectors
    RUN_TEST(test_crc32_all_zeros_4_bytes);
    RUN_TEST(test_crc32_all_ones_4_bytes);
    RUN_TEST(test_crc32_ascii_hello);
    RUN_TEST(test_crc32_ascii_hello_world);

    // Incremental / streaming
    RUN_TEST(test_crc32_incremental_two_halves_equal_one_pass);
    RUN_TEST(test_crc32_incremental_byte_by_byte);
    RUN_TEST(test_crc32_incremental_three_chunks);

    // Zero-length chunks
    RUN_TEST(test_crc32_zero_len_chunk_is_noop);
    RUN_TEST(test_crc32_interleaved_zero_len_chunks);

    // Table build idempotency
    RUN_TEST(test_crc32_table_build_idempotent);

    // Large buffers
    RUN_TEST(test_crc32_16k_all_zeros);
    RUN_TEST(test_crc32_16k_sequential_bytes);

    // Polynomial direction
    RUN_TEST(test_crc32_polynomial_reflected_not_normal);

    return UNITY_END();
}
