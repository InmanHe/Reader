# Reader

<p align="center">
  <img src="doc/images/reader.ico" alt="Reader Logo" width="80" height="80" onerror="this.style.display='none'"/>
</p>

<p align="center">
  <strong>基于 Win32 API 开发的原生 Windows 电子书阅读器</strong><br>
  轻量简洁 · 低资源占用 · 支持本地与在线阅读
</p>

<p align="center">
  <a href="https://github.com/InmanHe/Reader/releases"><img src="https://img.shields.io/github/v/release/InmanHe/Reader?color=blue&label=Release" alt="Release"></a>
  <a href="CHANGELOG.md"><img src="https://img.shields.io/badge/Changelog-更新日志-blue.svg" alt="Changelog"></a>
  <img src="https://img.shields.io/badge/Version-3.2.0.0-blue.svg" alt="Version">
  <img src="https://img.shields.io/badge/Built%20with-Claude%20(Anthropic)-d97757.svg" alt="Built with Claude">
  <a href="https://github.com/binbyu/Reader"><img src="https://img.shields.io/badge/Upstream-binbyu%2FReader-lightgrey.svg" alt="Upstream"></a>
  <img src="https://img.shields.io/badge/Platform-Windows-lightgrey.svg" alt="Platform">
</p>

<p align="center">
  <a href="#-v320-新增功能由-claude-开发">v3.2.0 新增</a> •
  <a href="#-核心功能">核心功能</a> •
  <a href="#-下载与版本说明">下载与版本</a> •
  <a href="#-使用指南与相关文档">文档指南</a> •
  <a href="#-常用快捷键">快捷键</a> •
  <a href="CHANGELOG.md">更新日志</a> •
  <a href="#-软件策略与免责声明">免责声明</a>
</p>

> [!NOTE]
> 本仓库是 [binbyu/Reader](https://github.com/binbyu/Reader) 的分支版本（**v3.2.0.0**，维护者 [InmanHe](https://github.com/InmanHe)，2026-11-04）。
> 在原版基础上新增了 **PDF / AZW3 支持、导出 Markdown（适用于 NotebookLM）、简繁英三语界面** 等功能，
> 这些新功能与修复由 **[Claude](https://claude.ai)（Anthropic 开发的 AI 助手）** 编写，详见 [v3.2.0 新增功能](#-v320-新增功能由-claude-开发)。
> 原版的全部功能与设计归原作者余彬彬所有。

---

## 📖 项目简介

**Reader** 是一款开源、免费的 Windows 平台轻量级小说阅读器。程序基于原生 Win32 API 与 C/C++ 开发，不依赖任何第三方运行时框架，体积小巧，启动迅速，内存资源占用低。

软件支持本地主流电子书格式（TXT、EPUB、PDF、MOBI、AZW3），支持基于规则的在线书源解析与阅读，并支持接入局域网内的开源阅读 (Legado) Web 服务。同时提供了细致的排版设置、无边框显示、窗口透明度调节及便捷的快速隐藏功能，适合多场景下的阅读使用。

---

## 🆕 v3.2.0 新增功能（由 Claude 开发）

> **贡献说明**：本节列出的所有新功能、Bug 修复与构建改动，均由 **Claude**（Anthropic 的 AI 助手）
> 应 [InmanHe](https://github.com/InmanHe) 的需求编写，包括设计、编码、测试与 GitHub Actions 自动编译。
> 代码基于原作者 [binbyu/Reader](https://github.com/binbyu/Reader) v3.1.2.0，遵循原项目的结构与风格，没有改变原有功能的行为。

### 📄 1. 新增 PDF 格式支持
- **自研轻量 PDF 文本解析器**（`Reader/books/pdf/`），不依赖 MuPDF、PDFium 等动辄数 MB 的第三方库，几乎不增加程序体积。
- 支持：交叉引用表 / 交叉引用流、对象流、增量更新，以及损坏文件的自动修复扫描；Flate、LZW、ASCIIHex、ASCII85、RunLength 压缩；
  标准字体、TrueType、Type3、Type0/CID 字体，ToUnicode 映射，GBK / Big5 / Shift-JIS / KSC 等中日韩编码。
- **加密 PDF**：支持无打开密码的加密文件（RC4 40–128 位、AES-128、AES-256），常见的“禁止复制/打印”电子书可以正常阅读。
- **为阅读而优化的排版重建**：
  - 把被换行、换页切断的段落重新拼接，英文行尾连字符自动合并；
  - 自动去除页码，以及每页重复出现的页眉页脚；
  - PDF 书签（大纲）作为目录，多级书签在目录中缩进显示；书签过少时按“章节规则”（与 TXT 相同）自动识别章节；
  - 第一页为大图时作为封面显示。
- 扫描版（纯图片）PDF 和需要打开密码的 PDF 会给出明确提示，而不是显示空白。
- 测试：1100 页 PDF 全书提取约 0.1 秒；中文样本提取结果与原文逐字一致；800 个随机损坏的 PDF 在内存检测（ASan/UBSan）下无崩溃、无卡死。

### 📚 2. 新增 AZW3 / AZW / PRC 格式
- 原版内置的 libmobi 其实能解析这些 Kindle 格式，但程序只认 `.mobi` 扩展名。现在 `AZW3`、`AZW`、`PRC` 都能直接打开（打开对话框、拖放、命令行均可）。
- 所有扩展名不再区分大小写（例如 `书.EPUB`、`书.PDF`）。

### 📝 3. 导出 Markdown（适用于 NotebookLM）
- 菜单 **文件 → 导出 Markdown**：把当前书籍（TXT / EPUB / PDF / MOBI / AZW3，以及已缓存的在线小说）导出为 `.md`，可直接作为来源上传到 NotebookLM。
- 输出格式为 RAG 检索优化：UTF-8 编码；每章一个标题（多级书签对应 `##` / `###`），NotebookLM 引用时能定位到章节；
  段落之间空行分隔；章节标题不在正文中重复；会被误认为 Markdown 语法的行首字符（`#`、`-`、`1.`、`>` 等）自动转义。
- 超过 NotebookLM 单个来源上限（50 万词）时，按章节边界自动分卷：`书名.part1.md`、`书名.part2.md` …
- **命令行批量转换**，阅读器运行时也可使用，且不会改动阅读进度配置：
  ```bat
  start /wait Reader.exe --to-md 书1.epub 书2.pdf D:\书库 -o D:\NotebookLM
  ```

### 🌐 4. 简体中文 / 繁體中文 / English 三语界面
- 全部菜单、16 个对话框和 216 条提示文字均有三种语言版本。
- 繁体界面采用台湾惯用词（档案、预设、滑鼠、字型等，并经人工校对）；英文界面调整了控件宽度，避免文字被截断。
- 默认跟随 Windows 系统语言，也可在 **设置 → 基本设置 → 界面语言** 手动切换（重启生效）；旧版配置文件可直接沿用。
- 默认章节识别新增繁体数字（兩、貳、參）与“節”“回”，繁体小说与章回体小说也能自动分章。

### 🖥️ 5. 目录随屏幕缩放
- 目录与书签列表会跟随窗口缩放、最大化、无边框 / 全屏切换，以及在不同 DPI 的显示器之间移动时自动调整大小与字体。

### 🐞 6. Bug 修复
- 拖放文件时 `DragQueryFileW` 把字节数当作字符数传入，导致栈溢出。
- 首次启动时，通过双击文件 / 文件关联 / 命令行传入的书籍不会被打开（原来只在程序已运行时生效）。
- x64 版静态引用 Windows 10 专属的 `SystemParametersInfoForDpi`，导致 Windows 7 / 8 无法启动；XP 版目录字体不随 DPI 缩放。
- 书签列表复用缓冲区但未加结束符，短书签会显示上一条书签的残留文字；书签位置越界时崩溃。
- 提示框内容（如长文件路径）超过 256 字符时字符串未终止，导致越界读取。
- EPUB：spine 引用不存在的条目时整本书无法打开；`text/html` 类型章节被丢弃；NCX 目录标题偶尔为空。
- MOBI / AZW3：带锚点的目录项丢失对应章节；若干内存分配未检查返回值；长路径导致缓冲区溢出（`Utils::fileName`）。
- 原文案错字“按住Ctrl建”。

### 📦 7. 体积优化与自动编译
- Release 改为体积优先优化（`/O1 /Os`）+ 全程序优化（LTCG）+ `/Gw`，miniz 去除写压缩包与时间相关代码。
- 新增 GitHub Actions 工作流（`.github/workflows/build.yml`）：每次推送自动编译 x64 与 x86（XP 兼容，v141_xp）两个 Release 版本，并发布到 Releases。

---

## ✨ 核心功能

- **轻量原生**：基于 C/C++ 与 Win32 API 开发，无臃肿依赖，系统资源开销极小。
- **多语言界面**：简体中文 / 繁體中文 / English，默认跟随系统，可在“设置 → 基本设置 → 界面语言”切换（重启生效）。
- **本地阅读格式支持**：
  - 支持 `TXT` 文件，内置文本编码自动识别机制，避免繁体与非中文系统的乱码问题。
  - 支持 `EPUB`、`MOBI`、`AZW3`（以及 `AZW`、`PRC`）格式电子书解析与阅读，扩展名不区分大小写。
  - 支持 `PDF`：内置轻量 PDF 文本解析器（不依赖 MuPDF/PDFium，几乎不增加体积），提取文字后按 Reader 的排版重新分页；
    自动拼接跨行、跨页的段落，去除页码与重复的页眉页脚，书签（大纲）作为章节目录，无书签时按“章节规则”自动识别；
    支持无打开密码的加密 PDF（RC4 / AES-128 / AES-256）。扫描版（纯图片）PDF 没有文字，无法阅读。
- **在线书源阅读**：
  - 支持基于 XPath 规则的自定义书源配置，可抓取并阅读网络连载小说。
  - 支持简易模式：可直接输入小说目录页 URL 自动提取章节，无需事先编写书源。
  - 支持正文文本过滤（关键字过滤、正则表达式匹配），去除无用文本与广告信息。
  - 支持手动刷新连载目录（`F5`）与章节内容（`Ctrl + F5`）。
- **局域网 Web 阅读**：
  - 支持接入局域网内「开源阅读 (Legado)」的 Web 服务，便于在 PC 端无缝阅读移动端书库。
- **窗口与显示调节**：
  - 支持无边框极简模式（`F12`）、全屏模式（`F11`）。
  - 支持窗口透明度与字体透明度调节（无边框模式下支持背景全透明、文字不透明显示）。
  - 支持快速隐藏窗口（`Alt + H`，或鼠标左右键同时按下）、鼠标移出窗口自动隐藏。
  - 支持自动翻页、左右侧区域点击翻页及滚轮平滑滚动。
- **排版与渲染**：
  - 绘图引擎支持 Direct2D 硬件加速，旧版操作系统自动降级兼容 GDI+。
  - 丰富的排版参数调节：字体、字号、颜色、字距、行距、段距、内边距、压缩空行、首行缩进及英文自动换行（Word wrap）。
  - 支持背景纯色、取色器与背景图片平铺。
  - 支持自定义正则表达式解析小说章节目录。
- **导出 Markdown（适用于 NotebookLM）**：
  - 菜单“文件 → 导出 Markdown”，把当前书籍（任意支持的格式，含已缓存的在线小说）导出为 UTF-8 的 `.md` 文件，可直接作为来源上传到 NotebookLM。
  - 每个章节一个标题（书签层级对应 `##`/`###`），段落之间空行分隔，自动转义会被误认为 Markdown 语法的行首字符。
  - 超过 NotebookLM 单个来源上限（50 万词）时按章节自动分卷：`书名.part1.md`、`书名.part2.md` …
  - 命令行批量转换（可在阅读器运行时使用）：`start /wait Reader.exe --to-md 书1.epub 书2.pdf 书库目录 -o 输出目录`
- **转换工具**：
  - 提供辅助转换工具 [`tool/olweb2txt.exe`](tool/olweb2txt.exe)，可将程序缓存的 `.ol` 和 `.web` 文件转换为标准 `.txt` 纯文本文件。

---

## 🚀 下载与版本说明

本分支（v3.2.0）的程序包可在 [InmanHe/Reader Releases](https://github.com/InmanHe/Reader/releases) 下载：`Reader-v3.2.0.0-x64.zip`（Win7 及以上）、`Reader-v3.2.0.0-x86-xp.zip`（兼容 XP）。
原作者的历史版本见 [binbyu/Reader Releases](https://github.com/binbyu/Reader/releases)。各构建版本说明如下：

| 版本分类 | 运行环境与说明 |
| :--- | :--- |
| **正式版 x64** | 适用于 64 位 Windows 系统（不支持 Windows XP 及以下系统） |
| **正式版 x86/win32** | 适用于 32 位及 64 位 Windows 系统，**兼容 Windows XP 及以上系统** |
| **无网络版 (Networkless)** | 移除了网络请求与爬虫相关模块，适用于纯本地离线阅读环境 |
| **调试版 (Debug)** | 输出详细的运行日志与诊断信息，便于排查书源配置规则 |

> 详细的各版本演进历程与更新明细，请直接查阅：👉 **[CHANGELOG.md (版本更新日志)](CHANGELOG.md)**。

---

## 📚 使用指南与相关文档

- **[版本更新日志 (CHANGELOG.md)](CHANGELOG.md)**：查看最新功能、历史版本迭代及 Bug 修复记录。
- **[书源配置说明 (doc/bs.md)](doc/bs.md)**：包含详细的书源规则说明、XPath 提取方法及参数配置教学。
- **[书源参考模板 (misc/bs.json)](misc/bs.json)**：预置书源示例文件，可在软件“书源配置”页面通过“导入”功能加载。
- **[缓存转换工具 (tool/olweb2txt.exe)](tool/olweb2txt.exe)**：可将缓存的 `.ol` / `.web` 文件转换为本地 `.txt` 格式。
- **Markdown 转换（命令行）**：`Reader.exe --to-md <文件或目录>... [-o <输出文件或目录>]`，支持 txt / epub / pdf / mobi / azw3 / azw / prc。
  不指定 `-o` 时输出到原文件旁；目录只转换其中的书籍文件（不递归）；章节规则沿用软件“高级设置”中的配置。
  Reader 是窗口程序，在批处理中请使用 `start /wait`，返回码 0 表示全部成功。
- **[完整使用说明 (misc/readme.txt)](misc/readme.txt)**：包含按键配置、高级功能、目录解析正则等详细参考手册。

---

## ⌨️ 常用快捷键

> 所有快捷键与热键均可在菜单 **【设置】 > 【按键设置】** 中自行修改。

| 类别 | 快捷键 | 功能说明 |
| :--- | :--- | :--- |
| **窗口控制** | `Alt + H` | 快速隐藏 / 显示窗口（亦可鼠标左右键同时按下） |
| | `F12` | 隐藏 / 显示窗口边框 |
| | `Ctrl + 鼠标滚轮` | 调节窗口透明度（无边框下支持背景透明、文字不透明） |
| | `Alt + 鼠标滚轮` | 调节字体透明度 |
| | `Ctrl + Alt + Shift + P` | 开启 / 关闭“鼠标移出窗口后自动隐藏” |
| **界面视图** | `F11` / `Esc` | 全屏切换 / 退出全屏 |
| | `Ctrl + =` / `Ctrl + -` | 字体放大 / 缩小 |
| | `Ctrl + T` | 窗口置顶 / 取消置顶 |
| **翻页与导航** | `空格键` | 开始 / 停止自动翻页 |
| | `←` / `→` | 上一页 / 下一页 |
| | `Ctrl + ←` / `Ctrl + →` | 跳转至上一章 / 下一章 |
| | `↑` / `↓` | 向上 / 向下滚动指定行数 |
| **阅读操作** | `Ctrl + O` | 打开文件（亦支持文件直接拖拽入窗口） |
| | `Ctrl + F` | 全文关键字查找与定位 |
| | `Ctrl + G` | 按百分比快速跳转进度 |
| | `Ctrl + M` | 添加书签 |
| | `Ctrl + E` | 进入文本编辑与复制模式 |
| **在线小说** | `F5` | 刷新当前在线小说的章节列表 |
| | `Ctrl + F5` | 强制刷新并重新获取当前章节内容 |

---

## ❓ 常见问题 (FAQ)

<details>
<summary><b>1. 按 F12 无法隐藏边框？</b></summary>

部分笔记本电脑默认开启了 Fn 功能键锁定，请尝试按 `Fn + F12`；或进入菜单 **【设置】>【按键设置】** 将该功能重新绑定到其他按键。
</details>

<details>
<summary><b>2. 打开小说后出现乱码？</b></summary>

软件内置编码自动识别逻辑。若遇到极少数编码异常情况，请检查菜单 **【设置】>【字体设置】**，确认未选用以 `@` 字符开头的竖排字体。
</details>

<details>
<summary><b>3. 英文排版时单词被断行截断？</b></summary>

请在菜单 **【设置】>【显示设置】** 中勾选 **【Word wrap】** 选项，即可按照完整英文单词进行换行。
</details>

<details>
<summary><b>4. 关于杀毒软件报毒提示？</b></summary>

本项目源码完全开放，无任何恶意行为。由于个人开源项目未购买商业代码签名证书，部分杀毒软件可能产生误报，属于正常现象，请加入信任白名单即可。
</details>

<details>
<summary><b>5. 软件异常或需要恢复初始状态？</b></summary>

删除程序同级目录下的配置文件 `.settings.json`（早期版本为隐藏文件 `.cache.dat`），重启程序即可恢复至默认设置。
</details>

---

## 📜 软件策略与免责声明

1. **项目性质**：Reader 为个人开源、免费软件，主要用于学习交流与电子书阅读，版权归作者所有。
2. **使用限制**：本软件严禁用于非法用途，严禁用于任何商业目的。
3. **内容免责**：软件本身不提供、不存储任何网络文学版权内容。在线书源及 Web 服务所获取的文本均来自第三方公开网页或用户自建服务，第三方内容的合法性与准确性由对应来源负责。
4. **版权保护**：所有图书作品版权归原作者及权利人所有，倡导大家前往正规渠道阅读与支持正版文学作品。

---

## 🤝 鸣谢与联系

感谢所有向本项目提出反馈建议和提交代码的朋友。

- **原作者**：余彬彬（binbyu@qq.com），项目主页 [https://github.com/binbyu/Reader](https://github.com/binbyu/Reader)
- **分支维护**：[InmanHe](https://github.com/InmanHe)，项目主页 [https://github.com/InmanHe/Reader](https://github.com/InmanHe/Reader)
- **v3.2.0 新功能开发**：[Claude](https://claude.ai)（Anthropic 的 AI 助手），详见 [v3.2.0 新增功能](#-v320-新增功能由-claude-开发)
- **更新日志**：[CHANGELOG.md](CHANGELOG.md)
