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
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <memory>

NetplayDialog::NetplayDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("联机房间 (Netplay)"));
    setModal(true);
    setMinimumWidth(640);

    auto* root = new QVBoxLayout(this);

    m_current_status = new QLabel(this);
    m_current_status->setWordWrap(true);
    root->addWidget(m_current_status);

    auto* room_group = new QGroupBox(tr("房间状态"), this);
    auto* room_form = new QFormLayout(room_group);
    m_room_state = new QLabel(room_group);
    m_room_state->setWordWrap(true);
    m_player_count = new QLabel(room_group);
    m_peer = new QLabel(room_group);
    m_peer->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_runtime_delay = new QLabel(room_group);
    m_log_path = new QLabel(room_group);
    m_log_path->setWordWrap(true);
    m_log_path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    room_form->addRow(tr("状态："), m_room_state);
    room_form->addRow(tr("玩家："), m_player_count);
    room_form->addRow(tr("对方："), m_peer);
    room_form->addRow(tr("当前延迟："), m_runtime_delay);
    room_form->addRow(tr("本次日志："), m_log_path);

    auto* log_buttons = new QHBoxLayout();
    auto* open_log_button = new QPushButton(tr("打开日志文件夹"), room_group);
    auto* copy_log_button = new QPushButton(tr("复制日志路径"), room_group);
    log_buttons->addWidget(open_log_button);
    log_buttons->addWidget(copy_log_button);
    log_buttons->addStretch(1);
    room_form->addRow(QString(), log_buttons);
    root->addWidget(room_group);

    auto* game_group = new QGroupBox(tr("同步游戏启动"), this);
    auto* game_form = new QFormLayout(game_group);
    m_game_combo = new QComboBox(game_group);
    m_game_combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_game_combo->setMinimumContentsLength(42);
    m_sync_game = new QPushButton(tr("房主选择并同步此游戏"), game_group);
    m_game_status = new QLabel(game_group);
    m_game_status->setWordWrap(true);
    m_boot_status = new QLabel(game_group);
    m_boot_status->setWordWrap(true);
    m_start_game = new QPushButton(tr("开始同步游戏"), game_group);
    m_start_game->setDefault(false);
    game_form->addRow(tr("本地游戏："), m_game_combo);
    game_form->addRow(QString(), m_sync_game);
    game_form->addRow(tr("游戏校验："), m_game_status);
    game_form->addRow(tr("启动屏障："), m_boot_status);
    game_form->addRow(QString(), m_start_game);
    root->addWidget(game_group);

    populateGameList();

    const QString current_mode = qEnvironmentVariable("PCSX2_NETPLAY_MODE").toLower();
    if (current_mode == QStringLiteral("host") || current_mode == QStringLiteral("client") || current_mode == QStringLiteral("join"))
        ModernNetplay::StartSessionAsync();

    auto* connection_group = new QGroupBox(tr("房间设置"), this);
    auto* connection_form = new QFormLayout(connection_group);

    m_mode = new QComboBox(connection_group);
    m_mode->addItem(tr("创建房间 / 主机 (P1)"), QStringLiteral("host"));
    m_mode->addItem(tr("加入房间 / 客户端 (P2)"), QStringLiteral("client"));
    if (current_mode == QStringLiteral("client") || current_mode == QStringLiteral("join"))
        m_mode->setCurrentIndex(1);
    connection_form->addRow(tr("模式："), m_mode);

    m_host = new QLineEdit(connection_group);
    m_host->setPlaceholderText(tr("例如 192.168.1.20 或主机名"));
    m_host->setText(qEnvironmentVariable("PCSX2_NETPLAY_HOST"));
    connection_form->addRow(tr("主机地址："), m_host);

    m_port = new QSpinBox(connection_group);
    m_port->setRange(1, 65535);
    m_port->setValue(qEnvironmentVariableIntValue("PCSX2_NETPLAY_PORT") > 0 ?
        qEnvironmentVariableIntValue("PCSX2_NETPLAY_PORT") : 27886);
    connection_form->addRow(tr("端口："), m_port);
    root->addWidget(connection_group);

    auto* delay_group = new QGroupBox(tr("输入延迟"), this);
    auto* delay_form = new QFormLayout(delay_group);
    m_delay = new QSpinBox(delay_group);
    m_delay->setRange(1, 12);
    m_delay->setSuffix(tr(" 帧"));
    const int env_delay = qEnvironmentVariableIntValue("PCSX2_NETPLAY_DELAY");
    m_delay->setValue((env_delay >= 1 && env_delay <= 12) ? env_delay : 2);
    delay_form->addRow(tr("主机延迟："), m_delay);

    m_delay_hint = new QLabel(delay_group);
    m_delay_hint->setWordWrap(true);
    delay_form->addRow(QString(), m_delay_hint);

    auto* explanation = new QLabel(
        tr("v0.5a 启动同步预览：房主必须在房间中选择游戏。客户端只有在 Serial + CRC 完全匹配后才算准备完成。房主点击开始后，两边会自动启动各自本地的同一游戏，并在 VM 初始化完成后互相等待，再一起放行。"), delay_group);
    explanation->setWordWrap(true);
    delay_form->addRow(QString(), explanation);
    root->addWidget(delay_group);

    auto* buttons = new QHBoxLayout();
    auto* normal_button = new QPushButton(tr("重新启动为离线模式"), this);
    m_launch = new QPushButton(tr("启动联机实例"), this);
    m_launch->setDefault(true);
    auto* close_button = new QPushButton(tr("关闭房间窗口"), this);
    buttons->addWidget(normal_button);
    buttons->addStretch(1);
    buttons->addWidget(m_launch);
    buttons->addWidget(close_button);
    root->addLayout(buttons);

    connect(m_mode, &QComboBox::currentIndexChanged, this, [this]() { updateModeUi(); });
    connect(m_delay, &QSpinBox::valueChanged, this, [this]() { updateDelayHint(); });
    connect(m_launch, &QPushButton::clicked, this, [this]() { launchConfiguredInstance(); });
    connect(normal_button, &QPushButton::clicked, this, [this]() { launchNormalInstance(); });
    connect(close_button, &QPushButton::clicked, this, &QDialog::reject);
    connect(open_log_button, &QPushButton::clicked, this, [this]() { openLogFolder(); });
    connect(copy_log_button, &QPushButton::clicked, this, [this]() { copyLogPath(); });
    connect(m_sync_game, &QPushButton::clicked, this, [this]() { syncSelectedGame(); });
    connect(m_start_game, &QPushButton::clicked, this, [this]() { requestStartGame(); });

    auto* status_timer = new QTimer(this);
    connect(status_timer, &QTimer::timeout, this, [this]() { refreshRuntimeStatus(); });
    status_timer->start(250);

    updateModeUi();
    updateDelayHint();
    refreshRuntimeStatus();
}

void NetplayDialog::populateGameList()
{
    m_game_combo->clear();
    auto lock = GameList::GetLock();
    const u32 count = GameList::GetEntryCount();
    for (u32 i = 0; i < count; i++)
    {
        const GameList::Entry* entry = GameList::GetEntryByIndex(i);
        if (!entry || !entry->IsDisc() || entry->serial.empty() || entry->crc == 0)
            continue;

        const QString title = QString::fromStdString(entry->GetTitle());
        const QString serial = QString::fromStdString(entry->serial);
        const QString crc = QStringLiteral("%1").arg(entry->crc, 8, 16, QLatin1Char('0')).toUpper();
        m_game_combo->addItem(tr("%1  [%2]  CRC %3").arg(title, serial, crc), QString::fromStdString(entry->path));
    }

    if (m_game_combo->count() == 0)
        m_game_combo->addItem(tr("没有找到带 Serial/CRC 的光盘游戏"), QString());
}

void NetplayDialog::updateModeUi()
{
    const bool joining = (m_mode->currentData().toString() == QStringLiteral("client"));
    m_host->setEnabled(joining);
    m_delay->setEnabled(!joining);
    if (joining)
        m_launch->setText(tr("加入房间并重启"));
    else
        m_launch->setText(tr("创建房间并重启"));
}

void NetplayDialog::updateDelayHint()
{
    const int delay = m_delay->value();
    QString hint;
    if (delay <= 2)
        hint = tr("建议场景：局域网 / 很低延迟网络。");
    else if (delay <= 3)
        hint = tr("建议场景：较低延迟网络，约 30–60 ms 可从这里开始测试。");
    else if (delay <= 5)
        hint = tr("建议场景：中等距离网络，约 60–100 ms 可从这里开始测试。");
    else if (delay <= 8)
        hint = tr("建议场景：较远距离或抖动明显的网络。");
    else
        hint = tr("高延迟模式：操作感会更迟，但可容忍更大的网络波动。");
    m_delay_hint->setText(tr("当前：%1 帧。%2").arg(delay).arg(hint));
}

void NetplayDialog::syncSelectedGame()
{
    const ModernNetplay::StatusSnapshot status = ModernNetplay::GetStatusSnapshot();
    if (!status.configured || status.role != "host")
        return;

    const QString path = m_game_combo->currentData().toString();
    if (path.isEmpty())
    {
        QMessageBox::warning(this, tr("联机"), tr("没有可同步的游戏。请先让 PCSX2 扫描游戏目录。"));
        return;
    }

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

    if (!ModernNetplay::HostSelectGame(local_path, title, serial, crc))
        QMessageBox::warning(this, tr("联机"), tr("无法同步所选游戏。请确认当前是房主、尚未开始启动，并且游戏有有效的 Serial/CRC。"));
}

void NetplayDialog::requestStartGame()
{
    if (!ModernNetplay::RequestSynchronizedBoot())
    {
        QMessageBox::warning(this, tr("联机"),
            tr("现在还不能开始。必须先连接 2 / 2，并确认双方的游戏 Serial + CRC 完全一致。"));
    }
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
            QMessageBox::critical(this, tr("联机"), tr("同步启动失败：本地匹配游戏已经从游戏列表中消失。"));
            return;
        }
        GameList::FillBootParametersForEntry(params.get(), entry);
    }

    g_emu_thread->startVM(std::move(params));
}

void NetplayDialog::refreshRuntimeStatus()
{
    const ModernNetplay::StatusSnapshot status = ModernNetplay::GetStatusSnapshot();
    if (!status.configured)
    {
        m_current_status->setText(tr("当前实例：普通离线模式。创建或加入房间后会重启为联机实例。"));
        m_room_state->setText(tr("未创建 / 未加入房间"));
        m_player_count->setText(tr("0 / 2"));
        m_peer->setText(QStringLiteral("-"));
        m_runtime_delay->setText(QStringLiteral("-"));
        m_log_path->setText(tr("联机实例启动后自动创建"));
        m_game_status->setText(tr("进入联机实例后由房主选择游戏。"));
        m_boot_status->setText(tr("未开始"));
        m_game_combo->setEnabled(false);
        m_sync_game->setEnabled(false);
        m_start_game->setEnabled(false);
        return;
    }

    const bool is_host = (status.role == "host");
    m_current_status->setText(is_host ?
        tr("当前实例：房主 (P1) · TCP %1 · 协议 v2").arg(status.port) :
        tr("当前实例：客户端 (P2) · TCP %1 · 协议 v2").arg(status.port));

    if (status.failed)
    {
        m_room_state->setText(tr("连接错误：%1").arg(QString::fromStdString(status.last_error)));
    }
    else if (status.connected)
    {
        m_room_state->setText(tr("● 已连接。请由房主选择游戏并等待双方校验通过。"));
    }
    else if (status.connecting)
    {
        m_room_state->setText(is_host ? tr("● 房间已创建，正在等待第 2 位玩家加入……") :
            tr("● 正在连接房主……"));
    }
    else
    {
        m_room_state->setText(tr("正在准备联机会话……"));
    }

    m_player_count->setText(tr("%1 / 2").arg(status.player_count));
    if (!status.peer.empty())
        m_peer->setText(QString::fromStdString(status.peer));
    else
        m_peer->setText(is_host ? tr("等待玩家加入") : qEnvironmentVariable("PCSX2_NETPLAY_HOST", QStringLiteral("?")));
    m_runtime_delay->setText(tr("%1 帧").arg(status.delay));
    m_log_path->setText(status.log_path.empty() ? tr("正在创建……") : QString::fromStdString(status.log_path));

    m_game_combo->setEnabled(is_host && !status.prepare_boot);
    m_sync_game->setEnabled(is_host && status.connected && !status.prepare_boot && !QtHost::IsVMValid());

    if (!status.game_selected)
    {
        m_game_status->setText(is_host ? tr("等待房主选择游戏。") : tr("等待房主发送游戏信息。"));
    }
    else
    {
        const QString crc = QStringLiteral("%1").arg(status.game_crc, 8, 16, QLatin1Char('0')).toUpper();
        const QString local_match = status.local_game_match ? tr("本机匹配 ✓") : tr("本机匹配 ✗");
        const QString peer_match = status.peer_game_match ? tr("对方匹配 ✓") : tr("对方匹配 ✗ / 等待中");
        m_game_status->setText(tr("%1\nSerial: %2 · CRC: %3\n%4 · %5")
            .arg(QString::fromStdString(status.game_title), QString::fromStdString(status.game_serial), crc, local_match, peer_match));
    }

    if (!status.prepare_boot)
        m_boot_status->setText(tr("等待房主点击“开始同步游戏”。"));
    else if (!status.local_boot_ready)
        m_boot_status->setText(tr("正在启动本地游戏并等待 VM 初始化完成……"));
    else if (!status.peer_boot_ready)
        m_boot_status->setText(tr("本机 BOOT_READY ✓，正在等待对方启动同一游戏……"));
    else if (!status.start_committed)
        m_boot_status->setText(tr("双方 BOOT_READY ✓，等待房主 START_COMMIT……"));
    else if (!status.first_poll_released)
        m_boot_status->setText(tr("START_COMMIT ✓，正在等待双方到达第一个手柄同步点……"));
    else
        m_boot_status->setText(tr("同步启动完成 ✓"));

    m_start_game->setVisible(is_host);
    m_start_game->setEnabled(is_host && status.connected && status.game_selected &&
        status.local_game_match && status.peer_game_match && !status.prepare_boot && !QtHost::IsVMValid());

    std::string launch_path;
    if (ModernNetplay::ConsumeBootLaunchRequest(&launch_path))
        launchPendingNetplayGame(QString::fromStdString(launch_path));
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

void NetplayDialog::copyLogPath()
{
    const ModernNetplay::StatusSnapshot status = ModernNetplay::GetStatusSnapshot();
    const QString path = status.log_path.empty() ?
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("logs/netplay")) :
        QString::fromStdString(status.log_path);
    QApplication::clipboard()->setText(path);
}

void NetplayDialog::launchConfiguredInstance()
{
    const QString mode = m_mode->currentData().toString();
    if (mode == QStringLiteral("client") && m_host->text().trimmed().isEmpty())
    {
        QMessageBox::warning(this, tr("联机"), tr("加入房间时必须填写主机地址。"));
        return;
    }

    const auto answer = QMessageBox::question(this, tr("启动联机实例"),
        tr("PCSX2 将以新的联机设置重新启动，并自动打开房间窗口。\n\n请确保当前没有正在运行的游戏。是否继续？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer != QMessageBox::Yes)
        return;

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PCSX2_NETPLAY_MODE"), mode);
    env.insert(QStringLiteral("PCSX2_NETPLAY_PORT"), QString::number(m_port->value()));
    env.insert(QStringLiteral("PCSX2_NETPLAY_SHOW_LOBBY"), QStringLiteral("1"));
    if (mode == QStringLiteral("host"))
    {
        env.remove(QStringLiteral("PCSX2_NETPLAY_HOST"));
        env.insert(QStringLiteral("PCSX2_NETPLAY_DELAY"), QString::number(m_delay->value()));
    }
    else
    {
        env.insert(QStringLiteral("PCSX2_NETPLAY_HOST"), m_host->text().trimmed());
        env.insert(QStringLiteral("PCSX2_NETPLAY_DELAY"), QStringLiteral("2"));
    }

    QProcess process;
    process.setProcessEnvironment(env);
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setWorkingDirectory(QCoreApplication::applicationDirPath());
    if (!process.startDetached())
    {
        QMessageBox::critical(this, tr("联机"), tr("无法重新启动 PCSX2 联机实例。"));
        return;
    }

    accept();
    QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
}

void NetplayDialog::launchNormalInstance()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("PCSX2_NETPLAY_MODE"));
    env.remove(QStringLiteral("PCSX2_NETPLAY_HOST"));
    env.remove(QStringLiteral("PCSX2_NETPLAY_PORT"));
    env.remove(QStringLiteral("PCSX2_NETPLAY_DELAY"));
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
