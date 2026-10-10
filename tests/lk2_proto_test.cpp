// Unit tests for the living-kotor2 bridge wire codec (newline-delimited JSON,
// flat {id, text} objects). Built and run on Linux with g++ -- no Windows
// headers may be included by the module under test.
#include "lk2_proto.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                           \
    do {                                                                      \
        if (cond) { ++g_pass; } else {                                        \
            ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                     \
    } while (0)

using namespace k2se::lk2;

static std::string Enc(int id, const char* text) {
    char buf[256];
    size_t n = EncodeLine(id, text, std::strlen(text), buf, sizeof(buf));
    return std::string(buf, n);
}

static void test_encode_plain() {
    CHECK(Enc(7, "hello") == "{\"id\":7,\"text\":\"hello\"}\n");
    CHECK(Enc(0, "") == "{\"id\":0,\"text\":\"\"}\n");
}

static void test_encode_escapes() {
    CHECK(Enc(1, "say \"hi\"\\") == "{\"id\":1,\"text\":\"say \\\"hi\\\"\\\\\"}\n");
    CHECK(Enc(2, "a\nb\tc") == "{\"id\":2,\"text\":\"a\\nb\\tc\"}\n");
    CHECK(Enc(3, "\x01") == "{\"id\":3,\"text\":\"\\u0001\"}\n");
}

static void test_encode_refuses_small_buffer() {
    char buf[8];
    std::memset(buf, 'X', sizeof(buf));
    CHECK(EncodeLine(7, "hello", 5, buf, sizeof(buf)) == 0);
    CHECK(buf[0] == 'X');  // nothing partial was written
}

struct Dec {
    bool ok = false; int id = -99; std::string text; bool truncated = false;
};
static Dec Decode(const char* line, size_t cap = 64) {
    Dec d; char buf[64]; uint32_t len = 0;
    d.ok = DecodeLine(line, std::strlen(line), &d.id, buf, cap, &len, &d.truncated);
    if (d.ok) d.text.assign(buf, len);
    return d;
}

static void test_decode_plain() {
    Dec d = Decode("{\"id\":7,\"text\":\"hi\"}");
    CHECK(d.ok); CHECK(d.id == 7); CHECK(d.text == "hi"); CHECK(!d.truncated);
}

static void test_decode_tolerates_whitespace_order_and_extra_keys() {
    Dec d = Decode(" { \"text\" : \"a b\" , \"seq\": 12, \"ok\": true, \"n\": null, \"id\" : 3 } ");
    CHECK(d.ok); CHECK(d.id == 3); CHECK(d.text == "a b");
}

static void test_decode_unescapes() {
    Dec d = Decode("{\"id\":1,\"text\":\"q\\\"b\\\\s\\nn\\tt\\/x\\u0041\\u00e9\"}");
    CHECK(d.ok);
    CHECK(d.text == std::string("q\"b\\s\nn\tt/xA\xE9"));
}

static void test_decode_non_latin_unicode_becomes_question_mark() {
    Dec d = Decode("{\"id\":1,\"text\":\"\\u4e2d\"}");
    CHECK(d.ok); CHECK(d.text == "?");
}

static void test_decode_truncates_long_text() {
    Dec d = Decode("{\"id\":5,\"text\":\"0123456789\"}", 4);
    CHECK(d.ok); CHECK(d.text == "012"); CHECK(d.truncated);
}

static void test_decode_rejects_bad_input() {
    CHECK(!Decode("").ok);
    CHECK(!Decode("not json").ok);
    CHECK(!Decode("{\"id\":1}").ok);                      // no text
    CHECK(!Decode("{\"text\":\"x\"}").ok);                // no id
    CHECK(!Decode("{\"id\":\"1\",\"text\":\"x\"}").ok);   // id must be a number
    CHECK(!Decode("{\"id\":1,\"text\":\"unterminated}").ok);
    CHECK(!Decode("[1,2]").ok);
    CHECK(Decode("{\"id\":-1,\"text\":\"neg ok\"}").id == -1);
}

static void test_decode_rejects_ids_beyond_int32() {
    CHECK(Decode("{\"id\":2147483647,\"text\":\"max\"}").id == 2147483647);
    CHECK(!Decode("{\"id\":2147483648,\"text\":\"x\"}").ok);
    // Eleven digits overflow a 32-bit accumulator before the bound check can run.
    CHECK(!Decode("{\"id\":21474836470,\"text\":\"x\"}").ok);
    CHECK(!Decode("{\"id\":99999999999999999999,\"text\":\"x\"}").ok);
}

static void test_roundtrip() {
    const char* src = "Kreia says: \"Apathy is death.\"\n\t\\ end";
    std::string line = Enc(42, src);
    Dec d = Decode(line.c_str());
    CHECK(d.ok); CHECK(d.id == 42); CHECK(d.text == src);
}

int main() {
    test_encode_plain();
    test_encode_escapes();
    test_encode_refuses_small_buffer();
    test_decode_plain();
    test_decode_tolerates_whitespace_order_and_extra_keys();
    test_decode_unescapes();
    test_decode_non_latin_unicode_becomes_question_mark();
    test_decode_truncates_long_text();
    test_decode_rejects_bad_input();
    test_decode_rejects_ids_beyond_int32();
    test_roundtrip();
    std::printf("lk2_proto_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
