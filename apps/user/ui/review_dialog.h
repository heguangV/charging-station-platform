#pragma once

#include <QDialog>
#include <QPointer>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

namespace ncs::user
{

class UserApi;
class UserClientService;

// UC-U-12：对本人已完成并结算的订单提交一次 1～5 星评价；在线走正式 REST 契约，
// 离线演示模式写入演示服务。失败后重试沿用同一 Idempotency-Key，保证幂等。
class ReviewDialog final : public QDialog
{
    Q_OBJECT
  public:
    ReviewDialog(UserApi* userApi, UserClientService* service, const QString& orderNo,
                 const QString& stationName, QWidget* parent = nullptr);

  signals:
    // 每当确认该订单已有评价（加载发现或提交成功）时发出。
    void reviewed();

  private:
    void loadExisting();
    void submit();
    void setRating(int rating);
    void setBusy(bool busy);
    void setStatus(const QString& message, bool error);
    void showExisting(int rating, const QString& content, bool justSubmitted);
    void enableEditing(bool editable);

    UserApi* userApi_;
    UserClientService* service_;
    QString orderNo_;
    QByteArray idempotencyKey_;
    QPointer<QToolButton> stars_[5];
    QLabel* ratingLabel_ = nullptr;
    QPlainTextEdit* content_ = nullptr;
    QLabel* counter_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* retryButton_ = nullptr;
    QPushButton* submitButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
    int rating_ = 0;
    bool busy_ = false;
    bool reviewed_ = false;
};

} // namespace ncs::user
