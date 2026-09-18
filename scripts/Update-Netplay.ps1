param(
    [string]$AppDir = "",
    [int]$ParentPid = 0
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms

function Show-Info([string]$Text, [string]$Title = "PCSX2 联机版更新") {
    [System.Windows.Forms.MessageBox]::Show(
        $Text, $Title,
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Information
    ) | Out-Null
}

function Show-Error([string]$Text) {
    [System.Windows.Forms.MessageBox]::Show(
        $Text, "PCSX2 联机版更新",
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Error
    ) | Out-Null
}

try {
    if ([string]::IsNullOrWhiteSpace($AppDir)) {
        $AppDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    }
    $AppDir = [System.IO.Path]::GetFullPath($AppDir)

    $channelFile = Join-Path $AppDir "NETPLAY_UPDATE_CHANNEL.txt"
    $channel = if (Test-Path $channelFile) { (Get-Content $channelFile -Raw).Trim().ToLowerInvariant() } else { "test" }
    if ($channel -ne "stable") { $channel = "test" }

    $tag = if ($channel -eq "stable") { "netplay-latest" } else { "netplay-test-latest" }
    $api = "https://api.github.com/repos/q13007868130/git-demo/releases/tags/$tag"
    $headers = @{
        "User-Agent" = "PCSX2-Modern-Netplay-Updater"
        "Accept" = "application/vnd.github+json"
    }

    $release = Invoke-RestMethod -Uri $api -Headers $headers
    $manifestAsset = @($release.assets | Where-Object { $_.name -eq "NETPLAY_UPDATE_MANIFEST.json" }) | Select-Object -First 1
    $zipAsset = @($release.assets | Where-Object { $_.name -eq "PCSX2-modern-netplay-windows-x64.zip" }) | Select-Object -First 1
    if (-not $manifestAsset -or -not $zipAsset) {
        throw "联机更新服务器当前没有完整的更新文件。"
    }

    $manifest = Invoke-RestMethod -Uri $manifestAsset.browser_download_url -Headers $headers
    $currentShaFile = Join-Path $AppDir "NETPLAY_CONTROL_SHA.txt"
    $currentSha = if (Test-Path $currentShaFile) { (Get-Content $currentShaFile -Raw).Trim() } else { "" }

    if ($currentSha -and $currentSha -eq [string]$manifest.control_sha) {
        Show-Info ("当前已经是最新联机版。\r\n\r\n联机版本：" + $manifest.version +
            "\r\nPCSX2 上游：" + $manifest.upstream_sha)
        exit 0
    }

    $message = "发现新的 PCSX2 Modern Netplay 版本。\r\n\r\n" +
        "联机版本：" + $manifest.version + "\r\n" +
        "PCSX2 上游：" + $manifest.upstream_sha + "\r\n\r\n" +
        "更新会替换模拟器程序文件，但不会删除 BIOS、记忆卡、游戏、截图、存档或个人配置。\r\n\r\n" +
        "现在下载并安装吗？"

    $answer = [System.Windows.Forms.MessageBox]::Show(
        $message, "PCSX2 联机版更新",
        [System.Windows.Forms.MessageBoxButtons]::YesNo,
        [System.Windows.Forms.MessageBoxIcon]::Question
    )
    if ($answer -ne [System.Windows.Forms.DialogResult]::Yes) { exit 0 }

    $tempRoot = Join-Path $env:TEMP ("PCSX2-Netplay-Update-" + [Guid]::NewGuid().ToString("N"))
    $zipPath = Join-Path $tempRoot "update.zip"
    $stageDir = Join-Path $tempRoot "stage"
    New-Item -ItemType Directory -Force -Path $tempRoot, $stageDir | Out-Null

    Invoke-WebRequest -UseBasicParsing -Uri $zipAsset.browser_download_url -Headers $headers -OutFile $zipPath

    if ($manifest.sha256) {
        $actual = (Get-FileHash -Algorithm SHA256 -Path $zipPath).Hash
        if ($actual -ne [string]$manifest.sha256) {
            throw "下载文件校验失败。为保护当前安装，更新已经取消。"
        }
    }

    Expand-Archive -Path $zipPath -DestinationPath $stageDir -Force
    $newExe = Join-Path $stageDir "pcsx2-qt.exe"
    if (-not (Test-Path $newExe)) {
        throw "下载包中缺少 pcsx2-qt.exe，更新已经取消。"
    }

    $backupRoot = Join-Path $AppDir "netplay-update-backup"
    New-Item -ItemType Directory -Force -Path $backupRoot | Out-Null
    foreach ($name in @("pcsx2-qt.exe", "NETPLAY_CONTROL_SHA.txt", "PCSX2_UPSTREAM_SHA.txt", "NETPLAY_BUILD_INFO.txt")) {
        $src = Join-Path $AppDir $name
        if (Test-Path $src) { Copy-Item $src (Join-Path $backupRoot $name) -Force }
    }

    if ($ParentPid -gt 0) {
        try {
            $parent = Get-Process -Id $ParentPid -ErrorAction Stop
            $null = $parent.CloseMainWindow()
            for ($i = 0; $i -lt 50 -and -not $parent.HasExited; $i++) {
                Start-Sleep -Milliseconds 100
                $parent.Refresh()
            }
            if (-not $parent.HasExited) {
                Stop-Process -Id $ParentPid -Force
                Wait-Process -Id $ParentPid -ErrorAction SilentlyContinue
            }
        } catch {
        }
    }

    Get-ChildItem -LiteralPath $stageDir -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $AppDir -Recurse -Force
    }

    Remove-Item $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    Start-Process -FilePath (Join-Path $AppDir "pcsx2-qt.exe")
}
catch {
    Show-Error ("联机版更新失败：\r\n\r\n" + $_.Exception.Message +
        "\r\n\r\n当前安装不会被主动删除。")
    exit 1
}
