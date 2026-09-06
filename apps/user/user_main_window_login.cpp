#include "user_main_window.h"

#include "net/api_client.h"
#include "net/user_api.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QToolButton>
#include <QToolTip>
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
        setFixedSize(44, 44);
        setAttribute(Qt::WA_TranslucentBackground);
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#E2F5F0")));
        painter.drawEllipse(rect());
        painter.setPen(QPen(QColor(QStringLiteral("#0F766E")), 2.2, Qt::SolidLine, Qt::RoundCap,
                            Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(12, 14, 20, 20), 4, 4);
        painter.drawLine(QPointF(19, 11), QPointF(25, 11));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#0F766E")));
        painter.drawPolygon(QPolygonF{{24, 17}, {17, 26}, {22, 26}, {20, 33}, {29, 22}, {24, 22}});
    }
};
} // namespace

QWidget* UserMainWindow::createLoginPage()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 38, 24, 24);
    layout->setSpacing(12);
    auto* brandHeader = new QHBoxLayout;
    auto* mark = new EnergyMark;
    auto* eyebrow = new QLabel(QStringLiteral("充电服务"));
    eyebrow->setStyleSheet(QStringLiteral(
        "color:#0F766E;background:transparent;font-size:13px;font-weight:700;letter-spacing:1px;"));
    brandHeader->addWidget(mark);
    brandHeader->addSpacing(10);
    brandHeader->addWidget(eyebrow);
    brandHeader->addStretch();
    auto* title = new QLabel(QStringLiteral("为下一程，充好电"));
    title->setStyleSheet(
        QStringLiteral("color:#1E293B;background:transparent;font-size:28px;font-weight:700;"));
    auto* subtitle = new QLabel(QStringLiteral("轻松完成验证，随时安心出发"));
    subtitle->setWordWrap(true);
    subtitle->setStyleSheet(QStringLiteral("color:#667085;background:transparent;font-size:14px;"));
    layout->addLayout(brandHeader);
    layout->addSpacing(18);
    layout->addWidget(title);
    layout->addSpacing(2);
    layout->addWidget(subtitle);
    layout->addSpacing(22);
    auto* loginTitle = new QLabel(QStringLiteral("登录"));
    loginTitle->setStyleSheet(QStringLiteral("font-size:21px;font-weight:700;color:#25324A;"));
    layout->addWidget(loginTitle);
    auto* loginHint = new QLabel(QStringLiteral("使用手机号完成验证"));
    loginHint->setStyleSheet(QStringLiteral("font-size:13px;color:#667085;"));
    layout->addWidget(loginHint);
    phoneEdit_ = new QLineEdit;
    phoneEdit_->setPlaceholderText(QStringLiteral("请输入 11 位手机号"));
    phoneEdit_->setMaxLength(11);
    phoneEdit_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("1\\d{0,10}")), phoneEdit_));
    phoneEdit_->setInputMethodHints(Qt::ImhDigitsOnly);
    layout->addWidget(phoneEdit_);
    auto* codeRow = new QHBoxLayout;
    codeEdit_ = new QLineEdit;
    codeEdit_->setPlaceholderText(QStringLiteral("请输入验证码"));
    codeEdit_->setMaxLength(6);
    codeButton_ = button(QStringLiteral("获取验证码"));
    codeButton_->setMinimumWidth(120);
    codeRow->addWidget(codeEdit_);
    codeRow->addWidget(codeButton_);
    layout->addLayout(codeRow);
    auto* login = button(QStringLiteral("登录"));
    layout->addWidget(login);
    auto* demoHelp = new QToolButton;
    demoHelp->setText(QStringLiteral("ⓘ 演示说明"));
    demoHelp->setToolTip(userApi_ ? QStringLiteral("开发环境会在获取验证码后显示本次验证码。")
                                  : QStringLiteral("演示验证码为 123456。"));
    demoHelp->setCursor(Qt::PointingHandCursor);
    demoHelp->setStyleSheet(
        QStringLiteral("QToolButton{color:#0F766E;border:0;background:transparent;font-size:12px;"
                       "padding:3px 0;text-align:left;}"));
    layout->addWidget(demoHelp, 0, Qt::AlignLeft);
    layout->addStretch();
    auto* agreement = new QLabel(QStringLiteral("登录即表示你同意 NCS 服务条款与隐私说明"));
    agreement->setAlignment(Qt::AlignCenter);
    agreement->setStyleSheet(QStringLiteral("font-size:11px;color:#98A2B3;"));
    layout->addWidget(agreement);
    connect(
        codeButton_, &QPushButton::clicked, this,
        [this]
        {
            if (phoneEdit_->text().size() != 11 || !phoneEdit_->text().startsWith(QLatin1Char('1')))
            {
                notify(QStringLiteral("请输入正确的 11 位手机号"), true);
                return;
            }
            codeButton_->setEnabled(false);
            if (!userApi_)
            {
                codeCountdown_ = 60;
                notify(QStringLiteral("验证码已发送，请输入 %1").arg(service_.developmentCode()));
                return;
            }
            userApi_->requestSmsCode(
                phoneEdit_->text(),
                [this](ApiReply reply)
                {
                    if (!reply.ok())
                    {
                        codeButton_->setEnabled(true);
                        notify(reply.message + QStringLiteral("；可在服务恢复前使用本机演示"),
                               true);
                        return;
                    }
                    codeCountdown_ = qMax(
                        1, reply.data.toObject().value(QStringLiteral("retryAfterSec")).toInt(60));
                    const QString developmentCode =
                        reply.data.toObject().value(QStringLiteral("developmentCode")).toString();
                    notify(developmentCode.isEmpty()
                               ? QStringLiteral("验证码已发送")
                               : QStringLiteral("验证码已发送，请输入 %1").arg(developmentCode));
                });
        });
    connect(demoHelp, &QToolButton::clicked, this,
            [demoHelp]
            {
                QToolTip::showText(demoHelp->mapToGlobal(QPoint(0, demoHelp->height())),
                                   demoHelp->toolTip(), demoHelp);
            });
    connect(
        login, &QPushButton::clicked, this,
        [this, login]
        {
            if (phoneEdit_->text().size() != 11)
            {
                notify(QStringLiteral("请输入正确的 11 位手机号"), true);
                return;
            }
            if (!userApi_)
            {
                QString message;
                if (!service_.login(phoneEdit_->text(), codeEdit_->text(), &message))
                {
                    notify(message, true);
                    return;
                }
                notify(message);
                showHome();
                return;
            }
            const QString phone = phoneEdit_->text();
            const QString smsCode = codeEdit_->text();
            login->setEnabled(false);
            userApi_->loginSms(
                phone, smsCode, QStringLiteral("ncs-user-desktop"),
                [this, login, phone, smsCode](ApiReply reply)
                {
                    login->setEnabled(true);
                    if (reply.ok())
                    {
                        const QString token =
                            reply.data.toObject().value(QStringLiteral("accessToken")).toString();
                        if (token.isEmpty())
                        {
                            notify(QStringLiteral("服务端登录响应缺少会话令牌"), true);
                            return;
                        }
                        userApi_->setAccessToken(token);
                        onlineSession_ = true;
                        notify(QStringLiteral("登录成功，腾讯地图路线服务已连接"));
                        showHome();
                        return;
                    }
                    if (reply.code != QStringLiteral("NetworkError"))
                    {
                        notify(reply.message, true);
                        return;
                    }
                    QString message;
                    if (!service_.login(phone, smsCode, &message))
                    {
                        notify(message, true);
                        return;
                    }
                    onlineSession_ = false;
                    notify(message + QStringLiteral("；服务端不可用，已进入本机降级模式"));
                    showHome();
                });
        });
    return page;
}

} // namespace ncs::user
