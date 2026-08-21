# DSH Launcher

面向 Windows 与 Linux 的非官方 DeepSeek Harness 图形化启动器。

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

开发提交以 GitHub 为准；面向国内用户的安装包默认从 Gitee Release 下载，GitHub Release 作为备用。两个仓库不同时独立修改，避免代码分叉。

## 文档

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
