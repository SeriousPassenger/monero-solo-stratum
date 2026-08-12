#include "monero_solo/duplicate_registry.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace monero_solo {

struct DuplicateRegistry::Entry {
    std::uint64_t source_id{};
    std::uint64_t height{};
    std::uint64_t generation{};
};

struct DuplicateRegistry::Bucket {
    std::size_t external_references{};
    bool retired{};
    std::set<DuplicateKey> keys;
};

DuplicateRegistry::DuplicateRegistry(std::size_t global_capacity,
                                     std::size_t source_capacity)
    : global_capacity_(global_capacity), source_capacity_(source_capacity) {
    if (global_capacity_ == 0U || source_capacity_ == 0U ||
        source_capacity_ > global_capacity_) {
        throw std::invalid_argument("invalid duplicate registry capacity");
    }
}

DuplicateRegistry::~DuplicateRegistry() = default;

DuplicateReserveResult DuplicateRegistry::reserve(
    const DuplicateKey &key, std::uint64_t source_id, std::uint64_t height,
    DuplicateToken &token) {
    token = {};
    std::lock_guard lock(mutex_);
    if (entries_.contains(key)) {
        return DuplicateReserveResult::duplicate;
    }
    const auto source = source_sizes_.find(source_id);
    if (entries_.size() >= global_capacity_ ||
        (source != source_sizes_.end() &&
         source->second >= source_capacity_)) {
        return DuplicateReserveResult::capacity;
    }
    if (next_generation_ == 0U) next_generation_ = 1U;
    const std::uint64_t generation = next_generation_++;
    const auto [entry, inserted] =
        entries_.emplace(key, Entry{source_id, height, generation});
    if (!inserted) return DuplicateReserveResult::duplicate;
    bool source_incremented = false;
    try {
        auto source_count = source_sizes_.try_emplace(source_id, 0U).first;
        ++source_count->second;
        source_incremented = true;
        buckets_[{source_id, height}].keys.insert(key);
    }
    catch (...) {
        entries_.erase(entry);
        if (source_incremented) {
            auto source_count = source_sizes_.find(source_id);
            if (source_count != source_sizes_.end() &&
                --source_count->second == 0U) {
                source_sizes_.erase(source_count);
            }
        }
        const auto bucket = buckets_.find({source_id, height});
        if (bucket != buckets_.end() && bucket->second.keys.empty() &&
            bucket->second.external_references == 0U) {
            buckets_.erase(bucket);
        }
        throw;
    }
    token = {key, generation};
    return DuplicateReserveResult::reserved;
}

bool DuplicateRegistry::release(const DuplicateToken &token) {
    if (!token) return false;
    std::lock_guard lock(mutex_);
    const auto iterator = entries_.find(token.key);
    if (iterator == entries_.end() || iterator->second.generation != token.generation) {
        return false;
    }
    const auto source = iterator->second.source_id;
    const auto height = iterator->second.height;
    entries_.erase(iterator);
    auto source_count = source_sizes_.find(source);
    if (source_count != source_sizes_.end() && --source_count->second == 0U) {
        source_sizes_.erase(source_count);
    }
    auto bucket = buckets_.find({source, height});
    if (bucket != buckets_.end()) {
        bucket->second.keys.erase(token.key);
        if (bucket->second.keys.empty() && bucket->second.external_references == 0U) {
            buckets_.erase(bucket);
        }
    }
    return true;
}

void DuplicateRegistry::retain_height(std::uint64_t source_id,
                                      std::uint64_t height) {
    std::lock_guard lock(mutex_);
    ++buckets_[{source_id, height}].external_references;
}

std::vector<DuplicateToken> DuplicateRegistry::retire_height(
    std::uint64_t source_id, std::uint64_t height) {
    std::lock_guard lock(mutex_);
    auto &bucket = buckets_[{source_id, height}];
    const bool was_retired = bucket.retired;
    bucket.retired = true;
    try {
        return collect_bucket(source_id, height);
    }
    catch (...) {
        bucket.retired = was_retired;
        throw;
    }
}

std::vector<DuplicateToken> DuplicateRegistry::release_height(
    std::uint64_t source_id, std::uint64_t height) {
    std::lock_guard lock(mutex_);
    auto iterator = buckets_.find({source_id, height});
    if (iterator == buckets_.end() || iterator->second.external_references == 0U) {
        return {};
    }
    --iterator->second.external_references;
    try {
        return collect_bucket(source_id, height);
    }
    catch (...) {
        ++iterator->second.external_references;
        throw;
    }
}

std::vector<DuplicateToken> DuplicateRegistry::collect_bucket(
    std::uint64_t source_id, std::uint64_t height) {
    auto iterator = buckets_.find({source_id, height});
    if (iterator == buckets_.end() || !iterator->second.retired ||
        iterator->second.external_references != 0U) {
        return {};
    }
    std::vector<DuplicateToken> collected;
    collected.reserve(iterator->second.keys.size());
    for (const auto &key : iterator->second.keys) {
        const auto entry = entries_.find(key);
        if (entry != entries_.end()) {
            collected.push_back(DuplicateToken{key, entry->second.generation});
            entries_.erase(entry);
            auto count = source_sizes_.find(source_id);
            if (count != source_sizes_.end() && --count->second == 0U) {
                source_sizes_.erase(count);
            }
        }
    }
    buckets_.erase(iterator);
    return collected;
}

void DuplicateRegistry::restore(const DuplicateKey &key, std::uint64_t source_id,
                                std::uint64_t height, std::uint64_t generation) {
    if (generation == 0U) throw std::invalid_argument("zero duplicate generation");
    std::lock_guard lock(mutex_);
    if (entries_.contains(key)) return;
    const auto source = source_sizes_.find(source_id);
    if (entries_.size() >= global_capacity_ ||
        (source != source_sizes_.end() &&
         source->second >= source_capacity_)) {
        throw std::runtime_error("persisted duplicate set exceeds configured capacity");
    }
    const auto [entry, inserted] =
        entries_.emplace(key, Entry{source_id, height, generation});
    if (!inserted) return;
    bool source_incremented = false;
    try {
        auto source_count = source_sizes_.try_emplace(source_id, 0U).first;
        ++source_count->second;
        source_incremented = true;
        buckets_[{source_id, height}].keys.insert(key);
    }
    catch (...) {
        entries_.erase(entry);
        if (source_incremented) {
            auto source_count = source_sizes_.find(source_id);
            if (source_count != source_sizes_.end() &&
                --source_count->second == 0U) {
                source_sizes_.erase(source_count);
            }
        }
        const auto bucket = buckets_.find({source_id, height});
        if (bucket != buckets_.end() && bucket->second.keys.empty() &&
            bucket->second.external_references == 0U) {
            buckets_.erase(bucket);
        }
        throw;
    }
    next_generation_ = std::max(next_generation_, generation + 1U);
}

std::size_t DuplicateRegistry::size() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

std::size_t DuplicateRegistry::source_size(std::uint64_t source_id) const {
    std::lock_guard lock(mutex_);
    const auto iterator = source_sizes_.find(source_id);
    return iterator == source_sizes_.end() ? 0U : iterator->second;
}

} // namespace monero_solo
