#include "app_theme.h"

#include <QApplication>

namespace ncs::user
{

void AppTheme::apply(QApplication& application)
{
    application.setStyleSheet(QStringLiteral(R"(
        QWidget {
            background: #F3F8F7;
            color: #1D2939;
            font-family: "Noto Sans CJK SC", "Microsoft YaHei", sans-serif;
        }
        QLineEdit {
            background: #FFFFFF;
            border: 1px solid #C9DEDA;
            border-radius: 10px;
            padding: 11px 12px;
            font-size: 14px;
            selection-background-color: #0F766E;
        }
        QLineEdit:focus { border: 2px solid #14B8A6; }
        QFrame#card {
            background: #FFFFFF;
            border: 1px solid #DDEBE8;
            border-radius: 16px;
        }
        QPushButton#primaryButton {
            background: #0F766E;
            color: #FFFFFF;
            border: 0;
            border-radius: 11px;
            font-size: 15px;
            font-weight: 600;
            padding: 0 14px;
        }
        QPushButton#primaryButton:hover { background: #0B625B; }
        QPushButton#primaryButton:pressed { background: #07534D; }
        QPushButton#primaryButton:disabled { background: #AEC9C4; color: #F8FAFC; }
        QProgressBar {
            background: #DDEDEA;
            border: 0;
            border-radius: 10px;
            color: #1D2939;
            font-size: 12px;
            font-weight: 600;
            text-align: center;
        }
        QProgressBar::chunk { background: #22A06B; border-radius: 10px; }
        QTableWidget {
            background: #FFFFFF;
            alternate-background-color: #F8FAFC;
            border: 1px solid #DDEBE8;
            border-radius: 12px;
            gridline-color: #EDF4F2;
            selection-background-color: #E2F3F0;
        }
        QHeaderView::section {
            background: #EDF5F3;
            color: #475467;
            border: 0;
            border-bottom: 1px solid #DDEBE8;
            padding: 8px;
            font-weight: 600;
        }
        QScrollBar:vertical {
            background: transparent;
            width: 8px;
            margin: 5px 2px 5px 0;
        }
        QScrollBar::handle:vertical {
            background: #B8CFCA;
            min-height: 34px;
            border-radius: 4px;
        }
        QScrollBar::handle:vertical:hover { background: #719C94; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
        QScrollBar:horizontal {
            background: transparent;
            height: 8px;
            margin: 0 5px 2px 5px;
        }
        QScrollBar::handle:horizontal {
            background: #B8CFCA;
            min-width: 34px;
            border-radius: 4px;
        }
        QScrollBar::handle:horizontal:hover { background: #719C94; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
        QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }
    )"));
}

} // namespace ncs::user
