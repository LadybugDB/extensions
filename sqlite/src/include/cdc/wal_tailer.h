#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cdc/wal_format.h"

namespace lbug {
namespace sqlite_extension {
namespace cdc {

// One verified WAL frame: page number plus its full page image.
struct WalFrame {
    uint32_t pageNo = 0;
    // Nonzero when this frame ends a commit (database size after commit).
    uint32_t commitSize = 0;
    std::vector<uint8_t> image;
};

// Frames of a single committed transaction, in log order.
struct CommitBatch {
    // Frame indices into WalLog::frames (0-based).
    size_t firstFrame = 0;
    size_t lastFrame = 0;
    uint32_t commitSize = 0;
};

struct WalLog {
    WalHeader header;
    // All checksum/salt-verified frames, in log order.
    std::vector<WalFrame> frames;
    // Committed transactions (frame ranges into frames).
    std::vector<CommitBatch> commits;
    // Verified frames past the last commit frame: uncommitted tail, not
    // safe to apply.
    size_t uncommittedFrames = 0;
};

// Parses and verifies a WAL file image held in memory:
// magic/format, header checksum, per-frame salts and chained checksums.
// A torn trailing frame (short read) is silently dropped; any other
// inconsistency fails the parse. Pure function over bytes: no I/O.
struct WalParseResult {
    bool ok = false;
    std::string error;
    WalLog log;
};

WalParseResult parseWal(const uint8_t* data, size_t size);

} // namespace cdc
} // namespace sqlite_extension
} // namespace lbug
