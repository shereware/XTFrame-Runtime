#include "doctest.h"

#include <string_view>

#include "xt/types.h"

using namespace xt;

TEST_CASE("Vec2 operators") {
    Vec2 a{1, 2};
    Vec2 b{3, 4};
    CHECK((a + b) == Vec2{4, 6});
    CHECK((a - b) == Vec2{-2, -2});
    CHECK((-a) == Vec2{-1, -2});
    CHECK((a * 2.0f) == Vec2{2, 4});
    CHECK((2.0f * a) == Vec2{2, 4});
    CHECK((b / 2.0f) == Vec2{1.5f, 2});
    Vec2 c{1, 2};
    c += b;
    CHECK(c == Vec2{4, 6});
    c -= b;
    CHECK(c == Vec2{1, 2});
    c *= 3.0f;
    CHECK(c == Vec2{3, 6});
    CHECK(a != b);
}

TEST_CASE("Rect::contains boundaries") {
    Rect r{0, 0, 10, 10};
    CHECK(contains(r, Vec2{0, 0}));         // left/top inclusive
    CHECK_FALSE(contains(r, Vec2{10, 5}));  // right exclusive
    CHECK_FALSE(contains(r, Vec2{5, 10}));  // bottom exclusive
    CHECK(contains(r, Vec2{9.999f, 9.999f}));
}

TEST_CASE("Rect::intersect") {
    Rect a{0, 0, 10, 10};
    Rect b{5, 5, 10, 10};
    Rect result = intersect(a, b);
    CHECK(result == Rect{5, 5, 5, 5});

    Rect c{20, 20, 5, 5};
    Rect none = intersect(a, c);
    CHECK(none.w == 0);
    CHECK(none.h == 0);

    // touching edges counts as no intersection
    Rect touching{10, 0, 5, 5};
    Rect touchResult = intersect(a, touching);
    CHECK(touchResult.w == 0);
    CHECK(touchResult.h == 0);
}

TEST_CASE("Rect::unite") {
    Rect a{0, 0, 10, 10};
    Rect b{5, 5, 10, 10};
    CHECK(unite(a, b) == Rect{0, 0, 15, 15});

    Rect empty{0, 0, 0, 0};
    CHECK(unite(a, empty) == a);
    CHECK(unite(empty, a) == a);
}

TEST_CASE("Result<int> ok") {
    Result<int> r = Result<int>::ok(5);
    CHECK(r.ok());
    CHECK(r.value() == 5);
    CHECK(r.valueOr(7) == 5);
}

TEST_CASE("Result<int> error") {
    Result<int> r = Result<int>::error(Status::kNotFound);
    CHECK_FALSE(r.ok());
    CHECK(r.status() == Status::kNotFound);
    CHECK(r.valueOr(7) == 7);
}

TEST_CASE("statusName") {
    CHECK(std::string_view(statusName(Status::kOk)) == "kOk");
    CHECK(std::string_view(statusName(Status::kInvalidArgument)) == "kInvalidArgument");
    CHECK(std::string_view(statusName(Status::kInvalidIr)) == "kInvalidIr");
    CHECK(std::string_view(statusName(Status::kNotFound)) == "kNotFound");
    CHECK(std::string_view(statusName(Status::kOutOfRange)) == "kOutOfRange");
    CHECK(std::string_view(statusName(Status::kUnsupported)) == "kUnsupported");
    CHECK(std::string_view(statusName(Status::kIoError)) == "kIoError");
    CHECK(std::string_view(statusName(Status::kInternal)) == "kInternal");
}
