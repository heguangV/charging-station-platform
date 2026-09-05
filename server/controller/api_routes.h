#pragma once

#include "server/server_app.h"

#include <string>
#include <string_view>

namespace ncs::server::controller {

class ApiRoutes final {
public:
    static constexpr std::string_view routePrefix = "/api/v1";

    explicit ApiRoutes(ServerApp &application);

    crow::DynamicRule &route(std::string_view relativePath);

private:
    ServerApp &application_;
};

} // namespace ncs::server::controller
