@echo off
chcp 936 >nul
title DeepSeek Harness

rem ---- 清理上次中断可能残留的临时文件 ----
del "%TEMP%\dsh-npm.running" 2>nul
del "%TEMP%\mirror_node_ver.txt" 2>nul
del "%TEMP%\node-setup.msi" 2>nul

rem ============ 静默检测 dsh 环境（此阶段无输出） ============
where dsh >nul 2>&1
if errorlevel 1 goto :install_dsh
call dsh --version >nul 2>&1
if errorlevel 1 goto :install_dsh

rem ============ 判断是否已在运行 ============
:ready
set "PIDFILE=%TEMP%\dsh-web.pid"
set "IS_RUNNING="
if exist "%PIDFILE%" (
    for /f "usebackq delims=" %%i in ("%PIDFILE%") do set "OLD_PID=%%i"
    powershell -NoProfile -Command "if(Get-Process -Id $env:OLD_PID -ErrorAction SilentlyContinue){exit 0}else{exit 1}"
    if not errorlevel 1 set "IS_RUNNING=1"
)
if not defined IS_RUNNING (
    netstat -ano | findstr /R /C:":3080 " | findstr "LISTENING" >nul
    if not errorlevel 1 set "IS_RUNNING=1"
)
if defined IS_RUNNING goto :running_menu

rem ---- 未运行：短暂选择（5秒无操作默认启动） ----
choice /C 1U0 /N /T 5 /D 1 /M "[1] 启动  [U] 卸载  [0] 退出 (5秒无操作默认启动): "
if errorlevel 3 exit
if errorlevel 2 goto :uninstall_menu

rem ============ 启动服务（后台运行，窗口1分钟后自动隐藏） ============
for /f "usebackq delims=" %%i in (`powershell -NoProfile -Command "(Start-Process -WindowStyle Hidden -FilePath 'cmd.exe' -ArgumentList '/c dsh web' -PassThru).Id"`) do set "DSH_PID=%%i"
> "%PIDFILE%" echo %DSH_PID%

echo ========================================
echo   DeepSeek Harness 正在运行...
echo   服务地址: http://127.0.0.1:3080
echo   浏览器即将自动打开
echo   本窗口约1分钟后自动隐藏，服务在后台继续运行
echo   停止服务: 再次双击本脚本，选择 S
echo ========================================

ping -n 5 127.0.0.1 >nul
powershell -NoProfile -Command "if(Get-Process -Id $env:DSH_PID -ErrorAction SilentlyContinue){exit 0}else{exit 1}"
if errorlevel 1 goto :start_fail

start "" http://127.0.0.1:3080

timeout /t 54 /nobreak
powershell -NoProfile -Command "if(Get-Process -Id $env:DSH_PID -ErrorAction SilentlyContinue){exit 0}else{exit 1}"
if errorlevel 1 goto :start_fail

echo.
echo 窗口即将隐藏，服务在后台运行中。
echo 停止服务: 再次双击本脚本，选择 S
ping -n 3 127.0.0.1 >nul
exit

:start_fail
del "%PIDFILE%" 2>nul
netstat -ano | findstr /R /C:":3080 " | findstr "LISTENING" >nul
if errorlevel 1 (
    echo.
    echo [错误] dsh 启动失败，请检查环境后重试。
    echo.
    pause
    exit /b 1
)
goto :running_menu

rem ============ 已在运行：选择操作 ============
:running_menu
echo.
echo ==========================================
echo   DeepSeek Harness 已在运行中
echo   服务地址: http://127.0.0.1:3080
echo ==========================================
choice /C SOU /N /T 30 /D O /M "S=停止服务, O=打开网页, U=卸载 (30秒无操作默认打开网页): "
if errorlevel 3 goto :uninstall_menu
if errorlevel 2 goto :do_open
if errorlevel 1 goto :do_stop
exit

:do_open
start "" http://127.0.0.1:3080
timeout /t 2 /nobreak >nul
exit

:do_stop
set "STOP_PID="
if exist "%PIDFILE%" for /f "usebackq delims=" %%i in ("%PIDFILE%") do set "STOP_PID=%%i"
if not defined STOP_PID (
    echo 此实例不是由本脚本后台启动，无法自动停止，请关闭对应程序后重试。
    timeout /t 3 /nobreak >nul
    exit
)
powershell -NoProfile -Command "if(Get-Process -Id $env:STOP_PID -ErrorAction SilentlyContinue){exit 0}else{exit 1}"
if errorlevel 1 (
    echo [dsh] 后台进程已不在运行，清理记录。
    del "%PIDFILE%" 2>nul
    timeout /t 2 /nobreak >nul
    exit
)
netstat -ano | findstr /R /C:":3080 " | findstr "LISTENING" >nul
if errorlevel 1 (
    echo [dsh] 端口 3080 未在监听，后台服务可能已退出，清理记录。
    del "%PIDFILE%" 2>nul
    timeout /t 2 /nobreak >nul
    exit
)
taskkill /PID %STOP_PID% /T /F >nul 2>&1
powershell -NoProfile -Command "if(Get-Process -Id $env:STOP_PID -ErrorAction SilentlyContinue){Stop-Process -Id $env:STOP_PID -Force -ErrorAction SilentlyContinue}"
del "%PIDFILE%" 2>nul
echo 已停止 DeepSeek Harness。
timeout /t 2 /nobreak >nul
exit

rem ============ 一键卸载 ============
rem ============ 一键卸载 ============
:uninstall_menu
echo.
echo ==========================================
echo   卸载选项
echo ==========================================
echo   [1] 卸载 dsh（完成后可选是否继续卸载 Node.js）
echo   [2] 卸载 Node.js（需先卸载 dsh）
echo   [3] 两个都卸载（dsh + Node.js）
echo   [0] 取消
echo ==========================================
choice /C 1230 /N /T 30 /D 0 /M "请选择 (1=dsh 2=Node 3=两个都卸载 0=取消, 30秒默认取消): "
if errorlevel 4 goto :un_cancel
if errorlevel 3 goto :un_both
if errorlevel 2 goto :un_node
if errorlevel 1 goto :un_dsh_only
goto :un_cancel

:un_dsh_only
call :un_core
echo.
choice /C YN /N /T 15 /D N /M "dsh 已卸载。是否继续卸载 Node.js？(Y=继续卸载 N=保留 Node.js, 15秒默认保留): "
if errorlevel 2 goto :un_done
goto :un_node_go

:un_node
where dsh.cmd >nul 2>&1
if not errorlevel 1 (
    echo.
    echo [dsh] 检测到 dsh 仍安装。卸载 Node.js 前必须先卸载 dsh。
    echo [dsh]   请选择 [3] 两个都卸载，或先执行 [1] 卸载 dsh。
    echo.
    pause
    goto :uninstall_menu
)
goto :un_node_go

:un_both
echo [dsh] 正在按顺序卸载：先 dsh，再 Node.js...
call :un_core
:un_node_go
echo.
echo [dsh] 正在卸载 Node.js LTS...
where winget >nul 2>&1
if not errorlevel 1 winget uninstall -e --id OpenJS.NodeJS.LTS --accept-source-agreements
where node >nul 2>&1
if errorlevel 1 goto :un_node_gone
echo [dsh] winget 未能移除 Node.js，尝试从安装信息定位卸载...
for /f "tokens=*" %%a in ('reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall" /s /f "Node.js" /d 2^>nul ^| findstr /R "{.*}"') do set "NODE_GUID=%%a"
if not defined NODE_GUID for /f "tokens=*" %%a in ('reg query "HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall" /s /f "Node.js" /d 2^>nul ^| findstr /R "{.*}"') do set "NODE_GUID=%%a"
for %%b in ("%NODE_GUID%") do set "NODE_GUID=%%~nxb"
if not defined NODE_GUID goto :un_node_manual
echo [dsh] 找到安装记录，执行卸载（如弹 UAC 请点是）...
start "" /wait msiexec /x %NODE_GUID% /qb
where node >nul 2>&1
if errorlevel 1 goto :un_node_gone
echo [dsh] 自动卸载仍未成功，请到 控制面板-程序和功能 手动卸载 Node.js。
goto :un_node_locate
:un_node_manual
echo [dsh] 未找到 Node.js 的安装记录，请到 控制面板-程序和功能 手动卸载。
:un_node_locate
for /f "delims=" %%i in ('where node 2^>nul') do echo [dsh]   检测到: %%i
goto :un_done
:un_node_gone
echo [dsh] Node.js 已卸载。
powershell -NoProfile -Command "$cur=[Environment]::GetEnvironmentVariable('Path','User'); if($cur){ $std='C:\Program Files\nodejs'; $keep=$cur -split ';' | Where-Object { ($_ -eq $std) -or ($_ -notlike '*nodejs*') }; [Environment]::SetEnvironmentVariable('Path', ($keep -join ';'), 'User') }"
goto :un_done

rem ---- 卸载核心：停止服务 + 卸载 dsh ----
:un_core
set "PIDFILE=%TEMP%\dsh-web.pid"
if exist "%PIDFILE%" (
    for /f "usebackq delims=" %%i in ("%PIDFILE%") do set "STOP_PID=%%i"
    powershell -NoProfile -Command "if(Get-Process -Id $env:STOP_PID -ErrorAction SilentlyContinue){exit 0}else{exit 1}"
    if not errorlevel 1 (
        taskkill /PID %STOP_PID% /T /F >nul 2>&1
        powershell -NoProfile -Command "Stop-Process -Id $env:STOP_PID -Force -ErrorAction SilentlyContinue"
        echo [dsh] 已停止后台服务。
    )
    del "%PIDFILE%" 2>nul
)
where dsh.cmd >nul 2>&1
if errorlevel 1 goto :un_dsh_skip
for /f "delims=" %%i in ('where dsh.cmd 2^>nul') do set "DSH_CMD=%%i"
for %%i in ("%DSH_CMD%") do set "DSH_DIR=%%~dpi"
for /f "delims=" %%i in ('call npm prefix -g 2^>nul') do set "NPM_PREFIX=%%i"
if /i "%DSH_DIR%"=="%NPM_PREFIX%\" goto :un_dsh_npm
echo [dsh] 检测到自定义目录安装: %DSH_DIR%
del "%DSH_DIR%dsh.cmd" "%DSH_DIR%dsh.ps1" "%DSH_DIR%dsh" 2>nul
if exist "%DSH_DIR%node_modules\@deepseek-ai" rmdir /s /q "%DSH_DIR%node_modules\@deepseek-ai"
powershell -NoProfile -Command "$d=($env:DSH_DIR).TrimEnd('\'); $cur=[Environment]::GetEnvironmentVariable('Path','User'); if($cur){ [Environment]::SetEnvironmentVariable('Path', (($cur -split ';') | Where-Object { $_ -ne $d }) -join ';', 'User') }"
echo [dsh] 已删除 dsh 文件，并从用户 PATH 移除该目录。
goto :un_dsh_done
:un_dsh_npm
echo [dsh] 检测到 npm 全局安装，执行 npm uninstall...
call npm uninstall -g @deepseek-ai/dsh
if not "%errorlevel%"=="0" echo [dsh] npm 卸载失败，请手动执行: npm uninstall -g @deepseek-ai/dsh
goto :un_dsh_done
:un_dsh_skip
echo [dsh] 未检测到 dsh，跳过 dsh 卸载。
:un_dsh_done
goto :eof

:un_cancel
echo 已取消。
pause
exit /b

:un_done
echo.
echo 卸载处理完成。
echo.
pause
exit /b

:install_dsh
echo.
echo [dsh] 未检测到 DeepSeek Harness，需要安装（需联网，约1-2分钟）。

rem ---- 先确认 Node.js / npm 可用 ----
where node >nul 2>&1
if errorlevel 1 goto :need_node
where npm >nul 2>&1
if errorlevel 1 goto :need_npm

echo ==========================================
echo   dsh 安装方式
echo ==========================================
echo   [1] 默认安装（npm 全局目录）
echo   [2] 自定义安装（指定目录）
echo   [0] 取消安装
echo ==========================================
choice /C 120 /N /T 30 /D 1 /M "请选择 (1=默认 2=自定义 0=取消, 30秒无操作默认1): "
if errorlevel 3 goto :install_cancel
if errorlevel 2 goto :dsh_custom
if errorlevel 1 goto :install_default
goto :install_cancel

:dsh_custom
set "DSH_INSTALL_DIR="
set /p "DSH_INSTALL_DIR=请输入 dsh 安装目录（例如 D:\柒柒\Software）: "
if not defined DSH_INSTALL_DIR goto :install_default
echo [dsh] 正在从国内镜像源（npmmirror）下载 dsh（每隔几秒有提示，请耐心等待）...
> "%TEMP%\dsh-npm.running" echo 1start "" /b powershell -NoProfile -Command "$sw=[Diagnostics.Stopwatch]::StartNew(); $m=$env:TEMP + '\dsh-npm.running'; while($sw.Elapsed.TotalSeconds -lt 115){ Start-Sleep 5; if(-not (Test-Path $m)){ break }; Write-Host ('[dsh] 仍在下载中，已用时 ' + [int]$sw.Elapsed.TotalSeconds + ' 秒...') }"
call npm install --prefix "%DSH_INSTALL_DIR%" @deepseek-ai/dsh --no-fund --no-audit --registry=https://registry.npmmirror.com --allow-scripts=@deepseek-ai/dsh-subprocess-local,koffi,node-pty,@google/genai,protobufjs
set "NPM_EC=%errorlevel%"
del "%TEMP%\dsh-npm.running" 2>nul
if not "%NPM_EC%"=="0" goto :npm_custom_plain
goto :npm_custom_ok
:npm_custom_plain
echo [dsh] 首次尝试失败（可能 npm 版本较旧），改用兼容方式重试...
> "%TEMP%\dsh-npm.running" echo 1start "" /b powershell -NoProfile -Command "$sw=[Diagnostics.Stopwatch]::StartNew(); $m=$env:TEMP + '\dsh-npm.running'; while($sw.Elapsed.TotalSeconds -lt 115){ Start-Sleep 5; if(-not (Test-Path $m)){ break }; Write-Host ('[dsh] 仍在下载中，已用时 ' + [int]$sw.Elapsed.TotalSeconds + ' 秒...') }"
call npm install --prefix "%DSH_INSTALL_DIR%" @deepseek-ai/dsh --no-fund --no-audit --registry=https://registry.npmmirror.com
set "NPM_EC=%errorlevel%"
del "%TEMP%\dsh-npm.running" 2>nul
if not "%NPM_EC%"=="0" goto :npm_custom_official
goto :npm_custom_ok
:npm_custom_official
echo [dsh] 国内镜像源下载失败，改用默认源（registry.npmjs.org）重试...
> "%TEMP%\dsh-npm.running" echo 1start "" /b powershell -NoProfile -Command "$sw=[Diagnostics.Stopwatch]::StartNew(); $m=$env:TEMP + '\dsh-npm.running'; while($sw.Elapsed.TotalSeconds -lt 115){ Start-Sleep 5; if(-not (Test-Path $m)){ break }; Write-Host ('[dsh] 仍在下载中，已用时 ' + [int]$sw.Elapsed.TotalSeconds + ' 秒...') }"
call npm install --prefix "%DSH_INSTALL_DIR%" @deepseek-ai/dsh --no-fund --no-audit --allow-scripts=@deepseek-ai/dsh-subprocess-local,koffi,node-pty,@google/genai,protobufjs --fetch-timeout=45000 --fetch-retries=0
set "NPM_EC=%errorlevel%"
del "%TEMP%\dsh-npm.running" 2>nul
if not "%NPM_EC%"=="0" goto :install_fail
:npm_custom_ok
if not exist "%DSH_INSTALL_DIR%\dsh.cmd" goto :install_fail
set "PATH=%DSH_INSTALL_DIR%;%PATH%"
powershell -NoProfile -Command "$d=($env:DSH_INSTALL_DIR).TrimEnd('\'); $cur=[Environment]::GetEnvironmentVariable('Path','User'); if($d -and $cur -and -not(($cur -split ';') -contains $d)){[Environment]::SetEnvironmentVariable('Path', $cur.TrimEnd(';') + ';' + $d, 'User')}"
goto :install_check
:install_default
echo [dsh] 正在从国内镜像源（npmmirror）下载 dsh（每隔几秒有提示，请耐心等待）...
> "%TEMP%\dsh-npm.running" echo 1start "" /b powershell -NoProfile -Command "$sw=[Diagnostics.Stopwatch]::StartNew(); $m=$env:TEMP + '\dsh-npm.running'; while($sw.Elapsed.TotalSeconds -lt 115){ Start-Sleep 5; if(-not (Test-Path $m)){ break }; Write-Host ('[dsh] 仍在下载中，已用时 ' + [int]$sw.Elapsed.TotalSeconds + ' 秒...') }"
call npm install -g @deepseek-ai/dsh --no-fund --no-audit --registry=https://registry.npmmirror.com --allow-scripts=@deepseek-ai/dsh-subprocess-local,koffi,node-pty,@google/genai,protobufjs
set "NPM_EC=%errorlevel%"
del "%TEMP%\dsh-npm.running" 2>nul
if not "%NPM_EC%"=="0" goto :npm_default_plain
goto :npm_default_ok
:npm_default_plain
echo [dsh] 首次尝试失败（可能 npm 版本较旧），改用兼容方式重试...
> "%TEMP%\dsh-npm.running" echo 1start "" /b powershell -NoProfile -Command "$sw=[Diagnostics.Stopwatch]::StartNew(); $m=$env:TEMP + '\dsh-npm.running'; while($sw.Elapsed.TotalSeconds -lt 115){ Start-Sleep 5; if(-not (Test-Path $m)){ break }; Write-Host ('[dsh] 仍在下载中，已用时 ' + [int]$sw.Elapsed.TotalSeconds + ' 秒...') }"
call npm install -g @deepseek-ai/dsh --no-fund --no-audit --registry=https://registry.npmmirror.com
set "NPM_EC=%errorlevel%"
del "%TEMP%\dsh-npm.running" 2>nul
if not "%NPM_EC%"=="0" goto :npm_default_official
goto :npm_default_ok
:npm_default_official
echo [dsh] 国内镜像源下载失败，改用默认源（registry.npmjs.org）重试...
> "%TEMP%\dsh-npm.running" echo 1start "" /b powershell -NoProfile -Command "$sw=[Diagnostics.Stopwatch]::StartNew(); $m=$env:TEMP + '\dsh-npm.running'; while($sw.Elapsed.TotalSeconds -lt 115){ Start-Sleep 5; if(-not (Test-Path $m)){ break }; Write-Host ('[dsh] 仍在下载中，已用时 ' + [int]$sw.Elapsed.TotalSeconds + ' 秒...') }"
call npm install -g @deepseek-ai/dsh --no-fund --no-audit --allow-scripts=@deepseek-ai/dsh-subprocess-local,koffi,node-pty,@google/genai,protobufjs --fetch-timeout=45000 --fetch-retries=0
set "NPM_EC=%errorlevel%"
del "%TEMP%\dsh-npm.running" 2>nul
if not "%NPM_EC%"=="0" goto :install_fail
:npm_default_ok
for /f "delims=" %%i in ('call npm prefix -g 2^>nul') do set "NPM_PREFIX=%%i"
if not exist "%NPM_PREFIX%\dsh.cmd" goto :install_fail
set "PATH=%NPM_PREFIX%;%PATH%"
powershell -NoProfile -Command "$d=($env:NPM_PREFIX).TrimEnd('\'); $cur=[Environment]::GetEnvironmentVariable('Path','User'); if($d -and $cur -and -not(($cur -split ';') -contains $d)){[Environment]::SetEnvironmentVariable('Path', $cur.TrimEnd(';') + ';' + $d, 'User')}"
goto :install_check
:need_node
echo.
echo [dsh] 未检测到 Node.js（dsh 的运行环境）。

rem ---- 安装 Node.js 需要管理员权限，提前申请（仅缺 Node 时触发一次） ----
net session >nul 2>&1
if errorlevel 1 (
    echo [dsh] 自动安装 Node.js 需要管理员权限，即将弹出 UAC 确认，请点「是」。
    powershell -NoProfile -Command "try { Start-Process -FilePath '%~f0' -Verb RunAs -ErrorAction Stop; exit 0 } catch { exit 1 }"
    if errorlevel 1 goto :manual_node
    exit /b
)

echo ==========================================
echo   Node.js 下载源
echo ==========================================
echo   [1] 官方源（nodejs.org，可能需要梯子）
echo   [2] 国内镜像（npmmirror，推荐免梯子）
echo   [0] 取消安装
echo ==========================================
choice /C 120 /N /T 30 /D 2 /M "请选择 (1=官方 2=镜像 0=取消, 30秒默认2): "
if errorlevel 3 goto :manual_node
if errorlevel 2 goto :node_src_mirror
if errorlevel 1 goto :node_src_official
goto :manual_node

rem ---- 第二步：选择安装位置 ----
:node_loc_menu
echo ==========================================
echo   Node.js 安装位置
echo ==========================================
echo   [1] 默认目录  C:\Program Files\nodejs
echo   [2] 自定义目录
echo ==========================================
choice /C 12 /N /T 30 /D 1 /M "请选择 (1=默认 2=自定义, 30秒默认1): "
if errorlevel 2 goto :loc_custom
set "NODE_LOC=default"
goto :eof
:loc_custom
set "NODE_LOC=custom"
goto :eof

rem ---- 官方源分支 ----
:node_src_official
where winget >nul 2>&1
if errorlevel 1 goto :manual_node
call :node_loc_menu
if "%NODE_LOC%"=="custom" goto :node_custom
goto :node_default

:node_default
echo [dsh] 正在通过 winget 安装 Node.js LTS（需要几分钟）...
winget install -e --id OpenJS.NodeJS.LTS --accept-source-agreements --accept-package-agreements
set "PATH=%ProgramFiles%\nodejs;%PATH%"
goto :node_check

:node_custom
set "NODE_DIR="
set /p "NODE_DIR=请输入 Node.js 安装目录（例如 D:\柒柒\Software\NodeJS）: "
if not defined NODE_DIR goto :node_default
echo [dsh] 正在通过 winget 安装 Node.js LTS（需要几分钟）...
winget install -e --id OpenJS.NodeJS.LTS --location "%NODE_DIR%" --accept-source-agreements --accept-package-agreements
set "PATH=%NODE_DIR%;%ProgramFiles%\nodejs;%PATH%"
goto :node_check

rem ---- 镜像源分支 ----
:node_src_mirror
echo [dsh] 正在从国内镜像获取最新 LTS 版本号...
powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $v=(Invoke-RestMethod 'https://npmmirror.com/mirrors/node/index.json' -TimeoutSec 30 | Where-Object {$_.lts} | Select-Object -Last 1).version; if(-not $v){exit 1}; Set-Content -Path ($env:TEMP + '\mirror_node_ver.txt') -Value $v -Encoding ASCII"
if not "%errorlevel%"=="0" goto :manual_node
set /p "MIRROR_VER=" < "%TEMP%\mirror_node_ver.txt"
echo [dsh] 正在从国内镜像下载 Node.js %MIRROR_VER%（约31MB，请耐心等待）...
powershell -NoProfile -Command "$ErrorActionPreference='Stop'; Invoke-WebRequest ('https://npmmirror.com/mirrors/node/' + $env:MIRROR_VER + '/node-' + $env:MIRROR_VER + '-x64.msi') -UseBasicParsing -OutFile ($env:TEMP + '\node-setup.msi') -TimeoutSec 900"
if not "%errorlevel%"=="0" goto :manual_node
echo [dsh] 下载完成。
call :node_loc_menu
if "%NODE_LOC%"=="custom" goto :mirror_custom
goto :mirror_default

:mirror_default
echo [dsh] 正在安装 Node.js（将弹出进度条窗口，需要1-2分钟）...
start "" /wait msiexec /i "%TEMP%\node-setup.msi" /qb
if not "%errorlevel%"=="0" goto :manual_node
set "PATH=%ProgramFiles%\nodejs;%PATH%"
goto :mirror_done

:mirror_custom
set "NODE_DIR="
set /p "NODE_DIR=请输入 Node.js 安装目录（直接回车=默认 C:\Program Files\nodejs）: "
if not defined NODE_DIR goto :mirror_default
echo [dsh] 正在安装 Node.js（将弹出进度条窗口，需要1-2分钟）...
start "" /wait msiexec /i "%TEMP%\node-setup.msi" INSTALLDIR="%NODE_DIR%" /qb
if not "%errorlevel%"=="0" goto :manual_node
set "PATH=%NODE_DIR%;%ProgramFiles%\nodejs;%PATH%"
goto :mirror_done

:mirror_done
del "%TEMP%\node-setup.msi" 2>nul
del "%TEMP%\mirror_node_ver.txt" 2>nul
goto :node_check
:node_check
where node >nul 2>&1
if errorlevel 1 goto :manual_node
where npm >nul 2>&1
if errorlevel 1 goto :manual_node
if defined NODE_DIR if exist "%NODE_DIR%\node.exe" powershell -NoProfile -Command "$d=($env:NODE_DIR).TrimEnd('\'); $cur=[Environment]::GetEnvironmentVariable('Path','User'); if($d -and $cur -and -not(($cur -split ';') -contains $d)){[Environment]::SetEnvironmentVariable('Path', $cur.TrimEnd(';') + ';' + $d, 'User')}"
echo [dsh] Node.js 安装完成。
echo.
goto :install_dsh

:manual_node
echo.
echo [dsh] 请手动安装 Node.js LTS 后重新双击运行本脚本：
echo [dsh]   下载地址: https://nodejs.org/
echo [dsh]   安装时保持勾选 "Add to PATH"，装完重新双击本脚本。
echo.
pause
exit /b 1

:need_npm
echo.
echo [dsh] 检测到 Node.js，但缺少 npm，说明 Node.js 安装不完整。
echo [dsh] 请重新安装 Node.js LTS（https://nodejs.org/）后重试。
echo.
pause
exit /b 1

:install_check
where dsh >nul 2>&1
if errorlevel 1 (
    echo [dsh] 已安装，但 PATH 尚未生效。请关闭本窗口后重新双击运行。
    pause
    exit /b 1
)
echo [dsh] 安装完成。
echo.
goto :ready

:install_cancel
echo.
echo [dsh] 已取消安装。
echo.
pause
exit /b 1

:install_fail
echo.
echo [dsh] 安装失败。可能原因：网络不通或 npm 源不可达，请稍后重试。
echo.
pause
exit /b 1
