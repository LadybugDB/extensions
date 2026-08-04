#pragma once

#include "extension/extension.h"

namespace lbug {
namespace main {
class ClientContext;
}

namespace timeseries_extension {

class TimeseriesExtension {
public:
    static constexpr const char* EXTENSION_NAME = "timeseries";
    static void load(main::ClientContext* context);
};

} // namespace timeseries_extension
} // namespace lbug
