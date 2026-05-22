#pragma once

#include "extension/extension.h"

namespace lbug {
namespace adbc_extension {

class ADBCExtension final : public extension::Extension {
public:
    static constexpr char EXTENSION_NAME[] = "ADBC";

public:
    static void load(main::ClientContext* context);
};

} // namespace adbc_extension
} // namespace lbug
