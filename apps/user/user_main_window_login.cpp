#include "user_main_window.h"

#include "net/api_client.h"
#include "net/user_api.h"
#include "ui/station_list_widget.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QPushButton>
#include <QVBoxLayout>

namespace ncs::user
{
namespace
{
class EnergyMark final : public QWidget
{
  public:
    explicit EnergyMark(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedSize(64, 64);
        setAccessibleName(QStringLiteral("NCS 充电标识"));
        setAttribute(Qt::WA_TranslucentBackground);
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#0F766E")));
        painter.drawRoundedRect(QRectF(2, 2, 60, 60), 18, 18);
        // An open energy ring surrounds a lightning bolt, ending in a two-pin plug.
        painter.setPen(QPen(QColor(QStringLiteral("#C9F2E5")), 3, Qt::SolidLine, Qt::RoundCap,
                            Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(14, 14, 36, 36), 45 * 16, 270 * 16);
        painter.drawLine(QPointF(45, 45), QPointF(49, 49));
        painter.drawLine(QPointF(47, 47), QPointF(50, 44));
        painter.drawLine(QPointF(49, 49), QPointF(52, 46));
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawPolygon(QPolygonF{{35, 19}, {24, 34}, {31, 34}, {28, 45}, {41, 29}, {34, 29}});
    }
};
} // namespace

QWidget* UserMainWindow::createLoginPage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    layout->addStretch(1);
    auto* hero = new QFrame;
    hero->setObjectName(QStringLiteral("loginHero"));
    hero->setMinimumHeight(48);
    auto* heroLayout = new QVBoxLayout(hero);
    heroLayout->setContentsMargins(0, 0, 0, 0);
    heroLayout->setSpacing(12);
    auto* mark = new EnergyMark;
    auto* eyebrow = new QLabel(QStringLiteral("NCS 充电"));
    eyebrow->setStyleSheet(
        QStringLiteral("color:#344054;background:transparent;font-size:16px;font-weight:600;"));
    heroLayout->addWidget(mark, 0, Qt::AlignHCenter);
    heroLayout->addWidget(eyebrow, 0, Qt::AlignHCenter);
    layout->addWidget(hero);

    auto* loginPanel = new QFrame;
    loginPanel->setObjectName(QStringLiteral("loginPanel"));
    auto* panelLayout = new QVBoxLayout(loginPanel);
    panelLayout->setContentsMargins(0, 12, 0, 0);
    panelLayout->setSpacing(10);
    loginPanel->setStyleSheet(QStringLiteral(
        "QFrame#loginPanel{background:transparent;border:0;}"));
    auto* loginTitle = new QLabel(QStringLiteral("登录"));
    loginTitle->setAlignment(Qt::AlignCenter);
    loginTitle->setStyleSheet(QStringLiteral("font-size:20px;font-weight:500;color:#344054;"));
    auto* loginHint = new QLabel(QStringLiteral("使用手机号验证码登录或注册"));
    loginHint->setWordWrap(true);
    loginHint->setAlignment(Qt::AlignCenter);
    loginHint->setStyleSheet(QStringLiteral("font-size:13px;color:#667085;"));
    panelLayout->addWidget(loginTitle);
    panelLayout->addWidget(loginHint);
    panelLayout->addSpacing(14);
    auto* phoneLabel = new QLabel(QStringLiteral("手机号"));
    phoneLabel->setStyleSheet(QStringLiteral("font-size:12px;color:#475467;font-weight:500;"));
    panelLayout->addWidget(phoneLabel);
    phoneEdit_ = new QLineEdit;
    phoneEdit_->setPlaceholderText(QStringLiteral("请输入手机号"));
    phoneEdit_->setMaxLength(11);
    phoneEdit_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("1\\d{0,10}")), phoneEdit_));
    phoneEdit_->setInputMethodHints(Qt::ImhDigitsOnly);
    phoneEdit_->setFixedHeight(48);
    const auto inputStyle = QStringLiteral(
        "QLineEdit{background:#FFFFFF;border:1px solid #DDE1E6;border-radius:10px;"
        "padding:0 14px;font-size:15px;font-weight:400;}"
        "QLineEdit:focus{background:#FFFFFF;border:1px solid #0F766E;}");
    phoneEdit_->setStyleSheet(inputStyle);
    panelLayout->addWidget(phoneEdit_);
    auto* codeLabel = new QLabel(QStringLiteral("短信验证码"));
    codeLabel->setStyleSheet(QStringLiteral("font-size:12px;color:#475467;font-weight:500;padding-top:6px;"));
    panelLayout->addWidget(codeLabel);
    auto* codeRow = new QHBoxLayout;
    codeRow->setSpacing(10);
    codeEdit_ = new QLineEdit;
    codeEdit_->setPlaceholderText(QStringLiteral("请输入验证码"));
    codeEdit_->setMaxLength(6);
    codeEdit_->setMinimumWidth(0);
    codeEdit_->setFixedHeight(48);
    codeEdit_->setStyleSheet(inputStyle);
    codeButton_ = button(QStringLiteral("获取验证码"));
    codeButton_->setObjectName(QStringLiteral("secondaryButton"));
    codeButton_->setMinimumWidth(108);
    codeButton_->setFixedHeight(48);
    codeRow->addWidget(codeEdit_, 1);
    codeRow->addWidget(codeButton_);
    panelLayout->addLayout(codeRow);
    auto* login = button(QStringLiteral("登录"));
    login->setFixedHeight(48);
    panelLayout->addSpacing(8);
    panelLayout->addWidget(login);
    layout->addWidget(loginPanel);
    auto* agreement = new QLabel(QStringLiteral("登录即表示你已阅读并同意服务条款与隐私说明"));
    agreement->setWordWrap(true);
    agreement->setAlignment(Qt::AlignCenter);
    agreement->setStyleSheet(QStringLiteral("font-size:11px;color:#98A2B3;padding-top:2px;"));
    layout->addWidget(agreement);
    layout->addStretch(2);
    connect(codeButton_, &QPushButton::clicked, this, [this] {
        if (phoneEdit_->text().size() != 11 || !phoneEdit_->text().startsWith(QLatin1Char('1')))
        {
            notify(QStringLiteral("请输入正确的 11 位手机号"), true);
            return;
        }
        if (!userApi_)
        {
            codeCountdown_ = 60;
            codeButton_->setEnabled(false);
            notify(QStringLiteral("验证码已发送，请输入 %1").arg(service_.developmentCode()));
            return;
        }
        codeButton_->setEnabled(false);
        userApi_->requestSmsCode(phoneEdit_->text(), [this](ApiReply reply) {
            if (!reply.ok())
            {
                codeButton_->setEnabled(true);
                notify(reply.message, true);
                return;
            }
            codeCountdown_ = qMax(1, reply.data.toObject().value(QStringLiteral("retryAfterSec")).toInt(60));
            const QString developmentCode =
                reply.data.toObject().value(QStringLiteral("developmentCode")).toString();
            notify(developmentCode.isEmpty() ? QStringLiteral("验证码已发送，请注意查收")
                                              : QStringLiteral("开发验证码：%1").arg(developmentCode));
        });
    });
    connect(codeEdit_, &QLineEdit::textChanged, this, [login](const QString&) {
        login->setEnabled(true);
    });
    connect(login, &QPushButton::clicked, this, [this, login] {
        if (phoneEdit_->text().size() != 11)
        {
            notify(QStringLiteral("请输入正确的 11 位手机号"), true);
            return;
        }
        if (userApi_)
        {
            if (codeEdit_->text().trimmed().size() != 6)
            {
                notify(QStringLiteral("请输入 6 位验证码"), true);
                codeEdit_->setFocus();
                return;
            }
            login->setEnabled(false);
            userApi_->loginSms(phoneEdit_->text(), codeEdit_->text(), QStringLiteral("ncs-qt-user"),
                               [this, login](ApiReply reply) {
                                   login->setEnabled(true);
                                   if (!reply.ok())
                                   {
                                       if (reply.code == QStringLiteral("20"))
                                       {
                                           codeEdit_->clear();
                                           codeEdit_->setEnabled(true);
                                           codeEdit_->setFocus();
                                           notify(QStringLiteral("验证码错误，请修改后再次登录"), true);
                                           return;
                                       }
                                       if (reply.code == QStringLiteral("21"))
                                       {
                                           codeEdit_->clear();
                                           codeEdit_->setEnabled(true);
                                           codeEdit_->setFocus();
                                           notify(QStringLiteral("验证码已过期，请重新获取验证码"), true);
                                           return;
                                       }
                                       notify(reply.message, true);
                                       return;
                                   }
                                   const QString token = reply.data.toObject()
                                                             .value(QStringLiteral("accessToken"))
                                                             .toString();
                                   if (token.isEmpty())
                                   {
                                       notify(QStringLiteral("登录响应缺少会话信息"), true);
                                       return;
                                   }
                                   userApi_->setAccessToken(token);
                                   onlineSession_ = true;
                                   stationList_->requestRefresh();
                                   notify(QStringLiteral("登录成功"));
                                   restoreActiveFlow();
                               });
            return;
        }
        QString message;
        if (!service_.login(phoneEdit_->text(), codeEdit_->text(), &message))
        {
            notify(message, true);
            return;
        }
        notify(message);
        showHome();
    });
    return page;
}

} // namespace ncs::user
