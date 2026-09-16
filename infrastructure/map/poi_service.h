// 腾讯地图 POI 服务：实现 core::application::PoiProvider，用 WebService 关键字周边检索
// （/ws/place/v1/search）为 Agent 提供充电站或用户位置附近的餐饮、咖啡、便利店和商场。
// 依赖 TencentMapClient（统一 HTTPS、Server Key、超时）；无 Key、超时、配额或响应异常
// 一律映射为 ExternalServiceUnavailable，由 Agent 工具降级为“无 POI”结果，绝不阻断回复。

#pragma once

#include "core/application/poi_service.h"
#include "infrastructure/map/tencent_map_client.h"

#include <QByteArray>
#include <QJsonObject>

#include <optional>
#include <vector>

namespace ncs::infrastructure::map
{

class TencentPoiService final : public core::application::PoiProvider
{
  public:
    explicit TencentPoiService(TencentMapClient client);

    bool available() const override;

    core::application::ServiceResult<std::vector<core::application::PoiItem>>
    search(const core::application::PoiQuery& query) override;

    // 离线可测：把腾讯 search 响应体解析为 POI 列表，坐标缺失或非法项被丢弃。
    static std::vector<core::application::PoiItem> parseResponse(const QJsonObject& root,
                                                                 int limit);

    // 类别到检索关键词的归一化；未识别类别返回空串，由调用方回退为用户原话关键词。
    static std::string keywordForCategory(const std::string& category);

  private:
    TencentMapClient client_;
};

} // namespace ncs::infrastructure::map
