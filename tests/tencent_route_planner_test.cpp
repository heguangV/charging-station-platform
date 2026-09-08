#include "infrastructure/map/tencent_route_planner.h"

#include <iostream>

int main()
{
    using ncs::core::application::TravelMode;
    using ncs::infrastructure::map::TencentRoutePlanner;
    const QByteArray success = R"({
    "status": 0,
    "result": {"routes": [{
      "distance": 18311,
      "duration": 77,
      "polyline": [39.9837,116.3152,-1000,2000],
      "steps": [{"instruction":"向东行驶","distance":300}]
    }]}
  })";
    const auto route = TencentRoutePlanner::parseResponse(success, TravelMode::Driving);
    if (!route || route->distanceMeter != 18311 || route->durationSecond != 4620 ||
        route->polyline.size() != 2 || route->polyline[1].latitudeE6 != 39982700 ||
        route->polyline[1].longitudeE6 != 116317200 || route->steps.size() != 1)
    {
        std::cerr << "FAIL: Tencent route response was not decoded safely\n";
        return 1;
    }
    if (TencentRoutePlanner::parseResponse(R"({"status":110,"message":"invalid key"})",
                                           TravelMode::Driving))
    {
        std::cerr << "FAIL: Tencent API failures must not produce a route\n";
        return 1;
    }
    if (TencentRoutePlanner::parseResponse("not-json", TravelMode::Walking))
    {
        std::cerr << "FAIL: malformed Tencent responses must be rejected\n";
        return 1;
    }
    for (const auto mode : {TravelMode::Driving, TravelMode::Walking, TravelMode::Transit})
    {
        for (
            const auto payload :
            {R"({"status":0,"result":{"routes":[{"distance":1,"duration":1,"polyline":[]}]}})",
             R"({"status":0,"result":{"routes":[{"distance":100,"duration":1,"polyline":[]}]}})",
             R"({"status":0,"result":{"routes":[{"distance":100,"duration":1,"polyline":[39,116,0,0]}]}})"})
        {
            if (TencentRoutePlanner::parseResponse(payload, mode))
            {
                std::cerr << "FAIL: status zero with unusable geometry must be rejected\n";
                return 1;
            }
        }
    }
    const auto gps = TencentRoutePlanner::parseGpsResponse(
        R"({"status":0,"locations":[{"lat":39.908372,"lng":116.457658}]})");
    if (!gps || gps->latitudeE6 != 39908372 || gps->longitudeE6 != 116457658)
        return 1;
    for (const auto invalid : {"not-json", R"({"status":0,"locations":[]})",
                               R"({"status":0,"locations":[{"lat":91,"lng":116}]})",
                               R"({"status":0,"locations":[{"lat":39}]})",
                               R"({"status":110,"locations":[{"lat":39,"lng":116}]})"})
    {
        if (TencentRoutePlanner::parseGpsResponse(invalid))
        {
            std::cerr << "FAIL: invalid coordinate conversion response must be rejected\n";
            return 1;
        }
    }
    return 0;
}
