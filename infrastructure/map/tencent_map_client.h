// 腾讯地图 WebService 通用客户端：所有服务端地图 HTTP 调用的唯一出口——固定 HTTPS 主机、
// 统一注入 Server Key、统一超时与错误处理。与前端使用的腾讯地图 JavaScript API 严格分工：
// 前端 JS API 只负责地图显示，服务端 WebService 只负责地理编码、POI 与路线规划。
// 约束：Server Key 只在服务端进程持有，绝不进入响应、URL 日志或客户端配置；任何失败
// （无 Key、网络错误、超时、非 0 业务状态）一律返回 nullopt，由调用方降级。

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QUrlQuery>

#include <optional>

namespace ncs::infrastructure::map
{

class TencentMapClient
{
  public:
    static constexpr int defaultTimeoutMs = 2500;
    static constexpr const char* webServiceHost = "https://apis.map.qq.com";

    explicit TencentMapClient(QString serverKey, int timeoutMs = defaultTimeoutMs);

    bool configured() const;

    // 同步 GET：path 形如 "/ws/place/v1/search"，query 不含 key（由本类注入）。
    // 返回已解析且 status==0 的响应体；失败（含 Key 缺失、超时、业务错误）返回 nullopt。
    std::optional<QJsonObject> get(const QString& path, const QUrlQuery& query) const;

    // 解析静态纯函数：把 {status, message, ...} 信封校验与业务数据解析分离，便于离线断言。
    static std::optional<QJsonObject> parseEnvelope(const QByteArray& payload);

  private:
    QString serverKey_;
    int timeoutMs_;
};

} // namespace ncs::infrastructure::map
