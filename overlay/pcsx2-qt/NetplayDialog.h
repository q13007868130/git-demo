// SPDX-FileCopyrightText: 2026 PCSX2 Modern Netplay Port contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <QtWidgets/QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

class NetplayDialog final : public QDialog
{
public:
    explicit NetplayDialog(QWidget* parent = nullptr);

private:
    void updateModeUi();
    void updateDelayHint();
    void refreshRuntimeStatus();
    void populateGameList();
    void syncSelectedGame();
    void requestStartGame();
    void launchPendingNetplayGame(const QString& path);
    void openLogFolder();
    void copyLogPath();
    void launchConfiguredInstance();
    void launchNormalInstance();

    QLabel* m_current_status = nullptr;
    QLabel* m_room_state = nullptr;
    QLabel* m_player_count = nullptr;
    QLabel* m_peer = nullptr;
    QLabel* m_runtime_delay = nullptr;
    QLabel* m_log_path = nullptr;

    QComboBox* m_game_combo = nullptr;
    QPushButton* m_sync_game = nullptr;
    QLabel* m_game_status = nullptr;
    QLabel* m_boot_status = nullptr;
    QPushButton* m_start_game = nullptr;

    QComboBox* m_mode = nullptr;
    QLineEdit* m_host = nullptr;
    QSpinBox* m_port = nullptr;
    QSpinBox* m_delay = nullptr;
    QLabel* m_delay_hint = nullptr;
    QPushButton* m_launch = nullptr;
};
