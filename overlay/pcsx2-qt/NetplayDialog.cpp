// SPDX-FileCopyrightText: 2026 PCSX2 Modern Netplay Port contributors
// SPDX-License-Identifier: GPL-3.0+

#include "NetplayDialog.h"

#include "Netplay/ModernNetplay.h"

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

NetplayDialog::NetplayDialog(QWidget* parent)
	: QDialog(parent)
{
	setWindowTitle(tr("联机房间 (Netplay)"));
	setModal(true);
	setMinimumWidth(560);

	auto* root = new QVBoxLayout(this);

	m_current_status = new QLabel(this);
	m_current_status->setWordWrap(true);
	root->addWidget(m_current_status);

	// v0.4: a real session/lobby status panel. The network connection is started
	// before the game, so host and client can confirm 1/2 -> 2/2 before booting.
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
		tr("延迟仍由主机统一决定。先确认房间显示 2 / 2，再在两台电脑上启动同一版本的游戏。距离较远或网络抖动较大时，可提高延迟帧数。"), delay_group);
	explanation->setWordWrap(true);
	delay_form->addRow(QString(), explanation);
	root->addWidget(delay_group);

	auto* note = new QLabel(
		tr("v0.4 稳定性版已恢复 v0.1 的手柄锁步方式。v0.3 的绝对 VSync / 内存 Hash 检查改为不影响游戏的诊断思路，不会再因为启动帧偏移直接断开。每台电脑都会自动生成独立 Netplay 日志。"), this);
	note->setWordWrap(true);
	root->addWidget(note);

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

	auto* status_timer = new QTimer(this);
	connect(status_timer, &QTimer::timeout, this, [this]() { refreshRuntimeStatus(); });
	status_timer->start(500);

	updateModeUi();
	updateDelayHint();
	refreshRuntimeStatus();
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
		return;
	}

	const bool is_host = (status.role == "host");
	m_current_status->setText(is_host ?
		tr("当前实例：房主 (P1) · TCP %1").arg(status.port) :
		tr("当前实例：客户端 (P2) · TCP %1").arg(status.port));

	if (status.failed)
	{
		m_room_state->setText(tr("连接错误：%1").arg(QString::fromStdString(status.last_error)));
	}
	else if (status.connected)
	{
		m_room_state->setText(tr("● 已连接。房间已满，可以关闭此窗口并在两台电脑上启动同一游戏。"));
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
		// Client value is only a bootstrap value. The host overwrites it during handshake.
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
