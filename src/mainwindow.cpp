#include "mainwindow.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle(QStringLiteral("Qt 6 入门演示"));
    resize(520, 330);
    setMinimumSize(420, 280);

    auto *titleLabel = new QLabel(QStringLiteral("你好，Qt！"), this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(20);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);

    auto *descriptionLabel = new QLabel(
        QStringLiteral("这个小程序演示布局、控件、信号与槽，以及 Qt 的父子对象管理。"),
        this);
    descriptionLabel->setWordWrap(true);

    auto *nameEdit = new QLineEdit(this);
    nameEdit->setPlaceholderText(QStringLiteral("输入你的名字"));
    nameEdit->setClearButtonEnabled(true);

    auto *greetingLabel = new QLabel(QStringLiteral("欢迎来到 Qt 世界！"), this);
    greetingLabel->setObjectName(QStringLiteral("greetingLabel"));

    counterLabel_ = new QLabel(this);
    counterLabel_->setAlignment(Qt::AlignCenter);
    counterLabel_->setObjectName(QStringLiteral("counterLabel"));
    updateCounter();

    auto *addButton = new QPushButton(QStringLiteral("计数 +1"), this);
    auto *resetButton = new QPushButton(QStringLiteral("重置"), this);

    auto *buttonLayout = new QHBoxLayout;
    buttonLayout->addWidget(addButton);
    buttonLayout->addWidget(resetButton);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(28, 24, 28, 24);
    mainLayout->setSpacing(14);
    mainLayout->addWidget(titleLabel);
    mainLayout->addWidget(descriptionLabel);
    mainLayout->addWidget(nameEdit);
    mainLayout->addWidget(greetingLabel);
    mainLayout->addWidget(counterLabel_, 1);
    mainLayout->addLayout(buttonLayout);

    connect(nameEdit, &QLineEdit::textChanged, this,
            [greetingLabel](const QString &name) {
                const QString displayName = name.trimmed();
                greetingLabel->setText(displayName.isEmpty()
                    ? QStringLiteral("欢迎来到 Qt 世界！")
                    : QStringLiteral("你好，%1！").arg(displayName));
            });

    connect(addButton, &QPushButton::clicked, this, [this] {
        ++count_;
        updateCounter();
    });

    connect(resetButton, &QPushButton::clicked, this, [this] {
        count_ = 0;
        updateCounter();
    });

    setStyleSheet(QStringLiteral(R"(
        QWidget {
            background: #f5f7fb;
            color: #1f2937;
            font-size: 14px;
        }
        QLineEdit {
            background: white;
            border: 1px solid #cbd5e1;
            border-radius: 7px;
            padding: 9px;
        }
        QLabel#greetingLabel {
            color: #475569;
        }
        QLabel#counterLabel {
            background: white;
            border: 1px solid #e2e8f0;
            border-radius: 10px;
            color: #2563eb;
            font-size: 28px;
            font-weight: bold;
            padding: 16px;
        }
        QPushButton {
            background: #2563eb;
            border: none;
            border-radius: 7px;
            color: white;
            padding: 10px 18px;
        }
        QPushButton:hover {
            background: #1d4ed8;
        }
        QPushButton:pressed {
            background: #1e40af;
        }
    )"));
}

void MainWindow::updateCounter()
{
    counterLabel_->setText(QStringLiteral("点击次数：%1").arg(count_));
}
