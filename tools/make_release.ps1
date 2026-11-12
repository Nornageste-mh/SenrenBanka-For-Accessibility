# ============================================================================
#  make_release.ps1 —— 组装发布包 SBA11y-<版本>-installer.7z
#
#  为什么要有这个脚本：0.1.0.5 的包是**手敲**出来的，靠人记住「哪几个文件、放哪个目录」。.
#  这一版多了一样最容易漏的东西 —— 随包的争渡 dll（两件），于是把它写成脚本：.
#  谁跑都一样，漏没漏一眼能看出来。.
#
#  包内布局（0.1.0.6 发布件 + mod\package\ 的两份手写文档）：.
#
#      SBA11y-<版本>-installer\
#        README.md                CHANGELOG.md
#        安装说明.md              常见问题.md      <- mod\package\ 下，手写，不进构建.
#        plugin\a11y.dll                        <- 构建产物（必须是 32 位）.
#        plugin\nvdaControllerClient.dll        <- NV Access 官方 x86 版.
#        plugin\a11y_labels*.ini                <- 通用 + jp/en/tw
#        plugin\a11y_speech.ini
#        plugin\ZDSRAPI.dll                     <- 争渡 32 位（**不随包带 ZDSRAPI.ini**）.
#        plugin\ZDSRAPI_x64.dll                 <- 争渡 64 位.
#        tools\注入工具.exe                      <- 复用现成的那份（见下）.
#        licenses\LGPL-2.1-nvdaControllerClient.txt
#
#  【发布前自检（P6 / 非 Unity 路线，硬失败）】任一项失败 exit 1，禁止「警告后继续」。.
#    · 版本三处一致：README.md 当前版本行 / CHANGELOG.md 顶部标题 / 包名（-Version 或读 README）。.
#    · 包内文件齐全：两份说明文档 + plugin\ 下的 dll/ini + tools\注入工具.exe + licenses\。.
#    · a11y.dll 是 32 位（PE Machine=0x014C），导出 V2Link/V2Unlink/GetModuleThreadModel 都在。.
#    · 发布 DLL 内不得出现 "a11yb" / "sba11y_cmd"（G40：开发通道不进发布件）。.
#    · 不得含 data.xp3 / *.xp3 / 游戏脚本 / 剧本文本 / 日志（IP 红线），包内清单白名单校验。.
#    · 争渡那两个 dll 找不到仍然只警告（那是兜底副本，不是本包的功能前提）。.
#
#  【争渡那两个 dll 从哪来】按**和插件同一套**的注册表优先顺序找本机的争渡安装目录：.
#      1) 注册表卸载项 DisplayName 含「争渡」的 InstallLocation（HKLM/HKCU × 两种视图）。.
#      2) %ProgramFiles(x86)%\zdsr\zdsr、%ProgramFiles%\zdsr\zdsr
#    找不到就**只警告、不失败**（这台机器没装争渡，照样能出一个不带兜底副本的包）。.
#
#  【注入工具从哪来】tools\注入工具.exe 不在仓库里（*.exe 被 .gitignore 挡住）。.
#    查找顺序：-Injector 参数 -> tools\_cache -> tools\inject_boot.exe（build_injector.ps1.
#    的产物）-> 上一次组装目录 build\release_stage 里那份 -> 从 dist 的上一个发布包解出。.
#    干净克隆可以直接重建：pwsh -File tools\build_injector.ps1（用 data\startup.tjs.bin，.
#    不再依赖外部 tjs2c.exe）。那份 exe 只负责改写 data.xp3 的启动槽位，与插件源码无关。.
#
#  用法：.
#      powershell -ExecutionPolicy Bypass -File tools\make_release.ps1                # 打包.
#      powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 -DryRun        # 只组装+自检+列清单，不出 7z
#      powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 -ArchiveDir '<输出目录>'
#
#  退出码：0 = 成功（含「没装争渡、跳过兜底副本」）；非 0 = 硬失败（缺件、位数错、自检不过）。.
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
    # 组装+自检+列清单，但不生成 7z（用于演练；自检同样一条不放过）.
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
#  PE 头：读 Machine 字段（0x014C = x86，0x8664 = x64）.
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
#  文本小工具：读 UTF-8、取两处版本号.
# 版本三处一致 = README 当前版本行 / CHANGELOG 顶部标题 / 包名，缺一不可。.
# ---------------------------------------------------------------------------
function Read-Utf8([string]$path) {
    return [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8)
}
function Get-ReadmeVersion([string]$path) {
    $m = [regex]::Match((Read-Utf8 $path), '当前版本：\*\*([0-9]+(?:\.[0-9]+)+)\*\*')
    if ($m.Success) { return $m.Groups[1].Value }
    return $null
}
function Get-ChangelogTopVersion([string]$path) {
    $m = [regex]::Match((Read-Utf8 $path), '(?m)^##[ \t]+([0-9]+(?:\.[0-9]+)+)')
    if ($m.Success) { return $m.Groups[1].Value }
    return $null
}

# ---------------------------------------------------------------------------
#  按注册表优先顺序找争渡安装目录。.
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

# 上一次组装目录里现成的文件（包名目录下的相对路径）—— 找不到工具链时的回落。.
function Find-PreviousStageFile([string]$leafName) {
    $stageParent = Join-Path $root 'build\release_stage'
    if (-not (Test-Path -LiteralPath $stageParent)) { return $null }
    $hit = @(Get-ChildItem -Path $stageParent -Recurse -Filter $leafName -File -ErrorAction SilentlyContinue | Where-Object { -not ($pkgDir -and $_.FullName.StartsWith($pkgDir, [StringComparison]::OrdinalIgnoreCase)) } |
             Sort-Object LastWriteTime -Descending)
    if ($hit.Count -gt 0) { return $hit[0].FullName }
    return $null
}

# ---------------------------------------------------------------------------
#  0. 版本号：三处一致（README 当前版本行 / CHANGELOG 顶部标题 / 包名）。.
#    -Version 允许显式重申，但**不允许**和 README 不一致。.
# ---------------------------------------------------------------------------
$readmePath = Join-Path $root 'README.md'
$changelogPath = Join-Path $root 'CHANGELOG.md'
if (-not (Test-Path -LiteralPath $readmePath)) { Die "找不到 README.md" }
if (-not (Test-Path -LiteralPath $changelogPath)) { Die "找不到 CHANGELOG.md" }

$vReadme = Get-ReadmeVersion $readmePath
if (-not $vReadme) { Die "README.md 里读不出「当前版本：**x.y.z.w**」，请先在 README 顶部补这一行" }
$vChangelog = Get-ChangelogTopVersion $changelogPath
if (-not $vChangelog) { Die "CHANGELOG.md 里找不到顶部版本标题「## x.y.z.w」" }
if ($vChangelog -ne $vReadme) {
    Die "版本三处不一致：README.md 当前版本 $vReadme ≠ CHANGELOG.md 顶部标题 $vChangelog（发版时两处必须一起改）"
}
if ($Version) {
    if ($Version -ne $vReadme) {
        Die "版本三处不一致：-Version $Version ≠ README.md 当前版本 $vReadme（不要用参数偷偷改版本）"
    }
    Say "版本号（-Version 指定）：$Version"
} else {
    $Version = $vReadme
    Say "版本号（取自 README.md）：$Version"
}
if ($Version -notmatch '^[0-9]+(\.[0-9]+){2,3}$') {
    Die "版本号 '$Version' 不是纯数字 3-4 段（P6 版本方案要求纯数字四段，如 0.1.0.6）"
}
Say "      版本三处一致：README.md = CHANGELOG.md 顶部 = 包名 $Version"
$pkgName = "SBA11y-$Version-installer"

# 组装目录提前定下来：后面「复用上一次组装目录」时必须排除**本次**正在重建的那个。.
# 否则先解析出目录里的注入工具、下一步又把它删掉重建，Copy-In 会报路径不存在（实测坑）。.
if (-not $StageRoot) { $StageRoot = Join-Path $root "build\release_stage\$Version" }
$pkgDir = Join-Path $StageRoot $pkgName

Say ""
Say "=== 组装 $pkgName ==="

# ---------------------------------------------------------------------------
#  1. 输入文件：先把缺件**一次列全**再退，不要让人一轮一轮补。.
# ---------------------------------------------------------------------------
$problems = New-Object Collections.Generic.List[string]

if (-not $PluginDll) { $PluginDll = Join-Path $root 'a11y.dll' }
if (-not (Test-Path -LiteralPath $PluginDll)) {
    $problems.Add("找不到插件产物 a11y.dll：$PluginDll —— 先跑 build\build.cmd 的 release 构建（不带 dev）")
}

if (-not $NvdaClient) {
    foreach ($cand in @(
        (Join-Path $root 'vendor\nvdaControllerClient.dll'),
        (Join-Path $root 'tools\_cache\nvdaControllerClient.dll'),
        'D:\DSHWorkSpace\zdsr_work\BelfryForAccessibility\mod\nvda_dl\x86\nvdaControllerClient.dll'
    )) { if (Test-Path -LiteralPath $cand) { $NvdaClient = $cand; break } }
}
if (-not $NvdaClient) { $NvdaClient = Find-PreviousStageFile 'nvdaControllerClient.dll' }
if (-not $NvdaClient -or -not (Test-Path -LiteralPath $NvdaClient)) {
    $problems.Add("找不到 nvdaControllerClient.dll（x86）—— 用 -NvdaClient <路径> 指定（NV Access 官方 x86 版）")
}

if (-not $Injector) {
    foreach ($cand in @(
        (Join-Path $root 'tools\_cache\注入工具.exe'),
        (Join-Path $root 'tools\注入工具.exe'),
        (Join-Path $root 'tools\inject_boot.exe')
    )) { if (Test-Path -LiteralPath $cand) { $Injector = $cand; break } }
}
if (-not $Injector) {
    $prev = Find-PreviousStageFile '注入工具.exe'
    if ($prev) { $Injector = $prev; Say "注入工具.exe（复用上一次组装目录）：$prev" }
}
if (-not $Injector -and (Test-Path -LiteralPath (Join-Path $root 'dist')) -and (Test-Path -LiteralPath $SevenZip)) {
    # 最后回落：从 dist 里最新的发布包解出注入工具（发布包布局固定为 <包名>\tools\）。.
    $prevPkg = @(Get-ChildItem -Path (Join-Path $root 'dist') -Filter 'SBA11y-*-installer.7z' -File -ErrorAction SilentlyContinue |
                 Sort-Object LastWriteTime -Descending)
    if ($prevPkg.Count -gt 0) {
        $tmp = Join-Path $env:TEMP ('sba11y_injector_' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Force -Path $tmp | Out-Null
        & $SevenZip e $prevPkg[0].FullName 'SBA11y-*/tools/注入工具.exe' "-o$tmp" -y | Out-Null
        $cand = Join-Path $tmp '注入工具.exe'
        if (Test-Path -LiteralPath $cand) {
            $Injector = $cand
            Say "注入工具.exe（从上一次发布包解出）：$($prevPkg[0].Name)"
        }
    }
}
if (-not $Injector -or -not (Test-Path -LiteralPath $Injector)) {
    $problems.Add("找不到 tools\注入工具.exe —— 跑 pwsh -File tools\build_injector.ps1 重建（用 data\startup.tjs.bin，不需要 tjs2c），或用 -Injector <路径> 指定现成那份")
}

$licenses = Join-Path $root 'licenses\LGPL-2.1-nvdaControllerClient.txt'
if (-not (Test-Path -LiteralPath $licenses)) {
    $problems.Add("缺少 licenses\LGPL-2.1-nvdaControllerClient.txt —— LGPL-2.1 要求随二进制分发附许可证文本，不能只警告")
}

# 两份手写发布文档：从 mod\package\ 复制到包根，**不重新生成**（构建不许冲掉手写文档）。.
$pkgDocs = @(
    @{ Src = (Join-Path $root 'mod\package\安装说明.md'); As = '安装说明.md' },
    @{ Src = (Join-Path $root 'mod\package\常见问题.md'); As = '常见问题.md' }
)
foreach ($doc in $pkgDocs) {
    if (-not (Test-Path -LiteralPath $doc.Src)) {
        $problems.Add("缺少发布文档 $($doc.Src) —— 包根要有 $($doc.As)（P7 手写交付，不能靠构建生成）")
    }
}

$iniNames = @('a11y_labels.ini','a11y_labels_jp.ini','a11y_labels_en.ini','a11y_labels_tw.ini','a11y_speech.ini')
foreach ($ini in $iniNames) {
    $p = Join-Path $root "data\$ini"
    if (-not (Test-Path -LiteralPath $p)) { $problems.Add("缺少 data\$ini —— 包内 plugin\ 需要它") }
}

if ($problems.Count -gt 0) {
    Say ""
    Say "=== 发布前自检：缺件 ==="
    foreach ($p in $problems) { Write-Host "      MISS  $p" -ForegroundColor Red }
    Die ("缺 $($problems.Count) 项，发布前自检不通过 —— 补齐后重跑，不要跳过检查")
}

# ---------------------------------------------------------------------------
#  2. 争渡那两个 dll（找不到就只警告）。.
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
#  3. 组装。.
# 组装目录（$pkgDir）已在版本段算好；这里只清理上一次的组装残留。.
# ---------------------------------------------------------------------------
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
foreach ($ini in $iniNames) {
    Copy-In (Join-Path $root "data\$ini") $pluginDir
}
Copy-In $Injector (Join-Path $pkgDir 'tools') '注入工具.exe'
Copy-In (Join-Path $root 'README.md')    $pkgDir
Copy-In (Join-Path $root 'CHANGELOG.md') $pkgDir
foreach ($doc in $pkgDocs) {
    Copy-In $doc.Src $pkgDir $doc.As
}
Copy-In $licenses (Join-Path $pkgDir 'licenses')

# ZDSRAPI.ini 是**用户自己**的配置（官方文档：它的优先级高于程序调用）。.
# 随包发一份等于替玩家定死通道参数，而且会跟着升级走样 —— 明确不发。.
Get-ChildItem -LiteralPath $pluginDir -Filter 'ZDSRAPI.ini' -ErrorAction SilentlyContinue |
    ForEach-Object { Die "包里出现了 ZDSRAPI.ini（$($_.FullName)）—— 它不该随包分发" }

# ---------------------------------------------------------------------------
#  4. 发布前自检（任一项失败 exit 1，禁止警告后继续）。.
# ---------------------------------------------------------------------------
Say ""
Say "=== 发布前自检 ==="

# 4a. 包内必备文件（逐项点名；缺一就失败）。.
$required = @(
    'README.md','CHANGELOG.md','安装说明.md','常见问题.md',
    'plugin\a11y.dll','plugin\nvdaControllerClient.dll',
    'plugin\a11y_labels.ini','plugin\a11y_labels_jp.ini',
    'plugin\a11y_labels_en.ini','plugin\a11y_labels_tw.ini',
    'plugin\a11y_speech.ini',
    'tools\注入工具.exe',
    'licenses\LGPL-2.1-nvdaControllerClient.txt'
)
foreach ($need in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $pkgDir $need))) { Die "包里缺少 $need" }
}
if ($zdsrX86) {
    foreach ($need in @('plugin\ZDSRAPI.dll','plugin\ZDSRAPI_x64.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $pkgDir $need))) { Die "包里缺少 $need" }
    }
}
Say "      必备文件齐全（$($required.Count) 项）"

# 4b. 位数：插件与随包 dll 必须是 32 位（x86），争渡 x64 那份例外。.
Assert-Machine (Join-Path $pluginDir 'a11y.dll')                0x014C 'a11y.dll'
Assert-Machine (Join-Path $pluginDir 'nvdaControllerClient.dll') 0x014C 'nvdaControllerClient.dll'
if ($zdsrX86) {
    Assert-Machine (Join-Path $pluginDir 'ZDSRAPI.dll')     0x014C 'ZDSRAPI.dll'
    Assert-Machine (Join-Path $pluginDir 'ZDSRAPI_x64.dll') 0x8664 'ZDSRAPI_x64.dll'
}

# 4c. 导出必须还在（krkrz 的 V2Link 约定），且发布 DLL 不得带开发通道字符串（G40）。.
$dllPath  = Join-Path $pluginDir 'a11y.dll'
$dllBytes = [IO.File]::ReadAllBytes($dllPath)
$dllText  = [Text.Encoding]::ASCII.GetString($dllBytes)
$wideLen  = $dllBytes.Length - ($dllBytes.Length % 2)
$dllWide  = [Text.Encoding]::Unicode.GetString($dllBytes, 0, $wideLen)
foreach ($exp in @('V2Link','V2Unlink','GetModuleThreadModel')) {
    if ($dllText.IndexOf($exp, [StringComparison]::Ordinal) -lt 0) { Die "a11y.dll 里找不到导出 $exp" }
}
Say "      导出 V2Link / V2Unlink / GetModuleThreadModel 都在"
foreach ($bad in @('a11yb','sba11y_cmd')) {
    if ($dllText.IndexOf($bad, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
        $dllWide.IndexOf($bad, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
        Die "发布 DLL 内仍有开发通道字符串「$bad」（G40）：$dllPath —— 这是 dev 构建，用 build\build.cmd 的 release 模式重编"
    }
}
Say "      发布 DLL 无开发通道字符串（a11yb / sba11y_cmd，G40）"

# 4d. IP 红线：包内不得出现游戏资源 / 剧本 / 日志 / 构建中间产物。.
$forbiddenExt = @(
    '.xp3','.ks','.scn','.psb','.func','.tjs','.asd','.pimg','.mtn','.sli','.tft',
    '.ogg','.wav','.mp3','.wmv','.png','.jpg','.jpeg','.otf','.ttf',
    '.log','.pdb','.obj','.lib','.exp','.ilk','.res','.bak'
)
$forbiddenLeaf = @('sba11y_cmd.txt','a11y_load.txt','boot_link.txt','data.xp3','logoutput.log','zdsrapi.ini')
$forbiddenNameRegex = '(?i)(_diag|_res|_report|keys_|pipe_|build_log)[^/]*\.(txt|log)$'
foreach ($f in @(Get-ChildItem -LiteralPath $pkgDir -Recurse -File)) {
    $rel  = $f.FullName.Substring($pkgDir.Length + 1).Replace('\', '/')
    $leaf = $f.Name.ToLowerInvariant()
    $ext  = [IO.Path]::GetExtension($leaf)
    if ($forbiddenExt -contains $ext) {
        Die "发布包里出现不该随包的文件：$rel（游戏资源/脚本/日志一律不进包，IP 红线）"
    }
    if ($forbiddenLeaf -contains $leaf) {
        Die "发布包里出现不该随包的文件：$rel（日志/调试通道文件，IP 红线）"
    }
    if ($rel -match $forbiddenNameRegex) {
        Die "发布包里出现不该随包的文件：$rel（诊断/探针日志，IP 红线）"
    }
}

# 4e. 白名单：包内文件必须与登记清单完全一致，多一个都算自检失败。.
$expected = @()
$expected += $required
$expected += @('plugin\ZDSRAPI.dll','plugin\ZDSRAPI_x64.dll')   # 仅当本机找到争渡，单独判断
$unexpected = @()
foreach ($f in @(Get-ChildItem -LiteralPath $pkgDir -Recurse -File)) {
    $rel = $f.FullName.Substring($pkgDir.Length + 1)
    if ($zdsrX86 -and ($rel -eq 'plugin\ZDSRAPI.dll' -or $rel -eq 'plugin\ZDSRAPI_x64.dll')) { continue }
    if ($expected -notcontains $rel) { $unexpected += $rel }
}
if ($unexpected.Count -gt 0) {
    Die ("发布包里出现未登记的文件（逐项自检失败）：" + ($unexpected -join '；') + " —— 要么删掉，要么把它登记进 make_release.ps1 的清单")
}
Say "      包内文件清单与登记一致（白名单校验通过）"

# ---------------------------------------------------------------------------
#  5. 内容清单 + 归档。.
# ---------------------------------------------------------------------------
$sha = (Get-FileHash -Algorithm SHA256 (Join-Path $pluginDir 'a11y.dll')).Hash

Say ""
Say "=== 内容清单 ==="
Get-ChildItem -LiteralPath $pkgDir -Recurse -File |
    Sort-Object FullName |
    ForEach-Object { Say ("      {0,-46} {1,10:N0} B" -f ($_.FullName.Substring($pkgDir.Length + 1)), $_.Length) }

if ($DryRun) {
    Say ""
    Say "=== DRY RUN：发布前自检全过，没有生成 7z ==="
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
