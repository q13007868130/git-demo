// SPDX-FileCopyrightText: 2026 PCSX2 Modern Netplay Port contributors
// SPDX-License-Identifier: GPL-3.0+

#include "NetplayDialog.h"

#include "Netplay/ModernNetplay.h"
#include "QtHost.h"
#include "pcsx2/GameList.h"
#include "pcsx2/VMManager.h"

#include <algorithm>
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
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QProgressBar>
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
                if (!entry || entry->serial.empty())
                    continue;
                const bool arcade = (entry->type == GameList::EntryType::ARCADE);
                if (!arcade && (!entry->IsDisc() || entry->crc == 0))
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
            tip->setText(tr("没有找到可联机的 PS2/街机游戏。请先在 PCSX2X6 主界面配置游戏目录并完成扫描。"));
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
    setModal(false);
    setWindowModality(Qt::NonModal);
    setWindowFlag(Qt::WindowStaysOnTopHint, false);

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
    m_host_delay->setRange(1, 100);
    m_host_delay->setValue(2);
    m_host_delay->setSuffix(tr(" 帧"));
    m_host_delay->setToolTip(tr("1～100 帧。数值越高越能容忍高延迟网络，但操作延迟也会明显增加。"));
    host_form->addRow(tr("输入延迟："), m_host_delay);

    m_memcard_sync = new QCheckBox(tr("同步房主存档/街机状态（PS2 记忆卡；X6 Dongle + SRAM，均使用临时副本）"), host_tab);
    m_memcard_sync->setChecked(true);
    m_memcard_sync->setEnabled(false);
    m_memcard_sync->setToolTip(tr("PCSX2X6 联机固定启用状态同步：普通 PS2 同步记忆卡；System 246/256 同步 Dongle + SRAM。"));
    host_form->addRow(QString(), m_memcard_sync);

    auto* host_note = new QLabel(tr("3～4 人房间会自动在 2 号手柄端口启用 Multitap，保持 P1/P2 与双人模式一致，P3/P4 使用扩展槽位。游戏开始后会锁定房间，普通 PS2 会校验完整启动环境；System 246/256 还会校验 GameIndex、.acgame、游戏INI、boot.elf、媒体和 Dongle。"), host_tab);
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

    auto* join_note = new QLabel(tr("人数、输入延迟和状态同步规则由房主统一决定。街机局会同步 Dongle + SRAM，并校验 GameIndex/.acgame/游戏INI。"), join_tab);
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
    m_players_table->setColumnCount(6);
    m_players_table->setHorizontalHeaderLabels({tr("连接"), tr("名称"), tr("手柄"), tr("游戏"), tr("状态同步"), tr("启动")});
    m_players_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_players_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_players_table->verticalHeader()->setVisible(false);
    m_players_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_players_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_players_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_players_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_players_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_players_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    root->addWidget(m_players_table, 1);

    const ModernNetplay::StatusSnapshot lobby_status = ModernNetplay::GetStatusSnapshot();
    m_runtime_toggle = new QPushButton(tr("实时联机控制…"), this);
    m_runtime_toggle->setCheckable(true);
    m_runtime_toggle->setChecked(false);
    root->addWidget(m_runtime_toggle);

    m_runtime_group = new QGroupBox(this);
    m_runtime_group->setVisible(false);
    auto* runtime_form = new QFormLayout(m_runtime_group);
    m_local_controller = new QComboBox(m_runtime_group);
    for (std::uint32_t i = 1; i <= lobby_status.max_players; i++)
        m_local_controller->addItem(tr("P%1").arg(i), static_cast<int>(i));
    runtime_form->addRow(tr("我控制："), m_local_controller);

    m_runtime_delay = new QSpinBox(m_runtime_group);
    m_runtime_delay->setRange(1, 100);
    m_runtime_delay->setSuffix(tr(" 帧"));
    m_runtime_delay->setValue(static_cast<int>(lobby_status.delay));
    runtime_form->addRow(tr("输入延迟："), m_runtime_delay);

    m_runtime_players = new QSpinBox(m_runtime_group);
    m_runtime_players->setRange(static_cast<int>(lobby_status.max_players), static_cast<int>(ModernNetplay::MAX_PLAYERS));
    m_runtime_players->setValue(static_cast<int>(lobby_status.max_players));
    m_runtime_players->setSuffix(tr(" 人"));
    runtime_form->addRow(tr("房间人数："), m_runtime_players);

    m_topology_mode = new QComboBox(m_runtime_group);
    m_topology_mode->addItem(tr("兼容模式 A：Multitap 接 1 号端口（暴走单车等）"), 0);
    m_topology_mode->addItem(tr("兼容模式 B：Multitap 接 2 号端口（按 Start 加入类）"), 1);
    m_topology_mode->setCurrentIndex(lobby_status.topology_mode == 0 ? 0 : 1);
    runtime_form->addRow(tr("Multitap 布局（全房间）："), m_topology_mode);

    m_apply_runtime = new QPushButton(tr("应用实时设置"), m_runtime_group);
    runtime_form->addRow(QString(), m_apply_runtime);
    auto* runtime_note = new QLabel(
        tr("输入延迟可在游戏中实时调整，并在全员相同的输入检查点统一生效。"
           "1～2 人房固定连接1=P1、连接2=P2，禁止互换；3～4 人房才开放 P1～P4 调整。"
           "房间人数可实时增加到 4 人，游戏途中加入的新玩家会在当前房间等待下一局。"
           "Multitap 是全房间统一硬件布局：3～4 人房由房主设置，游戏运行中锁定，切换游戏后可修改下一局。"), m_runtime_group);
    runtime_note->setWordWrap(true);
    runtime_form->addRow(QString(), runtime_note);
    root->addWidget(m_runtime_group);

    connect(m_runtime_toggle, &QPushButton::toggled, this, [this](bool expanded) {
        m_runtime_group->setVisible(expanded);
        m_runtime_toggle->setText(expanded ? tr("实时联机控制 ▲") : tr("实时联机控制…"));
    });

    auto* state_group = new QGroupBox(tr("同步状态"), this);
    auto* state_form = new QFormLayout(state_group);
    m_game_status = new QLabel(state_group);
    m_game_status->setWordWrap(true);
    m_memcard_status = new QLabel(state_group);
    m_memcard_status->setWordWrap(true);
    m_memcard_progress = new QProgressBar(state_group);
    m_memcard_progress->setRange(0, 1000);
    m_memcard_progress->setValue(0);
    m_memcard_progress->setTextVisible(true);
    m_memcard_progress->setVisible(false);
    m_boot_status = new QLabel(state_group);
    m_boot_status->setWordWrap(true);
    m_session_status = new QLabel(state_group);
    m_session_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    state_form->addRow(tr("游戏："), m_game_status);
    state_form->addRow(tr("状态同步："), m_memcard_status);
    state_form->addRow(QString(), m_memcard_progress);
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
    connect(m_start_game, &QPushButton::clicked, this, [this]() {
        if (QtHost::IsVMValid())
        {
            const auto answer = QMessageBox::question(this, tr("切换联机游戏"),
                tr("将同步结束所有玩家当前游戏，但保留联机房间和玩家连接。\n"
                   "停止完成后直接选择新游戏，不需要重启 PCSX2。是否继续？"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (answer != QMessageBox::Yes)
                return;
            if (!ModernNetplay::RequestReturnToLobby())
            {
                QMessageBox::warning(this, tr("切换联机游戏"), tr("无法同步返回联机房间。"));
                return;
            }
            m_switch_game_pending = true;
            return;
        }
        chooseGameAndStart();
    });
    connect(m_apply_runtime, &QPushButton::clicked, this, [this]() {
        ModernNetplay::StatusSnapshot current = ModernNetplay::GetStatusSnapshot();
        if (current.runtime_reconfiguring)
        {
            QMessageBox::information(this, tr("实时联机设置"), tr("上一项实时设置正在同步，请稍候。"));
            return;
        }

        if (current.role == "host")
        {
            const std::uint32_t capacity = static_cast<std::uint32_t>(m_runtime_players->value());
            if (capacity > current.max_players)
            {
                if (!ModernNetplay::RequestRoomCapacity(capacity))
                {
                    QMessageBox::warning(this, tr("实时联机设置"), tr("无法增加房间人数。"));
                    return;
                }
                current = ModernNetplay::GetStatusSnapshot();
            }
        }

        std::uint32_t controller = current.local_player_id;
        if (current.round_players >= 3)
            controller = static_cast<std::uint32_t>(m_local_controller->currentData().toUInt());

        const std::uint32_t delay = (current.role == "host") ?
            static_cast<std::uint32_t>(m_runtime_delay->value()) : current.delay;
        const std::uint32_t topology = (current.role == "host") ?
            static_cast<std::uint32_t>(m_topology_mode->currentData().toUInt()) : current.topology_mode;

        if (QtHost::IsVMValid() && current.role == "host" && topology != current.topology_mode)
        {
            QMessageBox::information(this, tr("实时联机设置"),
                tr("输入延迟可以在游戏中实时调整。\n\n"
                   "多人手柄布局会改变虚拟 PS2 手柄硬件，请切换/重新同步游戏后修改。"));
            return;
        }

        if (!ModernNetplay::RequestRuntimeSettings(controller, delay, topology))
            QMessageBox::warning(this, tr("实时联机设置"),
                tr("无法应用设置。可能正在进行另一项同步调整。"));
    });
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
        m_room_status->setText(status.round_in_progress && status.max_players > status.round_players ?
            tr("房间已扩容 · %1 / %2 人 · 本局 %3 人进行中 · 新玩家等待下一局 · 延迟 %4 帧")
                .arg(status.player_count).arg(status.max_players).arg(status.round_players).arg(status.delay) :
            tr("房间已就绪 · %1 / %2 人 · 输入延迟 %3 帧")
                .arg(status.player_count).arg(status.max_players).arg(status.delay));
    else if (is_host)
        m_room_status->setText(status.round_in_progress ?
            tr("本局 %1 人继续进行 · 房间已开放到 %2 人 · 等待新玩家加入… · TCP %3")
                .arg(status.round_players).arg(status.max_players).arg(status.port) :
            tr("等待玩家加入… · %1 / %2 人 · TCP %3")
                .arg(status.player_count).arg(status.max_players).arg(status.port));
    else if (status.connected)
        m_room_status->setText(tr("已连接房主，等待其他玩家… · %1 / %2 人").arg(status.player_count).arg(status.max_players));
    else
        m_room_status->setText(tr("正在连接房主…"));

    m_players_table->setRowCount(static_cast<int>(status.max_players));
    for (std::uint32_t i = 0; i < status.max_players; i++)
    {
        const ModernNetplay::PlayerSnapshot& player = status.players[i];
        m_players_table->setItem(static_cast<int>(i), 0, new QTableWidgetItem(tr("连接%1").arg(i + 1)));
        m_players_table->setItem(static_cast<int>(i), 1, new QTableWidgetItem(player.connected ? QString::fromStdString(player.name) : tr("等待加入")));
        m_players_table->setItem(static_cast<int>(i), 2, new QTableWidgetItem(player.connected && player.controller > 0 ? tr("P%1").arg(player.controller) : QStringLiteral("-")));
        const bool waiting_next_round = player.connected && status.round_in_progress && (i >= status.round_players);
        m_players_table->setItem(static_cast<int>(i), 3, new QTableWidgetItem(player.connected ?
            (waiting_next_round ? tr("等待下一局") : (player.game_match ? tr("匹配 ✓") : tr("等待"))) : QStringLiteral("-")));
        m_players_table->setItem(static_cast<int>(i), 4, new QTableWidgetItem(player.connected ? (player.memcard_ready ? tr("完成 ✓") : tr("等待")) : QStringLiteral("-")));
        m_players_table->setItem(static_cast<int>(i), 5, new QTableWidgetItem(player.connected ? (player.boot_ready ? tr("就绪 ✓") : tr("等待")) : QStringLiteral("-")));
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

    if (!status.memory_card_sync_enabled)
    {
        m_memcard_progress->setVisible(false);
    }
    else if (status.memory_card_transfer_active || status.memory_card_failed || status.memory_card_all_ready)
    {
        m_memcard_progress->setVisible(true);
        if (status.memory_card_size == 0)
        {
            if (status.memory_card_transfer_active)
            {
                m_memcard_progress->setRange(0, 0);
                m_memcard_progress->setFormat(tr("正在同步…"));
            }
            else
            {
                m_memcard_progress->setRange(0, 1000);
                m_memcard_progress->setValue(status.memory_card_all_ready ? 1000 : 0);
                m_memcard_progress->setFormat(status.memory_card_failed ? tr("同步失败") : tr("同步完成 ✓"));
            }
        }
        else
        {
            m_memcard_progress->setRange(0, 1000);
            const std::uint64_t done = std::min<std::uint64_t>(
                status.memory_card_transferred_bytes, status.memory_card_size);
            const int permille = static_cast<int>((done * 1000u) / status.memory_card_size);
            m_memcard_progress->setValue(status.memory_card_all_ready ? 1000 : permille);
            const double done_mb = static_cast<double>(done) / (1024.0 * 1024.0);
            const double total_mb = static_cast<double>(status.memory_card_size) / (1024.0 * 1024.0);
            const int percent = status.memory_card_all_ready ? 100 : (permille / 10);
            m_memcard_progress->setFormat(status.memory_card_failed ?
                tr("同步失败 · %1% · %2 / %3 MB").arg(percent).arg(done_mb, 0, 'f', 1).arg(total_mb, 0, 'f', 1) :
                tr("%1% · %2 / %3 MB").arg(percent).arg(done_mb, 0, 'f', 1).arg(total_mb, 0, 'f', 1));
        }
    }
    else
    {
        m_memcard_progress->setVisible(false);
    }

    if (status.memory_card_transfer_active)
        m_last_memcard_error_shown.clear();

    if (status.memory_card_failed && m_last_memcard_error_shown.isEmpty())
    {
        const QString error = QString::fromStdString(status.memory_card_status);
        if (!error.isEmpty())
        {
            m_last_memcard_error_shown = error;
            QMessageBox::warning(this, tr("联机状态同步失败"),
                tr("%1\n\n本次启动已取消，房间仍可继续使用。请检查网络后重新选择游戏开始；"
                   "如果同一玩家反复失败，可先点“重建连接”。").arg(error));
        }
    }

    if (status.game_switching)
        m_boot_status->setText(tr("正在同步结束当前游戏并返回联机房间…"));
    else if (!status.start_requested)
        m_boot_status->setText(tr("等待房主选择游戏。"));
    else if (!status.prepare_boot)
        m_boot_status->setText(tr("正在校验完整游戏环境并同步存档/街机状态…"));
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

    if (m_local_controller && m_local_controller->count() != static_cast<int>(status.max_players))
    {
        const int previous = m_local_controller->currentData().toInt();
        m_local_controller->blockSignals(true);
        m_local_controller->clear();
        for (std::uint32_t i = 1; i <= status.max_players; i++)
            m_local_controller->addItem(tr("P%1").arg(i), static_cast<int>(i));
        const int restored = m_local_controller->findData(previous);
        if (restored >= 0)
            m_local_controller->setCurrentIndex(restored);
        m_local_controller->blockSignals(false);
    }

    const std::uint32_t local_controller =
        (status.local_player_id >= 1 && status.local_player_id <= ModernNetplay::MAX_PLAYERS) ?
        status.players[status.local_player_id - 1].controller : status.local_player_id;
    m_session_status->setText(tr("连接%1 → P%2 · 会话 %3 · TCP %4 · Epoch %5%6")
        .arg(status.local_player_id)
        .arg(local_controller)
        .arg(QStringLiteral("%1").arg(status.session_id, 16, 16, QLatin1Char('0')).toUpper())
        .arg(status.port)
        .arg(status.input_epoch)
        .arg(status.runtime_reconfiguring ? tr(" · 正在同步实时设置…") : QString()));

    if (m_local_controller && !m_local_controller->hasFocus())
    {
        const int index = m_local_controller->findData(static_cast<int>(local_controller));
        if (index >= 0)
            m_local_controller->setCurrentIndex(index);
    }
    if (m_runtime_delay && !m_runtime_delay->hasFocus())
        m_runtime_delay->setValue(static_cast<int>(status.delay));
    if (m_runtime_players && !m_runtime_players->hasFocus())
    {
        m_runtime_players->setMinimum(static_cast<int>(status.max_players));
        m_runtime_players->setValue(static_cast<int>(status.max_players));
    }
    if (m_topology_mode && !m_topology_mode->hasFocus())
        m_topology_mode->setCurrentIndex(status.topology_mode == 0 ? 0 : 1);

    const bool vm_active = QtHost::IsVMValid();
    m_runtime_delay->setEnabled(is_host && !status.runtime_reconfiguring);
    m_runtime_players->setEnabled(is_host && status.max_players < ModernNetplay::MAX_PLAYERS && !status.runtime_reconfiguring);
    m_topology_mode->setEnabled(is_host && status.max_players >= 3 && !vm_active && !status.runtime_reconfiguring);
    if (!is_host)
        m_topology_mode->setToolTip(tr("这是整个房间共用的 Multitap 硬件布局，仅房主可以修改。"));
    else if (vm_active)
        m_topology_mode->setToolTip(tr("这是全房间共用的 Multitap 硬件布局。游戏运行中不能切换；请点“切换游戏”回到房间后设置下一局。"));
    else if (status.max_players < 3)
        m_topology_mode->setToolTip(tr("1～2 人不需要 Multitap；房间扩到 3～4 人后由房主选择布局。"));
    else
        m_topology_mode->setToolTip(tr("全房间统一设置，由房主选择；下一次同步启动时所有玩家使用同一布局。"));

    const bool controller_swapping_allowed = (status.round_players >= 3);
    m_local_controller->setEnabled(controller_swapping_allowed && !status.runtime_reconfiguring);
    if (!controller_swapping_allowed)
    {
        const int fixed_index = m_local_controller->findData(static_cast<int>(status.local_player_id));
        if (fixed_index >= 0)
            m_local_controller->setCurrentIndex(fixed_index);
        m_local_controller->setToolTip(tr("1～2 人房固定：连接1=P1、连接2=P2，不能互换。"));
    }
    else
    {
        m_local_controller->setToolTip(QString());
    }
    m_apply_runtime->setEnabled(!status.runtime_reconfiguring);

    m_start_game->setVisible(is_host);
    m_start_game->setText(vm_active ? tr("切换游戏...") : tr("选择游戏并开始..."));
    m_start_game->setEnabled(is_host && (vm_active ||
        (status.room_full && !status.start_requested && !status.game_switching)));

    if (m_switch_game_pending && !QtHost::IsVMValid() && status.room_full &&
        !status.start_requested)
    {
        m_switch_game_pending = false;
        QTimer::singleShot(0, this, [this]() { chooseGameAndStart(); });
    }

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
