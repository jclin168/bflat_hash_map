#include <ankerl/unordered_dense.h>
#include <fuzz/run.h>

#include <app/doctest.h>

#include <unordered_map>

namespace {

// Using DummyHash to make it easier for the fuzzer
struct dummy_hash {
    using is_avalanching = void;

    auto operator()(uint64_t x) const noexcept -> size_t {
        return static_cast<size_t>(x);
    }
};

template <typename Map>
void insert_erase(fuzz::provider p) {
    auto ank = Map();
    if (reserve_mode) ank.reserve(reserve_n);
    auto ref = std::unordered_map<uint64_t, uint64_t>();

    auto c = uint64_t();
    while (p.has_remaining_bytes()) {
        auto key = p.integral<uint64_t>();
        ank[key] = c;
        ref[key] = c;
        ++c;
        peak_live=std::max(peak_live,ank.size());

        key = p.integral<uint64_t>();
        REQUIRE(ank.erase(key) == ref.erase(key));
        REQUIRE(ank.size() == ref.size());
    }
    auto cpy = std::unordered_map(ank.begin(), ank.end());
    REQUIRE(cpy == ref);
}

} // namespace

