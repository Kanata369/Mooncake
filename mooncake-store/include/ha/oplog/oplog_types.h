#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ha/standby_segment.h"
#include "types.h"
#include "weight_management.h"

namespace mooncake {

enum class OpType : uint8_t {
    PUT_END = 1,
    PUT_REVOKE = 2,
    REMOVE = 3,
    LEASE_RENEW = 4,
    SEGMENT_MOUNT = 5,
    SEGMENT_UNMOUNT = 6,
    SEGMENT_UPDATE = 7,
    WEIGHT_METADATA_UPSERT = 8,
    WEIGHT_METADATA_DELETE = 9,
    WEIGHT_LEASE_UPSERT = 10,
    WEIGHT_LEASE_DELETE = 11,
    OBJECT_STORAGE_MUTATION = 12,
    OP_TYPE_MAX,
};

struct WeightMetadataUpsertOp {
    WeightRevisionMetadata metadata;
    std::optional<WeightResidencyOperation> operation;

    friend bool operator==(const WeightMetadataUpsertOp&,
                           const WeightMetadataUpsertOp&) = default;
};
YLT_REFL(WeightMetadataUpsertOp, metadata, operation);

struct WeightMetadataDeleteOp {
    WeightRevisionIdentity identity;
    uint64_t metadata_generation{0};

    friend bool operator==(const WeightMetadataDeleteOp&,
                           const WeightMetadataDeleteOp&) = default;
};
YLT_REFL(WeightMetadataDeleteOp, identity, metadata_generation);

struct WeightLeaseDeleteOp {
    uint64_t lease_id{0};
    WeightRevisionIdentity identity;
    uint64_t fenced_metadata_generation{0};

    friend bool operator==(const WeightLeaseDeleteOp&,
                           const WeightLeaseDeleteOp&) = default;
};
YLT_REFL(WeightLeaseDeleteOp, lease_id, identity, fenced_metadata_generation);

struct OpLogEntry {
    uint64_t sequence_id{0};
    uint64_t timestamp_ms{0};
    OpType op_type{OpType::PUT_END};
    std::string tenant_id{"default"};
    std::string object_key;
    std::string payload;
    uint32_t checksum{0};
    uint32_t prefix_hash{0};
};

inline constexpr size_t kMaxOpLogObjectKeySize = 4096;
inline constexpr size_t kMaxOpLogPayloadSize = 10 * 1024 * 1024;

bool NormalizeAndValidateClusterId(std::string& cluster_id);
uint32_t ComputeOpLogChecksum(std::string_view payload);
bool VerifyOpLogChecksum(const OpLogEntry& entry);
bool ValidateOpLogEntrySize(const OpLogEntry& entry,
                            std::string* reason = nullptr);

}  // namespace mooncake
