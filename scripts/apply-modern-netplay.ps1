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

$padCpp       = Join-Path $SourceRoot 'pcsx2\SIO\Pad\Pad.cpp'
$ds2Cpp       = Join-Path $SourceRoot 'pcsx2\SIO\Pad\PadDualshock2.cpp'
$cmake        = Join-Path $SourceRoot 'pcsx2\CMakeLists.txt'
$vmManager    = Join-Path $SourceRoot 'pcsx2\VMManager.cpp'
$mainWindow   = Join-Path $SourceRoot 'pcsx2-qt\MainWindow.cpp'
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

# VMManager: re-apply a strict deterministic Netplay profile every time core settings are loaded.
# This is the major safeguard the v0.1 proof-of-port was missing compared with classic PCSX2 Online.
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

# Qt MainWindow: add a first-class Netplay menu before Help.
$text = Read-Text $mainWindow
if ($text -notmatch '#include "NetplayDialog.h"') {
    $needle = '#include "MainWindow.h"'
    if (-not $text.Contains($needle)) { throw "MainWindow.cpp include anchor not found" }
    $text = $text.Replace($needle, "$needle`r`n#include `"NetplayDialog.h`"")
}

if ($text -notmatch 'PCSX2_MODERN_NETPLAY_MENU') {
    $needle = "`tsetupStatusBarWidgets();"
    if (-not $text.Contains($needle)) { throw "MainWindow.cpp setupAdditionalUi anchor not found" }
    $menuCode = @'

	// PCSX2_MODERN_NETPLAY_MENU
	QMenu* netplay_menu = new QMenu(tr("联机 (&Netplay)"), menuBar());
	menuBar()->insertMenu(m_ui.menuHelp->menuAction(), netplay_menu);
	QAction* netplay_open = netplay_menu->addAction(tr("创建 / 加入房间..."));
	connect(netplay_open, &QAction::triggered, this, [this]() {
		NetplayDialog dialog(this);
		dialog.exec();
	});
'@
    $text = $text.Replace($needle, $needle + $menuCode)
}
Write-Text $mainWindow $text

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

Write-Host 'Modern Netplay v0.3 core + deterministic profile + Qt UI patch applied successfully.' -ForegroundColor Green