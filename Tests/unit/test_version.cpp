#include <string_view>

#include "doctest.h"
#include "xt/version.h"

TEST_CASE("versionString returns 0.10.0") { CHECK(std::string_view(xt::versionString()) == "0.10.0"); }
