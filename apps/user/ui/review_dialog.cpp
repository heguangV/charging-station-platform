#include "review_dialog.h"

#include "../net/user_api.h"
#include "../user_demo_service.h"

#include <QByteArray>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

namespace ncs::user
{
namespace
{
constexpr int kMaxReviewCodePoints = 500;

// 服务端按 Unicode 码点计数，QPlainTextEdit 无法直接限制，这里统一截断。
QString clipToCodePoints(const QString& text, int maxCodePoints)
{
    const auto points = text.toUcs4();
    if (points.size() <= maxCodePoints)
        return text;
    return QString::fromUcs4(reinterpret_cast<const char32_t*>(points.constData()), maxCodePoints);
}

QString ratingText(int rating)
{
    switch (rating)
    {
    case 1:
        return QStringLiteral("很差");
    case 2:
        return QStringLiteral("不满意");
    case 3:
        return QStringLiteral("一般");
    case 4:
        return QStringLiteral("满意");
    case 5:
        return QStringLiteral("非常满意");
    default:
        return QStringLiteral("点击星星选择评分");
    }
}
} // namespace

ReviewDialog::ReviewDialog(UserApi* userApi, UserClientService* service, const QString& orderNo,
                           const QString& stationName, QWidget* parent)
    : QDialog(parent), userApi_(userApi), service_(service), orderNo_(orderNo),
      idempotencyKey_(QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8())
{
    setWindowTitle(QStringLiteral("评价充电体验"));
    setModal(true);
    setMinimumWidth(430);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(9);

    auto* title = new QLabel(stationName, this);
    title->setStyleSheet(QStringLiteral("font-size:17px;font-weight:700;color:#243F30;"));
    auto* subtitle = new QLabel(QStringLiteral("订单号  %1").arg(orderNo_), this);
    subtitle->setStyleSheet(QStringLiteral("font-size:12px;color:#607362;"));
    layout->addWidget(title);
    layout->addWidget(subtitle);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->hide();
    retryButton_ = new QPushButton(QStringLiteral("重试"), this);
    retryButton_->hide();
    retryButton_->setCursor(Qt::PointingHandCursor);
    retryButton_->setStyleSheet(
        QStringLiteral("QPushButton{background:#E4F0DC;color:#23794E;border:0;border-radius:9px;"
                       "font-size:13px;font-weight:700;padding:6px 14px;}"));
    auto* statusRow = new QHBoxLayout;
    statusRow->addWidget(status_, 1);
    statusRow->addWidget(retryButton_);
    layout->addLayout(statusRow);

    auto* starsRow = new QHBoxLayout;
    starsRow->setSpacing(6);
    for (int index = 0; index < 5; ++index)
    {
        auto* star = new QToolButton(this);
        star->setText(QStringLiteral("★"));
        star->setCursor(Qt::PointingHandCursor);
        star->setAutoRaise(true);
        star->setStyleSheet(
            QStringLiteral("QToolButton{border:0;background:transparent;font-size:30px;"
                           "color:#C9D6CE;padding:2px;}"));
        connect(star, &QToolButton::clicked, this, [this, index] { setRating(index + 1); });
        stars_[index] = star;
        starsRow->addWidget(star);
    }
    starsRow->addStretch();
    layout->addLayout(starsRow);

    ratingLabel_ = new QLabel(ratingText(0), this);
    ratingLabel_->setStyleSheet(QStringLiteral("font-size:13px;color:#52716C;"));
    layout->addWidget(ratingLabel_);

    content_ = new QPlainTextEdit(this);
    content_->setPlaceholderText(
        QStringLiteral("分享你的充电体验：桩况、速度、服务…（1～500 字）"));
    content_->setFixedHeight(110);
    content_->setStyleSheet(
        QStringLiteral("QPlainTextEdit{background:#FFFFFF;border:1px solid #DCE9DF;"
                       "border-radius:10px;padding:8px;font-size:14px;color:#243F30;}"));
    layout->addWidget(content_);

    counter_ = new QLabel(QStringLiteral("0/%1").arg(kMaxReviewCodePoints), this);
    counter_->setAlignment(Qt::AlignRight);
    counter_->setStyleSheet(QStringLiteral("font-size:12px;color:#8A9A90;"));
    layout->addWidget(counter_);

    connect(content_, &QPlainTextEdit::textChanged, this,
            [this]
            {
                const QString clipped =
                    clipToCodePoints(content_->toPlainText(), kMaxReviewCodePoints);
                if (clipped != content_->toPlainText())
                {
                    const int position = content_->textCursor().position();
                    content_->setPlainText(clipped);
                    QTextCursor cursor = content_->textCursor();
                    cursor.setPosition(qMin(position, clipped.size()));
                    content_->setTextCursor(cursor);
                }
                counter_->setText(
                    QStringLiteral("%1/%2").arg(clipped.toUcs4().size()).arg(kMaxReviewCodePoints));
            });

    auto* buttons = new QHBoxLayout;
    submitButton_ = new QPushButton(QStringLiteral("提交评价"), this);
    submitButton_->setObjectName(QStringLiteral("primaryButton"));
    submitButton_->setMinimumHeight(42);
    submitButton_->setCursor(Qt::PointingHandCursor);
    closeButton_ = new QPushButton(QStringLiteral("关闭"), this);
    closeButton_->setMinimumHeight(42);
    closeButton_->setStyleSheet(
        QStringLiteral("QPushButton{background:#EEF3EC;color:#475467;border:0;border-radius:9px;"
                       "font-size:14px;padding:9px 18px;}"));
    buttons->addWidget(submitButton_, 1);
    buttons->addWidget(closeButton_);
    layout->addLayout(buttons);

    connect(submitButton_, &QPushButton::clicked, this, &ReviewDialog::submit);
    connect(closeButton_, &QPushButton::clicked, this, &QDialog::close);
    connect(retryButton_, &QPushButton::clicked, this,
            [this]
            {
                retryButton_->hide();
                loadExisting();
            });

    setRating(0);
    loadExisting();
}

void ReviewDialog::loadExisting()
{
    if (!userApi_)
    {
        Q_ASSERT(service_);
        OrderReviewRecord record;
        if (service_->orderReview(orderNo_, &record))
        {
            showExisting(record.rating, record.content, false);
            emit reviewed();
        }
        else
        {
            enableEditing(true);
            setStatus(QString(), false);
        }
        return;
    }

    setBusy(true);
    setStatus(QStringLiteral("正在加载评价…"), false);
    QPointer<ReviewDialog> self(this);
    userApi_->orderReview(
        orderNo_,
        [this, self](ApiReply reply)
        {
            if (!self)
                return;
            setBusy(false);
            if (!reply.ok())
            {
                setStatus(QStringLiteral("评价加载失败：%1").arg(reply.message), true);
                retryButton_->show();
                return;
            }
            const QJsonValue review = reply.data.toObject().value(QStringLiteral("review"));
            if (review.isObject())
            {
                showExisting(review.toObject().value(QStringLiteral("rating")).toInt(),
                             review.toObject().value(QStringLiteral("content")).toString(), false);
                emit reviewed();
                return;
            }
            enableEditing(true);
            setStatus(QString(), false);
        });
}

void ReviewDialog::submit()
{
    const QString content = content_->toPlainText().trimmed();
    if (rating_ < 1 || rating_ > 5 || content.isEmpty())
    {
        setStatus(QStringLiteral("请选择 1～5 星并填写 1～500 字评价"), true);
        return;
    }
    if (busy_)
        return;

    if (!userApi_)
    {
        Q_ASSERT(service_);
        QString message;
        if (service_->submitOrderReview(orderNo_, rating_, content, &message))
        {
            showExisting(rating_, content, true);
            emit reviewed();
        }
        else
        {
            setStatus(message.isEmpty() ? QStringLiteral("提交失败，请稍后重试") : message, true);
        }
        return;
    }

    setBusy(true);
    setStatus(QStringLiteral("正在提交评价…"), false);
    QPointer<ReviewDialog> self(this);
    userApi_->submitOrderReview(
        orderNo_, rating_, content, idempotencyKey_,
        [this, self, content](ApiReply reply)
        {
            if (!self)
                return;
            if (reply.ok())
            {
                setBusy(false);
                showExisting(rating_, content, true);
                emit reviewed();
                return;
            }
            setBusy(false);
            if (reply.code == QStringLiteral("5"))
            {
                // 已有评价：重新读取服务端内容做只读展示。
                setStatus(QStringLiteral("该订单已评价，评价内容不可修改"), true);
                loadExisting();
                return;
            }
            // 失败保留草稿；重试沿用同一 Idempotency-Key，避免重复提交。
            setStatus(reply.message.isEmpty() ? QStringLiteral("提交失败，请重试") : reply.message,
                      true);
        });
}

void ReviewDialog::setRating(int rating)
{
    rating_ = rating;
    for (int index = 0; index < 5; ++index)
    {
        if (stars_[index])
            stars_[index]->setStyleSheet(
                QStringLiteral("QToolButton{border:0;background:transparent;font-size:30px;"
                               "color:%1;padding:2px;}")
                    .arg(index < rating ? QStringLiteral("#23794E") : QStringLiteral("#C9D6CE")));
    }
    ratingLabel_->setText(ratingText(rating));
}

void ReviewDialog::setBusy(bool busy)
{
    busy_ = busy;
    submitButton_->setEnabled(!busy);
    submitButton_->setText(busy ? QStringLiteral("处理中…") : QStringLiteral("提交评价"));
    const bool interactive = !busy && !reviewed_;
    for (const QPointer<QToolButton>& star : stars_)
    {
        if (star)
            star->setEnabled(interactive);
    }
    content_->setReadOnly(!interactive);
}

void ReviewDialog::setStatus(const QString& message, bool error)
{
    status_->setText(message);
    status_->setStyleSheet(QStringLiteral("font-size:12px;color:%1;")
                               .arg(error ? QStringLiteral("#B42318") : QStringLiteral("#52716C")));
    status_->setVisible(!message.isEmpty());
    if (error)
        retryButton_->hide();
}

void ReviewDialog::showExisting(int rating, const QString& content, bool justSubmitted)
{
    reviewed_ = true;
    setRating(rating);
    content_->setPlainText(content);
    counter_->setText(
        QStringLiteral("%1/%2").arg(content.toUcs4().size()).arg(kMaxReviewCodePoints));
    enableEditing(false);
    setStatus(justSubmitted ? QStringLiteral("评价已提交，感谢你的反馈！")
                            : QStringLiteral("该订单已评价，评价内容不可修改"),
              false);
    submitButton_->hide();
}

void ReviewDialog::enableEditing(bool editable)
{
    submitButton_->setVisible(editable);
    submitButton_->setEnabled(!busy_);
    for (const QPointer<QToolButton>& star : stars_)
    {
        if (star)
            star->setEnabled(editable && !busy_);
    }
    content_->setReadOnly(!editable || busy_);
}

} // namespace ncs::user
