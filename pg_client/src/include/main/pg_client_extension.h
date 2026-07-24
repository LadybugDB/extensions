#pragma once

#include "extension/extension.h"

namespace lbug {
namespace pg_client_extension {

class PgClientExtension final : public extension::Extension {
public:
    static constexpr char EXTENSION_NAME[] = "PG_CLIENT";

public:
    static void load(main::ClientContext* context);
};

} // namespace pg_client_extension
} // namespace lbug
