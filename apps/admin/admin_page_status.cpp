#include "admin_page_status.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
namespace ncs::admin
{
AdminPageStatus::AdminPageStatus(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 3, 0, 3);
    text_ = new QLabel;
    text_->setTextFormat(Qt::PlainText);
    text_->setWordWrap(true);
    text_->setObjectName("pageStateText");
    progress_ = new QProgressBar;
    progress_->setRange(0, 0);
    progress_->setFixedWidth(76);
    progress_->setFixedHeight(4);
    progress_->setTextVisible(false);
    retry_ = new QPushButton(QStringLiteral("重试"));
    retry_->setObjectName("secondaryButton");
    layout->addWidget(text_, 1);
    layout->addWidget(progress_);
    layout->addWidget(retry_);
    connect(retry_, &QPushButton::clicked, this, &AdminPageStatus::retryRequested);
    ready(QStringLiteral("等待加载"));
}
void AdminPageStatus::loading()
{
    text_->setText(QStringLiteral("正在加载，请稍候…"));
    text_->setStyleSheet("color:#65717B;");
    progress_->show();
    retry_->hide();
}
void AdminPageStatus::ready(const QString& text)
{
    text_->setText(text);
    text_->setStyleSheet("color:#65717B;");
    progress_->hide();
    retry_->hide();
}
void AdminPageStatus::failed(const QString& text)
{
    text_->setText(text);
    text_->setStyleSheet("color:#B42318;");
    progress_->hide();
    retry_->show();
}
} // namespace ncs::admin
