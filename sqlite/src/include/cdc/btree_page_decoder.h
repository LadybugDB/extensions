#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lbug {
namespace sqlite_extension {
namespace cdc {

// Decoded SQLite storage values (serial-type layer, no affinity applied).
struct SqliteValue {
    enum class Type : uint8_t { Null, Integer, Real, Text, Blob };
    Type type = Type::Null;
    int64_t intVal = 0;
    double realVal = 0.0;
    std::string textVal;
    std::vector<uint8_t> blobVal;
};

// One table-b-tree leaf cell: rowid + record values in schema order.
struct DecodedRow {
    int64_t rowid = 0;
    std::vector<SqliteValue> values;
};

// Returns the full pageSize-byte image for a page number, or nullptr when
// unknown. Implementations serve WAL frames first, then the DB file
// (standard pager precedence for overflow chains).
using PageFetcher = std::function<const uint8_t*(uint32_t pageNo)>;

struct PageDecodeResult {
    bool ok = false;
    std::string error;
    std::vector<DecodedRow> rows;
};

// Decodes all cells of a table-b-tree leaf page (flags 0x0D), following
// overflow chains via fetcher. imageSize must equal pageSize.
// reserved: DB "reserved space per page" byte (usually 0); textEncoding: DB
// text encoding (1=utf8, 2=utf16le, 3=utf16be) for TEXT blobs.
PageDecodeResult decodeTableLeafPage(const uint8_t* image, size_t imageSize, uint32_t pageSize,
    uint8_t reserved, uint8_t textEncoding, const PageFetcher& fetcher);

} // namespace cdc
} // namespace sqlite_extension
} // namespace lbug
