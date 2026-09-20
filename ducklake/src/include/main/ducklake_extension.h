#pragma once

#include "extension/extension.h"

namespace lbug {
namespace ducklake_extension {

class DuckLakeExtension final : public extension::Extension {
public:
    static constexpr char EXTENSION_NAME[] = "DUCKLAKE";

public:
    static void load(main::ClientContext* context);
};

} // namespace ducklake_extension
} // namespace lbug
