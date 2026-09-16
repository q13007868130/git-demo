// SPDX-FileCopyrightText: 2026 PCSX2 Modern Netplay Port contributors
// SPDX-License-Identifier: GPL-3.0+

#include "NetplayDialog.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QTimer>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
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
	setWindowTitle(tr("联机 (Netplay)"));
	setModal(true);
	setMinimumWidth(470);

	auto* root = new QVBoxLayout(this);

	m_current_status = new QLabel(this);
	m_current_status->setWordWrap(true);
	const QString current_mode = qEnvironmentVariable("PCSX2_NETPLAY_MODE").toLower();
	const QString current_port = qEnvironmentVariable("PCSX2_NETPLAY_PORT", QStringLiteral("27886"));
	const QString current_delay = qEnvironmentVariable("PCSX2_NETPLAY_DELAY", QStringLiteral("2"));
	if (current_mode == QStringLiteral("host"))
		m_current_status->setText(tr("当前实例：主机模式  ·  端口 %1  ·  延迟 %2 帧").arg(current_port, current_delay));
	else if (current_mode == QStringLiteral("client") || current_mode == QStringLiteral("join"))
		m_current_status->setText(tr("当前实例：加入模式  ·  主机 %1  ·  端口 %2  ·  延迟由主机决定")
			.arg(qEnvironmentVariable("PCSX2_NETPLAY_HOST", QStringLiteral("?")), current_port));
	else
		m_current_status->setText(tr("当前实例：普通离线模式"));
	root->addWidget(m_current_status);

	auto* connection_group = new QGroupBox(tr("连接"), this);
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
		tr("延迟由主机统一决定，加入方会在握手时自动采用主机数值。距离较远或网络抖动较大时可提高帧数，以换取更稳定的同步。"), delay_group);
	explanation->setWordWrap(true);
	delay_form->addRow(QString(), explanation);
	root->addWidget(delay_group);

	auto* note = new QLabel(
		tr("提示：v0.2 会按这里的设置重新启动 PCSX2 联机实例。请先停在游戏列表界面再创建/加入房间；双方随后启动同一版本的游戏。"), this);
	note->setWordWrap(true);
	root->addWidget(note);

	auto* buttons = new QHBoxLayout();
	auto* normal_button = new QPushButton(tr("重新启动为离线模式"), this);
	m_launch = new QPushButton(tr("启动联机实例"), this);
	m_launch->setDefault(true);
	auto* close_button = new QPushButton(tr("关闭"), this);
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

	updateModeUi();
	updateDelayHint();
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

void NetplayDialog::launchConfiguredInstance()
{
	const QString mode = m_mode->currentData().toString();
	if (mode == QStringLiteral("client") && m_host->text().trimmed().isEmpty())
	{
		QMessageBox::warning(this, tr("联机"), tr("加入房间时必须填写主机地址。"));
		return;
	}

	const auto answer = QMessageBox::question(this, tr("启动联机实例"),
		tr("PCSX2 将以新的联机设置重新启动。\n\n请确保当前没有正在运行的游戏。是否继续？"),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
	if (answer != QMessageBox::Yes)
		return;

	QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	env.insert(QStringLiteral("PCSX2_NETPLAY_MODE"), mode);
	env.insert(QStringLiteral("PCSX2_NETPLAY_PORT"), QString::number(m_port->value()));
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
