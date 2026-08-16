#pragma once

#include "monero_solo/types.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace monero_solo {

struct DuplicateToken {
    DuplicateKey key{};
    std::uint64_t generation{};
    explicit operator bool() const noexcept { return generation != 0U; }
};

enum class DuplicateReserveResult {
    reserved,
    duplicate,
    capacity,
};

class DuplicateRegistry final {
public:
    explicit DuplicateRegistry(std::size_t global_capacity = 131072U,
                               std::size_t source_capacity = 65536U);
    ~DuplicateRegistry();

    [[nodiscard]] DuplicateReserveResult reserve(
        const DuplicateKey &key, std::uint64_t source_id, std::uint64_t height,
        DuplicateToken &token);
    [[nodiscard]] bool release(const DuplicateToken &token);
    void retain_height(std::uint64_t source_id, std::uint64_t height);
    // Returns every generation-tagged reservation removed when the bucket's
    // final reference disappears. Tokens let callers observe exact retirement
    // without retaining an all-time durable duplicate ledger.
    [[nodiscard]] std::vector<DuplicateToken> retire_height(
        std::uint64_t source_id, std::uint64_t height);
    [[nodiscard]] std::vector<DuplicateToken> release_height(
        std::uint64_t source_id, std::uint64_t height);
    void restore(const DuplicateKey &key, std::uint64_t source_id,
                 std::uint64_t height, std::uint64_t generation);
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t source_size(std::uint64_t source_id) const;

private:
    struct Entry;
    struct Bucket;
    [[nodiscard]] std::vector<DuplicateToken> collect_bucket(
        std::uint64_t source_id, std::uint64_t height);

    std::size_t global_capacity_;
    std::size_t source_capacity_;
    mutable std::mutex mutex_;
    std::map<DuplicateKey, Entry> entries_;
    std::map<std::uint64_t, std::size_t> source_sizes_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, Bucket> buckets_;
    std::uint64_t next_generation_{1U};
};

} // namespace monero_solo
