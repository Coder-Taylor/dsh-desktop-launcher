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

- 双击启动器：启动 DSH，并在后台检查 DSH 与启动器更新；没有更新时自动打开网页并在短暂等待后关闭启动器窗口；
- DSH 已运行：可打开网页、停止服务或手动检查更新；
- 手动检查发现更新：进入更新页后可选一键更新、仅更新 DSH 或仅更新启动器；
- DSH 更新默认使用国内 npm 镜像，可切换 npm 官方源；更新始终写回检测到的原安装目录。

### 未安装 DSH

启动器会先给出“检测到 DSH 未安装”的明确提示。用户可选择默认或自定义安装目录、国内镜像或官方源；缺少 Node.js 时会先提示安装 Node.js。DSH 安装完成会进行版本校验，并在关闭启动器后自动打开 DSH 网页。

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

`v0.1.1-beta.1` 用于验证启动器自身的更新链路，不是稳定版。测试重点是：检查更新、下载、SHA-256 校验、替换重启，以及 DSH 的启动和网页打开。Release 默认将从 Gitee 发布，随后同步 GitHub。

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
