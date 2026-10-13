#include "cdc/btree_page_decoder.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <vector>

#include "cdc/wal_format.h"
#include <format>

namespace lbug {
namespace sqlite_extension {
namespace cdc {

namespace {

// Sign-extend helpers for serial types 1..6.
int64_t readIntBE(const uint8_t* p, size_t n) {
    int64_t v = (p[0] & 0x80) ? -1 : 0; // sign extend
    for (size_t i = 0; i < n; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

bool decodeSerialValue(uint64_t serial, const uint8_t*& p, size_t& remaining, uint8_t textEncoding,
    SqliteValue& out, std::string& error) {
    switch (serial) {
    case 0:
        out.type = SqliteValue::Type::Null;
        return true;
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6: {
        static const size_t widths[] = {0, 1, 2, 3, 4, 6, 8};
        size_t w = widths[serial];
        if (remaining < w) {
            error = "Truncated integer serial value";
            return false;
        }
        out.type = SqliteValue::Type::Integer;
        out.intVal = readIntBE(p, w);
        p += w;
        remaining -= w;
        return true;
    }
    case 7: {
        if (remaining < 8) {
            error = "Truncated float serial value";
            return false;
        }
        uint64_t bits = 0;
        for (size_t i = 0; i < 8; i++) {
            bits = (bits << 8) | p[i];
        }
        double d;
        static_assert(sizeof(d) == sizeof(bits));
        __builtin_memcpy(&d, &bits, 8);
        out.type = SqliteValue::Type::Real;
        out.realVal = d;
        p += 8;
        remaining -= 8;
        return true;
    }
    case 8:
        out.type = SqliteValue::Type::Integer;
        out.intVal = 0;
        return true;
    case 9:
        out.type = SqliteValue::Type::Integer;
        out.intVal = 1;
        return true;
    case 10:
    case 11:
        error = "Reserved serial type";
        return false;
    default: {
        bool isBlob = (serial % 2) == 0;
        size_t len = static_cast<size_t>(isBlob ? (serial - 12) / 2 : (serial - 13) / 2);
        if (remaining < len) {
            error = "Truncated blob/text serial value";
            return false;
        }
        if (isBlob) {
            out.type = SqliteValue::Type::Blob;
            out.blobVal.assign(p, p + len);
        } else {
            out.type = SqliteValue::Type::Text;
            // Phase 1 supports UTF-8 text; UTF-16-backed databases are
            // rejected loudly rather than mis-decoded.
            if (textEncoding != 1) {
                error = "Non-UTF8 text encoding not supported";
                return false;
            }
            out.textVal.assign(reinterpret_cast<const char*>(p), len);
        }
        p += len;
        remaining -= len;
        return true;
    }
    }
}

// Local payload size for a table-b-tree leaf cell (SQLite btree.c
// btreePayloadToLocal): payloads up to usable-35 stay fully local;
// larger ones keep surplus = minLocal + (P-minLocal)%(usable-4) bytes
// locally (or minLocal when surplus exceeds maxLocal), the rest spilling
// onto overflow pages. Verified against the vendored sources.
size_t leafLocalPayload(uint32_t usable, size_t payloadLen) {
    size_t maxLocal = usable - 35;
    if (payloadLen <= maxLocal) {
        return payloadLen;
    }
    size_t minLocal = (static_cast<size_t>(usable - 12) * 32 / 255) - 23;
    size_t surplus = minLocal + (payloadLen - minLocal) % (usable - 4);
    return surplus <= maxLocal ? surplus : minLocal;
}

} // namespace

LeafWalkResult collectLeafPages(uint32_t rootPage, const PageFetcher& fetcher, uint32_t pageSize) {
    LeafWalkResult out;
    if (pageSize < 512) {
        out.error = "Bad page size";
        return out;
    }
    std::set<uint32_t> visited;
    std::vector<std::pair<uint32_t, size_t>> stack{{rootPage, 0}};
    constexpr size_t maxDepth = 64;
    while (!stack.empty()) {
        auto [pageNo, depth] = stack.back();
        stack.pop_back();
        if (pageNo == 0 || depth > maxDepth || !visited.insert(pageNo).second) {
            continue;
        }
        const uint8_t* image = fetcher(pageNo);
        if (image == nullptr) {
            out.error = std::format("Page {} unavailable", pageNo);
            return out;
        }
        if (image[0] == 0x0D) {
            out.leaves.insert(pageNo);
            continue;
        }
        if (image[0] != 0x05) {
            out.error =
                std::format("Page {} is not a table b-tree page (flags {:#x})", pageNo, image[0]);
            return out;
        }
        uint16_t nCell = readU16BE(image + 3);
        // Interior header is 12 bytes (rightmost pointer at [8:12]), so
        // the cell pointer array starts at [12] -- unlike leaf pages.
        for (uint16_t i = 0; i < nCell; i++) {
            uint16_t cellOff = readU16BE(image + 12 + 2 * i);
            if (cellOff + 4 > pageSize) {
                out.error = "Interior cell out of range";
                return out;
            }
            stack.emplace_back(readU32BE(image + cellOff), depth + 1);
        }
        stack.emplace_back(readU32BE(image + 8), depth + 1);
    }
    out.ok = true;
    return out;
}

PageDecodeResult decodeTableLeafPage(const uint8_t* image, size_t imageSize, uint32_t pageSize,
    uint8_t reserved, uint8_t textEncoding, const PageFetcher& fetcher) {
    PageDecodeResult out;
    if (imageSize != pageSize || pageSize < 512) {
        out.error = "Bad page image size";
        return out;
    }
    if (image[0] != 0x0D) {
        out.error = std::format("Not a table leaf page (flags {:#x})", image[0]);
        return out;
    }
    uint16_t nCell = readU16BE(image + 3);
    uint32_t usable = pageSize - reserved;
    for (uint16_t ci = 0; ci < nCell; ci++) {
        uint16_t cellOff = readU16BE(image + 8 + 2 * ci);
        if (cellOff >= pageSize) {
            out.error = "Cell pointer out of range";
            return out;
        }
        uint64_t payloadLen = 0, rowid = 0;
        size_t lenLen = 0, idLen = 0;
        if (!readVarint(image + cellOff, pageSize - cellOff, payloadLen, lenLen) ||
            !readVarint(image + cellOff + lenLen, pageSize - cellOff - lenLen, rowid, idLen)) {
            out.error = "Truncated cell header";
            return out;
        }
        size_t recordOff = cellOff + lenLen + idLen;
        // Assemble the full record, following the overflow chain if spilled.
        // NOTE: for rowid tables the record slot at an INTEGER PRIMARY KEY
        // position holds NULL; the cell rowid is authoritative (verified
        // against real databases). The resolver maps IPK columns to rowid.
        size_t localLen = leafLocalPayload(usable, static_cast<size_t>(payloadLen));
        std::vector<uint8_t> record;
        record.reserve(static_cast<size_t>(payloadLen));
        if (localLen == payloadLen) {
            if (recordOff + payloadLen > pageSize) {
                out.error = "Cell extends past page end";
                return out;
            }
            record.assign(image + recordOff, image + recordOff + payloadLen);
        } else {
            if (recordOff + localLen + 4 > pageSize) {
                out.error = "Spilled cell header past page end";
                return out;
            }
            record.assign(image + recordOff, image + recordOff + localLen);
            uint32_t overflowRoot = readU32BE(image + recordOff + localLen);
            size_t remaining = static_cast<size_t>(payloadLen) - localLen;
            uint32_t next = overflowRoot;
            while (remaining > 0) {
                if (next == 0) {
                    out.error = "Truncated overflow chain";
                    return out;
                }
                const uint8_t* page = fetcher(next);
                if (page == nullptr) {
                    out.error = std::format("Overflow page {} unavailable", next);
                    return out;
                }
                uint32_t nextPage = readU32BE(page);
                size_t chunk = std::min<size_t>(remaining, usable - 4);
                record.insert(record.end(), page + 4, page + 4 + chunk);
                remaining -= chunk;
                next = nextPage;
            }
        }
        // Decode the record: header length + serial types, then values.
        uint64_t headerLen = 0;
        size_t headerLenBytes = 0;
        if (!readVarint(record.data(), record.size(), headerLen, headerLenBytes)) {
            out.error = "Truncated record header";
            return out;
        }
        std::vector<uint64_t> serials;
        size_t pos = headerLenBytes;
        while (pos < headerLen) {
            uint64_t serial = 0;
            size_t serialLen = 0;
            if (!readVarint(record.data() + pos, record.size() - pos, serial, serialLen)) {
                out.error = "Truncated record serial types";
                return out;
            }
            serials.push_back(serial);
            pos += serialLen;
        }
        DecodedRow row;
        // rowid is stored unsigned; values above INT64_MAX are pathological
        // (WITHOUT ROWID tables use generated keys) -- wrap safely.
        row.rowid = static_cast<int64_t>(rowid);
        const uint8_t* vp = record.data() + headerLen;
        size_t vremaining = record.size() - headerLen;
        for (auto serial : serials) {
            SqliteValue value;
            std::string error;
            if (!decodeSerialValue(serial, vp, vremaining, textEncoding, value, error)) {
                out.error = error;
                return out;
            }
            row.values.push_back(std::move(value));
        }
        out.rows.push_back(std::move(row));
    }
    out.ok = true;
    return out;
}

} // namespace cdc
} // namespace sqlite_extension
} // namespace lbug
