# DSH Launcher

面向 Windows 与 Linux 的非官方 DeepSeek Harness 图形化启动器。目标是让用户双击一个轻量 EXE，完成 DSH 的检测、安装、启动、更新与维护；所有耗时操作在后台执行，但会显示清晰的状态和本地日志。

项目目标：

- 一键安装、启动、停止和打开 DeepSeek Harness Web UI；
- 后台静默检查并安全更新 DSH；
- 后台检查并更新启动器自身；
- 保留用户的 `DSH_HOME` 配置、凭据和会话数据；
- 同一套 GUI 与核心逻辑同时支持 Windows 和 Linux；
- Gitee Releases 面向国内用户优先下载，GitHub Releases 作为备用源并支持双源故障切换。

当前根目录的 `启动DeepSeek Harness.bat` 是现有 Windows 原型，在新版 GUI 达到可替代状态前继续保留。

## 代码仓库

- GitHub 代码主仓库：<https://github.com/Coder-Taylor/dsh-desktop-launcher>
- Gitee 国内发布与镜像仓库：<https://gitee.com/taylorchengitee/dsh-desktop-launcher>

GitHub 用作代码主线与镜像；**Gitee 是 Release 主仓库和默认下载源**。面向国内用户的安装包先从 Gitee Release 下载，GitHub Release 只作为备用源。发布时先保证 Gitee 资产可用，再同步相同文件和 SHA-256 到 GitHub，最后才发布更新清单。

## 使用方式

### 已安装 DSH

- 双击启动器：启动 DSH，并在后台检查 DSH 与启动器更新；DSH 自身只请求打开一次网页，没有更新时启动器在 30 秒后关闭；
- DSH 已运行：可打开网页、停止服务或手动检查更新；
- “设置 → 通用”可开启“启动成功 30 秒后最小化到系统托盘”；托盘菜单可恢复窗口、打开网页、停止服务或退出；
- 点击右上角 × 时可选择退出、最小化到托盘或取消，并可勾选“记住我的选择”；“设置 → 通用”也能随时切换为每次询问、托盘或直接退出；这些操作都不会停止正在运行的 DSH 服务；
- 手动检查发现更新：进入更新页后可选一键更新、仅更新 DSH 或仅更新启动器；
- DSH 更新默认使用国内 npm 镜像，可切换 npm 官方源；更新始终写回检测到的原安装目录，但会先在同盘隔离目录安装和校验，成功后才替换程序文件。

### 未安装 DSH

启动器会先给出“检测到 DSH 未安装”的明确提示。用户可选择默认或自定义安装目录、国内镜像或官方源；缺少 Node.js 时会先提示安装 Node.js。它还会检查启动 shim、DSH 核心包和版本命令；发现安装被中断或资源缺失时，显示“安装不完整”和“修复 DSH”，并使用此前记录的原目录重建程序文件，不会新建第二份 DSH。安装完成会进行版本校验，随后启动 Web 服务并由 DSH 自身请求打开网页。

### 更新为什么可能需要数分钟

DSH 0.1.1 的依赖较多，npm 首次更新需要解析完整依赖树、下载软件包并执行安装脚本。旧版固定 1 GiB Node 堆上限，会在 `idealTree` 阶段内存不足，看起来像“下载卡住”。新版按可用内存动态设置受限的 Node 堆与子进程组内存上限，同时保留取消、超时和实时状态心跳。真实国内镜像更新已校验到 `0.1.1-rc.2`。

## 日志与隐私

日志默认存放在：

```text
%LOCALAPPDATA%\DshLauncher\logs\launcher.log
%LOCALAPPDATA%\DshLauncher\logs\dsh-web.log
```

日志会记录安装目录、版本、下载源、命令退出码、超时和校验结果；不会记录 API Key、凭据、会话正文或完整环境变量。日志会轮转，并自动清理过期文件。

## 测试版状态

`v0.1.1-beta.10` 是本地候选测试版。它针对 DSH 0.1.1-rc.2 已知的 npm peer 依赖解析循环，使用 `--legacy-peer-deps` 并显式安装其运行时 peer 包；更新采用“隔离安装、双重版本校验、带备份替换”。旧版备份只做瞬时目录改名并延后清理，不再在前台递归删除数万个文件；服务状态必须连续两次通过 HTTP 健康检查才会显示为运行中。通过 Win10/Win11 实机验收后发布到 Gitee，并同步 GitHub。

## 后续开发计划

以下核心流程仍需继续完善并完成真实 Windows 环境验收：

- 下载与安装 DSH；
- 下载与安装 Node.js；
- 卸载 DSH；
- 卸载 Node.js；
- 更新 DSH（包括取消、中断恢复、异常退出和失败诊断）。

后续可加入“半自动配置 API Key”：启动器先打开官方网页并引导用户获取 Key；用户自行复制后，启动器在明确授权的短时间内监听剪贴板并自动填写。Key 不写入日志、不上传、不在启动器内长期保存，监听结束后立即停止。

## 文档

- [开发指南与阶段门禁](docs/DEVELOPMENT.md)
- [完整实施计划](docs/PROJECT_PLAN.md)
- [技术架构](docs/ARCHITECTURE.md)
- [发布与更新约定](docs/RELEASES.md)

## 计划中的目录

```text
src/
  app/                   Windows Win32 / Linux GTK 原生轻量 GUI
  core/                  DSH 检测、更新和日志逻辑
  platform/              Windows/Linux 系统适配
tests/                   不依赖 GUI 的核心单元测试
platform/
  windows/               BAT 引导、任务计划和 Windows 打包
  linux/                 shell 引导、desktop 文件和 systemd 用户服务
packaging/
  manifests/             更新清单示例与校验规则
docs/                    架构、计划和发布文档
legacy/                  达到迁移条件后归档旧版脚本
```

## 项目状态

目前已进入 C++20 原生 GUI 最小可用版本开发。Windows 使用零第三方依赖的 Win32，Linux 使用系统 GTK；目标是单文件、快速启动和尽可能小的发布包。
