#include <cstdint>
#include <string>

#include "alloc_counter.h"
#include "doctest.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/parse_xt.h"

/// T-267d: parseXt()'s sibling-count guard, moved here from xt_tests (Tests/unit/test_parse_xt.cpp)
/// because it counts allocations, which needs xt_perf's global operator new replacement. A count
/// is exact and does not depend on the machine's load, so it holds at any size, unlike the
/// wall-clock ratio it replaces.

namespace {

/// A document of `count` sibling `<div/>`s.
std::string siblingText(int count) {
    std::string text = "<xt><body>";
    for (int i = 0; i < count; ++i) {
        text += "<div/>";
    }
    text += "</body></xt>";
    return text;
}

/// The operator new / new[] calls of one parseXt() of `count` siblings.
uint64_t siblingParseAllocations(int count) {
    const std::string text = siblingText(count);
    xt::compile::DiagnosticSink sink;
    xt::compile::DomTree tree;
    xt::perf::startCounting();
    const bool ok = xt::compile::parseXt(text, "<mem>", sink, tree);
    xt::perf::stopCounting();
    const uint64_t allocations = xt::perf::count();
    REQUIRE(ok);
    REQUIRE(tree.nodes[1].children.size() == static_cast<size_t>(count));
    return allocations;
}

}  // namespace

// Twice the siblings cost a linear parse about twice the allocations (vector growth adds a few
// more); a parse that copies or rebuilds per sibling costs about four times as many.
TEST_CASE("perf: parseXt of 20000 sibling elements allocates at most 2.2 times as often as 10000") {
    const uint64_t small = siblingParseAllocations(10000);
    const uint64_t large = siblingParseAllocations(20000);
    CAPTURE(small);
    CAPTURE(large);
    CHECK(small > 0);
    CHECK(static_cast<double>(large) <= 2.2 * static_cast<double>(small));
}
