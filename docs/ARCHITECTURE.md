# 技术架构

## 模块边界

```text
GUI (src/app, Windows Win32 / Linux GTK)
        |
        v
共享应用服务 (src/core)
  |          |           |             |
  v          v           v             v
安装检测    进程管理    DSH 更新      启动器更新检查
  |          |           |             |
  +----------+-----------+-------------+
                     |
                     v
          Windows / Linux 系统适配层

独立更新助手 (dsh-updater)
  -> 等待主程序退出
  -> 校验并切换版本
  -> 健康检查与回滚
  -> 重启主程序
```

## 设计原则

- UI 不直接执行 npm、kill 或文件替换；所有操作经共享服务完成。
- 外部命令都有明确超时、退出码和经过脱敏的错误摘要。
- 网络检查与本地状态读取分离，离线时仍可正常启动。
- DSH 程序文件、DSH 用户配置、启动器程序和启动器状态四者分离。
- 更新使用“下载到新目录后切换”，不在原目录边下载边覆盖。
- Windows 与 Linux 的差异集中在接口实现，不复制业务逻辑。

## 授权与第三方边界

- Launcher 自有代码按 [MIT License](../LICENSE) 发布；
- `@deepseek-ai/dsh` 及其依赖在运行时由 npm 安装或复用，仍按各自上游许可证和条款使用；
- 不将 API Key、账号凭据、DSH_HOME、会话数据或用户已安装的 DSH 文件打进源码或 Release；
- “DeepSeek”“DeepSeek Harness”“DSH”仅用于说明兼容对象，项目不宣称官方关系或商标授权；
- 新增第三方二进制、图标、字体和库时，必须在 Release 前记录来源及许可。详见[授权说明](LICENSING.md)。

## 关键状态机

DSH 状态：

```text
NotInstalled -> Installing -> Stopped -> Starting -> Running
                     |           |          |          |
                     v           v          v          v
                   Error <-------+----------+-------- Stopping

Stopped -> Updating -> Stopped
```

启动器更新状态：

```text
Idle -> Checking -> Available -> Downloading -> ReadyToApply
  ^        |             |             |              |
  |        v             v             v              v
  +------ Error <--------+-------------+---------- Applying
                                                    |     |
                                                    v     v
                                                 Restart Rollback
```

状态转换必须串行化，避免用户连续点击导致同时安装、启动和更新。
