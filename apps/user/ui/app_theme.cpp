#include "app_theme.h"

#include <QApplication>

namespace ncs::user
{

void AppTheme::apply(QApplication& application)
{
    // Use one widget renderer instead of mixing desktop theme focus decorations with QSS.
    application.setStyle(QStringLiteral("Fusion"));
    application.setStyleSheet(QStringLiteral(R"(
        QWidget {
            background: transparent;
            color: #1D2939;
            font-family: "Noto Sans CJK SC", "Microsoft YaHei", sans-serif;
        }
        QMainWindow, QDialog { background: #F6F7F9; }
        QWidget#appRoot { background: #F6F7F9; }
        QLabel { background: transparent; }
        QPushButton, QToolButton { outline: none; }
        QPushButton:focus, QToolButton:focus { border: 1px solid #0F766E; }
        QToolTip { background: #182230; color: white; padding: 6px; }
        QDialog QPushButton {
            background: #FFFFFF; color: #344054;
            border: 1px solid #D0D5DD; border-radius: 8px;
            min-height: 34px; min-width: 68px; padding: 0 14px;
            font-size: 14px;
        }
        QDialog QPushButton:hover { background: #EDF5F3; border-color: #0F766E; }
        QDialog QPushButton:pressed { background: #D8ECE6; }
        QDialog QPushButton:default {
            background: #0F766E; color: #FFFFFF; border-color: #0F766E;
        }
        QDialog QPushButton:default:hover { background: #0B625B; }
        QDialog QPushButton:default:pressed { background: #07534D; }
        QDialog QPushButton:focus { border: 2px solid #0F766E; }
        QDialog QPushButton:disabled { background: #F2F4F7; color: #98A2B3; border-color: #E7EAEE; }
        QDialogButtonBox { button-layout: 0; }
        QDialog { dialogbuttonbox-buttons-have-icons: 0; }
        QFrame#loginHero {
            background: transparent;
            border: 0;
            border-radius: 0;
        }
        QFrame#loginPanel { background: transparent; border: 0; }
        QLineEdit {
            background: #FFFFFF;
            border: 1px solid #E7EAEE;
            border-radius: 10px;
            padding: 11px 12px;
            font-size: 14px;
            selection-background-color: #0F766E;
        }
        QLineEdit:focus { border: 1px solid #0F766E; }
        QAbstractSpinBox {
            background: #FFFFFF; color: #182230;
            border: 1px solid #D0D5DD; border-radius: 8px;
            padding: 6px 12px; font-size: 15px;
            selection-background-color: #0F766E;
            selection-color: #FFFFFF;
        }
        QAbstractSpinBox:focus { border: 1px solid #0F766E; }
        QAbstractSpinBox QLineEdit { border: 0; padding: 0; background: transparent; }
        QFrame#card {
            background: #FFFFFF;
            border: 1px solid #E7EAEE;
            border-radius: 12px;
        }
        QFrame#card:hover { border-color: #BCCEC9; }
        QPushButton#secondaryButton, QPushButton#dangerButton {
            background: #FFFFFF; color: #344054;
            border: 1px solid #E7EAEE; border-radius: 10px;
            font-size: 14px; font-weight: 500; padding: 0 12px;
        }
        QPushButton#secondaryButton:hover { background: #EDF5F3; color: #0F766E; }
        QPushButton#secondaryButton:focus { border: 1px solid #0F766E; }
        QPushButton#primaryButton:focus { border: 1px solid #07534D; }
        QPushButton#dangerButton { color: #B42318; }
        QPushButton#dangerButton:hover { background: #FEF3F2; border-color: #FECDCA; }
        QPushButton#secondaryButton:disabled, QPushButton#dangerButton:disabled {
            color: #98A2B3; background: #F2F4F7;
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
