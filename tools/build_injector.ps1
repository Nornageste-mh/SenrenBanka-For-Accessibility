# 编译 tools\inject_boot.cs -> inject_boot.exe（安装器）.
#
# 只用 .NET Framework 自带的 csc.exe，不需要 Visual Studio、不需要 SDK.
# 启动脚本字节码默认从 data\startup.tjs.bin 读取，base64 后嵌进 exe ——
# 这样干净克隆就能重建，不需要 tjs2c.exe（该外部工具不在仓库里，见 data\startup.tjs）.
#
# 优先级：data\startup.tjs.bin（默认）> -TjsSource + -Tjs2c（旧的源码编译回退）.
# 只有显式传 -Tjs2c 且 data\startup.tjs.bin 不存在时，才走 tjs2c 编译路径.
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File tools\build_injector.ps1 [-Out <path>]
#   powershell -ExecutionPolicy Bypass -File tools\build_injector.ps1 -Tjs2c <path> -TjsSource <path>
#
# 注意：本文件每条注释都以 ASCII 字符结尾. Windows PowerShell 5.1 会把注释行结尾的
# 全角标点当成续行符，静默吃掉下一行（详见 tools\make_release.ps1 顶部说明）.

param(
    [string]$Tjs2c,
    [string]$Out,
    [string]$TjsSource
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent          # 仓库根
$tools = $PSScriptRoot

if (-not $Out) { $Out = Join-Path $tools 'inject_boot.exe' }

# ---- 1. 找 csc ----
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $csc)) {
    $csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
}
if (-not (Test-Path -LiteralPath $csc)) { throw "找不到 csc.exe（需要 .NET Framework 4.x）" }

# ---- 2. 取启动脚本字节码 ----
$binPath = Join-Path $root 'data\startup.tjs.bin'
$bytes = $null
if (Test-Path -LiteralPath $binPath) {
    # 2a. 默认路径：读已经验证过的字节码，不调用 tjs2c.
    #     .bin 允许两种形态：明文 TJS2100，或其 0x01 混淆形态 UKR3011.
    #     （当前入库的是明文 TJS2100；嵌入 exe 的 BOOT_B64 也保持明文形态.）
    if ($Tjs2c -or $TjsSource) {
        Write-Warning "data\startup.tjs.bin 存在，忽略 -Tjs2c / -TjsSource（优先用已入库的字节码）"
    }
    $bytes = [IO.File]::ReadAllBytes($binPath)
    if ($bytes.Length -lt 8) { throw "data\startup.tjs.bin 太短，不是有效字节码" }
    $hdr = [Text.Encoding]::ASCII.GetString($bytes, 0, 7)
    if ($hdr -eq 'UKR3011') {
        for ($i = 0; $i -lt $bytes.Length; $i++) { $bytes[$i] = $bytes[$i] -bxor 0x01 }
        $hdr = 'TJS2100'
    }
    if ($hdr -ne 'TJS2100') {
        throw "data\startup.tjs.bin 不是 TJS2100 字节码，也不是它的 0x01 混淆形态（头部: $hdr）"
    }
    Write-Host ("[1/3] 启动脚本 {0} 字节字节码，来自 data\startup.tjs.bin" -f $bytes.Length)
} else {
    # 2b. 回退路径：仅在 .bin 缺失且显式传 -Tjs2c 时，用旧流程编译源码.
    #     仓库里 data\startup.tjs 已改为说明文档，源码必须用 -TjsSource 显式指定.
    if (-not $Tjs2c) {
        throw "找不到 data\startup.tjs.bin —— 干净克隆应带这个文件；若确实要从源码重建，请显式传 -Tjs2c <path> 和 -TjsSource <path>"
    }
    if (-not (Test-Path -LiteralPath $Tjs2c)) { throw "找不到 tjs2c.exe: $Tjs2c" }
    if (-not $TjsSource) {
        throw "找不到 data\startup.tjs.bin，且没有 -TjsSource —— data\startup.tjs 是说明文档，不是 TJS 源码；请用 -TjsSource 指定重建出的源码"
    }
    if (-not (Test-Path -LiteralPath $TjsSource)) { throw "找不到 TJS 源码: $TjsSource" }
    $raw = Join-Path $env:TEMP 'a11y_boot.raw'
    Remove-Item -LiteralPath $raw -ErrorAction SilentlyContinue
    & $Tjs2c $TjsSource $raw | Out-Null
    if (-not (Test-Path -LiteralPath $raw)) { throw "tjs2c 编译失败: $TjsSource" }
    $bytes = [IO.File]::ReadAllBytes($raw)
    if ($bytes.Length -lt 8 -or [Text.Encoding]::ASCII.GetString($bytes, 0, 7) -ne 'TJS2100') {
        throw "编译产物不是 TJS2100 字节码"
    }
    Write-Host ("[1/3] 启动脚本 {0} 字节字节码，来自 {1}（tjs2c 编译）" -f $bytes.Length, $TjsSource)
}
$b64 = [Convert]::ToBase64String($bytes)

# ---- 3. 嵌入并编译 ----
$src = [IO.File]::ReadAllText((Join-Path $tools 'inject_boot.cs'), [Text.Encoding]::UTF8)
$needle = 'const string BOOT_B64 = "BOOT_B64_PLACEHOLDER";'
if (-not $src.Contains($needle)) { throw "源文件里找不到 BOOT_B64 占位声明" }
# 只替换 const 那一处 —— 源码里还有一处用它做完整性校验，必须保留.
$built = $src.Replace($needle, 'const string BOOT_B64 = "' + $b64 + '";')
$tmpCs = Join-Path $env:TEMP 'inject_boot_built.cs'
[IO.File]::WriteAllText($tmpCs, $built, (New-Object Text.UTF8Encoding($false)))
Write-Host "[2/3] 已嵌入，开始编译"

& $csc /nologo /target:exe /out:$Out /r:System.Windows.Forms.dll $tmpCs
if (-not (Test-Path -LiteralPath $Out)) { throw "编译失败" }

$fi = Get-Item -LiteralPath $Out
Write-Host ("[3/3] 完成: {0}  ({1:N0} 字节)" -f $fi.FullName, $fi.Length)
Write-Host "BUILD_OK"
Write-Host ""
Write-Host "用法：双击运行，或用命令行"
Write-Host "  inject_boot.exe <原 data.xp3> <输出的 data.patched.xp3>"
