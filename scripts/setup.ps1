<#
.SYNOPSIS
    paint-pc 消费者 Windows 一键环境搭建脚本（W2，PowerShell 原生）。

.DESCRIPTION
    在 Windows 真机上把 paint-pc 开发/测试环境搭起来（仓库内自包含）：
      1) 探测  VS2026 (MSVC cl.exe) / CMake / Ninja / Vulkan SDK / glslc / python / git；
      2) 补缺  硬依赖缺失给精确安装指引（VS installer / Vulkan SDK 下载器 / git / python），
              软依赖缺失仅警告；
      3) 拉取  git submodule update --remote（SDK 跟随 paintDemo main 最新，不钉 pin）；
      4) 构建  vcvars64 环境内 cmake -B build -G Ninja + cmake --build；
      5) 测试  --test 模式跑 tests/smoke.sh（Git Bash/WSL 内跑，headless 离屏 PNG 真实笔迹断言）；
      6) 生成  -Sln 模式用 CMake VS 生成器产出 build/msvc/paint_pc.sln 并构建 Debug 验证链接。

    用法（PowerShell 5.1+ / Core 7+）:
      .\scripts\setup.ps1              默认（开发）：探测+补缺指引+拉 submodule+构建
      .\scripts\setup.ps1 --check      只探测不安装，输出缺项清单
      .\scripts\setup.ps1 --test       探测+构建+跑测试门（需 Git Bash 或 WSL 提供 bash）
      .\scripts\setup.ps1 -Sln         生成 VS 解决方案 build\msvc\paint_pc.sln 并构建 Debug
      .\scripts\setup.ps1 -Help        打印本帮助

    口径来源: docs/env/env-setup.md §3（Windows VS2026）+ SDK scripts/setup-env.ps1。
    依赖分级: 硬依赖缺失 → 非零退出并给安装指引；软依赖缺失 → 仅警告。
.PARAMETER Check
    只探测不安装，输出缺项清单。硬依赖缺失 → exit 1，仅软依赖缺失 → exit 0。
.PARAMETER Test
    探测 + 构建 + 跑测试门（tests/smoke.sh）。
.PARAMETER Sln
    生成 VS 解决方案（build\msvc\paint_pc.sln）并构建 Debug 验证链接。
.PARAMETER Help
    打印帮助后退出。
#>
[CmdletBinding()]
param(
    [switch]$Check,
    [switch]$Test,
    [switch]$Sln,
    [switch]$Help
)

if ($PSVersionTable.PSEdition -eq "Desktop" -and $PSVersionTable.PSVersion.Major -lt 5) {
    Write-Host "ERROR: 需要 PowerShell 5.1+。" -ForegroundColor Red; exit 2
}
if ($Help) { Get-Help $PSCommandPath -Detailed; exit 0 }
$ErrorActionPreference = "Stop"

# ---------- 输出辅助 ----------
function Info($m)  { Write-Host $m }
function Warn($m)  { Write-Host ("WARN: " + $m) -ForegroundColor Yellow }
function Err($m)   { Write-Host ("ERROR: " + $m) -ForegroundColor Red }
function Ok($m)    { Write-Host ("[OK]   " + $m) -ForegroundColor Green }

# ---------- 探测存储 ----------
$script:ChkNames  = [System.Collections.Generic.List[string]]::new()
$script:ChkLevels = [System.Collections.Generic.List[string]]::new()
$script:ChkStatus = [System.Collections.Generic.List[string]]::new()
$script:ChkDetail = [System.Collections.Generic.List[string]]::new()
$script:HardMiss  = 0
$script:SoftMiss  = 0
function Record($name, $level, $status, $detail) {
    $script:ChkNames.Add($name); $script:ChkLevels.Add($level)
    $script:ChkStatus.Add($status); $script:ChkDetail.Add($detail)
}

# ---------- 探测实现 ----------
function Find-VsInstallation {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { $vswhere = Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe" }
    if (-not (Test-Path $vswhere)) { return "" }
    # -all 包含所有版本，-prerelease 纳入 Insiders/预览版（缺 -prerelease 会过滤 2026 Insiders）。
    $vs = & $vswhere -all -prerelease -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    return ($vs | Select-Object -First 1).Trim()
}

# 定位 VS 的 installationVersion（与 Find-VsInstallation 同参数 -all -prerelease -latest），
# 供 -Sln 模式推导 CMake VS 生成器名。vswhere 无结果时退回安装路径版本段。
function Find-VsVersion {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { $vswhere = Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe" }
    if (Test-Path $vswhere) {
        $ver = & $vswhere -all -prerelease -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion 2>$null
        if (-not $ver) { $ver = & $vswhere -all -prerelease -latest -products * -property installationVersion 2>$null }
        if ($ver) { return ($ver | Select-Object -First 1).Trim() }
    }
    $vs = Find-VsInstallation
    if ($vs) { return (Split-Path (Split-Path $vs -Parent) -Leaf) }  # <major>\<SKU> → 版本段在 SKU 上一级
    return ""
}

# 版本/年份 → CMake VS 生成器名（major 16/17/18 → 2019/2022/2026）。
function Get-VsGeneratorName {
    param([string]$Version)
    if (-not $Version) { return "" }
    $major = ($Version -split '\.')[0]
    switch ($major) {
        "16" { return "Visual Studio 16 2019" }
        "2019" { return "Visual Studio 16 2019" }
        "17" { return "Visual Studio 17 2022" }
        "2022" { return "Visual Studio 17 2022" }
        "18" { return "Visual Studio 18 2026" }
        "2026" { return "Visual Studio 18 2026" }
        default { return "" }
    }
}

function Probe-VisualStudio {
    $vs = Find-VsInstallation
    if (-not $vs) { Record "Visual Studio (MSVC)" "硬" "MISS(硬)" "未找到含 VC 工具集的 VS"; $script:HardMiss++; return }
    $cl = Get-ChildItem -Path (Join-Path $vs "VC\Tools\MSVC") -Recurse -Filter cl.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "Hostx64\\x64\\cl\.exe$" } | Select-Object -First 1
    if (-not $cl) { Record "Visual Studio (MSVC)" "硬" "MISS(硬)" "找到 VS 但缺 cl.exe（VC 工具集未装）"; $script:HardMiss++; return }
    Record "Visual Studio (MSVC)" "硬" "OK" "VS @ $vs"
}

function Probe-CMake {
    $cm = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $cm) { Record "CMake" "硬" "MISS(硬)" "未安装"; $script:HardMiss++; return }
    $v = (& cmake --version 2>$null | Select-Object -First 1)
    if ($v -match "cmake version ([0-9]+)\.([0-9]+)") {
        $maj = [int]$Matches[1]; $min = [int]$Matches[2]
        if ($maj -gt 3 -or ($maj -eq 3 -and $min -ge 22)) { Record "CMake" "硬" "OK" $v }
        else { Record "CMake" "硬" "MISS(硬)" "$v 过旧（需 ≥ 3.22）"; $script:HardMiss++ }
    } else { Record "CMake" "硬" "OK" "已安装但版本无法确认" }
}

function Probe-Ninja {
    $nj = Get-Command ninja -ErrorAction SilentlyContinue
    if (-not $nj) {
        $vs = Find-VsInstallation
        if ($vs) {
            $ninjaInVs = Get-ChildItem -Path (Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake") -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($ninjaInVs) { Record "Ninja" "硬" "OK" "VS 自带 $($ninjaInVs.FullName)"; return }
        }
        Record "Ninja" "硬" "MISS(硬)" "未安装（VS 'C++ CMake tools' 提供）"; $script:HardMiss++; return
    }
    Record "Ninja" "硬" "OK" "ninja $(& ninja --version 2>$null | Select-Object -First 1)"
}

function Probe-Vulkan {
    $vsdk = $env:VULKAN_SDK
    if (-not $vsdk -or -not (Test-Path $vsdk)) {
        Record "Vulkan SDK" "硬" "MISS(硬)" "\$env:VULKAN_SDK 未设或缺失（paint-pc 真实 VkBackend 链接 vulkan-1.lib 必败）"; $script:HardMiss++; return
    }
    if (-not (Test-Path (Join-Path $vsdk "Lib\vulkan-1.lib"))) { Record "Vulkan SDK" "硬" "MISS(硬)" "VULKAN_SDK 缺 Lib\vulkan-1.lib"; $script:HardMiss++; return }
    Record "Vulkan SDK" "硬" "OK" $vsdk
}

function Probe-Python {
    $py = Get-Command python3 -ErrorAction SilentlyContinue
    if (-not $py) { $py = Get-Command python -ErrorAction SilentlyContinue }
    if (-not $py) { Record "python3" "硬" "MISS(硬)" "未安装（--test 的 smoke.sh 依赖 python3 解码 PNG）"; $script:HardMiss++; return }
    Record "python3" "硬" "OK" $py.Source
}

function Probe-Git {
    $g = Get-Command git -ErrorAction SilentlyContinue
    if (-not $g) { Record "git" "硬" "MISS(硬)" "未安装（拉 submodule 必败）"; $script:HardMiss++; return }
    Record "git" "硬" "OK" "git $(& git --version 2>$null | Select-Object -First 1)"
}

function Probe-Glslc {
    $g = Get-Command glslc -ErrorAction SilentlyContinue
    if (-not $g) { Record "glslc" "软" "WARN(软)" "未在 PATH（Vulkan SDK 提供，仅提示）"; $script:SoftMiss++; return }
    Record "glslc" "软" "OK" "glslc 可用"
}

function Probe-All {
    $script:ChkNames.Clear(); $script:ChkLevels.Clear(); $script:ChkStatus.Clear(); $script:ChkDetail.Clear()
    $script:HardMiss = 0; $script:SoftMiss = 0
    Probe-VisualStudio; Probe-CMake; Probe-Ninja; Probe-Vulkan; Probe-Python; Probe-Git; Probe-Glslc
}

# ---------- 输出 ----------
function Print-Check {
    Info "=== 环境探测结果 ==="
    for ($i = 0; $i -lt $script:ChkNames.Count; $i++) {
        Write-Host ("[{0}] {1}: {2}" -f $script:ChkStatus[$i], $script:ChkNames[$i], $script:ChkDetail[$i])
    }
    if ($script:HardMiss -gt 0) { Err "硬依赖缺失 $($script:HardMiss) 项，构建/测试将失败。" }
    else { Info "硬依赖齐全；软依赖缺失不阻断。" }
}

function Print-Guidance {
    Info "请手动补缺："
    Info "  - VS2026: 安装 + 「使用 C++ 的桌面开发」+「C++ CMake tools for Windows」"
    Info "      vs_installer.exe modify --installPath '<VS>' --add Microsoft.VisualStudio.Workload.NativeDesktop --add Microsoft.VisualStudio.Component.VC.CMake.Project --quiet --norestart"
    Info "  - Vulkan SDK: https://vulkan.lunarg.com/sdk/home → 设 \$env:VULKAN_SDK 指向根目录"
    Info "  - git: https://git-scm.com/download/win"
    Info "  - python3: https://www.python.org/downloads/（--test 需要）"
    Info "  - glslc 为软依赖，缺失仅警告。"
}

# ---------- 动作 ----------
function Sync-Submodule {
    param([string]$Root)
    Info "同步 SDK submodule（跟随 paintDemo main 最新）…"
    Push-Location $Root
    & git submodule update --init --recursive --remote
    if ($LASTEXITCODE -ne 0) { Err "submodule 同步失败"; Pop-Location; exit 1 }
    Pop-Location
}

function Build-Pc {
    param([string]$Root)
    $vs = Find-VsInstallation
    if (-not $vs) { Err "未找到 VS（探测阶段应已拦截）"; exit 1 }
    $vcvars = Get-ChildItem -Path (Join-Path $vs "VC\Auxiliary\Build") -Filter vcvars64.bat -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $vcvars) { Err "未找到 vcvars64.bat"; exit 1 }
    $env:PATH = "$env:VULKAN_SDK\Bin;$env:PATH"
    Info "构建 paint_pc（vcvars64 + Ninja）…"
    & cmd /c "`"$($vcvars.FullName)`" && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDGCPAIN_BUILD_TESTS=OFF -DDGCPAIN_BUILD_CLI=OFF && cmake --build build"
    if ($LASTEXITCODE -ne 0) { Err "构建失败"; exit 1 }
    Ok "构建产物: build\paint_pc.exe"
}

# -Sln 模式：用 CMake VS 生成器在独立目录 build\msvc\ 生成 paint_pc.sln，并构建 Debug 验证链接。
# 与 setup.sh build_pc_sln 逐项对应：-G <gen> -A x64 + CMAKE_GENERATOR_INSTANCE 钉到具体 VS 实例，
# 显式传 CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL 与 SDK 守卫一致（/MD，匹配 shaderc Release lib）。
function Build-Sln {
    param([string]$Root)
    $vs = Find-VsInstallation
    if (-not $vs) { Err "未找到 VS 安装（-Sln 需要 VS2019/2022/2026）"; exit 1 }
    $ver = Find-VsVersion
    $gen = Get-VsGeneratorName $ver
    if (-not $gen) { Err "无法从 VS 版本『$ver』推导 CMake 生成器（支持 VS2019/2022/2026）"; exit 1 }
    Info "生成 VS 解决方案：$gen（实例：$vs）…"
    Push-Location $Root
    $cmakeArgs = @("-S", $Root, "-B", (Join-Path $Root "build\msvc"), "-G", $gen, "-A", "x64",
        "-DCMAKE_GENERATOR_INSTANCE=$vs",
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL",
        "-DDGCPAIN_BUILD_TESTS=OFF", "-DDGCPAIN_BUILD_CLI=OFF")
    if ($env:DGCPAIN_DEPS_ROOT) { $cmakeArgs += "-DDGCPAIN_DEPS_ROOT=$env:DGCPAIN_DEPS_ROOT" }
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { Err "VS 解决方案 configure 失败"; Pop-Location; exit 1 }
    Info "构建 Debug 配置验证链接…"
    & cmake --build (Join-Path $Root "build\msvc") --config Debug -j
    if ($LASTEXITCODE -ne 0) { Err "Debug 构建失败"; Pop-Location; exit 1 }
    Pop-Location
    Ok "已生成：build\msvc\paint_pc.sln（VS 打开，选 Debug 配置开发）"
}

function Run-Test {
    param([string]$Root)
    # smoke.sh 是 Bash 脚本；需 Git Bash 或 WSL 提供 bash。
    $bash = Get-Command bash -ErrorAction SilentlyContinue
    if (-not $bash) {
        Warn "未找到 bash（Git Bash 或 WSL）—— --test 的 smoke.sh 需要 bash。"
        Info "  - Git Bash: https://gitforwindows.org/ 或安装 VS 自带组件"
        Info "  - 或先跑默认模式构建，手动在 Git Bash/WSL 里执行 tests/smoke.sh"
        exit 1
    }
    Info "跑测试门 tests/smoke.sh（$($bash.Source)）…"
    Push-Location $Root
    & $bash.Source "tests/smoke.sh"
    $rc = $LASTEXITCODE
    Pop-Location
    if ($rc -ne 0) { Err "测试门失败（smoke.sh exit $rc）"; exit 1 }
    Ok "测试门通过（smoke.sh）"
}

# ---------- 主流程 ----------
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Split-Path -Parent $Root   # 仓库根

Probe-All
Print-Check

if ($Check) { if ($script:HardMiss -gt 0) { Print-Guidance; exit 1 }; exit 0 }
if ($script:HardMiss -gt 0) { Print-Guidance; Err "硬依赖缺失 $($script:HardMiss) 项。请按指引补缺后重跑。"; exit 1 }

Sync-Submodule $Root
if ($Sln) { Build-Sln $Root } else { Build-Pc $Root }

if ($Test) { Run-Test $Root }

if ($Sln) { Info "已生成 VS 解决方案：build\msvc\paint_pc.sln（VS 打开选 Debug 开发）" }
else { Info "paint-pc 环境就绪。运行: .\build\paint_pc.exe（有显示）/ .\build\paint_pc.exe --headless out.png（离屏）" }
exit 0
