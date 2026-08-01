#pragma once

#include "extension/extension.h"

namespace lbug {
namespace gql_extension {

class GqlExtension final : public extension::Extension {
public:
    static constexpr char EXTENSION_NAME[] = "GQL";

public:
    static void load(main::ClientContext* context);
};

} // namespace gql_extension
} // namespace lbug
