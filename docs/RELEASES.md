# 发布与更新约定

## 仓库角色

- GitHub 代码主仓库：<https://github.com/Coder-Taylor/dsh-desktop-launcher>
- Gitee 国内发布与镜像仓库：<https://gitee.com/taylorchengitee/dsh-desktop-launcher>

GitHub 是唯一开发主线；Gitee 接收同步结果，并作为中国大陆用户的首选 Release 下载源。GitHub Release 是国内用户的备用源。

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
3. 运行单元测试和启动冒烟测试；
4. 生成 SHA-256 与更新清单；
5. 创建 GitHub Release 草稿；
6. 同步完全相同的产物到 Gitee Release；
7. 核对两个源的版本和哈希；
8. 先公开 GitHub/Gitee 两端的 Release；
9. 最后发布更新清单，国内默认清单将 Gitee 写为首选 URL，避免客户端看到尚未上传完成的版本。

## 回退规则

- 客户端至少保留上一版程序目录；
- 新版第一次启动必须写入健康标志；
- 指定时间内未产生健康标志，更新助手恢复上一版；
- 回退只处理启动器文件，不触碰 DSH 和 `DSH_HOME`；
- 撤回 Release 后，更新清单应恢复到最后一个安全版本。
