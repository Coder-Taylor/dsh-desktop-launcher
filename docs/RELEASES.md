# 发布与更新约定

## 仓库角色

- GitHub 代码主仓库：<https://github.com/Coder-Taylor/dsh-desktop-launcher>
- Gitee 国内发布与镜像仓库：<https://gitee.com/taylorchengitee/dsh-desktop-launcher>

GitHub 保留为代码主线与 Release 备用源；**Gitee 是发布主仓库和默认 Release 下载源**。发布时先保证
Gitee 的产物完整可用，再同步相同产物到 GitHub；两个仓库不同时独立修改，避免代码分叉。

## 授权与发布页说明

Launcher 自有代码按 [MIT License](../LICENSE) 发布。每个 Release 页面都应写明“DSH Launcher 是非官方工具，与 DeepSeek/DeepSeek Harness 不存在官方隶属或背书关系”，并链接[授权说明](LICENSING.md)。MIT 不覆盖 `@deepseek-ai/dsh`、其依赖或 DeepSeek/DSH 名称和商标。

发布前确认源码中存在 `LICENSE`；安装包也应包含 `LICENSE` 或在发布说明中提供清晰链接。不得把 API Key、Token、Cookie、DSH_HOME、日志或任何用户数据加入 Release 资产。

## 版本号

启动器使用语义化版本：

- `1.0.0`：稳定版；
- `1.1.0-beta.1`：测试版；
- Git Tag：`v1.0.0`。

DSH 的版本号单独显示和比较，不与启动器版本绑定。

## Release 文件名

```text
dsh-launcher-windows-x64.zip
dsh-launcher-windows-arm64.zip
dsh-launcher-linux-x64.tar.gz
dsh-launcher-linux-x64.AppImage
update-manifest.json
SHA256SUMS.txt
```

## 发布顺序

1. 创建版本 Tag；
2. 自动构建全部目标；
3. 运行单元测试和启动冒烟测试，并完成第三方组件与许可证检查；
4. 生成 SHA-256 与更新清单；
5. 创建并核对 Gitee Release 草稿；
6. 同步完全相同的产物到 GitHub Release 草稿；
7. 核对两个源的版本和哈希；
8. 先公开 Gitee，再公开 GitHub 的 Release；
9. 最后发布更新清单，默认清单将 Gitee 写为首选 URL，避免客户端看到尚未上传完成的版本。

## 回退规则

- 客户端至少保留上一版程序目录；
- 新版第一次启动必须写入健康标志；
- 指定时间内未产生健康标志，更新助手恢复上一版；
- 回退只处理启动器文件，不触碰 DSH 和 `DSH_HOME`；
- 撤回 Release 后，更新清单应恢复到最后一个安全版本。
