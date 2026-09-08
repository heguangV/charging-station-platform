#pragma once
#include <QWidget>
class QLabel;
class QPushButton;
class QProgressBar;
namespace ncs::admin
{
class AdminPageStatus final : public QWidget
{
    Q_OBJECT
  public:
    explicit AdminPageStatus(QWidget* parent = nullptr);
    void loading();
    void ready(const QString& text);
    void failed(const QString& text);
  signals:
    void retryRequested();

  private:
    QLabel* text_;
    QPushButton* retry_;
    QProgressBar* progress_;
};
} // namespace ncs::admin
