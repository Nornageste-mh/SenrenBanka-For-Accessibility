# ============================================================================
#  make_release.ps1 —— 组装发布包 SBA11y-<版本>-installer.7z
#
#  为什么要有这个脚本：0.1.0.5 的包是**手敲**出来的，靠人记住「哪几个文件、放哪个目录」。.
#  这一版多了一样最容易漏的东西 —— 随包的争渡 dll（两件），于是把它写成脚本：.
#  谁跑都一样，漏没漏一眼能看出来。.
#
#  包内布局（与 0.1.0.5 发布件一致，只是 plugin\ 里多了两个 ZDSR 文件）：.
#
#      SBA11y-<版本>-installer\
#        README.md                CHANGELOG.md
#        plugin\a11y.dll                        <- 构建产物（必须是 32 位）.
#        plugin\nvdaControllerClient.dll        <- NV Access 官方 x86 版.
#        plugin\a11y_labels*.ini                <- 通用 + jp/en/tw
#        plugin\a11y_speech.ini
#        plugin\ZDSRAPI.dll                     <- 争渡 32 位（**不随包带 ZDSRAPI.ini**）.
#        plugin\ZDSRAPI_x64.dll                 <- 争渡 64 位.
#        tools\注入工具.exe                      <- 复用现成的那份（见下）.
#        licenses\LGPL-2.1-nvdaControllerClient.txt
#
#  【争渡那两个 dll 从哪来】按**和插件同一套**的注册表优先顺序找本机的争渡安装目录：.
#      1) 注册表卸载项 DisplayName 含「争渡」的 InstallLocation（HKLM/HKCU × 两种视图）.
#      2) %ProgramFiles(x86)%\zdsr\zdsr、%ProgramFiles%\zdsr\zdsr
#    找不到就**只警告、不失败**（这台机器没装争渡，照样能出一个不带兜底副本的包）。.
#
#  【为什么不像其它文件那样重新生成】tools\注入工具.exe 需要 32 位 tjs2c.exe 才能编.
#    （见 tools\build_injector.ps1），本机没有 tjs2c，所以脚本**复用**已有那份：.
#    先找 -Injector 参数，再找仓库里现成的，最后回落到上一个发布包。.
#    这不是偷懒：那份 exe 与本版源码无关（它只负责改写 data.xp3 的启动槽位），.
#    0.1.0.5 用的就是它，重编只会引入差异。**要重编请先拿到 tjs2c。**
#
#  用法：.
#      powershell -ExecutionPolicy Bypass -File tools\make_release.ps1                # 打包.
#      powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 -DryRun        # 只组装+校验+列清单，不出 7z
#      powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 -ArchiveDir '<输出目录>'
#
#  退出码：0 = 成功（含「没装争渡、跳过兜底副本」）；非 0 = 硬失败（缺文件、写错位数）。.
#
#  【坑 · 本文件每条注释都以 ASCII 字符结尾，别改成中文标点】。.
#  Windows PowerShell 5.1（powershell.exe，不是 pwsh）把注释行结尾的**全角标点**
#  （」』；，。：）当成续行符，会**静默吃掉下一行**：param 块里写.
#      # 版本号；留空则从 README.md 顶部读.
#      [string]$Version,
#  结果是 $Version 这个参数根本不存在，报错却是「找不到参数 -Version」。.
#  这个坑不报语法错（Parser 说 no parse errors），只吞行 —— 所以注释最后必须是 ASCII。.
# ============================================================================
[CmdletBinding()]
param(
    # 版本号；留空则从 README.md 顶部的「当前版本：**x.y.z.w**」读，避免手抄出错.
    [string]$Version,
    # 构建产物；默认仓库根的 a11y.dll
    [string]$PluginDll,
    # nvdaControllerClient.dll（x86）；默认从同工作区的 Framework 下载缓存里找.
    [string]$NvdaClient,
    # 注入工具 exe；默认自动找.
    [string]$Injector,
    # 争渡安装目录；不给就按注册表优先顺序自动找.
    [string]$ZdsrDir,
    # 7z.exe
    [string]$SevenZip = 'C:\Program Files\7-Zip\7z.exe',
    # 归档输出目录；默认 <仓库>\dist
    [string]$ArchiveDir,
    # 组装+校验+列清单，但不生成 7z（用于演练；仍会做位数校验与内容清单）.
    [switch]$DryRun,
    # 组装目录；默认 <仓库>\build\release_stage\<版本>（.gitignore 已拉黑）.
    [string]$StageRoot
)
$ErrorActionPreference = 'Stop'

# 中文输出：老控制台默认 GBK 代码页，直接 Write-Host 中文会成乱码。.
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding($false) } catch { }

$root = Split-Path $PSScriptRoot -Parent
if (-not $ArchiveDir) { $ArchiveDir = Join-Path $root 'dist' }

function Say([string]$msg) { Write-Host $msg }
function Warn([string]$msg) { Write-Host "WARN  $msg" -ForegroundColor Yellow }
function Die([string]$msg) { Write-Host "FAIL  $msg" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------------------
# PE 头：读 Machine 字段（0x014C = x86，0x8664 = x64）.
# 用文件字节直接读，不依赖 dumpbin（脚本要能在没装 VS 的机器上跑）。.
# ---------------------------------------------------------------------------
function Get-PeMachine([string]$path) {
    $fs = [IO.File]::OpenRead($path)
    try {
        $br = New-Object IO.BinaryReader($fs)
        if ($br.ReadUInt16() -ne 0x5A4D) { return -1 }        # 'MZ'
        $fs.Position = 0x3C
        $peOff = $br.ReadUInt32()
        $fs.Position = $peOff
        if ($br.ReadUInt32() -ne 0x00004550) { return -1 }    # 'PE\0\0'
        return [int]$br.ReadUInt16()
    } finally { $fs.Dispose() }
}
function Machine-Name([int]$m) {
    switch ($m) {
        0x014C { 'x86' }
        0x8664 { 'x64' }
        0xAA64 { 'arm64' }
        default { ('unknown(0x{0:X4})' -f $m) }
    }
}
function Assert-Machine([string]$path, [int]$want, [string]$what) {
    $m = Get-PeMachine $path
    if ($m -ne $want) {
        Die "$what 的位数不对：期望 $(Machine-Name $want)（0x$('{0:X4}' -f $want)），实际 $(Machine-Name $m) —— $path"
    }
    Say ("      PE Machine=0x{0:X4} ({1})  {2}" -f $m, (Machine-Name $m), (Split-Path $path -Leaf))
}

# ---------------------------------------------------------------------------
# 按注册表优先顺序找争渡安装目录。.
# 与 src\zdsr_api.h 的 ZdsrRegistryDirs 同一套判据：.
#   · HKLM + HKCU，默认视图与 64 位视图都看；.
#   · DisplayName 含「争渡」的排前面，只含 zdsr 的排后面（本机那个 ZDSR hook 工具.
#     就是后者，它的目录里没有 ZDSRAPI.dll）；.
#   · 读不到就返回空 —— 注册表不该成为故障源。.
# ---------------------------------------------------------------------------
function Find-ZdsrInstallDir {
    $exact = New-Object Collections.Generic.List[string]
    $loose = New-Object Collections.Generic.List[string]
    $views = @(
        @{ Hive = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall';        Path = $null },
        @{ Hive = 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'; Path = $null },
        @{ Hive = 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall';        Path = $null },
        @{ Hive = 'HKCU:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'; Path = $null }
    )
    foreach ($v in $views) {
        if (-not (Test-Path -LiteralPath $v.Hive)) { continue }
        foreach ($sub in (Get-ChildItem -LiteralPath $v.Hive -ErrorAction SilentlyContinue)) {
            $props = Get-ItemProperty -LiteralPath $sub.PSPath -ErrorAction SilentlyContinue
            if (-not $props -or -not $props.DisplayName) { continue }
            $disp = [string]$props.DisplayName
            $isProduct = $disp.Contains([char]0x4E89 + [char]0x6E21)   # 「争渡」
            $isLoose = $disp -match '(?i)zdsr'
            if (-not ($isProduct -or $isLoose)) { continue }
            if (-not $props.InstallLocation) { continue }
            $loc = ([string]$props.InstallLocation).Trim().TrimEnd('\')
            if (-not $loc) { continue }
            if ($isProduct) { $exact.Add($loc) } else { $loose.Add($loc) }
        }
    }
    foreach ($d in @($exact + $loose)) { if ($d) { return $d } }
    return $null
}

function Find-ZdsrApiPair([string]$dir) {
    $x86 = Join-Path $dir 'ZDSRAPI.dll'
    $x64 = Join-Path $dir 'ZDSRAPI_x64.dll'
    if ((Test-Path -LiteralPath $x86) -and (Test-Path -LiteralPath $x64)) { return @($x86, $x64) }
    return $null
}

# ---------------------------------------------------------------------------
# 0. 版本号.
# ---------------------------------------------------------------------------
if (-not $Version) {
    $readme = Join-Path $root 'README.md'
    if (-not (Test-Path -LiteralPath $readme)) { Die "找不到 README.md，且没有给 -Version" }
    $m = [regex]::Match((Get-Content -LiteralPath $readme -Raw -Encoding UTF8),
                        '当前版本：\*\*([0-9]+(?:\.[0-9]+)+)\*\*')
    if (-not $m.Success) { Die "README.md 里读不出「当前版本：**x.y.z.w**」，请用 -Version 指定" }
    $Version = $m.Groups[1].Value
    Say "版本号（取自 README.md）：$Version"
}
$pkgName = "SBA11y-$Version-installer"

Say ""
Say "=== 组装 $pkgName ==="

# ---------------------------------------------------------------------------
# 1. 输入文件.
# ---------------------------------------------------------------------------
if (-not $PluginDll) { $PluginDll = Join-Path $root 'a11y.dll' }
if (-not (Test-Path -LiteralPath $PluginDll)) {
    Die "找不到插件产物 $PluginDll —— 先跑 build\build.cmd"
}

if (-not $NvdaClient) {
    foreach ($cand in @(
        (Join-Path $root 'vendor\nvdaControllerClient.dll'),
        'D:\DSHWorkSpace\zdsr_work\BelfryForAccessibility\mod\nvda_dl\x86\nvdaControllerClient.dll'
    )) { if (Test-Path -LiteralPath $cand) { $NvdaClient = $cand; break } }
}
if (-not $NvdaClient -or -not (Test-Path -LiteralPath $NvdaClient)) {
    Die "找不到 nvdaControllerClient.dll（x86）；用 -NvdaClient <路径> 指定"
}

if (-not $Injector) {
    foreach ($cand in @(
        (Join-Path $root 'tools\_cache\注入工具.exe'),
        (Join-Path $root 'tools\注入工具.exe'),
        (Join-Path $root 'tools\inject_boot.exe')
    )) { if (Test-Path -LiteralPath $cand) { $Injector = $cand; break } }
}
if (-not $Injector -or -not (Test-Path -LiteralPath $Injector)) {
    Die ("找不到 tools\注入工具.exe。它需要 32 位 tjs2c.exe 才能重编（tools\build_injector.ps1），" +
         "本机没有 tjs2c —— 请用 -Injector <路径> 指定现成那份（例如从上一个发布包里解出来的）")
}

$licenses = Join-Path $root 'licenses\LGPL-2.1-nvdaControllerClient.txt'
$haveLicenses = Test-Path -LiteralPath $licenses

# ---------------------------------------------------------------------------
# 2. 争渡那两个 dll（找不到就只警告）.
# ---------------------------------------------------------------------------
$zdsrX86 = $null; $zdsrX64 = $null
if (-not $ZdsrDir) {
    $ZdsrDir = Find-ZdsrInstallDir
    if ($ZdsrDir) { Say "争渡安装目录（注册表）：$ZdsrDir" }
}
if ($ZdsrDir) {
    $pair = Find-ZdsrApiPair $ZdsrDir
    if ($pair) { $zdsrX86 = $pair[0]; $zdsrX64 = $pair[1] }
    else { Warn "在 $ZdsrDir 里没有同时找到 ZDSRAPI.dll 与 ZDSRAPI_x64.dll" }
} else {
    Warn "本机没有找到争渡的安装登记（注册表 / 默认目录都没有）"
}
if (-not $zdsrX86) {
    foreach ($pf in @(${env:ProgramFiles(x86)}, ${env:ProgramFiles})) {
        if (-not $pf) { continue }
        $pair = Find-ZdsrApiPair (Join-Path $pf 'zdsr\zdsr')
        if ($pair) { $zdsrX86 = $pair[0]; $zdsrX64 = $pair[1]; Say "争渡安装目录（默认路径）：$(Join-Path $pf 'zdsr\zdsr')"; break }
    }
}
if (-not $zdsrX86) {
    Warn "**没有随包带上争渡的兜底 dll**：这台机器上没找到争渡安装目录。"
    Warn "包仍然会生成；玩家侧的行为回到「只读他自己机器上的争渡安装目录」。"
}

# ---------------------------------------------------------------------------
# 3. 组装.
# ---------------------------------------------------------------------------
if (-not $StageRoot) { $StageRoot = Join-Path $root "build\release_stage\$Version" }
$pkgDir = Join-Path $StageRoot $pkgName
if (Test-Path -LiteralPath $pkgDir) { Remove-Item -LiteralPath $pkgDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $pkgDir 'plugin')   | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $pkgDir 'tools')    | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $pkgDir 'licenses') | Out-Null

function Copy-In([string]$src, [string]$dstDir, [string]$as) {
    $target = if ($as) { Join-Path $dstDir $as } else { Join-Path $dstDir (Split-Path $src -Leaf) }
    Copy-Item -LiteralPath $src -Destination $target -Force
    $fi = Get-Item -LiteralPath $target
    Say ("      {0,-38} {1,10:N0} B" -f ($target.Substring($pkgDir.Length + 1)), $fi.Length)
}

$pluginDir = Join-Path $pkgDir 'plugin'
Copy-In $PluginDll    $pluginDir 'a11y.dll'
Copy-In $NvdaClient   $pluginDir 'nvdaControllerClient.dll'
if ($zdsrX86) {
    Copy-In $zdsrX86  $pluginDir 'ZDSRAPI.dll'
    Copy-In $zdsrX64  $pluginDir 'ZDSRAPI_x64.dll'
}
foreach ($ini in @('a11y_labels.ini','a11y_labels_jp.ini','a11y_labels_en.ini','a11y_labels_tw.ini','a11y_speech.ini')) {
    Copy-In (Join-Path $root "data\$ini") $pluginDir
}
Copy-In $Injector (Join-Path $pkgDir 'tools') '注入工具.exe'
Copy-In (Join-Path $root 'README.md')    $pkgDir
Copy-In (Join-Path $root 'CHANGELOG.md') $pkgDir
if ($haveLicenses) { Copy-In $licenses (Join-Path $pkgDir 'licenses') }
else { Warn "仓库里没有 licenses\LGPL-2.1-nvdaControllerClient.txt —— 包里就没有它（玩家应能从 NV Access 拿到）" }

# ZDSRAPI.ini 是**用户自己**的配置（官方文档：它的优先级高于程序调用）。.
# 随包发一份等于替玩家定死通道参数，而且会跟着升级走样 —— 明确不发。.
Get-ChildItem -LiteralPath $pluginDir -Filter 'ZDSRAPI.ini' -ErrorAction SilentlyContinue |
    ForEach-Object { Die "包里出现了 ZDSRAPI.ini（$($_.FullName)）—— 它不该随包分发" }

# ---------------------------------------------------------------------------
# 4. 校验.
# ---------------------------------------------------------------------------
Say ""
Say "=== 校验 ==="
Assert-Machine (Join-Path $pluginDir 'a11y.dll')                0x014C 'a11y.dll'
Assert-Machine (Join-Path $pluginDir 'nvdaControllerClient.dll') 0x014C 'nvdaControllerClient.dll'
if ($zdsrX86) {
    Assert-Machine (Join-Path $pluginDir 'ZDSRAPI.dll')     0x014C 'ZDSRAPI.dll'
    Assert-Machine (Join-Path $pluginDir 'ZDSRAPI_x64.dll') 0x8664 'ZDSRAPI_x64.dll'
}

foreach ($need in @('plugin\a11y.dll','plugin\nvdaControllerClient.dll','plugin\a11y_labels.ini',
                    'plugin\a11y_speech.ini','tools\注入工具.exe','README.md','CHANGELOG.md')) {
    if (-not (Test-Path -LiteralPath (Join-Path $pkgDir $need))) { Die "包里缺少 $need" }
}
if ($zdsrX86) {
    foreach ($need in @('plugin\ZDSRAPI.dll','plugin\ZDSRAPI_x64.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $pkgDir $need))) { Die "包里缺少 $need" }
    }
}

# 三个导出必须还在（krkrz 的 V2Link 约定）。用字节找导出名 —— 名字在 .edata 里是明文。.
$dllBytes = [IO.File]::ReadAllBytes((Join-Path $pluginDir 'a11y.dll'))
$dllText  = [Text.Encoding]::ASCII.GetString($dllBytes)
foreach ($exp in @('V2Link','V2Unlink','GetModuleThreadModel')) {
    if ($dllText.IndexOf($exp, [StringComparison]::Ordinal) -lt 0) { Die "a11y.dll 里找不到导出 $exp" }
}
Say "      导出 V2Link / V2Unlink / GetModuleThreadModel 都在"

# ---------------------------------------------------------------------------
# 5. 归档.
# ---------------------------------------------------------------------------
$sha = (Get-FileHash -Algorithm SHA256 (Join-Path $pluginDir 'a11y.dll')).Hash

Say ""
Say "=== 内容清单 ==="
Get-ChildItem -LiteralPath $pkgDir -Recurse -File |
    Sort-Object FullName |
    ForEach-Object { Say ("      {0,-46} {1,10:N0} B" -f ($_.FullName.Substring($pkgDir.Length + 1)), $_.Length) }

if ($DryRun) {
    Say ""
    Say "=== DRY RUN：没有生成 7z ==="
    Say "      组装目录 : $pkgDir"
    Say "      a11y.dll : SHA256 $sha"
    exit 0
}

if (-not (Test-Path -LiteralPath $SevenZip)) { Die "找不到 7z.exe：$SevenZip（用 -SevenZip 指定）" }
New-Item -ItemType Directory -Force -Path $ArchiveDir | Out-Null
$archive = Join-Path $ArchiveDir ($pkgName + '.7z')
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }

Say ""
Say "=== 打包 ==="
Say "      源目录 : $StageRoot"
# 先切到 staging 的**上一层**再压 $pkgName：归档里自然带上这一层目录，解压不会散落一地.
# 不要用 7z 的 -w <目录> 来做这件事：那条路径必须是绝对的，写相对路径时 7z 会.
# 「找不到指定的文件」然后**照样返回成功**，只留下一个 32 字节的空归档.
Push-Location -LiteralPath $StageRoot
try {
    & $SevenZip a -t7z -mx=9 -ms=on "$archive" $pkgName | Out-Null
    if ($LASTEXITCODE -ne 0) { Die "7z 退出码 $LASTEXITCODE" }
} finally { Pop-Location }
if (-not (Test-Path -LiteralPath $archive)) { Die "7z 没有生成 $archive" }
if ((Get-Item -LiteralPath $archive).Length -lt 10240) { Die "归档小得不像话（$((Get-Item -LiteralPath $archive).Length) 字节），7z 多半没真的打包" }

$fi = Get-Item -LiteralPath $archive
Say ""
Say "=== 完成 ==="
Say ("      归档   : {0}" -f $fi.FullName)
Say ("      大小   : {0:N0} 字节" -f $fi.Length)
Say ("      SHA256 : {0}" -f (Get-FileHash -Algorithm SHA256 $fi.FullName).Hash)
Say ("      a11y.dll SHA256 : {0}" -f $sha)
if (-not $zdsrX86) { Warn "（这份包里**没有**争渡兜底 dll —— 见上面的 WARN）" }
exit 0
