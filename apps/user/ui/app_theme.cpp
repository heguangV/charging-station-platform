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
            color: #18212B;
            font-family: "Noto Sans CJK SC", "Microsoft YaHei", sans-serif;
        }
        QMainWindow, QDialog { background: #F3F7EE; }
        QWidget#appRoot { background: #F5F6F8; }
        QLabel { background: transparent; }
        QPushButton, QToolButton { outline: none; }
        QPushButton:focus, QToolButton:focus { border: 1px solid #23794E; }
        QToolTip { background: #243F30; color: white; padding: 6px; }
        QDialog QPushButton {
            background: #FFFFFF; color: #344054;
            border: 1px solid #D0D5DD; border-radius: 8px;
            min-height: 34px; min-width: 68px; padding: 0 14px;
            font-size: 14px;
        }
        QDialog QPushButton:hover { background: #EDF5E8; border-color: #23794E; }
        QDialog QPushButton:pressed { background: #D5E8CA; }
        QDialog QPushButton:default {
            background: #23794E; color: #FFFFFF; border-color: #23794E;
        }
        QDialog QPushButton:default:hover { background: #1D6541; }
        QDialog QPushButton:default:pressed { background: #174F34; }
        QDialog QPushButton:focus { border: 2px solid #23794E; }
        QDialog QPushButton:disabled { background: #F2F4F7; color: #98A2B3; border-color: #D8E3D3; }
        QDialogButtonBox { button-layout: 0; }
        QDialog { dialogbuttonbox-buttons-have-icons: 0; }
        QFrame#loginHero {
            background: transparent;
            border: 0;
            border-radius: 0;
        }
        QFrame#loginPanel { background: transparent; border: 0; }
        QLineEdit {
            background: #F6F7F9;
            border: 1px solid #E5E8EC;
            border-radius: 10px;
            padding: 11px 12px;
            font-size: 14px;
            selection-background-color: #23794E;
        }
        QLineEdit:focus { border: 1px solid #23794E; }
        QAbstractSpinBox {
            background: #FFFFFF; color: #18212B;
            border: 1px solid #D0D5DD; border-radius: 8px;
            padding: 6px 12px; font-size: 15px;
            selection-background-color: #23794E;
            selection-color: #FFFFFF;
        }
        QAbstractSpinBox:focus { border: 1px solid #23794E; }
        QAbstractSpinBox QLineEdit { border: 0; padding: 0; background: transparent; }
        QFrame#card { background: #FFFFFF; border: 1px solid #E8EBEF; border-radius: 18px; }
        QFrame#card:hover { border-color: #BBC5CD; }
        QPushButton#secondaryButton, QPushButton#dangerButton {
            background: #FFFFFF; color: #344054;
            border: 1px solid #D2DFD7; border-radius: 14px;
            font-size: 14px; font-weight: 500; padding: 0 12px;
        }
        QPushButton#secondaryButton:hover { background: #EDF5E8; color: #23794E; }
        QPushButton#secondaryButton:focus { border: 1px solid #23794E; }
        QPushButton#primaryButton:focus { border: 1px solid #174F34; }
        QPushButton#dangerButton { color: #B42318; }
        QPushButton#dangerButton:hover { background: #FEF3F2; border-color: #FECDCA; }
        QPushButton#secondaryButton:disabled, QPushButton#dangerButton:disabled {
            color: #98A2B3; background: #F2F4F7;
        }
        QPushButton#primaryButton {
            background: #102F46;
            color: #FFFFFF;
            border: 0;
            border-radius: 14px;
            font-size: 15px;
            font-weight: 600;
            padding: 0 14px;
        }
        QPushButton#primaryButton:hover { background: #1E4764; }
        QPushButton#primaryButton:pressed { background: #0C2436; }
        QPushButton#primaryButton:disabled { background: #AEC9C4; color: #F8FAFC; }
        QProgressBar {
            background: #DDEDEA;
            border: 0;
            border-radius: 10px;
            color: #18212B;
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
            selection-background-color: #E4F0DC;
        }
        QHeaderView::section {
            background: #EDF5E8;
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
