# Meowth GBA Translator — 自定义 API GUI 补丁

> 本分支保存对 [Olcmyk/Meowth-GBA-Translator](https://github.com/Olcmyk/Meowth-GBA-Translator) v0.3.5 的 **GUI 补丁**。并不是独立可运行的完整 ROM 汉化器，也不是已经编译好的 Windows EXE。

## 保存的功能

- Provider 增加 \`custom (OpenAI-compatible)\`；
- 提供 \`API Base URL\` 编辑框、模型 ID 和 API Key；
- \`Test API Connection\` 通过 \`/chat/completions\` 发出一次极短请求，不上传 ROM（可能产生少量费用）；
- 切换服务商自动恢复其默认 URL 和模型，并清空旧 API Key；
- 校验 API 地址和模型是否填写，公网地址只支持 HTTPS；
- 修复未选择输出目录导致 GUI 工作目录为 \`None\` 的问题；
- 保留上游原有 DeepSeek、OpenAI 等提供商设置。

**注意**：仅支持 Chat Completions 兼容 API；不能直接使用只支持 Responses 接口的服务。

## 将来在 Windows 上使用

暂时无需安装 Python，改动已经保存在 GitHub。

等准备好测试时：

1. 安装 Python 3.10 或更高版本。
2. 下载上游源码：https://github.com/Olcmyk/Meowth-GBA-Translator
3. 将本目录中的 \`src/meowth/gui/components/config_form.py\` 覆盖上游同路径文件（覆盖前建议备份原文件）。
4. 进入上游源码根目录执行 \`py -m pip install -e ".[gui]"\`。
5. 运行 \`meowth-gui\` 启动 GUI。

也可把本目录的 \`Install-Custom-API.bat\` 和 \`Launch-Custom-GUI.bat\` 放到上游源码根目录，并把本目录的 \`src/meowth/gui/components/config_form.py\` 路径保留下来，使用批处理完成覆盖和启动。

## 1A1API 填写示例（尚未使用真实密钥验证）

| GUI 字段 | 填写 |
|---|---|
| Provider | \`custom (OpenAI-compatible)\` |
| API Base URL | \`https://1a1api.top/v1\`（需以服务提供商文档确认） |
| Model | 供应商控制台实际可用的模型 ID |
| API Key | 用户自己的密钥 |
| Source / Target | English / Chinese |

测试 API 的成功只代表短请求成功，不意味着任何一个 GBA ROM 一定能够顺利提取、翻译和写回。

## 项目来源与说明

- 上游项目：https://github.com/Olcmyk/Meowth-GBA-Translator （MIT License）
- 本补丁只修改上游的 \`src/meowth/gui/components/config_form.py\`。
- 上游已实现将 \`TranslationConfig.api_base\` 传给 \`Translator\` 并调用 \`/chat/completions\`，不需要修改底层 API 客户端。
- 本补丁做过离线逻辑检查，**未**用真实 API Key 访问 1A1API，**未**通过 Windows GUI 或真实 GBA ROM 的端到端实测。
- **不要将 API Key 或游戏 ROM 提交到公开 GitHub 仓库。**
