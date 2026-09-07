#include "ui/station_list_widget.h"
#include "user_main_window.h"

#include <QComboBox>
#include <QDateTime>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#ifdef NCS_HAS_POSITIONING
#include <QGeoPositionInfoSource>
#endif

namespace ncs::user
{
void UserMainWindow::cancelNavigationLocation()
{
    ++navigationLocationRequest_;
    navigationLocating_ = false;
    if (navigationPositionSource_)
    {
        navigationPositionSource_->disconnect(this);
        navigationPositionSource_->deleteLater();
        navigationPositionSource_ = nullptr;
    }
}

void UserMainWindow::locateNavigationOrigin()
{
    cancelNavigationLocation();
    ++navigationRequestId_;
    const int request = navigationLocationRequest_;
    navigationLocating_ = true;
    navigationHasDeviceCoordinate_ = false;
    navigationMapReady_ = false;
    const QSignalBlocker blocker(navigationOrigin_);
    navigationOrigin_->setCurrentIndex(0);
    navigationBrowserButton_->hide();
    navigationMapPanel_->setCurrentIndex(0);
    navigationSummary_->setText(QStringLiteral("正在获取当前位置…"));
    navigationMapMessage_->setText(QStringLiteral("正在请求系统定位\n获取位置后将自动规划路线"));
    const auto failed = [this, request](const QString& reason)
    {
        if (request != navigationLocationRequest_ || pages_->currentIndex() != 7)
            return;
        cancelNavigationLocation();
        const QSignalBlocker originBlocker(navigationOrigin_);
        navigationOrigin_->setEditText(stationList_->navigationOriginText());
        navigationSummary_->setText(QStringLiteral("自动定位不可用，请确认起点后规划"));
        navigationMapMessage_->setText(
            reason + QStringLiteral("\n\n可输入起点地址，或选择标有「模拟位置」的起点"));
        navigationRetryButton_->setText(QStringLiteral("规划路线"));
    };
#ifdef NCS_HAS_POSITIONING
    auto* source = QGeoPositionInfoSource::createDefaultSource(this);
    if (!source)
    {
        failed(QStringLiteral("此设备没有可用的系统定位源"));
        return;
    }
    navigationPositionSource_ = source;
    connect(source, &QGeoPositionInfoSource::positionUpdated, this,
            [this, request, failed](const QGeoPositionInfo& position)
            {
                if (request != navigationLocationRequest_ || pages_->currentIndex() != 7)
                    return;
                const auto age = position.timestamp().secsTo(QDateTime::currentDateTimeUtc());
                if (!position.isValid() || age < -30 || age > 120 ||
                    (position.hasAttribute(QGeoPositionInfo::HorizontalAccuracy) &&
                     position.attribute(QGeoPositionInfo::HorizontalAccuracy) > 2000))
                {
                    failed(QStringLiteral("定位结果过旧或精度不足，请重试"));
                    return;
                }
                navigationDeviceCoordinate_ = {position.coordinate().longitude(),
                                               position.coordinate().latitude()};
                navigationHasDeviceCoordinate_ = true;
                cancelNavigationLocation();
                showNavigation();
            });
    connect(source, &QGeoPositionInfoSource::errorOccurred, this,
            [failed](QGeoPositionInfoSource::Error error)
            {
                if (error == QGeoPositionInfoSource::NoError)
                    return;
                failed(error == QGeoPositionInfoSource::AccessError
                           ? QStringLiteral("系统未允许访问位置，请在系统设置中开启定位权限")
                           : QStringLiteral("暂时无法获取系统位置，请重试"));
            });
    QTimer::singleShot(5500, this,
                       [failed] { failed(QStringLiteral("系统定位超时，未获取到当前位置")); });
    source->requestUpdate(5000);
#else
    failed(QStringLiteral("当前版本未包含系统定位组件"));
#endif
}
} // namespace ncs::user
