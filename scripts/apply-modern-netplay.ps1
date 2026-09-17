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
    if ($Text.Contains($OldLf)) {
        return $Text.Replace($OldLf, $NewLf)
    }
    $oldCrlf = $OldLf.Replace("`n", "`r`n")
    $newCrlf = $NewLf.Replace("`n", "`r`n")
    if ($Text.Contains($oldCrlf)) {
        return $Text.Replace($oldCrlf, $newCrlf)
    }
    throw $ErrorMessage
}

$padCpp       = Join-Path $SourceRoot 'pcsx2\SIO\Pad\Pad.cpp'
$ds2Cpp       = Join-Path $SourceRoot 'pcsx2\SIO\Pad\PadDualshock2.cpp'
$cmake        = Join-Path $SourceRoot 'pcsx2\CMakeLists.txt'
$vmManager    = Join-Path $SourceRoot 'pcsx2\VMManager.cpp'
$mainWindow   = Join-Path $SourceRoot 'pcsx2-qt\MainWindow.cpp'
$qtHost       = Join-Path $SourceRoot 'pcsx2-qt\QtHost.cpp'
$qtCmake      = Join-Path $SourceRoot 'pcsx2-qt\CMakeLists.txt'

# Pad.cpp: include Netplay service, force virtual controller port 2 to DS2 while Netplay is configured,
# and make Pad shutdown terminate the network receive thread cleanly.
$text = Read-Text $padCpp
if ($text -notmatch 'Netplay/ModernNetplay.h') {
    $needle = '#include "SIO/Pad/Pad.h"'
    if (-not $text.Contains($needle)) { throw "Pad.cpp include anchor not found" }
    $text = $text.Replace($needle, "$needle`r`n#include `"Netplay/ModernNetplay.h`"")
}

$oldController = 'const ControllerInfo* ci = GetControllerInfo(EmuConfig.Pad.Ports[i].Type);'
$newController = @'
const ControllerInfo* ci = GetControllerInfo(
            (ModernNetplay::IsConfigured() && i == 1) ? Pad::ControllerType::DualShock2 : EmuConfig.Pad.Ports[i].Type);
'@.TrimEnd()
if ($text.Contains($oldController)) {
    $text = $text.Replace($oldController, $newController)
} elseif ($text -notmatch 'ModernNetplay::IsConfigured\(\).*i == 1') {
    throw "Pad.cpp controller-type anchor not found"
}

if ($text -notmatch 'ModernNetplay::Shutdown\(\)') {
    $shutdownAnchor = "void Pad::Shutdown()`r`n{"
    if (-not $text.Contains($shutdownAnchor)) {
        $shutdownAnchor = "void Pad::Shutdown()`n{"
    }
    if (-not $text.Contains($shutdownAnchor)) { throw "Pad.cpp Shutdown anchor not found" }
    $text = $text.Replace($shutdownAnchor, "$shutdownAnchor`r`n`tModernNetplay::Shutdown();")
}
Write-Text $padCpp $text

# PadDualshock2.cpp: intercept exactly the six response bytes used by legacy PCSX2 Online.
$text = Read-Text $ds2Cpp
if ($text -notmatch 'Netplay/ModernNetplay.h') {
    $needle = '#include "SIO/Pad/Pad.h"'
    if (-not $text.Contains($needle)) { throw "PadDualshock2.cpp include anchor not found" }
    $text = $text.Replace($needle, "$needle`r`n#include `"Netplay/ModernNetplay.h`"")
}

if ($text -notmatch 'ModernNetplay::HandlePadResponse') {
    $increment = "`tthis->commandBytesReceived++;"
    $index = $text.LastIndexOf($increment)
    if ($index -lt 0) { throw "PadDualshock2.cpp command counter anchor not found" }

    $hook = @'
	if (this->currentCommand == Pad::Command::POLL && this->commandBytesReceived >= 3 && this->commandBytesReceived <= 8)
	{
		ret = ModernNetplay::HandlePadResponse(this->unifiedSlot,
			static_cast<std::uint32_t>(this->commandBytesReceived), ret);
	}

'@
    $text = $text.Insert($index, $hook)
}
Write-Text $ds2Cpp $text

# Add the Netplay source directly to the modern PCSX2 core target and link Winsock on Windows.
$text = Read-Text $cmake
if ($text -notmatch 'Netplay/ModernNetplay.cpp') {
    $addition = @'

# PCSX2 Modern Netplay port (legacy Online-style SIO/PAD synchronization)
target_sources(PCSX2 PRIVATE
    Netplay/ModernNetplay.cpp
    Netplay/ModernNetplay.h
)
if(WIN32)
    target_link_libraries(PCSX2 PRIVATE ws2_32)
endif()
'@
    $text += $addition
}
Write-Text $cmake $text

# Keep the existing VMManager hook for later deterministic settings/memory-card shadowing.
$text = Read-Text $vmManager
if ($text -notmatch '#include "Netplay/ModernNetplay.h"') {
    $needle = '#include "VMManager.h"'
    if (-not $text.Contains($needle)) { throw "VMManager.cpp include anchor not found" }
    $text = $text.Replace($needle, "$needle`r`n#include `"Netplay/ModernNetplay.h`"")
}
if ($text -notmatch 'ModernNetplay::ApplyDeterministicConfig\(\)') {
    $needle = "`tPatch::ApplyPatchSettingOverrides();"
    if (-not $text.Contains($needle)) { throw "VMManager.cpp LoadCoreSettings anchor not found" }
    $text = $text.Replace($needle, "$needle`r`n`tModernNetplay::ApplyDeterministicConfig();")
}
Write-Text $vmManager $text

# Qt MainWindow: add a first-class Netplay menu before Help and automatically
# reopen the lobby after Create/Join restarts PCSX2.
$text = Read-Text $mainWindow
if ($text -notmatch '#include "NetplayDialog.h"') {
    $needle = '#include "MainWindow.h"'
    if (-not $text.Contains($needle)) { throw "MainWindow.cpp include anchor not found" }
    $text = $text.Replace($needle, "$needle`r`n#include `"NetplayDialog.h`"")
}
if ($text -notmatch '#include <QtCore/QTimer>') {
    $needle = '#include "NetplayDialog.h"'
    if (-not $text.Contains($needle)) { throw "MainWindow.cpp Netplay include anchor not found" }
    $text = $text.Replace($needle, "$needle`r`n#include <QtCore/QTimer>")
}

if ($text -notmatch 'PCSX2_MODERN_NETPLAY_MENU') {
    $needle = "`tsetupStatusBarWidgets();"
    if (-not $text.Contains($needle)) { throw "MainWindow.cpp setupAdditionalUi anchor not found" }
    $menuCode = @'

	// PCSX2_MODERN_NETPLAY_MENU
	QMenu* netplay_menu = new QMenu(tr("联机 (&Netplay)"), menuBar());
	menuBar()->insertMenu(m_ui.menuHelp->menuAction(), netplay_menu);
	QAction* netplay_open = netplay_menu->addAction(tr("创建 / 加入 / 房间状态..."));
	connect(netplay_open, &QAction::triggered, this, [this]() {
		NetplayDialog dialog(this);
		dialog.exec();
	});

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

# QtHost: in a configured Netplay instance, ordinary game boot is blocked until
# the room has issued PREPARE_BOOT. Once VM initialization completes, both peers
# stop at BOOT_READY and only enter Running after host START_COMMIT.
$text = Read-Text $qtHost
if ($text -notmatch '#include "Netplay/ModernNetplay.h"') {
    $needle = '#include "QtHost.h"'
    if (-not $text.Contains($needle)) { throw "QtHost.cpp include anchor not found" }
    $text = $text.Replace($needle, "$needle`r`n#include `"Netplay/ModernNetplay.h`"")
}

if ($text -notmatch 'NETPLAY_SYNCHRONIZED_BOOT_GUARD') {
    $needle = "`t// Determine whether to start fullscreen or not."
    if (-not $text.Contains($needle)) { throw "QtHost.cpp startVM guard anchor not found" }
    $guard = @'
	// NETPLAY_SYNCHRONIZED_BOOT_GUARD
	if (ModernNetplay::IsConfigured() && !ModernNetplay::CanStartVM())
	{
		Host::ReportErrorAsync("Netplay", "This Netplay instance can only boot a game from the synchronized room Start flow.");
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
				// This will come back and call OnVMResumed().
				VMManager::SetState(VMState::Running);
			}
			else
			{
				g_emu_thread->redrawDisplayWindow();
				Host::OnVMPaused();
				Host::ReportErrorAsync("Netplay", "Synchronized boot timed out. The VM remains paused; collect both Netplay logs.");
			}
		}
		else if (!Host::GetBoolSettingValue("UI", "StartPaused", false))
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
    $text = Replace-Portable $text $oldDone $newDone "QtHost.cpp done_callback barrier anchor not found"
}
Write-Text $qtHost $text

# Qt target: compile the programmatic Netplay dialog.
$text = Read-Text $qtCmake
if ($text -notmatch 'NetplayDialog.cpp') {
    $addition = @'

# PCSX2 Modern Netplay Qt UI
target_sources(pcsx2-qt PRIVATE
    NetplayDialog.cpp
    NetplayDialog.h
)
'@
    $text += $addition
}
Write-Text $qtCmake $text

Write-Host 'Modern Netplay v0.5a synchronized game validation + boot barrier patch applied successfully.' -ForegroundColor Green
