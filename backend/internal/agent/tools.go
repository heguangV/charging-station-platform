package agent

import (
	"context"
	"strconv"
	"strings"

	"github.com/heguangV/charging-station-platform/backend/internal/station"
)

// The four tools the assistant can use.
//
// Each one owns its argument schema and its ranges, and nothing else: it reads
// through the domain ports, turns the result into a compact observation for the
// model and into structured data for the client, and reports rather than
// raises when it cannot look.
//
// The ranges are the contract. A model will happily ask for a 900 km radius or
// for two thousand stations, and every bound below exists because an answer
// that ignores it is worse than an answer that clamps it: the reader gets
// something they can act on, and the platform does bounded work.

const (
	// stationSearchSchema describes the station search arguments.
	stationSearchSchema = `{
  "type": "object",
  "properties": {
    "keyword": {"type": "string", "description": "用户位置地址或站点名称关键词，用户只说地址时使用"},
    "latitudeE6": {"type": "integer", "description": "用户位置纬度，1e-6 度整数，GCJ-02"},
    "longitudeE6": {"type": "integer", "description": "用户位置经度，1e-6 度整数，GCJ-02"},
    "chargerType": {"type": "integer", "enum": [0, 1], "description": "0 慢充，1 快充"},
    "maxDistanceMeter": {"type": "integer", "description": "距离上限，单位米，最大 500000"},
    "maxTotalPriceCentPerKwh": {"type": "integer", "description": "总价上限，单位分/千瓦时"},
    "minIdleCount": {"type": "integer", "description": "至少需要的空闲充电桩数量"},
    "limit": {"type": "integer", "description": "返回条数，1-10"}
  }
}`

	// stationDetailSchema describes the station detail arguments.
	stationDetailSchema = `{
  "type": "object",
  "properties": {
    "stationId": {"type": "integer", "description": "站点 ID，来自 station_search 的结果"},
    "chargerType": {"type": "integer", "enum": [0, 1], "description": "0 慢充，1 快充"}
  },
  "required": ["stationId"]
}`

	// poiSearchSchema describes the nearby-place arguments.
	poiSearchSchema = `{
  "type": "object",
  "properties": {
    "category": {"type": "string", "enum": ["餐饮", "咖啡", "便利店", "商场"],
                 "description": "要检索的地点类别"},
    "keyword": {"type": "string", "description": "自定义检索关键词，优先级高于类别"},
    "latitudeE6": {"type": "integer", "description": "检索圆心纬度，1e-6 度整数，GCJ-02"},
    "longitudeE6": {"type": "integer", "description": "检索圆心经度，1e-6 度整数，GCJ-02"},
    "radiusMeter": {"type": "integer", "description": "检索半径，100-10000 米"},
    "limit": {"type": "integer", "description": "返回条数，1-10"}
  }
}`

	// routeSchema describes the route arguments.
	routeSchema = `{
  "type": "object",
  "properties": {
    "destinationLatitudeE6": {"type": "integer", "description": "目的地纬度，1e-6 度整数，GCJ-02"},
    "destinationLongitudeE6": {"type": "integer", "description": "目的地经度，1e-6 度整数，GCJ-02"},
    "destinationName": {"type": "string", "description": "目的地名称，用于导航链接与文案"},
    "originLatitudeE6": {"type": "integer", "description": "起点纬度，缺省使用用户当前位置"},
    "originLongitudeE6": {"type": "integer", "description": "起点经度，缺省使用用户当前位置"},
    "mode": {"type": "string", "enum": ["driving", "walking", "transit"],
             "description": "出行方式，缺省驾车"}
  },
  "required": ["destinationLatitudeE6", "destinationLongitudeE6"]
}`

	// chargerDetailPageSize bounds the charger list inside a station detail.
	chargerDetailPageSize = 10

	// Coordinate ranges, mirroring the database constraints.
	minLatitudeE6  = -90_000_000
	maxLatitudeE6  = 90_000_000
	minLongitudeE6 = -180_000_000
	maxLongitudeE6 = 180_000_000
)

// —— formatted fragments shared by the tools and the service ——

// yuan renders cents per kWh the way the platform states prices.
func yuan(centPerKwh int64) string {
	return strconv.FormatInt(centPerKwh/100, 10) + "." +
		padTwo(centPerKwh%100)
}

func padTwo(value int64) string {
	if value < 10 {
		return "0" + strconv.FormatInt(value, 10)
	}
	return strconv.FormatInt(value, 10)
}

// distanceText renders metres for a spoken answer, switching to kilometres at
// the point where metres stop being readable.
func distanceText(meter int64) string {
	if meter < 1000 {
		return strconv.FormatInt(meter, 10) + "米"
	}
	// Display conversion only: rounded to 0.1 km.
	hundredMeter := (meter + 50) / 100
	return strconv.FormatInt(hundredMeter/10, 10) + "." +
		strconv.FormatInt(hundredMeter%10, 10) + "公里"
}

// durationText renders seconds for a spoken answer, and says so when the
// provider gave no duration rather than reporting zero.
func durationText(seconds int64) string {
	if seconds <= 0 {
		return "未知"
	}
	minutes := seconds / 60
	if minutes < 60 {
		return strconv.FormatInt(minutes, 10) + "分钟"
	}
	return strconv.FormatInt(minutes/60, 10) + "小时" +
		strconv.FormatInt(minutes%60, 10) + "分钟"
}

// stationObservation renders a station list as evidence for the model.
func stationObservation(stations []StationSummary) string {
	var text strings.Builder
	text.WriteString("station_search 返回 ")
	text.WriteString(strconv.Itoa(len(stations)))
	text.WriteString(" 个站点：")
	for index, station := range stations {
		text.WriteString("\n")
		text.WriteString(strconv.Itoa(index + 1))
		text.WriteString(". ")
		text.WriteString(station.Name)
		text.WriteString("（id=")
		text.WriteString(strconv.FormatInt(station.ID, 10))
		text.WriteString("）地址=")
		text.WriteString(station.Address)
		if station.HasDistance {
			text.WriteString(" 距离=")
			text.WriteString(strconv.FormatInt(station.DistanceMeter, 10))
			text.WriteString("米")
		}
		text.WriteString(" 总价=")
		text.WriteString(yuan(station.TotalPriceCentPerKwh))
		text.WriteString("元/千瓦时（电费 ")
		text.WriteString(yuan(station.ElectricityPriceCentPerKwh))
		text.WriteString(" + 服务费 ")
		text.WriteString(yuan(station.ServicePriceCentPerKwh))
		text.WriteString("） 空闲=")
		text.WriteString(strconv.FormatInt(station.IdleChargerCount, 10))
		text.WriteString("/")
		text.WriteString(strconv.FormatInt(station.ChargerCount, 10))
	}
	return text.String()
}

// chargerTypesText renders the connector types a station supports.
func chargerTypesText(types []string) string {
	if len(types) == 0 {
		return "未知"
	}
	return strings.Join(types, ",")
}

// —— station_search ——

type stationSearchTool struct {
	stations StationDirectory
}

// NewStationSearchTool binds the station search tool to a directory.
func NewStationSearchTool(stations StationDirectory) Tool {
	return &stationSearchTool{stations: stations}
}

func (t *stationSearchTool) Name() string { return toolStationSearch }

func (t *stationSearchTool) Description() string {
	return "按用户位置、距离、充电类型、价格和空闲数量检索附近充电站，返回站点列表与距离。" +
		"用户询问附近充电站、找快充、便宜站点或“哪里能充电”时使用。"
}

func (t *stationSearchTool) ParametersSchema() string { return stationSearchSchema }

func (t *stationSearchTool) Invoke(ctx context.Context, conv Context, args Arguments) ToolResult {
	// A half-supplied pair is not a position: dropping both is what sends the
	// query down the keyword path instead of searching from latitude zero.
	latitude, hasLatitude := intArgument(args, "latitudeE6", minLatitudeE6, maxLatitudeE6)
	longitude, hasLongitude := intArgument(args, "longitudeE6", minLongitudeE6, maxLongitudeE6)
	if hasLatitude != hasLongitude {
		hasLatitude, hasLongitude = false, false
	}
	// The browser's own position is the fallback. When neither is present the
	// query goes out with a keyword alone, which is a legitimate search rather
	// than a failure.
	if !hasLatitude && conv.HasLocation() {
		latitude, longitude = conv.Location.LatitudeE6, conv.Location.LongitudeE6
		hasLatitude, hasLongitude = true, true
	}

	chargerType, hasChargerType := intArgument(args, "chargerType", 0, 1)
	if !hasChargerType && conv.ChargerType != nil {
		chargerType, hasChargerType = int64(*conv.ChargerType), true
	}

	limit, ok := intArgument(args, "limit", 1, 10)
	if !ok {
		limit = 5
	}
	maxDistance, ok := intArgument(args, "maxDistanceMeter", 1, 500_000)
	if !ok {
		maxDistance = 0
	}
	maxPrice, ok := intArgument(args, "maxTotalPriceCentPerKwh", 1, 100_000)
	if !ok {
		maxPrice = 0
	}
	minIdle, ok := intArgument(args, "minIdleCount", 0, 100)
	if !ok {
		minIdle = 0
	}

	filter := station.SummaryFilter{
		Keyword:            stringArgument(args, "keyword", 200),
		RadiusMeters:       maxDistance,
		MaxPriceCentPerKwh: maxPrice,
		MinIdleChargers:    minIdle,
		Limit:              int(limit),
	}
	if hasLatitude {
		filter.HasLocation = true
		filter.Latitude = float64(latitude) / 1e6
		filter.Longitude = float64(longitude) / 1e6
	}
	if hasChargerType {
		filter.ConnectorType = chargerTypeRestriction(int(chargerType))
	}

	found, err := t.stations.SearchSummaries(ctx, filter)
	if err != nil {
		return toolFailure("station search failed: "+err.Error(),
			"station_search 暂时无法查询站点，请稍后再试或直接查看站点列表。")
	}

	result := ToolResult{OK: true, Stations: found}
	if len(found) == 0 {
		// Empty is a successful search with no match, and the entry point needs
		// to walk the chain to build its own wording, so it stays OK.
		result.Observation = "station_search 查询成功，但没有符合筛选条件的站点。"
		return result
	}
	result.Observation = stationObservation(found)
	return result
}

// —— station_detail ——

type stationDetailTool struct {
	stations StationDirectory
}

// NewStationDetailTool binds the station detail tool to a directory.
func NewStationDetailTool(stations StationDirectory) Tool {
	return &stationDetailTool{stations: stations}
}

func (t *stationDetailTool) Name() string { return toolStationDetail }

func (t *stationDetailTool) Description() string {
	return "查询单个充电站的详情：价格构成、空闲数量、设备数量与充电桩明细" +
		"（快充/慢充、功率、状态）。用户追问某个站点细节时使用。"
}

func (t *stationDetailTool) ParametersSchema() string { return stationDetailSchema }

func (t *stationDetailTool) Invoke(ctx context.Context, conv Context, args Arguments) ToolResult {
	stationID, ok := intArgument(args, "stationId", 1, 9_007_199_254_740_991)
	if !ok {
		return toolFailure("station_detail requires a valid station id",
			"station_detail 调用缺少有效的 stationId，请先检索附近充电站。")
	}

	chargerType, hasChargerType := intArgument(args, "chargerType", 0, 1)
	if !hasChargerType && conv.ChargerType != nil {
		chargerType, hasChargerType = int64(*conv.ChargerType), true
	}
	restriction := ""
	if hasChargerType {
		restriction = chargerTypeRestriction(int(chargerType))
	}

	detail, err := t.stations.GetStationSummary(ctx, stationID, restriction, chargerDetailPageSize)
	if err != nil {
		return toolFailure("station detail failed: "+err.Error(),
			"station_detail 未找到该站点，请重新检索附近充电站。")
	}

	summary := detail.Summary
	var observation strings.Builder
	observation.WriteString("station_detail 站点详情：")
	observation.WriteString(summary.Name)
	observation.WriteString("（id=")
	observation.WriteString(strconv.FormatInt(summary.ID, 10))
	observation.WriteString("）地址=")
	observation.WriteString(summary.Address)
	observation.WriteString(" 支持类型=")
	observation.WriteString(chargerTypesText(summary.ChargerTypes))
	observation.WriteString(" 总价=")
	observation.WriteString(yuan(summary.TotalPriceCentPerKwh))
	observation.WriteString("元/千瓦时 空闲=")
	observation.WriteString(strconv.FormatInt(summary.IdleChargerCount, 10))
	observation.WriteString("/")
	observation.WriteString(strconv.FormatInt(summary.ChargerCount, 10))
	observation.WriteString(" 可用设备=")
	observation.WriteString(strconv.FormatInt(summary.OperationalChargerCount, 10))
	if len(detail.Chargers) > 0 {
		observation.WriteString(" 充电桩明细=")
		observation.WriteString(strconv.FormatInt(detail.ChargerTotal, 10))
		observation.WriteString(" 条：")
		for _, charger := range detail.Chargers {
			observation.WriteString("\n- ")
			observation.WriteString(charger.Code)
			observation.WriteString(" ")
			observation.WriteString(charger.Type)
			observation.WriteString(" ")
			observation.WriteString(strconv.FormatInt(charger.PowerWatt/1000, 10))
			observation.WriteString("千瓦 状态=")
			observation.WriteString(charger.Status)
		}
	}

	return ToolResult{
		OK:          true,
		Stations:    []StationSummary{summary},
		Observation: observation.String(),
	}
}

// —— poi_search ——

type poiSearchTool struct {
	pois PoiProvider
	// defaultRadiusMeter is the configured radius used when the model names
	// none. It is carried here rather than read from the service so that a tool
	// stays a self-contained unit of work with no back-reference.
	defaultRadiusMeter int64
}

// NewPoiSearchTool binds the nearby-place tool to a provider and the default
// search radius.
func NewPoiSearchTool(pois PoiProvider, defaultRadiusMeter int64) Tool {
	return &poiSearchTool{pois: pois, defaultRadiusMeter: defaultRadiusMeter}
}

func (t *poiSearchTool) Name() string { return toolPoiSearch }

func (t *poiSearchTool) Description() string {
	return "检索充电站或用户位置周边的餐厅、咖啡店、便利店和商场，返回名称、类别与距离。" +
		"用户问“附近哪里可以吃饭/喝咖啡/买东西”时使用。"
}

func (t *poiSearchTool) ParametersSchema() string { return poiSearchSchema }

func (t *poiSearchTool) Invoke(ctx context.Context, conv Context, args Arguments) ToolResult {
	if t.pois == nil || !t.pois.Available() {
		return toolFailure("poi provider is not configured",
			"poi_search 未配置地图服务，无法检索周边地点。")
	}

	latitude, hasLatitude := intArgument(args, "latitudeE6", minLatitudeE6, maxLatitudeE6)
	longitude, hasLongitude := intArgument(args, "longitudeE6", minLongitudeE6, maxLongitudeE6)
	if hasLatitude != hasLongitude {
		hasLatitude, hasLongitude = false, false
	}
	// The caller injects an anchor - usually the nearest charging station it
	// just found - before this tool runs, so the user's own position is only
	// the second choice.
	if !hasLatitude && conv.HasLocation() {
		latitude, longitude = conv.Location.LatitudeE6, conv.Location.LongitudeE6
		hasLatitude = true
	}
	if !hasLatitude {
		return toolFailure("poi_search has no usable center coordinate",
			"poi_search 缺少可用的圆心坐标，请先获取用户位置或指定站点。")
	}

	radius, ok := intArgument(args, "radiusMeter", 100, 10_000)
	if !ok {
		radius = t.defaultRadiusMeter
	}
	if radius <= 0 {
		radius = 2000
	}
	limit, ok := intArgument(args, "limit", 1, 10)
	if !ok {
		limit = 5
	}

	found, err := t.pois.SearchPois(ctx, PoiQuery{
		Location:    Location{LatitudeE6: latitude, LongitudeE6: longitude},
		Keyword:     stringArgument(args, "keyword", 60),
		Category:    stringArgument(args, "category", 20),
		RadiusMeter: radius,
		Limit:       int(limit),
	})
	if err != nil {
		return toolFailure("poi search failed: "+err.Error(),
			"poi_search 地图服务暂时不可用，未能检索周边地点。")
	}

	result := ToolResult{OK: true, Pois: found}
	if len(found) == 0 {
		result.Observation = "poi_search 查询成功，但圆心附近没有匹配的地点。"
		return result
	}
	var observation strings.Builder
	observation.WriteString("poi_search 返回 ")
	observation.WriteString(strconv.Itoa(len(found)))
	observation.WriteString(" 个地点：")
	for index, poi := range found {
		observation.WriteString("\n")
		observation.WriteString(strconv.Itoa(index + 1))
		observation.WriteString(". ")
		observation.WriteString(poi.Name)
		observation.WriteString(" 类别=")
		observation.WriteString(poi.Category)
		observation.WriteString(" 距离=")
		observation.WriteString(strconv.FormatInt(poi.DistanceMeter, 10))
		observation.WriteString("米 地址=")
		observation.WriteString(poi.Address)
	}
	result.Observation = observation.String()
	return result
}

// —— route ——

type routeTool struct {
	routes RoutePlanner
}

// NewRouteTool binds the route tool to a planner.
func NewRouteTool(routes RoutePlanner) Tool { return &routeTool{routes: routes} }

func (t *routeTool) Name() string { return toolRoute }

func (t *routeTool) Description() string {
	return "计算从用户当前位置（或指定起点）到目的地的距离、路线与预计通行时间，" +
		"并给出地图导航链接。用户问“怎么走/多远/要多久/导航过去”时使用。"
}

func (t *routeTool) ParametersSchema() string { return routeSchema }

func (t *routeTool) Invoke(ctx context.Context, conv Context, args Arguments) ToolResult {
	destinationLatitude, hasDestinationLatitude :=
		intArgument(args, "destinationLatitudeE6", minLatitudeE6, maxLatitudeE6)
	destinationLongitude, hasDestinationLongitude :=
		intArgument(args, "destinationLongitudeE6", minLongitudeE6, maxLongitudeE6)
	if !hasDestinationLatitude || !hasDestinationLongitude {
		return toolFailure("route requires a destination coordinate",
			"route 缺少目的地坐标，请先检索附近充电站再规划路线。")
	}

	originLatitude, hasOriginLatitude := intArgument(args, "originLatitudeE6", minLatitudeE6, maxLatitudeE6)
	originLongitude, hasOriginLongitude := intArgument(args, "originLongitudeE6", minLongitudeE6, maxLongitudeE6)
	if hasOriginLatitude != hasOriginLongitude {
		hasOriginLatitude, hasOriginLongitude = false, false
	}
	if !hasOriginLatitude && conv.HasLocation() {
		originLatitude, originLongitude = conv.Location.LatitudeE6, conv.Location.LongitudeE6
		hasOriginLatitude = true
	}
	if !hasOriginLatitude {
		return toolFailure("route has no usable origin coordinate",
			"route 缺少可用起点，请先允许定位或提供出发地址。")
	}

	mode, ok := travelModeArgument(args, "mode")
	if !ok {
		mode = TravelDriving
	}

	route, err := t.routes.PlanRoute(ctx, RouteRequest{
		Origin:          Location{LatitudeE6: originLatitude, LongitudeE6: originLongitude},
		Destination:     Location{LatitudeE6: destinationLatitude, LongitudeE6: destinationLongitude},
		Mode:            mode,
		DestinationName: stringArgument(args, "destinationName", 80),
	})
	if err != nil {
		return toolFailure("route planning failed: "+err.Error(),
			"route 无法规划到该目的地的路线，请确认目的地后重试。")
	}
	if route.DestinationName == "" {
		route.DestinationName = "目的地"
	}

	var observation strings.Builder
	observation.WriteString("route ")
	observation.WriteString(TravelModeName(mode))
	observation.WriteString(" 到 ")
	observation.WriteString(route.DestinationName)
	observation.WriteString("：距离=")
	observation.WriteString(strconv.FormatInt(route.DistanceMeter, 10))
	observation.WriteString("米，预计=")
	observation.WriteString(durationText(route.DurationSecond))
	if route.Fallback {
		observation.WriteString("（地图服务不可用，为直线距离估算）。")
	} else {
		observation.WriteString("。")
	}
	if len(route.Steps) > 0 {
		observation.WriteString(" 路线要点：")
		for _, step := range route.Steps {
			observation.WriteString("\n- ")
			observation.WriteString(step.Instruction)
			observation.WriteString(" ")
			observation.WriteString(strconv.FormatInt(step.DistanceMeter, 10))
			observation.WriteString("米")
		}
	}

	return ToolResult{OK: true, Route: &route, Observation: observation.String()}
}
