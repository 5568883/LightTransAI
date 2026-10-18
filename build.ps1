# ============================================================
#  鲤桥翻译 (LightTransAI) 构建脚本
#
#  用法:
#    pwsh -File build.ps1              构建 Release
#    pwsh -File build.ps1 -Config Debug
#    pwsh -File build.ps1 -Test        构建并运行单元测试
#    pwsh -File build.ps1 -Clean       先删掉输出目录再构建
#    pwsh -File build.ps1 -Slim        构建后清掉中间产物，只留 exe
#
#  关于磁盘占用：
#    build 目录里 .obj/.pdb/.ilk 是编译中间产物，合计可达 180 MB 以上
#    （nlohmann/json 的模板展开让三个 obj 就有 21 MB；Debug 的 pdb+ilk 有 90 MB）。
#    它们对运行毫无影响，删掉后重新构建即可再生。
#    平时想省空间就用 -Slim，或直接删 build 目录下这些扩展名。
# ============================================================
[CmdletBinding()]
param(
    [ValidateSet('Release','Debug')]
    [string]$Config = 'Release',
    [switch]$Test,
    [switch]$Clean,
    [switch]$Slim
)

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
if (-not $Root) { $Root = (Get-Location).Path }

if ($Test) { $Config = 'Debug' }

function Write-Step($msg) { Write-Host $msg -ForegroundColor Cyan }
function Write-Ok($msg)   { Write-Host $msg -ForegroundColor Green }
function Write-Err($msg)  { Write-Host $msg -ForegroundColor Red }

# ---------------- 定位 Visual Studio ----------------
Write-Step "[1/4] 定位 Visual Studio 与 MSVC 工具链..."

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    Write-Err "未找到 vswhere.exe，请确认已安装 Visual Studio。"
    exit 1
}

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath 2>$null

if (-not $vsPath) {
    $vsPath = & $vswhere -latest -products * -property installationPath 2>$null
}
if (-not $vsPath) {
    Write-Err "未找到 Visual Studio 安装路径。"
    exit 1
}

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    Write-Err "未找到 vcvars64.bat: $vcvars"
    exit 1
}
Write-Host "  VS: $vsPath"

# ---------------- 导入 MSVC 环境变量 ----------------
# 通过 cmd 执行 vcvars64.bat 并回读环境变量
$envLines = cmd /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $envLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        $name = $Matches[1]
        $value = $Matches[2]
        if ($name -and $name -notmatch '^(PROMPT|_)') {
            Set-Item -Path "env:$name" -Value $value -ErrorAction SilentlyContinue
        }
    }
}

$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $cl) {
    Write-Err "cl.exe 不可用，MSVC 环境初始化失败。"
    exit 1
}
Write-Host "  编译器: $($cl.Source)"

# ---------------- 准备输出目录 ----------------
$OutDir = Join-Path $Root "build\$Config"
if ($Clean -and (Test-Path $OutDir)) {
    Write-Step "清理 $OutDir ..."
    Remove-Item $OutDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ---------------- 编译选项 ----------------
# /utf-8          源码与执行字符集均为 UTF-8
#                 中文注释/字符串在 GBK 代码页下会被错误解码导致编译失败，此选项必需
# /Zc:__cplusplus 使 __cplusplus 报告真实标准版本（否则恒为 199711）
# /permissive-    严格一致性模式
# 注意：刻意不定义 WIN32_LEAN_AND_MEAN，它会让 windows.h 不再包含 unknwn.h，
#       导致 UIAutomation.h 的接口声明缺少 IUnknown 基类而编译失败。
$cxxFlags = @(
    '/nologo', '/std:c++20', '/utf-8', '/Zc:__cplusplus', '/permissive-',
    '/EHsc', '/W4', '/MP',
    '/DUNICODE', '/D_UNICODE', '/DNOMINMAX',
    "/I`"$Root\src`"", "/I`"$Root\third_party`""
)

if ($Config -eq 'Release') {
    $cxxFlags += @('/O2', '/GL', '/DNDEBUG', '/MT')
    $ldFlags  = @('/LTCG', '/OPT:REF', '/OPT:ICF')
} else {
    $cxxFlags += @('/Od', '/Zi', '/MTd', '/D_DEBUG')
    $ldFlags  = @('/DEBUG')
}

# 通过 cmd /c 执行命令行字符串。
# 直接向原生程序传参数组时，PowerShell 会把内嵌的引号当作字面量传给 cl/link，
# 导致 "无法识别的源文件类型" 与路径解析失败，故统一走 cmd。
function Invoke-CmdLine {
    param([string]$CmdLine)
    $out = & cmd.exe /c $CmdLine 2>&1
    return @{ Output = $out; Code = $LASTEXITCODE }
}

$sources = @(
    'src\utils\string_utils.cpp',
    'src\utils\logger.cpp',
    'src\utils\perf_monitor.cpp',
    'src\config\crypto_dpapi.cpp',
    'src\config\config_manager.cpp',
    'src\network\winhttp_client.cpp',
    'src\translation\deepseek_client.cpp',
    'src\input\hotkey_manager.cpp',
    'src\input\mouse_selection_hook.cpp',
    'src\input\text_extractor.cpp',
    'src\capture\image_encoder.cpp',
    'src\capture\screen_capturer.cpp',
    'src\recognition\ai_vision_provider.cpp',
    'src\recognition\local_ocr_provider.cpp',
    'src\ui\ui_common.cpp',
    'src\ui\float_button_window.cpp',
    'src\ui\popup_window.cpp',
    'src\ui\main_window.cpp',
    'src\app\tray_manager.cpp',
    'src\app\settings_window.cpp',
    'src\app\app_controller.cpp',
    'src\main.cpp'
)

$libs = @(
    'user32.lib','gdi32.lib','shell32.lib','ole32.lib','oleaut32.lib',
    'advapi32.lib','psapi.lib','winhttp.lib','crypt32.lib','d2d1.lib',
    'dwrite.lib','windowscodecs.lib','dwmapi.lib','uiautomationcore.lib',
    'comctl32.lib','shlwapi.lib','shcore.lib'
)

# ---------------- 编译 ----------------
Write-Step "[2/4] 编译源文件 ($Config)..."

$objList = @()
$failed  = @()

$flagStr = ($cxxFlags -join ' ')

foreach ($src in $sources) {
    $full = Join-Path $Root $src
    if (-not (Test-Path $full)) {
        Write-Err "  缺失源文件: $src"
        $failed += $src
        continue
    }
    Write-Host "  $src"

    $objOut = Join-Path $OutDir ((Split-Path $src -Leaf) -replace '\.cpp$', '.obj')
    $cmdLine = "cl.exe $flagStr /Fo`"$objOut`" /c `"$full`""
    $r = Invoke-CmdLine $cmdLine
    $r.Output | ForEach-Object {
        if ($_ -match 'error|warning C') { Write-Host "    $_" }
    }
    if ($r.Code -ne 0) { $failed += $src } else { $objList += $objOut }
}

if ($failed.Count -gt 0) {
    Write-Err "[错误] 以下文件编译失败:"
    $failed | ForEach-Object { Write-Err "  $_" }
    exit 1
}

# ---------------- 资源 ----------------
# rc.exe 的两个坑：
#  1) 源码含中文时必须以 /c65001 声明 UTF-8 代码页，否则报 RC2133/RC2132
#  2) 命令行里的路径若含空格（本工程路径含"deepseek工作区"无空格，但目录名可能变），
#     rc 会截断路径。因此改为切到目录后用相对文件名调用。
#
# 另外 app.rc 里用相对路径引用了 app.ico，而 rc 是在输出目录下执行的，
# 所以必须用 /i 把工程根目录加入搜索路径，否则找不到图标。
$resFile = Join-Path $OutDir 'app.res'
$rcFile  = Join-Path $Root 'app.rc'
$icoFile = Join-Path $Root 'app.ico'
if (-not (Test-Path $icoFile)) {
    Write-Host "  警告: 缺少 app.ico，exe 将没有自定义图标（可运行 tools\make_icon.py 生成）" -ForegroundColor Yellow
}
if (Test-Path $rcFile) {
    $r = Invoke-CmdLine "cd /d `"$OutDir`" && rc.exe /nologo /c65001 /i `"$Root`" /fo app.res `"$rcFile`""
    if ((Test-Path $resFile) -and $r.Code -eq 0) {
        $objList += $resFile
        Write-Host "  资源已编译（含版本信息与图标）"
    } else {
        # 资源失败不该静默：图标与版本信息都在里面，缺了用户会看到默认图标
        Write-Host "  资源编译失败，错误输出如下：" -ForegroundColor Yellow
        $r.Output | ForEach-Object { Write-Host "    $_" }
    }
}

# ---------------- 链接 ----------------
Write-Step "[3/4] 链接..."

$exePath = Join-Path $OutDir 'LightTransAI.exe'
$ldStr   = ($ldFlags -join ' ')
$objStr  = ($objList | ForEach-Object { "`"$_`"" }) -join ' '
$libStr  = ($libs -join ' ')

$linkCmd = "link.exe /nologo $ldStr /SUBSYSTEM:WINDOWS " +
           "/OUT:`"$exePath`" /MANIFEST:EMBED /MANIFESTINPUT:`"$Root\app.manifest`" " +
           "$objStr $libStr"

$r = Invoke-CmdLine $linkCmd
$r.Output | ForEach-Object { Write-Host "    $_" }

if ($r.Code -ne 0 -or -not (Test-Path $exePath)) {
    Write-Err "[错误] 链接失败。"
    exit 1
}

# ---------------- 完成 ----------------
Write-Step "[4/4] 完成"
$size = (Get-Item $exePath).Length
Write-Ok "[成功] $exePath"
Write-Host ("        大小: {0:N0} 字节 ({1:N1} KB)" -f $size, ($size/1KB))

# ---------------- 同步一份到工程根目录 ----------------
# 目的：让 exe 好找。build\Release\ 埋得较深，日常要运行或拷贝给别人时不方便。
#
# 为什么是"拷贝"而不是"链接/快捷方式"：
#   快捷方式在 build 目录被 -Clean 清掉后会失效；拷贝出来的 exe 是独立文件，
#   随时可双击、可发给别人，不依赖工程目录结构。
#
# 只对 Release 做同步：Debug 版依赖调试环境，拷到根目录没有意义，
# 而且会和 Release 版同名冲突，容易让人误运行到慢速的调试版。
if ($Config -eq 'Release') {
    $rootExe = Join-Path $Root '鲤桥翻译.exe'
    # 目标文件可能正在运行，拷贝会失败；这时给出明确提示而不是让脚本报错中断
    try {
        Copy-Item $exePath $rootExe -Force -ErrorAction Stop
        Write-Ok "[根目录] $rootExe"
    } catch {
        Write-Host "  提示: 无法更新根目录的 鲤桥翻译.exe（文件可能正在运行）" -ForegroundColor Yellow
        Write-Host "        请先退出程序再重新构建，或手动拷贝："
        Write-Host "        copy `"$exePath`" `"$rootExe`""
    }
}

# ---------------- 单元测试 ----------------
if ($Test) {
    $testSrc = Join-Path $Root 'tests\test_all.cpp'
    if (Test-Path $testSrc) {
        Write-Step "构建并运行单元测试..."
        $testDir = Join-Path $OutDir 'tests'
        New-Item -ItemType Directory -Force -Path $testDir | Out-Null

        $testObj = Join-Path $testDir 'test_all.obj'
        $r = Invoke-CmdLine "cl.exe $flagStr /Fo`"$testObj`" /c `"$testSrc`""
        $r.Output | ForEach-Object { if ($_ -match 'error') { Write-Host "    $_" } }

        if ($r.Code -eq 0 -and (Test-Path $testObj)) {
            $testExe = Join-Path $OutDir 'LightTransAI_tests.exe'

            # 只链接被测模块，排除 UI / 入口相关目标文件
            $exclude = @('main.obj','app_controller.obj','settings_window.obj',
                         'tray_manager.obj','popup_window.obj','float_button_window.obj',
                         'ui_common.obj','app.res')
            $testObjs = $objList | Where-Object {
                $leaf = Split-Path $_ -Leaf
                $exclude -notcontains $leaf
            }

            $testLibs = $libs | Where-Object {
                $_ -notin @('d2d1.lib','dwrite.lib','dwmapi.lib','windowscodecs.lib','shcore.lib')
            }

            $tObjStr  = ($testObjs | ForEach-Object { "`"$_`"" }) -join ' '
            $tLibStr  = ($testLibs -join ' ')
            $tLdStr   = ($ldFlags -join ' ')
            $testLink = "link.exe /nologo $tLdStr /SUBSYSTEM:CONSOLE " +
                        "/OUT:`"$testExe`" `"$testObj`" $tObjStr $tLibStr"

            $lr = Invoke-CmdLine $testLink
            $lr.Output | ForEach-Object { Write-Host "    $_" }

            if ((Test-Path $testExe) -and $lr.Code -eq 0) {
                Write-Host ""
                & $testExe
                Write-Host ("测试退出码: {0}" -f $LASTEXITCODE)
            } else {
                Write-Err "测试程序链接失败。"
            }
        } else {
            Write-Err "测试代码编译失败。"
        }
    } else {
        Write-Host "  未找到 tests\test_all.cpp，跳过测试。" -ForegroundColor Yellow
    }
}

# ---------------- 清理中间产物（-Slim）----------------
# 只删 .obj/.pdb/.ilk/.res/.exp 与 cl 的 vc140.pdb，**保留所有 exe**。
# 这些文件合计可达 180 MB 以上，删掉不影响运行，重新构建即再生。
if ($Slim) {
    Write-Step "清理中间产物（保留 exe）..."
    $freed = 0

    # build 目录下的常规中间产物
    foreach ($ext in @('*.obj','*.pdb','*.ilk','*.res','*.exp')) {
        $files = Get-ChildItem (Join-Path $Root 'build') -Recurse -File -Filter $ext -ErrorAction SilentlyContinue
        if ($files) {
            $freed += ($files | Measure-Object Length -Sum).Sum
            $files | Remove-Item -Force -ErrorAction SilentlyContinue
        }
    }

    # cl.exe 在 Debug 编译时会把 vc140.pdb 生成到**当前工作目录**，
    # 也就是工程根目录（不是 build 目录）。不清理的话它会一直躺在根目录，
    # 每编译一次就多占约 3 MB。
    $vcPdb = Join-Path $Root 'vc140.pdb'
    if (Test-Path $vcPdb) {
        $freed += (Get-Item $vcPdb).Length
        Remove-Item $vcPdb -Force -ErrorAction SilentlyContinue
    }

    # 删掉因此变空的子目录
    Get-ChildItem (Join-Path $Root 'build') -Recurse -Directory -ErrorAction SilentlyContinue |
        Where-Object { (Get-ChildItem $_.FullName -File -ErrorAction SilentlyContinue).Count -eq 0 } |
        Remove-Item -Force -ErrorAction SilentlyContinue

    Write-Ok ("  已释放 {0:N1} MB" -f ($freed / 1MB))
    $remain = (Get-ChildItem (Join-Path $Root 'build') -Recurse -File -ErrorAction SilentlyContinue |
               Measure-Object Length -Sum).Sum
    Write-Host ("  build 目录现为 {0:N1} MB" -f ($remain / 1MB))
}
