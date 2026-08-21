# DSH Launcher：Windows 与 Linux 完整实施计划

## 1. 产品范围

启动器管理两类版本，它们必须彼此独立：

1. **DSH 版本**：来自 `@deepseek-ai/dsh` 的 npm 发布版本。
2. **启动器版本**：来自本项目 GitHub/Gitee Release 的桌面程序版本。

任何一类更新失败都不能影响另一类，也不能删除或覆盖用户的 `DSH_HOME`。

## 2. 用户体验

主界面只展示新手需要的信息：

- DSH 状态：未安装、已停止、正在启动、运行中、更新中或异常；
- DSH 当前版本与可用版本；
- 启动器当前版本与可用版本；
- 启动、停止、打开网页三个主按钮；
- 安装/修复、更新、设置、日志和卸载等次级入口；
- 后台自动更新开关和稳定版/测试版通道。

默认行为：

- 打开启动器后立即显示本地状态，不等待网络；
- 网络检查完全在后台运行；
- 每 6 小时最多检查一次，避免每次点击都访问网络；
- DSH 正在运行时只下载或记录更新，停止后再安全应用；
- 启动器自身更新在 GUI 退出后由独立更新助手替换文件并重启；
- 更新失败时继续使用旧版本，并在界面显示可理解的结果。

## 3. 技术路线

采用 **C++20 + 平台原生 GUI**：

- GUI、版本比较、下载、校验和状态管理共用；
- Windows 和 Linux 只实现各自的系统适配层；
- Release 发布原生单文件，普通用户无需预装编译器或 .NET；
- 不采用 Electron，控制安装体积、内存和冷启动耗时；
- Windows 使用 Win32 API，零第三方 GUI 运行时；
- Linux 使用发行版普遍提供的 GTK，并提供缺少 GTK 时的明确安装提示；
- Windows 和 Linux 分别实现很薄的界面、进程和浏览器适配层，共享全部业务逻辑；
- 不把 PowerShell WinForms 作为正式 GUI，因为它无法原样运行在 Linux。

发布硬指标：

- 主窗口本地状态不依赖网络，尽量在 1 秒内出现；
- 发布包优先控制在约 10–20 MiB；
- 默认单个主程序，额外更新助手仅在自更新时使用；
- 不使用 UPX 等容易触发安全软件误报的可执行压缩器；
- 调试符号与发布程序分离。

支持架构优先级：

1. Windows x64；
2. Linux x64；
3. Windows arm64、Linux arm64（第二阶段）；
4. macOS 暂不承诺，但共享 UI 为以后保留可能。

## 4. DSH 安装策略

### 4.1 启动器托管安装（默认）

为了避免管理员权限和不同 Node/npm 环境造成的不确定性，启动器将 DSH 安装到用户目录：

- Windows：`%LOCALAPPDATA%\DshLauncher\runtime\dsh`
- Linux：`${XDG_DATA_HOME:-~/.local/share}/dsh-launcher/runtime/dsh`

启动器始终调用该目录中的精确可执行文件，不依赖 PATH 顺序。

### 4.2 现有安装兼容

首次启动时检测：

- PATH 中的 `dsh`；
- npm 全局目录中的 `@deepseek-ai/dsh`；
- 现有 BAT 所支持的自定义 `npm --prefix` 目录；
- nvm、fnm 等 Linux/Windows Node 版本管理环境中的安装。

用户可以继续使用现有安装，也可以一键迁移到启动器托管目录。迁移不得移动 `~/.dsh`。

### 4.3 Node.js 策略

按实现难度分两步：

- 第一阶段复用系统 Node.js LTS，并给出明确的安装引导；
- 第二阶段可增加启动器私有 Node.js runtime，实现真正的一键安装且不修改系统 PATH。

Linux 不静默执行 `sudo`。需要系统权限的操作必须由用户明确确认，默认选择用户级安装。

## 5. 配置和数据边界

DSH 用户数据继续由 DSH 管理：

- 默认：`~/.dsh`
- 自定义：环境变量 `DSH_HOME` 指向的位置

启动器不得读取、复制到日志或上传以下内容：

- API Key 和 `.credentials.yaml` 的正文；
- 环境变量中的密钥；
- 会话正文和工作区文件。

启动器自己的状态目录：

- Windows：`%LOCALAPPDATA%\DshLauncher`
- Linux 配置：`${XDG_CONFIG_HOME:-~/.config}/dsh-launcher`
- Linux 状态：`${XDG_STATE_HOME:-~/.local/state}/dsh-launcher`
- Linux 缓存：`${XDG_CACHE_HOME:-~/.cache}/dsh-launcher`

## 6. 后台检查与日志

后台检查不弹出 CMD、PowerShell 或终端窗口，也不阻塞主界面。

日志位置：

- Windows：`%LOCALAPPDATA%\DshLauncher\logs\launcher.log`
- Linux：`${XDG_STATE_HOME:-~/.local/state}/dsh-launcher/logs/launcher.log`

清理策略：

- 单文件达到 512 KiB 时轮转；
- 保留最近 3 份；
- 清理 30 天前的旧日志；
- 错误正文限制长度；
- 永不记录密钥、用户配置正文或完整环境变量。

默认只在启动器运行期间检查。可选的“每日后台检查”功能使用：

- Windows Task Scheduler；
- Linux systemd user timer。

此功能默认关闭，启用和关闭都必须能在设置页完成。

## 7. 启动器自更新

### 7.1 发布源

代码开发以 GitHub 为主仓库。面向国内用户时，Gitee Releases 是安装包首选源，GitHub Releases 是备用源；海外用户可以使用相反顺序。两边必须发布相同版本、相同文件和相同 SHA-256。

更新清单包含：

- 清单格式版本；
- 稳定版与测试版版本号；
- 每个平台和 CPU 架构的下载地址；
- 文件大小和 SHA-256；
- 最低可直接升级版本；
- Release Notes 地址；
- 是否属于必须升级的安全版本。

客户端并行或短超时探测两个更新清单，选择可用且响应更快的源；国内发行配置在结果相近时优先 Gitee。任一源超时或下载失败后自动切换另一个源。版本号或 SHA-256 不一致时不自动升级，并记录“发布源未同步”。

### 7.2 安全更新流程

1. 后台读取更新清单；
2. 比较语义化版本；
3. 下载到缓存中的 `.partial` 文件；
4. 校验文件长度和 SHA-256；
5. 解包到新的版本目录；
6. 启动独立 updater；
7. updater 等待 GUI 和 DSH 管理操作退出；
8. 原子切换到新版本并重启；
9. 新版启动自检失败时回滚上一版。

成熟阶段对清单增加 Ed25519 签名；公钥编译在启动器内，避免仅依赖同一服务器提供的哈希值。

## 8. Windows 平台适配

- 根目录 BAT 作为早期版本和紧急备用入口；
- 正式版提供 `.exe`，不显示控制台窗口；
- 使用 Job Object 或进程树跟踪 DSH，避免只杀父进程；
- 通过端口和健康请求共同判断 Web UI 是否已就绪；
- 浏览器使用系统默认处理程序打开；
- 可选创建开始菜单、桌面快捷方式和开机启动；
- 安装包后续提供便携 ZIP 与安装版两种形式。

## 9. Linux 平台适配

- 提供 AppImage 或 tar.gz 便携包；
- 提供 `dsh-launcher.desktop`，可从应用菜单启动；
- 优先使用 systemd user service 管理长期运行的 DSH；
- 无 systemd 环境回退到普通子进程管理；
- 遵循 XDG Base Directory 规范；
- 不要求 root，不自动修改 `/usr`、`/opt` 或系统 npm 目录；
- 检测 Wayland/X11 环境，但 GUI 逻辑保持一致；
- 后续根据需求增加 deb/rpm 包。

## 10. GitHub 与 Gitee 工作流

建议仓库同名：`dsh-launcher-windows` 如果只发布 Windows；既然现在确定支持两个平台，更推荐改为：

```text
dsh-desktop-launcher
```

仓库描述：

```text
非官方 DeepSeek Harness 跨平台图形化启动器，支持 Windows/Linux 一键安装、运行管理及静默自动更新。
```

英文：

```text
An unofficial cross-platform desktop launcher for DeepSeek Harness with one-click setup, service management, and silent updates.
```

仓库策略：

- GitHub 作为代码开发主仓库；
- Gitee 作为代码镜像和国内首选 Release 下载源；
- 开发只在主仓库合并，自动同步到镜像，避免双向提交冲突；
- Tag 使用 `v主版本.次版本.修订版本`；
- `main` 只放可构建代码，正式文件通过 Release 发布；
- Release 先设为草稿，所有平台构建和校验通过后再公开。

## 11. 开发阶段与验收条件

### 阶段 0：整理现状

- 保留原 BAT；
- 列出 BAT 的安装、启动、停止、卸载和异常分支；
- 建立测试矩阵和日志脱敏规则。

验收：现有 BAT 行为已被文档覆盖，没有未知的重要功能。

### 阶段 1：共享核心

- 实现版本比较、命令执行、超时、日志和配置；
- 实现 DSH/Node/npm 检测；
- 实现进程状态和端口健康检查；
- 为 Windows/Linux 路径与进程行为编写测试。

验收：核心逻辑无需 GUI 即可自动测试。

### 阶段 2：Windows GUI 最小可用版

- 实现状态页和主按钮；
- 接入现有安装与托管安装；
- 接入 DSH 后台检查；
- 日志轮转、错误提示和配置保留验证。

验收：普通 Windows 用户从未安装到打开 Web UI 不需要输入命令。

### 阶段 3：Linux 版

- 接入 XDG 路径、shell 环境和 systemd user service；
- 适配 npm/nvm/fnm；
- 生成 desktop 文件和 Linux 发布包。

验收：在至少 Ubuntu/Debian 与一个非 Debian 发行版上完成全流程。

### 阶段 4：启动器自更新

- 生成双源清单；
- 下载、校验、替换、重启和回滚；
- 模拟断网、下载中断、磁盘空间不足和损坏更新包。

验收：任何失败场景都能继续启动上一版。

### 阶段 5：发布自动化

- Windows/Linux 自动构建；
- 生成哈希和更新清单；
- 创建 GitHub Release 并同步 Gitee；
- 添加冒烟测试和安装包扫描。

验收：创建一个版本 Tag 后能够得到可验证的双平台 Release 草稿。

## 12. 测试矩阵

至少覆盖：

- Windows 10/11 x64；
- Ubuntu LTS x64；
- Linux Mint 或 Fedora x64；
- 无 Node、Node 过旧、Node 正常；
- DSH 未安装、全局安装、自定义安装、损坏安装；
- 3080 端口空闲、被 DSH 占用、被其他程序占用；
- 断网、镜像不可用、官方源不可用；
- 路径含空格、中文和非 ASCII 字符；
- DSH 正在运行时发现更新；
- 更新包下载中断、哈希不符、解包失败和新版启动失败；
- GitHub 可用/Gitee 不可用及反向情况。

## 13. 当前下一步

1. GitHub 主仓库：`https://github.com/Coder-Taylor/dsh-desktop-launcher`；
2. Gitee 镜像仓库：`https://gitee.com/taylorchengitee/dsh-desktop-launcher`；
3. 确定许可证；
4. 使用 C++20 创建 Win32/GTK 原生 GUI 工程；
5. 将原 BAT 的行为拆成测试清单；
6. 先交付 Windows GUI MVP，再接 Linux 系统适配；
7. 发布第一个 Release 后启用正式的启动器自动更新清单。
