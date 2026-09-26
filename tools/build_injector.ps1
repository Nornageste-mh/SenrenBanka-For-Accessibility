# 编译 tools\inject_boot.cs -> inject_boot.exe（安装器）
#
# 只用 .NET Framework 自带的 csc.exe，不需要 Visual Studio、不需要 SDK。
# 编译前把 data\startup.tjs 编译成字节码并混淆，再以 base64 嵌进 exe ——
# 这样用户拿到的 exe 是自包含的，不用再带 .tjs 或 .bin。
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File tools\build_injector.ps1 [-Tjs2c <path>] [-Out <path>]

param(
    [string]$Tjs2c,
    [string]$Out
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

# ---- 2. 找 tjs2c ----
if (-not $Tjs2c) {
    foreach ($cand in @(
        (Join-Path $env:USERPROFILE 'tjs2b\tjs2c.exe'),
        'D:\tjs2b\tjs2c.exe'
    )) { if (Test-Path -LiteralPath $cand) { $Tjs2c = $cand; break } }
}
if (-not $Tjs2c) { $Tjs2c = $env:TJS2C }
if (-not $Tjs2c -or -not (Test-Path -LiteralPath $Tjs2c)) {
    throw "找不到 tjs2c.exe —— 用 -Tjs2c <path> 指定（32 位 tjs2 工具链）"
}

# ---- 3. 编译启动脚本 ----
$script = Join-Path $root 'data\startup.tjs'
$raw    = Join-Path $env:TEMP 'a11y_boot.raw'
Remove-Item -LiteralPath $raw -ErrorAction SilentlyContinue
& $Tjs2c $script $raw | Out-Null
if (-not (Test-Path -LiteralPath $raw)) { throw "tjs2c 编译失败: $script" }
$bytes = [IO.File]::ReadAllBytes($raw)
if ([Text.Encoding]::ASCII.GetString($bytes, 0, 7) -ne 'TJS2100') {
    throw "编译产物不是 TJS2100 字节码"
}
# 落盘形态：逐字节 XOR 0x01（TJS2100 <-> UKR3011）
$stored = New-Object byte[] $bytes.Length
for ($i = 0; $i -lt $bytes.Length; $i++) { $stored[$i] = $bytes[$i] -bxor 0x01 }
$b64 = [Convert]::ToBase64String($stored)
Write-Host ("[1/3] 启动脚本 {0} 字节字节码 -> base64 {1} 字符" -f $bytes.Length, $b64.Length)

# ---- 4. 嵌入并编译 ----
$src = [IO.File]::ReadAllText((Join-Path $tools 'inject_boot.cs'), [Text.Encoding]::UTF8)
$needle = 'const string BOOT_B64 = "BOOT_B64_PLACEHOLDER";'
if (-not $src.Contains($needle)) { throw "源文件里找不到 BOOT_B64 占位声明" }
# 只替换 const 那一处 —— 源码里还有一处用它做完整性校验，必须保留
$built = $src.Replace($needle, 'const string BOOT_B64 = "' + $b64 + '";')
$tmpCs = Join-Path $env:TEMP 'inject_boot_built.cs'
[IO.File]::WriteAllText($tmpCs, $built, (New-Object Text.UTF8Encoding($false)))
Write-Host "[2/3] 已嵌入，开始编译"

& $csc /nologo /target:exe /out:$Out /r:System.Windows.Forms.dll $tmpCs
if (-not (Test-Path -LiteralPath $Out)) { throw "编译失败" }

$fi = Get-Item -LiteralPath $Out
Write-Host ("[3/3] 完成: {0}  ({1:N0} 字节)" -f $fi.FullName, $fi.Length)
Write-Host ""
Write-Host "用法：双击运行，或用命令行"
Write-Host "  inject_boot.exe <原 data.xp3> <输出的 data.patched.xp3>"
