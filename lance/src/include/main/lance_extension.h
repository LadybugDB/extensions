#pragma once

#include "extension/extension.h"

namespace lbug {
namespace lance_extension {

class LanceExtension final : public extension::Extension {
public:
    static constexpr char EXTENSION_NAME[] = "LANCE";

public:
    static void load(main::ClientContext* context);
};

} // namespace lance_extension
} // namespace lbug
