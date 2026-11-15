#include <ankerl/unordered_dense.h>
#include <fuzz/provider.h>
#include <fuzz/run.h>

#include <app/doctest.h>

#include <unordered_map>

namespace {

// The maximum key length is 224 bytes.
// This length covers the hash paths for short and long strings.
// The provider stops at the first terminator.
static constexpr auto max_key_length = size_t{224};

template <typename Map>
void do_string(fuzz::provider p) {
    auto ank = Map();
    if (reserve_mode) ank.reserve(reserve_n);
    auto ref = std::unordered_map<std::string, std::string>();

    while (p.has_remaining_bytes()) {
        auto str = p.string(max_key_length);
        REQUIRE(ank.try_emplace(str, "hello!").second == ref.try_emplace(str, "hello!").second);
        peak_live=std::max(peak_live,ank.size());

        str = p.string(max_key_length);
        auto it_ank = ank.find(str);
        auto it_ref = ref.find(str);
        REQUIRE((it_ank == ank.end()) == (it_ref == ref.end()));

        if (it_ank != ank.end()) {
            ank.erase(it_ank);
            ref.erase(it_ref);
        }
        REQUIRE(ank.size() == ref.size());

        str = p.string(max_key_length);
        REQUIRE(ank.try_emplace(str, "huh").second == ref.try_emplace(str, "huh").second);
        peak_live=std::max(peak_live,ank.size());

        str = p.string(max_key_length);
        REQUIRE(ank.erase(str) == ref.erase(str));
    }

    REQUIRE(std::unordered_map(ank.begin(), ank.end()) == ref);
}

} // namespace

