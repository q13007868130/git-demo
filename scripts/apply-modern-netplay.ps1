param(
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'

function Read-Text([string]$Path) {
    return [System.IO.File]::ReadAllText($Path)
}

function Write-Text([string]$Path, [string]$Text) {
    [System.IO.File]::WriteAllText($Path, $Text, [System.Text.UTF8Encoding]::new($false))
}

function Replace-Portable([string]$Text, [string]$OldLf, [string]$NewLf, [string]$ErrorMessage) {
    if ($Text.Contains($OldLf)) { return $Text.Replace($OldLf, $NewLf) }
    $oldCrlf = $OldLf.Replace("`n", "`r`n")
    $newCrlf = $NewLf.Replace("`n", "`r`n")
    if ($Text.Contains($oldCrlf)) { return $Text.Replace($oldCrlf, $newCrlf) }
    throw $ErrorMessage
}

$padCpp     = Join-Path $SourceRoot 'pcsx2\SIO\Pad\Pad.cpp'
$ds2Cpp     = Join-Path $SourceRoot 'pcsx2\SIO\Pad\PadDualshock2.cpp'
$coreCmake  = Join-Path $SourceRoot 'pcsx2\CMakeLists.txt'
$vmManager  = Join-Path $SourceRoot 'pcsx2\VMManager.cpp'
$mainWindow = Join-Path $SourceRoot 'pcsx2-qt\MainWindow.cpp'
$autoUpdater = Join-Path $SourceRoot 'pcsx2-qt\AutoUpdaterDialog.cpp'
$qtHost     = Join-Path $SourceRoot 'pcsx2-qt\QtHost.cpp'
$qtCmake    = Join-Path $SourceRoot 'pcsx2-qt\CMakeLists.txt'

# Pad core: use only the virtual controller slots owned by the Netplay room.
$text = Read-Text $padCpp
if ($text -notmatch 'Netplay/ModernNetplay.h') {
    $needle = '#include "SIO/Pad/Pad.h"'
    if (-not $text.Contains($needle)) { throw 'Pad.cpp include anchor not found' }
    $text = $text.Replace($needle, "$needle`r`n#include `"Netplay/ModernNetplay.h`"")
}

$oldController = 'const ControllerInfo* ci = GetControllerInfo(EmuConfig.Pad.Ports[i].Type);'
$newController = @'
const ControllerInfo* ci = GetControllerInfo(ModernNetplay::IsConfigured() ?
            (ModernNetplay::ShouldForceDualShock2Slot(i) ? Pad::ControllerType::DualShock2 : Pad::ControllerType::NotConnected) :
            EmuConfig.Pad.Ports[i].Type);
'@.TrimEnd()
if ($text.Contains($oldController)) {
    $text = $text.Replace($oldController, $newController)
} elseif ($text -notmatch 'ShouldForceDualShock2Slot') {
    throw 'Pad.cpp controller-type anchor not found'
}

# Netplay room lifetime intentionally outlives a single VM run.
# Do not terminate the network backend from Pad::Shutdown(); otherwise a failed
# or completed game makes the lobby unrecoverable until PCSX2 itself restarts.
Write-Text $padCpp $text

# Intercept the full DualShock2 response: digital + analog + pressure bytes (3..20).
$text = Read-Text $ds2Cpp
if ($text -notmatch 'Netplay/ModernNetplay.h') {
    $needle = '#include "SIO/Pad/Pad.h"'
    if (-not $text.Contains($needle)) { throw 'PadDualshock2.cpp include anchor not found' }
    $text = $text.Replace($needle, "$needle`r`n#include `"Netplay/ModernNetplay.h`"")
}
if ($text -notmatch 'ModernNetplay::HandlePadResponse') {
    $increment = "`tthis->commandBytesReceived++;"
    $index = $text.LastIndexOf($increment)
    if ($index -lt 0) { throw 'PadDualshock2.cpp command counter anchor not found' }
    $hook = @'
	if (this->currentCommand == Pad::Command::POLL && this->commandBytesReceived >= 3 && this->commandBytesReceived <= 20)
	{
		ret = ModernNetplay::HandlePadResponse(this->unifiedSlot,
			static_cast<std::uint32_t>(this->commandBytesReceived), ret);
	}

'@
    $text = $text.Insert($index, $hook)
}
Write-Text $ds2Cpp $text

# Core target + Winsock.
$text = Read-Text $coreCmake
if ($text -notmatch 'Netplay/ModernNetplay.cpp') {
    $text += @'

# PCSX2 Modern Netplay
target_sources(PCSX2 PRIVATE
    Netplay/ModernNetplay.cpp
    Netplay/ModernNetplay.h
)
if(WIN32)
    target_link_libraries(PCSX2 PRIVATE ws2_32)
endif()
'@
}
Write-Text $coreCmake $text

# Runtime deterministic settings: multitap + shadow memory card are applied here.
$text = Read-Text $vmManager
if ($text -notmatch '#include "Netplay/ModernNetplay.h"') {
    $needle = '#include "VMManager.h"'
    if (-not $text.Contains($needle)) { throw 'VMManager.cpp include anchor not found' }
    $text = $text.Replace($needle, "$needle`r`n#include `"Netplay/ModernNetplay.h`"")
}
if ($text -notmatch 'ModernNetplay::ApplyDeterministicConfig\(\)') {
    $needle = "`tPatch::ApplyPatchSettingOverrides();"
    if (-not $text.Contains($needle)) { throw 'VMManager.cpp LoadCoreSettings anchor not found' }
    $text = $text.Replace($needle, "$needle`r`n`tModernNetplay::ApplyDeterministicConfig();")
}
Write-Text $vmManager $text

# Chinese first-class Netplay menu and automatic room reopen after restart.
$text = Read-Text $mainWindow
if ($text -notmatch '#include "NetplayDialog.h"') {
    $needle = '#include "MainWindow.h"'
    if (-not $text.Contains($needle)) { throw 'MainWindow.cpp include anchor not found' }
    $text = $text.Replace($needle, "$needle`r`n#include `"NetplayDialog.h`"")
}
if ($text -notmatch '#include <QtCore/QTimer>') {
    $needle = '#include "NetplayDialog.h"'
    $text = $text.Replace($needle, "$needle`r`n#include <QtCore/QTimer>`r`n#include <QtCore/QProcess>`r`n#include <QtCore/QFileInfo>`r`n#include <QtCore/QCoreApplication>")
}
if ($text -notmatch 'PCSX2_MODERN_NETPLAY_MENU') {
    $needle = "`tsetupStatusBarWidgets();"
    if (-not $text.Contains($needle)) { throw 'MainWindow.cpp setupAdditionalUi anchor not found' }
    $menuCode = @'

	// PCSX2_MODERN_NETPLAY_MENU
	QMenu* netplay_menu = new QMenu(tr("联机(&N)"), menuBar());
	menuBar()->insertMenu(m_ui.menuHelp->menuAction(), netplay_menu);
	QAction* netplay_open = netplay_menu->addAction(tr("联机大厅..."));
	connect(netplay_open, &QAction::triggered, this, [this]() {
		NetplayDialog dialog(this);
		dialog.exec();
	});

	netplay_menu->addSeparator();
	QAction* netplay_update = netplay_menu->addAction(tr("检查联机版更新..."));
	connect(netplay_update, &QAction::triggered, this, [this]() {
		if (QtHost::IsVMValid())
		{
			QMessageBox::information(this, tr("联机版更新"),
				tr("请先停止当前游戏，再进行联机版更新。记忆卡、BIOS、游戏和个人配置不会被删除。"));
			return;
		}
		const QString updater = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Update-Netplay.bat"));
		if (!QFileInfo::exists(updater))
		{
			QMessageBox::warning(this, tr("联机版更新"),
				tr("当前版本缺少联机更新器，请先手工下载一次新版完整包。"));
			return;
		}
		const QStringList args = {QStringLiteral("/c"), updater,
			QString::number(QCoreApplication::applicationPid())};
		if (!QProcess::startDetached(QStringLiteral("cmd.exe"), args, QCoreApplication::applicationDirPath()))
			QMessageBox::critical(this, tr("联机版更新"), tr("无法启动联机版更新器。"));
	});

	QAction* upstream_update = netplay_menu->addAction(tr("检查 PCSX2 官方更新（仅查看）..."));
	connect(upstream_update, &QAction::triggered, this, [this]() { checkForUpdates(true, true); });
	m_ui.actionCheckForUpdates->setText(tr("检查 PCSX2 官方更新（仅查看）..."));

	if (qEnvironmentVariableIsSet("PCSX2_NETPLAY_SHOW_LOBBY"))
	{
		QTimer::singleShot(250, this, [this]() {
			NetplayDialog dialog(this);
			dialog.exec();
		});
	}
'@
    $text = $text.Replace($needle, $needle + $menuCode)
}
Write-Text $mainWindow $text

# Protect the custom Netplay binary from the stock PCSX2 installer.
# Official update checks remain available for information, but clicking
# "Download and Install" cannot overwrite the custom Netplay executable.
$text = Read-Text $autoUpdater
if ($text -notmatch '#include "Netplay/ModernNetplay.h"') {
    $needle = '#include "AutoUpdaterDialog.h"'
    if (-not $text.Contains($needle)) { throw 'AutoUpdaterDialog.cpp include anchor not found' }
    $text = $text.Replace($needle, "$needle`r`n#include `"Netplay/ModernNetplay.h`"")
}
if ($text -notmatch 'PCSX2_MODERN_NETPLAY_OFFICIAL_UPDATE_GUARD') {
    $needle = "void AutoUpdaterDialog::downloadUpdateClicked()`r`n{"
    if (-not $text.Contains($needle)) { $needle = "void AutoUpdaterDialog::downloadUpdateClicked()`n{" }
    if (-not $text.Contains($needle)) { throw 'AutoUpdaterDialog.cpp downloadUpdateClicked anchor not found' }
    $guard = @'
	// PCSX2_MODERN_NETPLAY_OFFICIAL_UPDATE_GUARD
	if (ModernNetplay::IsCustomBuild())
	{
		QMessageBox::warning(this, tr("Modern Netplay"),
			tr("当前是 Modern Netplay 修改版。直接安装 PCSX2 官方更新会覆盖联机功能。\n\n"
			   "你仍可查看官方更新内容，但请使用“联机 → 检查联机版更新...”安装已经重新合并 Netplay 的版本。"));
		return;
	}

'@
    $text = $text.Replace($needle, $needle + "`r`n" + $guard)
}
Write-Text $autoUpdater $text

# Only the synchronized room is allowed to start a VM. Once initialized, hold
# every machine at BOOT_READY until all configured players arrive.
$text = Read-Text $qtHost
if ($text -notmatch '#include "Netplay/ModernNetplay.h"') {
    $needle = '#include "QtHost.h"'
    if (-not $text.Contains($needle)) { throw 'QtHost.cpp include anchor not found' }
    $text = $text.Replace($needle, "$needle`r`n#include `"Netplay/ModernNetplay.h`"")
}
if ($text -notmatch 'NETPLAY_SYNCHRONIZED_BOOT_GUARD') {
    $needle = "`t// Determine whether to start fullscreen or not."
    if (-not $text.Contains($needle)) { throw 'QtHost.cpp startVM guard anchor not found' }
    $guard = @'
	// NETPLAY_SYNCHRONIZED_BOOT_GUARD
	if (ModernNetplay::IsConfigured() && !ModernNetplay::CanStartVM())
	{
		Host::ReportErrorAsync("Netplay", "联机实例只能从联机房间的同步启动流程打开游戏。");
		return;
	}

'@
    $text = $text.Replace($needle, $guard + $needle)
}
if ($text -notmatch 'NETPLAY_BOOT_READY_BARRIER') {
    $oldDone = @'
		if (!Host::GetBoolSettingValue("UI", "StartPaused", false))
		{
			// This will come back and call OnVMResumed().
			VMManager::SetState(VMState::Running);
		}
		else
		{
			// When starting paused, redraw the window, so there's at least something there.
			g_emu_thread->redrawDisplayWindow();
			Host::OnVMPaused();
		}
'@
    $newDone = @'
		// NETPLAY_BOOT_READY_BARRIER
		if (ModernNetplay::ShouldHoldBootBarrier())
		{
			VMManager::SetState(VMState::Paused);
			ModernNetplay::NotifyBootReady();
			if (ModernNetplay::WaitForStartCommit())
			{
				VMManager::SetState(VMState::Running);
			}
			else
			{
				g_emu_thread->redrawDisplayWindow();
				Host::OnVMPaused();
				Host::ReportErrorAsync("Netplay", "同步启动超时。虚拟机保持暂停，请导出所有玩家的联机诊断日志。");
			}
		}
		else if (!Host::GetBoolSettingValue("UI", "StartPaused", false))
		{
			VMManager::SetState(VMState::Running);
		}
		else
		{
			g_emu_thread->redrawDisplayWindow();
			Host::OnVMPaused();
		}
'@
    $text = Replace-Portable $text $oldDone $newDone 'QtHost.cpp done_callback barrier anchor not found'
}
Write-Text $qtHost $text

# Qt dialog.
$text = Read-Text $qtCmake
if ($text -notmatch 'NetplayDialog.cpp') {
    $text += @'

# PCSX2 Modern Netplay Qt UI
target_sources(pcsx2-qt PRIVATE
    NetplayDialog.cpp
    NetplayDialog.h
)
'@
}
Write-Text $qtCmake $text

Write-Host 'Modern Netplay all-in-one multiplayer + shadow-memory-card + synchronized boot patch applied.' -ForegroundColor Green
