#include "server/controller/api_routes.h"

#include <stdexcept>
#include <string>

namespace ncs::server::controller {

ApiRoutes::ApiRoutes(ServerApp &application)
    : application_(application)
{
}

crow::DynamicRule &ApiRoutes::route(const std::string_view relativePath)
{
    if (relativePath.empty() || relativePath.front() != '/') {
        throw std::invalid_argument("API route must be relative to /api/v1 and start with '/'");
    }
    return application_.route_dynamic(std::string(routePrefix) + std::string(relativePath));
}

} // namespace ncs::server::controller
