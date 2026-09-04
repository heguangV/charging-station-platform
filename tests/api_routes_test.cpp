#include "server/controller/api_routes.h"

#include <crow.h>

#include <iostream>
#include <stdexcept>

int main()
{
    ncs::server::ServerApp application;
    ncs::server::controller::ApiRoutes routes(application);

    if (routes.routePrefix != "/api/v1") {
        std::cerr << "API route prefix must remain /api/v1\n";
        return 1;
    }
    routes.route("/test-only")([] {
        return crow::response(204);
    });
    application.validate();

    try {
        routes.route("missing-leading-slash");
        std::cerr << "API route registration accepted an unscoped path\n";
        return 1;
    } catch (const std::invalid_argument &) {
    }
    return 0;
}
