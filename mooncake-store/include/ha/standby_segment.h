#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ylt/struct_pack.hpp"

namespace mooncake {

struct StandbySegmentRange {
    std::pair<uint64_t, uint64_t> id;
    uint64_t base;
};

struct SegmentMountOp {
    std::string segment_name;
    std::string transport_endpoint;
    uint64_t capacity{0};
    bool is_memory_segment{false};
    std::string file_path;
    struct_pack::compatible<StandbySegmentRange, 1> range;
    YLT_REFL(SegmentMountOp, segment_name, transport_endpoint, capacity,
             is_memory_segment, file_path, range);
};

struct SegmentUnmountOp {
    std::string transport_endpoint;
    struct_pack::compatible<decltype(StandbySegmentRange::id), 1> segment_id;
    YLT_REFL(SegmentUnmountOp, transport_endpoint, segment_id);
};

struct SegmentUpdateOp {
    std::string segment_name;
    std::string transport_endpoint;
    uint64_t capacity{0};
    bool is_memory_segment{false};
    std::string file_path;
    struct_pack::compatible<StandbySegmentRange, 1> range;
    YLT_REFL(SegmentUpdateOp, segment_name, transport_endpoint, capacity,
             is_memory_segment, file_path, range);
};

struct StandbySegmentInfo {
    std::string segment_name;
    std::string transport_endpoint;
    uint64_t capacity{0};
    bool is_memory_segment{false};
    std::string file_path;
    struct_pack::compatible<StandbySegmentRange, 1> range;
    YLT_REFL(StandbySegmentInfo, segment_name, transport_endpoint, capacity,
             is_memory_segment, file_path, range);
};

using StandbySegmentId = decltype(StandbySegmentRange::id);
using StandbyUsageKey = std::pair<std::string, std::optional<StandbySegmentId>>;
using StandbyUsage = std::map<StandbyUsageKey, uint64_t>;
inline StandbyUsageKey UsageKey(const StandbySegmentInfo& s) {
    return {s.segment_name,
            s.range ? std::optional(s.range->id) : std::nullopt};
}

// Views refer to the immutable promotion input, never to the live registry.
class StandbyRangeIndex {
   public:
    bool Add(const StandbySegmentInfo& segment);
    using Candidates = std::vector<const StandbySegmentInfo*>;
    const Candidates* Find(const std::string& alias) const;
    static const StandbySegmentInfo* Resolve(const Candidates& candidates,
                                             uint64_t address, uint64_t size,
                                             bool cxl);

   private:
    std::unordered_map<std::string, Candidates> aliases_;
    std::set<StandbySegmentId> ids_;
};

class StandbySegmentRegistry {
   public:
    void OnSegmentMount(const StandbySegmentInfo& info);
    bool OnSegmentUnmount(const std::string& endpoint,
                          std::optional<StandbySegmentId> id = std::nullopt);
    void OnSegmentUpdate(const StandbySegmentInfo& info);
    bool HasSegment(const std::string& endpoint) const;
    std::optional<StandbySegmentInfo> GetSegment(
        const std::string& endpoint) const;
    std::vector<StandbySegmentInfo> GetAllSegments() const;
    void Clear();

   private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::map<std::optional<StandbySegmentId>,
                                             StandbySegmentInfo>>
        segments_by_endpoint_;
};

}  // namespace mooncake
