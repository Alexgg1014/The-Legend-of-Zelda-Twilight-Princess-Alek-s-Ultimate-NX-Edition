// SHA-256 (FIPS 180-4 vectors), base64 and the JSON parser/serializer.

#include "../core/codec.hpp"
#include "../core/json.hpp"
#include "../core/sha256.hpp"

#include <cstdio>
#include <string>

using namespace crosssave;

namespace {
int g_checks = 0;
int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)

std::string sha(std::string_view text) {
    Sha256 hasher;
    hasher.update(text);
    return hasher.finish_hex();
}
}  // namespace

int main() {
    CHECK(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    {
        Sha256 hasher;
        const std::string block(1000, 'a');
        for (int i = 0; i < 1000; ++i) {
            hasher.update(block);
        }
        CHECK(hasher.finish_hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }
    // Uneven update sizes give the same digest.
    {
        std::string text(777, 'x');
        for (size_t i = 0; i < text.size(); ++i) {
            text[i] = static_cast<char>(i * 31);
        }
        Sha256 a;
        a.update(text);
        Sha256 b;
        for (size_t i = 0; i < text.size(); i += 13) {
            b.update(std::string_view{text}.substr(i, 13));
        }
        CHECK(a.finish_hex() == b.finish_hex());
    }

    for (const std::string text : {"", "f", "fo", "foo", "foob", "fooba", "foobar"}) {
        const std::vector<uint8_t> bytes{text.begin(), text.end()};
        const auto encoded = base64_encode(bytes);
        const auto decoded = base64_decode(encoded);
        CHECK(decoded && *decoded == bytes);
    }
    CHECK(base64_encode(std::vector<uint8_t>{'f', 'o', 'o', 'b', 'a'}) == "Zm9vYmE=");
    CHECK(!base64_decode("Zm9vYmE"));
    CHECK(!base64_decode("Zm9v!mE="));
    CHECK(!base64_decode("Zm9vYmF="));  // non-zero trailing bits
    CHECK(!base64_decode("Zg=a"));
    CHECK(url_encode("a b&c=d/é") == "a%20b%26c%3Dd%2F%C3%A9");

    const auto parsed = json::parse(R"({"a":[1,2.5,-3e2,true,false,null],"s":"x\"\\\n\u00e9\ud83d\ude00","o":{}})");
    CHECK(parsed && parsed->is_object());
    CHECK(parsed && (*parsed)["a"].as_array().size() == 6);
    CHECK(parsed && (*parsed)["a"].as_array()[2].as_number() == -300);
    CHECK(parsed && (*parsed)["s"].as_string() == "x\"\\\n\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(parsed && json::parse(json::dump(*parsed)) && json::dump(*json::parse(json::dump(*parsed))) == json::dump(*parsed));
    CHECK(!json::parse("{\"a\":1,}"));
    CHECK(!json::parse("[1,2"));
    CHECK(!json::parse("01"));
    CHECK(!json::parse("{} x"));
    CHECK(!json::parse("\"\\ud800\""));
    CHECK(!json::parse(std::string(70, '[') + std::string(70, ']')));
    CHECK(json::parse(std::string(60, '[') + std::string(60, ']')).has_value());
    CHECK(!json::parse("\"a\x01\""));
    json::Value v{json::Object{}};
    v["n"] = json::Value{static_cast<double>(1700000000123LL)};
    v["t"] = "tab\there";
    CHECK(json::dump(v) == R"({"n":1700000000123,"t":"tab\there"})");

    std::printf("core_test: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("core_test: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
