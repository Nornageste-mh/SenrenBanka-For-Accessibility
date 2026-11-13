# ============================================================================
#  check-staged.ps1 —— 提交前守卫（千恋＊万花 / KiriKiri Z 路线）.
#
#  0.2.0.0 起界面操作 = 无模式键（方向键直接选、Enter/Space 激活、Esc 返回），.
#  旧的 Tab / 导航模式已删；下面的文档断言按这个新口径查。.
#
#  这一版要挡什么（P8 提交前守卫 + IP 红线）：.
#    · 分析笔记 / 反编译产物 / 逆向工具的输出（这些留本地，不入库）。.
#    · 游戏资源与脚本：*.xp3 / *.psb / *.ks / *.scn / *.pimg / *.ogg 等。.
#    · 剧本文本、玩家存档样本（剧透 + 版权双重风险）。.
#    · 日志与探针输出：*_diag.txt / *_res.txt / sba11y_cmd.txt / build_log* 等。.
#    · 构建产物与第三方二进制：*.obj / *.lib / *.pdb / a11y.dll / *.exe / *.dll。.
#    · 仓外路径混入（_a11y-sb / zdsr_work / Framework 之类只该在 D:\DSHWorkSpace 下）。.
#
#  机械断言（不依赖人的自觉，任一条不满足就 exit 1）：.
#    · docs\交互形态判定.md 存在，且 §1 已填 A/B/C 结论（勾选或「结论：X 类」）。.
#    · docs\操作约定.md 存在，且「是否使用遗留导航模式」= 否（0.2.0.0 起无模式键）。.
#    · 仓库文档不得把「按 Tab 进入 / 先按 Tab / 导航模式」写成推荐用法（历史说明放行）。.
#    · README「主要快捷键」附近必须声明「无需模式键 / 不需要按任何模式键」。.
#    · 版本三处一致：README 当前版本 = CHANGELOG 顶部标题 = dist 里最新包名（若已打包）。.
#
#  用法：.
#      pwsh -File tools\check-staged.ps1
#      powershell -ExecutionPolicy Bypass -File tools\check-staged.ps1
#
#  退出码：0 = PASS；1 = 命中禁止项或断言失败。.
#
#  【为什么暂存区为空也要跑】发布前 git add 之后才是完整的闸，但「文件内容对不对」.
#  不依赖暂存区：文档的无模式键口径、判定文件的结论、版本一致性随时可查。.
#  所以暂存区为空时只跳过「暂存扫描」，内容断言照跑。.
#
#  【坑 · 本文件每条注释都以 ASCII 字符结尾，别改成中文标点】。.
#  Windows PowerShell 5.1 把注释行结尾的全角标点当成续行符，会静默吃掉下一行。.
#  详见 tools\make_release.ps1 顶部说明。.
# ============================================================================
[CmdletBinding()]
param(
    # 本地排查用：只报问题，不改变退出码.
    [switch]$NoFail
)
$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding($false) } catch { }

$root = Split-Path $PSScriptRoot -Parent
function Say([string]$msg) { Write-Host $msg }

$violations = New-Object Collections.Generic.List[string]
function Add-Fail([string]$msg) {
    Write-Host "FAIL  $msg" -ForegroundColor Red
    [void]$violations.Add($msg)
}
function Add-Ok([string]$msg) { Write-Host "  OK  $msg" -ForegroundColor Green }
function Add-Info([string]$msg) { Write-Host "      $msg" }

# 取 markdown 里某一节：从匹配 $headingPattern 的标题行到下一个二级标题。.
# 返回 @{ Found = $true/$false; Lines = 行数组 }，避免 PowerShell 把单行数组展开成标量。.
function Get-MarkdownSection([string[]]$lines, [string]$headingPattern) {
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match $headingPattern) { $start = $i; break }
    }
    if ($start -lt 0) { return @{ Found = $false; Lines = @() } }
    $end = $lines.Count
    for ($j = $start + 1; $j -lt $lines.Count; $j++) {
        if ($lines[$j] -match '^##\s') { $end = $j; break }
    }
    return @{ Found = $true; Lines = @($lines[$start..($end - 1)]) }
}

# 取「§1」那一节：先找编号标题（1/一/§1），找不到就退回全文前 120 行（够放 §1）。.
function Get-Section1Text([string]$text) {
    $lines = @($text -split "`r?`n")
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^#{1,4}\s*(1|一)[\.、）\) ]') { $start = $i; break }
        if ($lines[$i] -match '^#{1,4}\s*§\s*1') { $start = $i; break }
    }
    if ($start -lt 0) { return (($lines | Select-Object -First 120) -join "`n") }
    $end = $lines.Count
    for ($j = $start + 1; $j -lt $lines.Count; $j++) {
        if ($lines[$j] -match '^#{1,4}\s*(2|二)[\.、）\) ]') { $end = $j; break }
        if ($lines[$j] -match '^#{1,4}\s*§\s*2') { $end = $j; break }
    }
    return (($lines[$start..($end - 1)]) -join "`n")
}

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

# ============================================================================
#  1. 暂存区扫描.
# ============================================================================
Say ""
Say "=== 1. 暂存区扫描 ==="

$staged = @()
if (Get-Command git -ErrorAction SilentlyContinue) {
    $staged = @(& git -C $root -c core.quotepath=false diff --cached --name-only 2>$null)
}
$staged = @($staged | ForEach-Object { ([string]$_).Trim() } | Where-Object { $_ -ne '' })

# 路径片段黑名单（对「相对仓库根、正斜杠」的路径做正则匹配）。.
$pathRules = @(
    @{ Re = '(?i)(^|/)(analysis|notes?|decompil(ed|er)|extract(ed)?|dumps?|probe|evidence|_notes|_a11y-sb)(/|$)'; Why = '分析笔记/反编译产物/仓外证据' },
    @{ Re = '(?i)(^|/)(bep_dl|nvda_dl|ndl|dl)(/|$)';                                                            Why = '外部下载目录' },
    @{ Re = '(?i)(^|/)(dist|savedata|save_backup|shots|__pycache__|\.pytest_cache)(/|$)';                       Why = '构建/存档/截图/缓存目录' },
    @{ Re = '(?i)(^|/)tools/(dec|scn|scn_dec|lang|ks|tjs2dis|tjsdis2|patch|extract)(/|$)';                      Why = '逆向/解包工具目录' },
    @{ Re = '(?i)(^|/)build/(release_stage|bin|obj|release)(/|$)';                                              Why = '构建输出目录' },
    @{ Re = '(?i)\.xp3($|\.)';                                                                                  Why = '游戏资源归档 *.xp3' },
    @{ Re = '(?i)(^|/)(zdsr_work|Framework|MeiKongForAccessibility)(/|$)';                                       Why = '仓外路径混入' }
)
# 扩展名黑名单：游戏资源 / 脚本 / 二进制 / 日志 / 存档 / 构建中间产物。.
$badExt = @(
    '.xp3','.sig','.cf','.psb','.ks','.scn','.func','.asd','.tjs','.pimg','.mtn','.sli','.tft',
    '.ogg','.wav','.mp3','.wmv','.m4a','.opus','.png','.jpg','.jpeg','.gif','.webp','.otf','.ttf',
    '.exe','.dll','.pdb','.obj','.lib','.exp','.ilk','.res','.zip','.7z','.rar',
    '.log','.sav','.dat','.ksd','.pyc','.bin'
)
$badLeaf = @(
    'tmp_commit_msg.txt','tmp_msg.txt','commit_msg.txt','msg.txt','_review_diff.txt',
    'sba11y_cmd.txt','a11y_load.txt','boot_link.txt','build_log.txt','logoutput.log',
    'zdsrapi.ini','data.xp3'
)
$badNameRegex = @(
    '(?i)(^|/)[^/]*(_diag|_res|_report|keys_|pipe_|a11y_load|boot_link|build_log)[^/]*\.(txt|log)$',
    '(?i)(^|/)(spoiler|剧透|台词|剧本|scenario|script)[^/]*\.(md|txt|json|ks|scn)$'
)
# 例外：安装器的启动槽说明文件（G45「安装器必须能从仓库重建」）。.
# data\startup.tjs 是 krkrz 的 startup 脚本模板（BSD 许可），只被安装器读取用来改写.
# data.xp3 的启动槽位，不含剧情；startup.tjs.bin 是它的预编译字节码。.
$allowedPaths = @('data/startup.tjs','data/startup.tjs.bin')

if ($staged.Count -eq 0) {
    Add-Info "暂存区为空（还没有 git add，或当前目录不是 git 仓库）：跳过暂存扫描，下面的内容断言照跑。"
    Add-Info "发布前执行 git add -A 之后再跑一次，才是完整的提交前守卫。"
} else {
    Add-Info "暂存区有 $($staged.Count) 个文件，逐项扫描"
    foreach ($f in $staged) {
        $rel = $f.Replace('\', '/')
        # 删除动作（暂存了删除，但工作区已无此文件）不算违规：清理历史垃圾是好事。.
        if (-not (Test-Path -LiteralPath (Join-Path $root $rel))) { continue }
        $leaf = ($rel -split '/')[-1]
        $lower = $rel.ToLowerInvariant()
        $allowed = ($allowedPaths -contains $lower)
        $hit = $false
        foreach ($rule in $pathRules) {
            if ($rel -match $rule.Re) {
                Add-Fail "禁止入库（$($rule.Why)）：$rel"
                $hit = $true
                break
            }
        }
        if ($hit) { continue }
        if (-not $allowed) {
            $ext = [IO.Path]::GetExtension($leaf).ToLowerInvariant()
            if ($badExt -contains $ext) {
                Add-Fail "禁止入库（$ext 是游戏资源/脚本/日志/二进制/构建产物）：$rel"
                continue
            }
        }
        if ($badLeaf -contains $leaf.ToLowerInvariant()) {
            Add-Fail "禁止入库（临时提交说明/日志/调试文件）：$rel"
            continue
        }
        $nameHit = $false
        foreach ($re in $badNameRegex) {
            if ($rel -match $re) { Add-Fail "禁止入库（剧本/剧透/日志文件）：$rel"; $nameHit = $true; break }
        }
        if ($nameHit) { continue }
        # 编码纪律：.ps1 必须 UTF-8 BOM；.cmd/.bat 必须纯 ASCII 且无 BOM。.
        if ($rel -match '(?i)\.ps1$') {
            $b = [IO.File]::ReadAllBytes((Join-Path $root $rel))
            if ($b.Length -lt 3 -or $b[0] -ne 0xEF -or $b[1] -ne 0xBB -or $b[2] -ne 0xBF) {
                Add-Fail "PowerShell 脚本必须带 UTF-8 BOM（Windows PowerShell 5.1 否则会把中文读成乱码）：$rel"
            }
        }
        if ($rel -match '(?i)\.(cmd|bat)$') {
            $b = [IO.File]::ReadAllBytes((Join-Path $root $rel))
            $hasBom = ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF)
            $nonAscii = @($b | Where-Object { $_ -gt 127 }).Count
            if ($hasBom) { Add-Fail "批处理文件不得带 UTF-8 BOM（cmd.exe 会把 BOM 当命令）：$rel" }
            elseif ($nonAscii -gt 0) { Add-Fail "批处理文件必须纯 ASCII（中文注释会让 cmd.exe 按代码页乱码）：$rel" }
        }
    }
    if ($violations.Count -eq 0) { Add-Ok "暂存区扫描通过（没有游戏资源 / 日志 / 构建产物 / 仓外路径混入）" }
}

# ============================================================================
#  2. 内容断言.
# ============================================================================
Say ""
Say "=== 2. 内容断言 ==="

# 2a. 交互形态判定：§1 必须填 A/B/C 结论。.
$judgePath = Join-Path $root 'docs\交互形态判定.md'
if (-not (Test-Path -LiteralPath $judgePath)) {
    Add-Fail "缺少 docs\交互形态判定.md（第 0 条纪律的交付物；§1 必须填 A/B/C 结论）"
} else {
    $judgeText = Read-Utf8 $judgePath
    $sec1 = Get-Section1Text $judgeText
    $mChecked = [regex]::Match($sec1, '(?m)^\s*[-*]\s*\[[xX]\]\s*\*\*([ABC])[\.．、]')
    $mConcl = [regex]::Match($sec1, '结论[^。\r\n]*?([ABC])\s*类')
    if (-not ($mChecked.Success -or $mConcl.Success)) {
        Add-Fail "docs\交互形态判定.md 的 §1 没有填 A/B/C 结论（要勾选一项，或写「结论：X 类」）"
    } else {
        $letter = 'A'
        if ($mChecked.Success) { $letter = $mChecked.Groups[1].Value }
        elseif ($mConcl.Success) { $letter = $mConcl.Groups[1].Value }
        Add-Ok "交互形态判定 §1 已填结论：$letter 类"
        if ($mChecked.Success -and $mConcl.Success -and $mChecked.Groups[1].Value -ne $mConcl.Groups[1].Value) {
            Add-Fail "交互形态判定 §1 的勾选（$($mChecked.Groups[1].Value) 类）与结论（$($mConcl.Groups[1].Value) 类）不一致"
        }
    }
}

# 2b. 操作约定：「是否使用遗留导航模式」必须 = 否（0.2.0.0 起无模式键）。.
$opsPath = Join-Path $root 'docs\操作约定.md'
$readmePath = Join-Path $root 'README.md'
if (-not (Test-Path -LiteralPath $opsPath)) {
    Add-Fail "缺少 docs\操作约定.md（第 0 条纪律的交付物：操作逻辑公约的落地说明）"
} else {
    $opsLines = [IO.File]::ReadAllLines($opsPath, [Text.Encoding]::UTF8)
    $idx = -1
    for ($i = 0; $i -lt $opsLines.Count; $i++) {
        if ($opsLines[$i] -match '遗留导航模式') { $idx = $i; break }
    }
    if ($idx -lt 0) {
        Add-Fail "docs\操作约定.md 里找不到「是否使用遗留导航模式」条目（0.2.0.0 起应填「否」）"
    } else {
        $line = $opsLines[$idx]
        $ln = $idx + 1
        $yesChecked = ($line -match '[☑✓√●▓]\s*\**\s*是')
        $noChecked = ($line -match '[☑✓√●▓]\s*\**\s*否')
        if ($yesChecked) {
            Add-Fail "docs\操作约定.md 第 $ln 行仍写着「使用遗留导航模式 = 是」，应改为「☑ 否」或「否（0.2.0.0 起移除）」：$($line.Trim())"
        } elseif ($noChecked -or ($line -match '否' -and $line -notmatch '是')) {
            Add-Ok "docs\操作约定.md 已登记「不使用遗留导航模式」（第 $ln 行）"
        } else {
            Add-Fail "docs\操作约定.md 第 $ln 行的「遗留导航模式」条目判不出「否」（写成「☑ 否」或「否（0.2.0.0 起移除）」）：$($line.Trim())"
        }
    }
}

# 2c. 五份文档不得把「按 Tab 进入 / 先按 Tab / 导航模式」写成推荐用法。.
#     只拦「要求玩家按模式键」的句子；「已移除 / 不再需要 Tab」这类历史说明放行。.
$modeKeyDocs = @(
    @{ Path = $readmePath; Label = 'README.md' },
    @{ Path = (Join-Path $root 'mod\package\安装说明.md'); Label = 'mod\package\安装说明.md' },
    @{ Path = (Join-Path $root 'mod\package\常见问题.md'); Label = 'mod\package\常见问题.md' },
    @{ Path = (Join-Path $root 'docs\交互形态判定.md'); Label = 'docs\交互形态判定.md' },
    @{ Path = (Join-Path $root 'docs\操作约定.md'); Label = 'docs\操作约定.md' }
)
$historyWords = '(已移除|已删除|已废弃|移除|删除|废弃|作废|不再需要|不再|不需要|无需|不用|不提供|未提供|不是推荐|不推荐|不得|禁止|没有|無|无模式键|历史)'
$badModePatterns = @(
    @{ Re = '(先|首先|然后|再|请|需要|須|须)\s*(按|按下|敲|点)?\s*.{0,4}Tab'; Why = '「先按 Tab」' },
    @{ Re = '(按|按下|敲)\s*.{0,4}Tab\s*.{0,6}(进入|退出|打开|开启|切换|启动)'; Why = '「按 Tab 进入」' },
    @{ Re = 'Tab\s*.{0,8}(进入|退出|打开|开启|切换|启动).{0,12}(导航|界面)'; Why = '「Tab 进入导航模式」' },
    @{ Re = '(进入|打开|开启|切换|使用|启用)\s*.{0,6}(导航模式|导航)'; Why = '「进入导航模式」' },
    @{ Re = '导航模式\s*.{0,8}(推荐|建议|默认|首选|请用|使用)'; Why = '把「导航模式」写成推荐/默认' }
)
$modeKeyHits = 0
foreach ($doc in $modeKeyDocs) {
    if (-not (Test-Path -LiteralPath $doc.Path)) {
        Add-Fail "缺少 $($doc.Label)（无模式键口径的文档断言要读它）"
        continue
    }
    $docLines = [IO.File]::ReadAllLines($doc.Path, [Text.Encoding]::UTF8)
    $offenders = New-Object Collections.Generic.List[string]
    for ($i = 0; $i -lt $docLines.Count; $i++) {
        $text = $docLines[$i]
        if ($text -notmatch 'Tab|导航模式|模式键') { continue }
        if ($text -match $historyWords) { continue }
        foreach ($p in $badModePatterns) {
            if ($text -match $p.Re) {
                [void]$offenders.Add("第 $($i + 1) 行（$($p.Why)）：$($text.Trim())")
                break
            }
        }
    }
    if ($offenders.Count -gt 0) {
        $modeKeyHits += $offenders.Count
        $show = @($offenders | Select-Object -First 8)
        Add-Fail ("$($doc.Label) 把模式键写成了推荐用法（$($offenders.Count) 处，逐句列在下面）：" + [Environment]::NewLine + ($show -join [Environment]::NewLine))
        if ($offenders.Count -gt 8) { Add-Info "……还有 $($offenders.Count - 8) 处（先修上面这些，改完再跑一次）" }
    }
}
if ($modeKeyHits -eq 0) { Add-Ok "五份文档都没有把「按 Tab / 先按 Tab / 导航模式」写成推荐用法（历史说明已放行）" }

# 2d. README「主要快捷键」附近必须有「无需模式键 / 不需要按任何模式键」声明。.
if (-not (Test-Path -LiteralPath $readmePath)) {
    Add-Fail "缺少 README.md"
} else {
    $readmeLines = [IO.File]::ReadAllLines($readmePath, [Text.Encoding]::UTF8)
    $sec = Get-MarkdownSection -lines $readmeLines -headingPattern '^##\s*主要快捷键'
    if (-not $sec.Found) {
        Add-Fail "README.md 里找不到「## 主要快捷键」一节（快捷键表是发布文档的固定结构）"
    } else {
        $headIdx = -1
        for ($i = 0; $i -lt $readmeLines.Count; $i++) {
            if ($readmeLines[$i] -match '^##\s*主要快捷键') { $headIdx = $i; break }
        }
        $ctxStart = [Math]::Max(0, $headIdx - 8)
        $ctxEnd = $headIdx + $sec.Lines.Count - 1
        $nearLines = @($readmeLines[$ctxStart..$ctxEnd])
        $declRe = '无需模式键|不需要模式键|不需要按任何模式键|无需按任何模式键|不用按任何模式键|无模式键|没有模式键|不用模式键|不需要先按|不需要按.{0,8}模式键|无需按.{0,8}模式键'
        $decl = @($nearLines | Where-Object { $_ -match $declRe })
        if ($decl.Count -eq 0) {
            Add-Fail "README.md 的「## 主要快捷键」附近没有声明「无需模式键 / 不需要按任何模式键」（0.2.0.0 起界面操作无模式键）"
        } else {
            Add-Ok "README「主要快捷键」附近已声明「无需模式键 / 不需要按任何模式键」"
        }
    }
}

# 2e. 版本三处一致：README 当前版本 / CHANGELOG 顶部标题 / dist 最新包名。.
$changelogPath = Join-Path $root 'CHANGELOG.md'
if (-not (Test-Path -LiteralPath $changelogPath)) {
    Add-Fail "缺少 CHANGELOG.md"
} else {
    $vReadme = Get-ReadmeVersion $readmePath
    $vChangelog = Get-ChangelogTopVersion $changelogPath
    if (-not $vReadme) {
        Add-Fail "README.md 里读不出「当前版本：**x.y.z.w**」"
    } elseif (-not $vChangelog) {
        Add-Fail "CHANGELOG.md 里找不到顶部版本标题「## x.y.z.w」"
    } elseif ($vChangelog -ne $vReadme) {
        Add-Fail "版本不一致：README.md 当前版本 $vReadme ≠ CHANGELOG.md 顶部标题 $vChangelog"
    } else {
        Add-Ok "README 当前版本 = CHANGELOG 顶部标题 = $vReadme"
        $pkgs = @(Get-ChildItem -LiteralPath (Join-Path $root 'dist') -Filter 'SBA11y-*-installer.7z' -File -ErrorAction SilentlyContinue)
        if ($pkgs.Count -eq 0) {
            Add-Info "dist 里还没有发布包；打包（make_release.ps1）之后复跑，即可完成「包名」这第三处校验"
        } else {
            $newest = $pkgs | Sort-Object LastWriteTime -Descending | Select-Object -First 1
            $mPkg = [regex]::Match($newest.Name, '^SBA11y-([0-9]+(?:\.[0-9]+)+)-installer\.7z$')
            if (-not $mPkg.Success) {
                Add-Fail "dist 里的发布包名不合规（应为 SBA11y-<版本>-installer.7z）：$($newest.Name)"
            } elseif ($mPkg.Groups[1].Value -ne $vReadme) {
                Add-Fail "版本三处不一致：README $vReadme ≠ dist 最新包 $($newest.Name)"
            } else {
                Add-Ok "版本三处一致：README = CHANGELOG = 最新包名 $vReadme"
            }
        }
    }
}

# ============================================================================
#  3. 汇总.
# ============================================================================
Say ""
if ($violations.Count -gt 0) {
    Write-Host "=== 提交前守卫 FAIL（$($violations.Count) 项）===" -ForegroundColor Red
    foreach ($v in $violations) { Write-Host "  - $v" -ForegroundColor Red }
    if ($NoFail) { exit 0 }
    exit 1
}
Write-Host "PASS  提交前守卫全过（暂存扫描 + 内容断言）" -ForegroundColor Green
exit 0
