// SPDX-FileCopyrightText: 2026 PCSX2 Modern Netplay Port contributors
// SPDX-License-Identifier: GPL-3.0+

#include "NetplayDialog.h"

#include "Netplay/ModernNetplay.h"
#include "QtHost.h"
#include "pcsx2/GameList.h"
#include "pcsx2/VMManager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <memory>

namespace
{
class GameSelectionDialog final : public QDialog
{
public:
    explicit GameSelectionDialog(QWidget* parent)
        : QDialog(parent)
    {
        setWindowTitle(tr("选择联机游戏"));
        resize(780, 460);

        auto* root = new QVBoxLayout(this);
        auto* tip = new QLabel(tr("请选择房主要启动的游戏。其他玩家会按 Serial + CRC 自动匹配自己电脑上的同一游戏。"), this);
        tip->setWordWrap(true);
        root->addWidget(tip);

        m_table = new QTableWidget(this);
        m_table->setColumnCount(4);
        m_table->setHorizontalHeaderLabels({tr("游戏名称"), tr("Serial"), tr("CRC"), tr("本机路径")});
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setSelectionMode(QAbstractItemView::SingleSelection);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->verticalHeader()->setVisible(false);
        m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
        root->addWidget(m_table, 1);

        {
            auto lock = GameList::GetLock();
            const u32 count = GameList::GetEntryCount();
            for (u32 i = 0; i < count; i++)
            {
                const GameList::Entry* entry = GameList::GetEntryByIndex(i);
                if (!entry || !entry->IsDisc() || entry->serial.empty() || entry->crc == 0)
                    continue;
                const int row = m_table->rowCount();
                m_table->insertRow(row);
                auto* title = new QTableWidgetItem(QString::fromStdString(entry->GetTitle()));
                title->setData(Qt::UserRole, QString::fromStdString(entry->path));
                m_table->setItem(row, 0, title);
                m_table->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(entry->serial)));
                m_table->setItem(row, 2, new QTableWidgetItem(QStringLiteral("%1").arg(entry->crc, 8, 16, QLatin1Char('0')).toUpper()));
                m_table->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(entry->path)));
            }
        }

        auto* buttons = new QDialogButtonBox(this);
        m_start = buttons->addButton(tr("选择并开始"), QDialogButtonBox::AcceptRole);
        buttons->addButton(tr("取消"), QDialogButtonBox::RejectRole);
        root->addWidget(buttons);

        connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
            if (m_table->currentRow() < 0)
            {
                QMessageBox::information(this, tr("联机"), tr("请先选择一个游戏。"));
                return;
            }
            accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
            if (row >= 0)
            {
                m_table->selectRow(row);
                accept();
            }
        });
        if (m_table->rowCount() > 0)
        {
            m_table->selectRow(0);
            m_table->setCurrentCell(0, 0);
        }
        else
        {
            m_start->setEnabled(false);
            tip->setText(tr("没有找到可联机的光盘游戏。请先在 PCSX2 主界面配置游戏目录并完成扫描。"));
        }
    }

    QString selectedPath() const
    {
        const int row = m_table->currentRow();
        if (row < 0 || !m_table->item(row, 0))
            return {};
        return m_table->item(row, 0)->data(Qt::UserRole).toString();
    }

private:
    QTableWidget* m_table = nullptr;
    QPushButton* m_start = nullptr;
};
} // namespace

NetplayDialog::NetplayDialog(QWidget* parent)
    : QDialog(parent)
{
    m_lobby_mode = ModernNetplay::IsConfigured();
    setWindowTitle(m_lobby_mode ? tr("PCSX2 联机房间") : tr("PCSX2 联机"));
    setModal(true);

    if (m_lobby_mode)
        buildLobbyUi();
    else
        buildSetupUi();
}

void NetplayDialog::buildSetupUi()
{
    resize(540, 360);
    auto* root = new QVBoxLayout(this);

    auto* title = new QLabel(tr("PCSX2 联机"), this);
    QFont title_font = title->font();
    title_font.setPointSize(title_font.pointSize() + 3);
    title_font.setBold(true);
    title->setFont(title_font);
    root->addWidget(title);

    auto* user_form = new QFormLayout();
    m_username = new QLineEdit(this);
    m_username->setMaxLength(38);
    m_username->setText(qEnvironmentVariable("USERNAME", tr("玩家")));
    user_form->addRow(tr("玩家名称："), m_username);
    root->addLayout(user_form);

    m_tabs = new QTabWidget(this);

    auto* host_tab = new QWidget(m_tabs);
    auto* host_form = new QFormLayout(host_tab);
    m_host_port = new QSpinBox(host_tab);
    m_host_port->setRange(1, 65535);
    m_host_port->setValue(27886);
    host_form->addRow(tr("监听端口："), m_host_port);

    m_host_players = new QSpinBox(host_tab);
    m_host_players->setRange(1, 4);
    m_host_players->setValue(2);
    m_host_players->setSuffix(tr(" 人"));
    host_form->addRow(tr("房间人数："), m_host_players);

    m_host_delay = new QSpinBox(host_tab);
    m_host_delay->setRange(1, 12);
    m_host_delay->setValue(2);
    m_host_delay->setSuffix(tr(" 帧"));
    host_form->addRow(tr("输入延迟："), m_host_delay);

    m_memcard_sync = new QCheckBox(tr("同步房主 1 号记忆卡（使用临时副本，不修改原记忆卡）"), host_tab);
    m_memcard_sync->setChecked(true);
    host_form->addRow(QString(), m_memcard_sync);

    auto* host_note = new QLabel(tr("3～4 人房间会自动启用 Multitap。游戏开始后会锁定房间，所有玩家必须使用相同 Serial + CRC 的游戏。"), host_tab);
    host_note->setWordWrap(true);
    host_form->addRow(QString(), host_note);

    auto* host_button = new QPushButton(tr("创建房间"), host_tab);
    host_button->setDefault(true);
    host_form->addRow(QString(), host_button);
    connect(host_button, &QPushButton::clicked, this, [this]() { launchHost(); });
    m_tabs->addTab(host_tab, tr("创建房间"));

    auto* join_tab = new QWidget(m_tabs);
    auto* join_form = new QFormLayout(join_tab);
    m_join_address = new QLineEdit(join_tab);
    m_join_address->setPlaceholderText(tr("例如 192.168.1.20 或主机名"));
    join_form->addRow(tr("主机地址："), m_join_address);
    m_join_port = new QSpinBox(join_tab);
    m_join_port->setRange(1, 65535);
    m_join_port->setValue(27886);
    join_form->addRow(tr("主机端口："), m_join_port);

    auto* join_note = new QLabel(tr("人数、输入延迟和记忆卡同步规则由房主统一决定。连接后等待房主选择游戏即可。"), join_tab);
    join_note->setWordWrap(true);
    join_form->addRow(QString(), join_note);

    auto* join_button = new QPushButton(tr("加入房间"), join_tab);
    join_form->addRow(QString(), join_button);
    connect(join_button, &QPushButton::clicked, this, [this]() { launchClient(); });
    m_tabs->addTab(join_tab, tr("加入房间"));

    root->addWidget(m_tabs, 1);

    auto* bottom = new QHBoxLayout();
    bottom->addStretch(1);
    auto* cancel = new QPushButton(tr("取消"), this);
    bottom->addWidget(cancel);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    root->addLayout(bottom);
}

void NetplayDialog::buildLobbyUi()
{
    resize(660, 470);
    auto* root = new QVBoxLayout(this);

    m_room_status = new QLabel(this);
    m_room_status->setWordWrap(true);
    root->addWidget(m_room_status);

    m_players_table = new QTableWidget(this);
    m_players_table->setColumnCount(5);
    m_players_table->setHorizontalHeaderLabels({tr("玩家"), tr("名称"), tr("游戏"), tr("记忆卡"), tr("启动")});
    m_players_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_players_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_players_table->verticalHeader()->setVisible(false);
    m_players_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_players_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_players_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_players_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_players_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    root->addWidget(m_players_table, 1);

    auto* state_group = new QGroupBox(tr("同步状态"), this);
    auto* state_form = new QFormLayout(state_group);
    m_game_status = new QLabel(state_group);
    m_game_status->setWordWrap(true);
    m_memcard_status = new QLabel(state_group);
    m_memcard_status->setWordWrap(true);
    m_boot_status = new QLabel(state_group);
    m_boot_status->setWordWrap(true);
    m_session_status = new QLabel(state_group);
    m_session_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    state_form->addRow(tr("游戏："), m_game_status);
    state_form->addRow(tr("记忆卡："), m_memcard_status);
    state_form->addRow(tr("启动："), m_boot_status);
    state_form->addRow(tr("会话："), m_session_status);
    root->addWidget(state_group);

    auto* buttons = new QHBoxLayout();
    auto* log_button = new QPushButton(tr("打开日志"), this);
    auto* diag_button = new QPushButton(tr("导出诊断包"), this);
    auto* retry_button = new QPushButton(tr("重建连接"), this);
    m_start_game = new QPushButton(tr("选择游戏并开始..."), this);
    auto* leave_button = new QPushButton(tr("退出联机"), this);
    buttons->addWidget(log_button);
    buttons->addWidget(diag_button);
    buttons->addWidget(retry_button);
    buttons->addStretch(1);
    buttons->addWidget(m_start_game);
    buttons->addWidget(leave_button);
    root->addLayout(buttons);

    connect(log_button, &QPushButton::clicked, this, [this]() { openLogFolder(); });
    connect(diag_button, &QPushButton::clicked, this, [this]() { exportDiagnostics(); });
    connect(retry_button, &QPushButton::clicked, this, [this]() {
        if (QtHost::IsVMValid())
        {
            QMessageBox::information(this, tr("联机"), tr("请先停止当前游戏，再重建联机连接。"));
            return;
        }
        if (!ModernNetplay::RestartSession())
            QMessageBox::warning(this, tr("联机"), tr("无法重建联机连接。"));
        refreshLobby();
    });
    connect(m_start_game, &QPushButton::clicked, this, [this]() { chooseGameAndStart(); });
    connect(leave_button, &QPushButton::clicked, this, [this]() { launchNormalInstance(); });

    const ModernNetplay::StatusSnapshot initial_status = ModernNetplay::GetStatusSnapshot();
    if (!QtHost::IsVMValid() && (initial_status.failed || initial_status.start_committed))
        ModernNetplay::RestartSession();
    else
        ModernNetplay::StartSessionAsync();
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this]() { refreshLobby(); });
    timer->start(200);
    refreshLobby();
}

void NetplayDialog::launchHost()
{
    if (m_username->text().trimmed().isEmpty())
    {
        QMessageBox::information(this, tr("联机"), tr("请输入玩家名称。"));
        return;
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PCSX2_NETPLAY_MODE"), QStringLiteral("host"));
    env.insert(QStringLiteral("PCSX2_NETPLAY_USERNAME"), m_username->text().trimmed());
    env.insert(QStringLiteral("PCSX2_NETPLAY_PORT"), QString::number(m_host_port->value()));
    env.insert(QStringLiteral("PCSX2_NETPLAY_PLAYERS"), QString::number(m_host_players->value()));
    env.insert(QStringLiteral("PCSX2_NETPLAY_DELAY"), QString::number(m_host_delay->value()));
    env.insert(QStringLiteral("PCSX2_NETPLAY_MEMCARD_SYNC"), m_memcard_sync->isChecked() ? QStringLiteral("1") : QStringLiteral("0"));
    env.insert(QStringLiteral("PCSX2_NETPLAY_SHOW_LOBBY"), QStringLiteral("1"));
    env.remove(QStringLiteral("PCSX2_NETPLAY_HOST"));

    QProcess process;
    process.setProcessEnvironment(env);
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setWorkingDirectory(QCoreApplication::applicationDirPath());
    if (!process.startDetached())
    {
        QMessageBox::critical(this, tr("联机"), tr("无法启动联机房间实例。"));
        return;
    }
    accept();
    QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
}

void NetplayDialog::launchClient()
{
    if (m_username->text().trimmed().isEmpty() || m_join_address->text().trimmed().isEmpty())
    {
        QMessageBox::information(this, tr("联机"), tr("请输入玩家名称和主机地址。"));
        return;
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PCSX2_NETPLAY_MODE"), QStringLiteral("client"));
    env.insert(QStringLiteral("PCSX2_NETPLAY_USERNAME"), m_username->text().trimmed());
    env.insert(QStringLiteral("PCSX2_NETPLAY_HOST"), m_join_address->text().trimmed());
    env.insert(QStringLiteral("PCSX2_NETPLAY_PORT"), QString::number(m_join_port->value()));
    env.insert(QStringLiteral("PCSX2_NETPLAY_SHOW_LOBBY"), QStringLiteral("1"));

    QProcess process;
    process.setProcessEnvironment(env);
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setWorkingDirectory(QCoreApplication::applicationDirPath());
    if (!process.startDetached())
    {
        QMessageBox::critical(this, tr("联机"), tr("无法启动联机客户端实例。"));
        return;
    }
    accept();
    QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
}

void NetplayDialog::chooseGameAndStart()
{
    const ModernNetplay::StatusSnapshot status = ModernNetplay::GetStatusSnapshot();
    if (status.role != "host" || !status.room_full || QtHost::IsVMValid())
        return;

    GameSelectionDialog picker(this);
    if (picker.exec() != QDialog::Accepted)
        return;
    const QString path = picker.selectedPath();
    if (path.isEmpty())
        return;

    std::string local_path;
    std::string title;
    std::string serial;
    u32 crc = 0;
    {
        auto lock = GameList::GetLock();
        const std::string path_string = path.toStdString();
        const GameList::Entry* entry = GameList::GetEntryForPath(path_string.c_str());
        if (!entry)
        {
            QMessageBox::warning(this, tr("联机"), tr("所选游戏已经不在当前游戏列表中。"));
            return;
        }
        local_path = entry->path;
        title = entry->GetTitle();
        serial = entry->serial;
        crc = entry->crc;
    }

    if (!ModernNetplay::HostSelectGame(local_path, title, serial, crc) || !ModernNetplay::RequestSynchronizedBoot())
    {
        QMessageBox::warning(this, tr("联机"), tr("无法开始联机游戏。请确认房间已满，并且当前没有正在运行的游戏。"));
    }
}

void NetplayDialog::refreshLobby()
{
    const ModernNetplay::StatusSnapshot status = ModernNetplay::GetStatusSnapshot();
    const bool is_host = (status.role == "host");

    if (status.failed)
        m_room_status->setText(tr("连接错误：%1 · 可点击“重建连接”恢复，无需重启 PCSX2").arg(QString::fromStdString(status.last_error)));
    else if (!status.last_error.empty() && !status.start_requested)
        m_room_status->setText(tr("%1 · 房间仍可继续使用").arg(QString::fromStdString(status.last_error)));
    else if (status.room_full)
        m_room_status->setText(tr("房间已就绪 · %1 / %2 人 · 输入延迟 %3 帧").arg(status.player_count).arg(status.max_players).arg(status.delay));
    else if (is_host)
        m_room_status->setText(tr("等待玩家加入… · %1 / %2 人 · TCP %3").arg(status.player_count).arg(status.max_players).arg(status.port));
    else if (status.connected)
        m_room_status->setText(tr("已连接房主，等待其他玩家… · %1 / %2 人").arg(status.player_count).arg(status.max_players));
    else
        m_room_status->setText(tr("正在连接房主…"));

    m_players_table->setRowCount(static_cast<int>(status.max_players));
    for (std::uint32_t i = 0; i < status.max_players; i++)
    {
        const ModernNetplay::PlayerSnapshot& player = status.players[i];
        m_players_table->setItem(static_cast<int>(i), 0, new QTableWidgetItem(tr("P%1").arg(i + 1)));
        m_players_table->setItem(static_cast<int>(i), 1, new QTableWidgetItem(player.connected ? QString::fromStdString(player.name) : tr("等待加入")));
        m_players_table->setItem(static_cast<int>(i), 2, new QTableWidgetItem(player.connected ? (player.game_match ? tr("匹配 ✓") : tr("等待")) : QStringLiteral("-")));
        m_players_table->setItem(static_cast<int>(i), 3, new QTableWidgetItem(player.connected ? (player.memcard_ready ? tr("完成 ✓") : tr("等待")) : QStringLiteral("-")));
        m_players_table->setItem(static_cast<int>(i), 4, new QTableWidgetItem(player.connected ? (player.boot_ready ? tr("就绪 ✓") : tr("等待")) : QStringLiteral("-")));
    }

    if (!status.game_selected)
    {
        m_game_status->setText(is_host ? tr("尚未选择。点击“选择游戏并开始...”后再选择。") : tr("等待房主选择游戏。"));
    }
    else
    {
        const QString crc = QStringLiteral("%1").arg(status.game_crc, 8, 16, QLatin1Char('0')).toUpper();
        m_game_status->setText(tr("%1 · %2 · CRC %3 · %4")
            .arg(QString::fromStdString(status.game_title), QString::fromStdString(status.game_serial), crc,
                status.all_games_match ? tr("全员匹配 ✓") : tr("正在等待其他玩家匹配")));
    }

    if (!status.memory_card_sync_enabled)
        m_memcard_status->setText(tr("本房间未启用记忆卡同步。"));
    else if (status.memory_card_all_ready)
        m_memcard_status->setText(status.memory_card_present ? tr("全员同步完成 ✓ · 临时副本 · %1 KB").arg(status.memory_card_size / 1024) : tr("全员同步为无记忆卡状态 ✓"));
    else
        m_memcard_status->setText(QString::fromStdString(status.memory_card_status.empty() ? std::string("等待房主开始同步") : status.memory_card_status));

    if (!status.start_requested)
        m_boot_status->setText(tr("等待房主选择游戏。"));
    else if (!status.prepare_boot)
        m_boot_status->setText(tr("正在校验游戏并同步记忆卡…"));
    else if (!status.local_boot_ready)
        m_boot_status->setText(tr("正在启动本机游戏…"));
    else if (!status.all_boot_ready)
        m_boot_status->setText(tr("本机已到启动屏障，等待其他玩家…"));
    else if (!status.start_committed)
        m_boot_status->setText(tr("所有玩家已加载，等待统一放行…"));
    else if (!status.first_poll_released)
        m_boot_status->setText(tr("已统一启动，正在等待第一个手柄同步点…"));
    else
        m_boot_status->setText(tr("同步启动完成 ✓"));

    m_session_status->setText(tr("P%1 · 会话 %2 · TCP %3")
        .arg(status.local_player_id)
        .arg(QStringLiteral("%1").arg(status.session_id, 16, 16, QLatin1Char('0')).toUpper())
        .arg(status.port));

    m_start_game->setVisible(is_host);
    m_start_game->setEnabled(is_host && status.room_full && !status.start_requested && !QtHost::IsVMValid());

    std::string launch_path;
    if (ModernNetplay::ConsumeBootLaunchRequest(&launch_path))
        launchPendingNetplayGame(QString::fromStdString(launch_path));
}

void NetplayDialog::launchPendingNetplayGame(const QString& path)
{
    std::shared_ptr<VMBootParameters> params = std::make_shared<VMBootParameters>();
    {
        auto lock = GameList::GetLock();
        const std::string path_string = path.toStdString();
        const GameList::Entry* entry = GameList::GetEntryForPath(path_string.c_str());
        if (!entry)
        {
            QMessageBox::critical(this, tr("联机"), tr("同步启动失败：本机匹配游戏已经从游戏列表中消失。"));
            return;
        }
        GameList::FillBootParametersForEntry(params.get(), entry);
    }
    g_emu_thread->startVM(std::move(params));
}

void NetplayDialog::openLogFolder()
{
    const ModernNetplay::StatusSnapshot status = ModernNetplay::GetStatusSnapshot();
    QString folder;
    if (!status.log_path.empty())
        folder = QFileInfo(QString::fromStdString(status.log_path)).absolutePath();
    else
        folder = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("logs/netplay"));
    QDir().mkpath(folder);
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void NetplayDialog::exportDiagnostics()
{
    const QString root = QCoreApplication::applicationDirPath();
    const QString batch = QDir(root).filePath(QStringLiteral("Collect-Netplay-Diagnostics.bat"));
    if (!QFileInfo::exists(batch))
    {
        openLogFolder();
        return;
    }
    QProcess process;
    process.setProgram(batch);
    process.setWorkingDirectory(root);
    if (!process.startDetached())
        openLogFolder();
}

void NetplayDialog::launchNormalInstance()
{
    const auto answer = QMessageBox::question(this, tr("退出联机"), tr("将关闭当前联机实例并重新启动普通 PCSX2。是否继续？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer != QMessageBox::Yes)
        return;

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("PCSX2_NETPLAY_MODE"));
    env.remove(QStringLiteral("PCSX2_NETPLAY_USERNAME"));
    env.remove(QStringLiteral("PCSX2_NETPLAY_HOST"));
    env.remove(QStringLiteral("PCSX2_NETPLAY_PORT"));
    env.remove(QStringLiteral("PCSX2_NETPLAY_PLAYERS"));
    env.remove(QStringLiteral("PCSX2_NETPLAY_DELAY"));
    env.remove(QStringLiteral("PCSX2_NETPLAY_MEMCARD_SYNC"));
    env.remove(QStringLiteral("PCSX2_NETPLAY_SHOW_LOBBY"));

    QProcess process;
    process.setProcessEnvironment(env);
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setWorkingDirectory(QCoreApplication::applicationDirPath());
    if (!process.startDetached())
    {
        QMessageBox::critical(this, tr("联机"), tr("无法重新启动 PCSX2。"));
        return;
    }
    accept();
    QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
}
