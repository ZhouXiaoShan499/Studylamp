## 做了什么

<!-- 一句话说明目的；细节留给 diff -->

## 关联 Issue

Closes #

## 改动类型

- [ ] `feat` 新功能
- [ ] `fix` 缺陷修复
- [ ] `docs` 文档
- [ ] `chore` 构建 / CI / 仓库配置
- [ ] `refactor` 重构（不改行为）

## 验证方式

<!-- 串口日志、截图、命令输出；不要只写“已测” -->

```text
（粘贴关键输出）
```

## 自查清单

- [ ] 分支从最新 `main` 拉出，命名符合 `feat/` `fix/` `docs/` `chore/` `refactor/`
- [ ] 目标分支是 `main`，合并方式为 **Squash and merge**
- [ ] 没有提交 `secrets.h`、`.pio/`、`managed_components/`、模型与录音
- [ ] 涉及固件时，`pio run -e <env>` 本地通过
- [ ] 涉及接线 / 串口命令 / 行为变化时，README 或 `docs/` 已同步