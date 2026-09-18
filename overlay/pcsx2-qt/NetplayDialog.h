// SPDX-FileCopyrightText: 2026 PCSX2 Modern Netplay Port contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <QtCore/QString>
#include <QtWidgets/QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QProgressBar;
class QGroupBox;
class QSpinBox;
class QTabWidget;
class QTableWidget;

class NetplayDialog final : public QDialog
{
public:
    explicit NetplayDialog(QWidget* parent = nullptr);

private:
    void buildSetupUi();
    void buildLobbyUi();
    void refreshLobby();
    void launchHost();
    void launchClient();
    void launchNormalInstance();
    void chooseGameAndStart();
    void launchPendingNetplayGame(const QString& path);
    void openLogFolder();
    void exportDiagnostics();

    bool m_lobby_mode = false;

    // Setup mode.
    QLineEdit* m_username = nullptr;
    QTabWidget* m_tabs = nullptr;
    QSpinBox* m_host_port = nullptr;
    QSpinBox* m_host_players = nullptr;
    QSpinBox* m_host_delay = nullptr;
    QCheckBox* m_memcard_sync = nullptr;
    QLineEdit* m_join_address = nullptr;
    QSpinBox* m_join_port = nullptr;

    // Lobby mode.
    QLabel* m_room_status = nullptr;
    QLabel* m_game_status = nullptr;
    QLabel* m_memcard_status = nullptr;
    QProgressBar* m_memcard_progress = nullptr;
    QLabel* m_boot_status = nullptr;
    QLabel* m_session_status = nullptr;
    QTableWidget* m_players_table = nullptr;
    QPushButton* m_start_game = nullptr;
    QComboBox* m_local_controller = nullptr;
    QComboBox* m_topology_mode = nullptr;
    QSpinBox* m_runtime_delay = nullptr;
    QPushButton* m_runtime_toggle = nullptr;
    QGroupBox* m_runtime_group = nullptr;
    QPushButton* m_apply_runtime = nullptr;
    QString m_last_memcard_error_shown;
};
