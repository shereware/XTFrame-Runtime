#include <cstdint>
#include <string>
#include <vector>

#include "../../Code/Runtime/compile/src/lottie/base64.h"
#include "doctest.h"

/// RFC 4648 test vectors. See Docs/tasks/T-16.md "測試".

using namespace xt::compile;

namespace {

std::string toStr(const std::vector<uint8_t>& b) { return std::string(b.begin(), b.end()); }

}  // namespace

TEST_CASE("base64Decode: RFC 4648 test vectors") {
    std::vector<uint8_t> out;

    REQUIRE(base64Decode("", out));
    CHECK(out.empty());

    REQUIRE(base64Decode("Zg==", out));
    CHECK(toStr(out) == "f");

    REQUIRE(base64Decode("Zm8=", out));
    CHECK(toStr(out) == "fo");

    REQUIRE(base64Decode("Zm9v", out));
    CHECK(toStr(out) == "foo");

    REQUIRE(base64Decode("Zm9vYg==", out));
    CHECK(toStr(out) == "foob");

    REQUIRE(base64Decode("Zm9vYmE=", out));
    CHECK(toStr(out) == "fooba");

    REQUIRE(base64Decode("Zm9vYmFy", out));
    CHECK(toStr(out) == "foobar");
}

TEST_CASE("base64Decode: tolerates embedded whitespace/newlines") {
    std::vector<uint8_t> out;
    REQUIRE(base64Decode("Zm9v\nYmFy\r\n", out));
    CHECK(toStr(out) == "foobar");
}

TEST_CASE("base64Decode: without padding still decodes") {
    std::vector<uint8_t> out;
    REQUIRE(base64Decode("Zg", out));
    CHECK(toStr(out) == "f");
    REQUIRE(base64Decode("Zm8", out));
    CHECK(toStr(out) == "fo");
}

TEST_CASE("base64Decode: illegal character returns false") {
    std::vector<uint8_t> out;
    CHECK_FALSE(base64Decode("Zm9v!YmFy", out));
    CHECK_FALSE(base64Decode("****", out));
}

TEST_CASE("base64Decode: non-padding character after padding started returns false") {
    std::vector<uint8_t> out;
    CHECK_FALSE(base64Decode("Zg=A", out));
}
