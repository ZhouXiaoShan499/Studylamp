# 参与与协作规范（GitHub Flow）

`Studylamp` 采用 **GitHub Flow**：`main` 永远是可用分支，任何改动都通过短分支 + Pull Request 进入。

## 1. 分支模型

- `main`：唯一长期分支，保持可编译、可部署（网页由 GitHub Pages 自动发布）；
- 短期分支：从最新 `main` 拉出，合并后立即删除；
- 命名前缀与提交类型保持一致：

| 前缀 | 用途 | 示例 |
|------|------|------|
| `feat/` | 新功能 / 新能力 | `feat/bailian-provider` |
| `fix/` | 缺陷修复 | `fix/wifi-scan-hang` |
| `docs/` | 纯文档 | `docs/offline-voice-research` |
| `chore/` | 构建、CI、依赖、仓库配置 | `chore/github-flow` |
| `refactor/` | 不改行为的重构 | `refactor/voice-client` |

分支名用小写英文 + 连字符，不使用中文。

## 2. 标准流程

```bash
# 1) 同步 main
git switch main
git pull --ff-only

# 2) 拉短分支
git switch -c feat/my-change main

# 3) 小步提交（见第 3 节）
git add <files>
git commit -m "feat(firmware): ..."

# 4) 推送并开 PR
git push -u origin feat/my-change
gh pr create --base main --fill

# 5) CI 通过后 Squash and merge，再删除分支
```

PR 合并后本地清理：

```bash
git switch main
git pull --ff-only
git branch -d feat/my-change
```

## 3. 提交信息

采用 Conventional Commits，描述用中文，标题一行讲清楚「改了什么」：

```text
<type>(<scope>): <一句话>

可选的正文：为什么改、影响面、实测结论。
```

- `type`：`feat` / `fix` / `docs` / `chore` / `refactor` / `test` / `perf`；
- `scope` 常用：`firmware` / `server` / `web` / `docs` / `tools`；
- 一个提交只做一件事；不要把固件、文档、CI 混在同一个提交里。

## 4. PR 与合并

- 目标分支只能是 `main`；
- 合并方式固定 **Squash and merge**，保持 `main` 线性历史；
- PR 标题用 `<type>(<scope>): ...`，它会成为 squash 后的提交标题；
- CI 必须全绿：仓库卫生检查、Python 语法检查、PlatformIO 全环境编译；
- 涉及行为变化时，PR 里写清楚验证方式（串口日志、截图、命令输出）。

## 5. 不能入库的东西

| 内容 | 原因 | 放哪里 |
|------|------|--------|
| `firmware/include/secrets.h` | Wi-Fi 密码、云端 Key | 本地从 `secrets.h.example` 复制 |
| `firmware/.pio/`、`firmware/idf/**/build/` | 编译产物 | 忽略，重新编译 |
| `firmware/idf/**/managed_components/` | 组件管理器下载的第三方依赖 | 由 `idf.py` 重新拉取 |
| `server/models/`、`*.wav` | Vosk 模型与录音体积大 | 外部下载 / 本地保存 |
| `docs/design/` | 设计稿与渲染中间稿 | 网页实际引用的图放 `web/assets/` |

云端 Key 只走两条路：串口 `key` 命令写入 NVS，或本地 `secrets.h` 里的编译期常量。

## 6. 分支保护建议

在 GitHub 仓库设置里给 `main` 打开：

- Require a pull request before merging（禁止直推）；
- Require status checks to pass（选择 `CI` 的 job）；
- Require branches to be up to date before merging；
- Automatically delete head branches。

合并后自动删分支也可以先用 `gh` 打开（需要仓库管理员权限）：

```bash
gh api --method PATCH repos/ZhouXiaoShan499/Studylamp \
  -f delete_branch_on_merge=true
```

> 分支保护规则属于仓库设置，不在代码里；换机器或重建仓库时记得重设。

## 7. 发布

阶段性节点打 tag，例如：

```bash
git tag -a v0.2.0 -m "stage-0 离线语音可用"
git push origin v0.2.0
```