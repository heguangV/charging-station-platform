#pragma once

#include <QWidget>

class QLabel;

class MainWindow final : public QWidget
{
public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    void updateCounter();

    QLabel *counterLabel_ = nullptr;
    int count_ = 0;
};
