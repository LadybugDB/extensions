#include "cdc/wal_tailer.h"

#include <format>

namespace lbug {
namespace sqlite_extension {
namespace cdc {

WalParseResult parseWal(const uint8_t* data, size_t size) {
    WalParseResult out;
    if (size < WAL_HDR_SIZE) {
        out.error = "WAL too short for header";
        return out;
    }
    WalHeader hdr;
    hdr.magic = readU32BE(data);
    hdr.format = readU32BE(data + 4);
    hdr.pageSize = readU32BE(data + 8);
    hdr.checkpointSeq = readU32BE(data + 12);
    hdr.salt1 = readU32BE(data + 16);
    hdr.salt2 = readU32BE(data + 20);
    hdr.checksum1 = readU32BE(data + 24);
    hdr.checksum2 = readU32BE(data + 28);
    if (hdr.magic != WAL_MAGIC_NATIVE && hdr.magic != WAL_MAGIC_NON_NATIVE) {
        out.error = std::format("Bad WAL magic: {:#x}", hdr.magic);
        return out;
    }
    if (hdr.format != WAL_FORMAT_VERSION) {
        out.error = std::format("Unsupported WAL format version: {}", hdr.format);
        return out;
    }
    if (hdr.pageSize < 512 || hdr.pageSize > 65536 || (hdr.pageSize & (hdr.pageSize - 1)) != 0) {
        out.error = std::format("Invalid WAL page size: {}", hdr.pageSize);
        return out;
    }
    WalChecksum cksum;
    cksum.native = (hdr.magic == WAL_MAGIC_NATIVE);
    // Header checksum covers magic..salt2 (first 24 bytes).
    cksum.update(data, WAL_HDR_SIZE - 8);
    if (cksum.s0 != hdr.checksum1 || cksum.s1 != hdr.checksum2) {
        out.error = "WAL header checksum mismatch";
        return out;
    }
    out.log.header = hdr;

    size_t frameSize = WAL_FRAME_HDR_SIZE + hdr.pageSize;
    size_t offset = WAL_HDR_SIZE;
    // Running checksum chains from the header values.
    WalChecksum running = cksum;
    std::vector<WalFrame> frames;
    while (true) {
        if (size - offset < WAL_FRAME_HDR_SIZE) {
            break; // No more (complete) frame headers: clean end or torn write.
        }
        WalFrameHeader fh;
        fh.pageNo = readU32BE(data + offset);
        fh.commitSize = readU32BE(data + offset + 4);
        fh.salt1 = readU32BE(data + offset + 8);
        fh.salt2 = readU32BE(data + offset + 12);
        fh.checksum1 = readU32BE(data + offset + 16);
        fh.checksum2 = readU32BE(data + offset + 20);
        if (size - offset < frameSize) {
            break; // Torn page image: stop, keep verified prefix.
        }
        if (fh.pageNo == 0 || fh.salt1 != hdr.salt1 || fh.salt2 != hdr.salt2) {
            break; // Salt mismatch / invalid page: WAL was restarted; stop.
        }
        WalChecksum frameCksum = running;
        // Frame checksum covers pageNo+commitSize (8 bytes) then the image.
        frameCksum.update(data + offset, 8);
        frameCksum.update(data + offset + WAL_FRAME_HDR_SIZE, hdr.pageSize);
        if (frameCksum.s0 != fh.checksum1 || frameCksum.s1 != fh.checksum2) {
            out.error = std::format("WAL frame {} checksum mismatch", frames.size());
            return out;
        }
        running = frameCksum;
        WalFrame frame;
        frame.pageNo = fh.pageNo;
        frame.commitSize = fh.commitSize;
        frame.image.assign(data + offset + WAL_FRAME_HDR_SIZE,
            data + offset + WAL_FRAME_HDR_SIZE + hdr.pageSize);
        frames.push_back(std::move(frame));
        offset += frameSize;
    }
    out.log.frames = std::move(frames);

    // Group into commits at nonzero commitSize boundaries.
    size_t batchStart = 0;
    for (size_t i = 0; i < out.log.frames.size(); i++) {
        if (out.log.frames[i].commitSize != 0) {
            CommitBatch batch;
            batch.firstFrame = batchStart;
            batch.lastFrame = i;
            batch.commitSize = out.log.frames[i].commitSize;
            out.log.commits.push_back(batch);
            batchStart = i + 1;
        }
    }
    out.log.uncommittedFrames = out.log.frames.size() - batchStart;
    out.ok = true;
    return out;
}

} // namespace cdc
} // namespace sqlite_extension
} // namespace lbug
