#include <gtest/gtest.h>

#include "ha/standby_segment.h"

namespace mooncake {
namespace {
// Exact pre-range wire layout, not a new record with the field unset.
struct LegacySegment {
    std::string name, endpoint;
    uint64_t capacity;
    bool memory;
    std::string file;
    YLT_REFL(LegacySegment, name, endpoint, capacity, memory, file);
};

constexpr uint64_t MiB = 1024 * 1024;
StandbySegmentInfo MakeRange(uint64_t n) {
    return {"pool", "endpoint", 16 * MiB,
            true,   "",         StandbySegmentRange{{0, n}, n * 16 * MiB}};
}

TEST(StandbySegmentTest, ReplaysTwoRangesThroughSnapshot) {
    StandbySegmentRegistry registry;
    for (const auto n : {0, 1}) {
        auto s = MakeRange(n);
        auto wire = struct_pack::serialize(
            SegmentMountOp{s.segment_name, s.transport_endpoint, s.capacity,
                           true, "", s.range});
        auto op = struct_pack::deserialize<SegmentMountOp>(wire);
        ASSERT_TRUE(op);
        registry.OnSegmentMount({op->segment_name, op->transport_endpoint,
                                 op->capacity, op->is_memory_segment,
                                 op->file_path, op->range});
    }
    auto snapshot = struct_pack::deserialize<std::vector<StandbySegmentInfo>>(
        struct_pack::serialize(registry.GetAllSegments()));
    ASSERT_TRUE(snapshot);
    ASSERT_EQ(snapshot->size(), 2u);
    StandbyRangeIndex index;
    for (const auto& s : *snapshot) ASSERT_TRUE(index.Add(s));
    StandbyUsage usage;
    for (const auto n : {0, 1}) {
        auto expected = MakeRange(n);
        const auto* group = index.Find(expected.transport_endpoint);
        ASSERT_NE(group, nullptr);
        auto* s = StandbyRangeIndex::Resolve(*group, expected.range->base,
                                             9 * MiB, false);
        ASSERT_NE(s, nullptr);
        ASSERT_EQ(s->range->id, expected.range->id);
        auto& bytes = usage[UsageKey(*s)];
        bytes += 9 * MiB;
        EXPECT_LE(bytes, s->capacity);
    }
    EXPECT_EQ(usage.size(), 2u);
    EXPECT_FALSE(registry.GetSegment("endpoint"));
    EXPECT_FALSE(registry.OnSegmentUnmount("endpoint"));
    EXPECT_TRUE(registry.OnSegmentUnmount("endpoint", MakeRange(0).range->id));
    ASSERT_TRUE(registry.GetSegment("endpoint"));
    EXPECT_EQ(registry.GetSegment("endpoint")->range->id,
              MakeRange(1).range->id);
}

TEST(StandbySegmentTest, OldAndNewWireLayouts) {
    LegacySegment legacy{"pool", "endpoint", 16 * MiB, true, ""};
    auto wire = struct_pack::serialize(legacy);
    auto op = struct_pack::deserialize<SegmentMountOp>(wire);
    ASSERT_TRUE(op);
    EXPECT_FALSE(op->range);
    auto old_vector = struct_pack::serialize(std::vector{legacy, legacy});
    auto upgraded =
        struct_pack::deserialize<std::vector<StandbySegmentInfo>>(old_vector);
    ASSERT_TRUE(upgraded);
    ASSERT_EQ(upgraded->size(), 2u);
    EXPECT_FALSE((*upgraded)[1].range);
    auto old_reader = struct_pack::deserialize<std::vector<LegacySegment>>(
        struct_pack::serialize(std::vector{MakeRange(0), MakeRange(1)}));
    ASSERT_TRUE(old_reader);
    ASSERT_EQ(old_reader->size(), 2u);
    EXPECT_EQ((*old_reader)[1].endpoint, "endpoint");
}

TEST(StandbySegmentTest, RejectsAmbiguousAndCrossBoundaryRanges) {
    auto a = MakeRange(0), b = MakeRange(1), bad = b;
    StandbyRangeIndex index;
    ASSERT_TRUE(index.Add(a));
    bad.range->base--;
    EXPECT_FALSE(index.Add(bad));
    ASSERT_TRUE(index.Add(b));
    EXPECT_FALSE(index.Add(a));
    auto group = index.Find("pool");
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(StandbyRangeIndex::Resolve(*group, 16 * MiB - 1, 1, false), &a);
    EXPECT_EQ(StandbyRangeIndex::Resolve(*group, 16 * MiB - 1, 2, false),
              nullptr);
    EXPECT_EQ(StandbyRangeIndex::Resolve(*group, UINT64_MAX, 2, false),
              nullptr);
    EXPECT_EQ(StandbyRangeIndex::Resolve(*group, 1, 2, true), nullptr);
    StandbyRangeIndex single;
    ASSERT_TRUE(single.Add(b));
    EXPECT_EQ(StandbyRangeIndex::Resolve(*single.Find("pool"), 1, 2, true), &b);
}
}  // namespace
}  // namespace mooncake
