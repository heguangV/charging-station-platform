#include "station_list_widget.h"

#include "station_card.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ncs::user
{
namespace
{
class SearchGlyph final : public QWidget
{
  public:
    explicit SearchGlyph(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedSize(20, 20);
        setAttribute(Qt::WA_TranslucentBackground);
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(QStringLiteral("#667085")), 1.8, Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QRectF(3.0, 3.0, 9.5, 9.5));
        painter.drawLine(QPointF(11.0, 11.0), QPointF(16.5, 16.5));
    }
};

QPointF locationCoordinate(const QString& location)
{
    if (location == QStringLiteral("望京"))
        return {116.473200, 39.993300};
    if (location == QStringLiteral("国贸"))
        return {116.460900, 39.909100};
    return {116.318600, 39.984000};
}

double distanceKm(const QPointF& location, const StationSummary& station)
{
    constexpr double kEarthRadiusKm = 6371.0;
    constexpr double kPi = 3.14159265358979323846;
    const auto radians = [](double degree) { return degree * kPi / 180.0; };
    const double latitudeDelta = radians(station.latitude - location.y());
    const double longitudeDelta = radians(station.longitude - location.x());
    const double fromLatitude = radians(location.y());
    const double toLatitude = radians(station.latitude);
    const double sineLatitude = std::sin(latitudeDelta / 2.0);
    const double sineLongitude = std::sin(longitudeDelta / 2.0);
    const double a = sineLatitude * sineLatitude +
                     std::cos(fromLatitude) * std::cos(toLatitude) * sineLongitude * sineLongitude;
    return 2.0 * kEarthRadiusKm * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}
} // namespace

StationListWidget::StationListWidget(const QVector<StationSummary>& stations, QWidget* parent)
    : QWidget(parent), stations_(stations)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto* searchBar = new QFrame;
    searchBar->setObjectName(QStringLiteral("stationSearchBar"));
    searchBar->setStyleSheet(QStringLiteral("QFrame#stationSearchBar{background:#FFFFFF;border:1px "
                                            "solid #DDE9E6;border-radius:15px;}"));
    auto* controls = new QHBoxLayout(searchBar);
    controls->setContentsMargins(6, 6, 6, 6);
    controls->setSpacing(6);
    locationBox_ = new QComboBox;
    locationBox_->addItems({QStringLiteral("附近"), QStringLiteral("中关村"),
                            QStringLiteral("望京"), QStringLiteral("国贸")});
    locationBox_->setFixedWidth(80);
    locationBox_->setStyleSheet(
        QStringLiteral("QComboBox{background:#F1F6F5;color:#47635D;border:0;border-radius:10px;"
                       "padding:7px 22px 7px 10px;font-size:13px;font-weight:600;}"
                       "QComboBox::drop-down{border:0;width:22px;}"
                       "QComboBox QAbstractItemView{background:#FFFFFF;border:1px solid #D9E9E5;"
                       "selection-background-color:#E7F5F1;selection-color:#0F766E;}"));
    auto* searchSymbol = new SearchGlyph;
    searchEdit_ = new QLineEdit;
    searchEdit_->setPlaceholderText(QStringLiteral("搜索站点或地址"));
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setStyleSheet(QStringLiteral(
        "QLineEdit{background:transparent;border:0;padding:8px 0;color:#25324A;font-size:14px;}"
        "QLineEdit:focus{border:0;}"));
    refreshButton_ = new QPushButton(QStringLiteral("刷新"));
    refreshButton_->setToolTip(QStringLiteral("刷新站点"));
    refreshButton_->setFixedSize(44, 38);
    refreshButton_->setStyleSheet(QStringLiteral(
        "QPushButton{background:transparent;color:#0F766E;border:0;border-radius:10px;"
        "font-size:13px;font-weight:600;}"
        "QPushButton:hover{background:#E7F5F1;}"
        "QPushButton:pressed{background:#D7ECE6;}"
        "QPushButton:disabled{color:#A9BDB8;}"));
    controls->addWidget(locationBox_);
    controls->addWidget(searchSymbol);
    controls->addWidget(searchEdit_, 1);
    controls->addWidget(refreshButton_);
    layout->addWidget(searchBar);
    summary_ = new QLabel;
    summary_->setStyleSheet(QStringLiteral("color:#667085;font-size:12px;"));
    layout->addWidget(summary_);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    cards_ = new QVBoxLayout(content);
    cards_->setContentsMargins(0, 2, 0, 2);
    cards_->setSpacing(10);
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
    connect(locationBox_, &QComboBox::currentTextChanged, this, &StationListWidget::requestRefresh);
    connect(searchEdit_, &QLineEdit::textChanged, this, &StationListWidget::requestRefresh);
    connect(refreshButton_, &QPushButton::clicked, this, &StationListWidget::requestRefresh);
    refresh();
}

void StationListWidget::setStations(QVector<StationSummary> stations)
{
    stations_ = std::move(stations);
    refresh();
}

void StationListWidget::setRemoteSource(bool enabled)
{
    remoteSource_ = enabled;
    if (remoteSource_)
        requestRefresh();
}

void StationListWidget::clearCards()
{
    while (QLayoutItem* item = cards_->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
}

void StationListWidget::setLoading(bool loading)
{
    refreshButton_->setEnabled(!loading);
    if (!loading)
        return;
    clearCards();
    auto* loadingLabel = new QLabel(QStringLiteral("正在更新附近可用电桩…"));
    loadingLabel->setAlignment(Qt::AlignCenter);
    loadingLabel->setStyleSheet(QStringLiteral("padding:48px 12px;color:#52716C;font-size:14px;"));
    cards_->addWidget(loadingLabel);
    cards_->addStretch();
    summary_->setText(QStringLiteral("正在刷新站点信息"));
}

void StationListWidget::showError(const QString& userMessage)
{
    clearCards();
    auto* error = new QLabel(userMessage.isEmpty() ? QStringLiteral("网络服务不可用，请稍后重试")
                                                   : userMessage);
    error->setWordWrap(true);
    error->setAlignment(Qt::AlignCenter);
    error->setStyleSheet(QStringLiteral("padding:38px 18px 12px;color:#B42318;font-size:14px;"));
    auto* retry = new QPushButton(QStringLiteral("重新加载"));
    retry->setObjectName(QStringLiteral("primaryButton"));
    retry->setMinimumHeight(38);
    connect(retry, &QPushButton::clicked, this, [this] { requestRefresh(); });
    cards_->addWidget(error);
    cards_->addWidget(retry, 0, Qt::AlignHCenter);
    cards_->addStretch();
    summary_->setText(QStringLiteral("站点加载失败"));
}

void StationListWidget::requestRefresh()
{
    if (!remoteSource_)
    {
        refresh();
        return;
    }
    setLoading(true);
    const QPointF coordinate = locationCoordinate(locationBox_->currentText());
    // Per the REST contract, keyword is an address/location hint, not a local name filter.
    emit loadRequested(qRound64(coordinate.y() * 1000000), qRound64(coordinate.x() * 1000000),
                       searchEdit_->text().trimmed());
}

void StationListWidget::refresh()
{
    clearCards();
    const QString location = locationBox_->currentText();
    const QString keyword = searchEdit_->text().trimmed();
    const QPointF currentLocation = locationCoordinate(location);
    QVector<StationSummary> sortedStations = stations_;
    if (!remoteSource_)
        std::sort(sortedStations.begin(), sortedStations.end(),
                  [&currentLocation](const StationSummary& left, const StationSummary& right) {
                      return distanceKm(currentLocation, left) < distanceKm(currentLocation, right);
                  });
    int count = 0;
    for (StationSummary station : sortedStations)
    {
        const bool keywordMatch = remoteSource_ || keyword.isEmpty() ||
                                  station.name.contains(keyword, Qt::CaseInsensitive) ||
                                  station.address.contains(keyword, Qt::CaseInsensitive);
        if (!keywordMatch)
            continue;
        if (!remoteSource_)
            station.distance =
                QStringLiteral("%1 km").arg(distanceKm(currentLocation, station), 0, 'f', 1);
        auto* card = new StationCard(station);
        connect(card, &StationCard::selected, this,
                [this, station](int) { emit stationSelected(station); });
        cards_->addWidget(card);
        ++count;
    }
    if (count == 0)
    {
        auto* empty = new QLabel(QStringLiteral("暂无匹配的充电站\n请切换位置或调整搜索条件"));
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet(QStringLiteral("padding:48px 12px;color:#667085;"));
        cards_->addWidget(empty);
    }
    cards_->addStretch();
    const QString locationLabel =
        location == QStringLiteral("附近") ? QStringLiteral("当前位置") : location;
    summary_->setText(
        QStringLiteral("%1 · 按距离排序 · 找到 %2 个充电站").arg(locationLabel).arg(count));
}

} // namespace ncs::user
