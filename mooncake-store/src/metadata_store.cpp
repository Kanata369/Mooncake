#include "ha/standby_segment.h"

#include <mutex>

namespace mooncake {

bool StandbyRangeIndex::Add(const StandbySegmentInfo& s) {
    if (s.segment_name.empty() || s.transport_endpoint.empty() || !s.capacity ||
        (s.range && (s.range->base > UINT64_MAX - s.capacity ||
                     ids_.contains(s.range->id))))
        return false;
    // A shared alias is valid only within one logical pool of disjoint ranges.
    auto compatible = [&](const std::string& alias) {
        const auto* candidates = Find(alias);
        if (!candidates) return true;
        for (const auto* other : *candidates) {
            if (!s.range || !other->range ||
                s.segment_name != other->segment_name ||
                s.transport_endpoint != other->transport_endpoint ||
                (s.range->base < other->range->base + other->capacity &&
                 other->range->base < s.range->base + s.capacity))
                return false;
        }
        return true;
    };
    if (!compatible(s.transport_endpoint) || !compatible(s.segment_name))
        return false;
    if (s.range) ids_.insert(s.range->id);
    aliases_[s.transport_endpoint].push_back(&s);
    if (s.segment_name != s.transport_endpoint)
        aliases_[s.segment_name].push_back(&s);
    return true;
}

const StandbyRangeIndex::Candidates* StandbyRangeIndex::Find(
    const std::string& alias) const {
    auto it = aliases_.find(alias);
    return it == aliases_.end() ? nullptr : &it->second;
}

const StandbySegmentInfo* StandbyRangeIndex::Resolve(
    const Candidates& candidates, uint64_t address, uint64_t size, bool cxl) {
    if (!size || address > UINT64_MAX - size) return nullptr;
    for (const auto* s : candidates) {
        // Legacy records have no base. CXL descriptors contain offsets, not
        // VAs.
        if (!s->range || cxl) return candidates.size() == 1 ? s : nullptr;
        if (address >= s->range->base) {
            const auto offset = address - s->range->base;
            if (offset < s->capacity && size <= s->capacity - offset) return s;
        }
    }
    return nullptr;
}

void StandbySegmentRegistry::OnSegmentMount(const StandbySegmentInfo& info) {
    std::unique_lock lock(mutex_);
    segments_by_endpoint_[info.transport_endpoint][UsageKey(info).second] =
        info;
}

bool StandbySegmentRegistry::OnSegmentUnmount(
    const std::string& endpoint, std::optional<StandbySegmentId> id) {
    std::unique_lock lock(mutex_);
    auto it = segments_by_endpoint_.find(endpoint);
    if (it == segments_by_endpoint_.end()) return true;
    auto& group = it->second;
    if (!id && group.size() > 1) return false;
    if (!id || (group.size() == 1 && !group.begin()->first))
        group.clear();
    else
        group.erase(id);
    if (group.empty()) segments_by_endpoint_.erase(it);
    return true;
}

void StandbySegmentRegistry::OnSegmentUpdate(const StandbySegmentInfo& info) {
    OnSegmentMount(info);
}

bool StandbySegmentRegistry::HasSegment(const std::string& endpoint) const {
    std::shared_lock lock(mutex_);
    return segments_by_endpoint_.contains(endpoint);
}

std::optional<StandbySegmentInfo> StandbySegmentRegistry::GetSegment(
    const std::string& endpoint) const {
    std::shared_lock lock(mutex_);
    auto it = segments_by_endpoint_.find(endpoint);
    if (it == segments_by_endpoint_.end() || it->second.size() != 1)
        return std::nullopt;
    return it->second.begin()->second;
}

std::vector<StandbySegmentInfo> StandbySegmentRegistry::GetAllSegments() const {
    std::shared_lock lock(mutex_);
    std::vector<StandbySegmentInfo> result;
    for (const auto& [endpoint, group] : segments_by_endpoint_) {
        for (const auto& [id, info] : group) result.push_back(info);
    }
    return result;
}

void StandbySegmentRegistry::Clear() {
    std::unique_lock lock(mutex_);
    segments_by_endpoint_.clear();
}

}  // namespace mooncake
