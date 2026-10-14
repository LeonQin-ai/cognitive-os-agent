# 附件格式与验证

附件不仅保存文件，还会提取可用内容并绑定聊天任务。支持情况如下。

| 类型 | 格式 | 送入模型的内容 |
|---|---|---|
| 图片 | PNG、JPEG、WebP、GIF、BMP、TIFF、HEIC/HEIF、AVIF、SVG | 原图或标准化后的 JPEG；动态图和多页图片采样画面 |
| Word | DOCX、DOC、RTF、ODT | 提取文字、采样页面图片；表格、插图及页面排版保留在页面图片中 |
| PPT | PPTX、PPT、ODP | 幻灯片文字、备注和采样页面图片 |
| PDF | 文本 PDF、扫描 PDF | 提取文字与采样页面图片；扫描件由视觉模型读取图像 |
| 视频 | MP4、MOV、AVI、MKV、WebM、M4V、MPEG/MPG、WMV、FLV、3GP | 最多六个时间点的画面、时间标签、时长及采样说明 |
| 其他文本 | XLSX、CSV、TXT、Markdown、代码和配置文件等 | 提取或读取文字，分块索引 |

格式支持仍取决于文件自身的编码和完整性。损坏、加密、无法解码、缺少依赖的文件会返回明确的 `failed` 状态；不会仅因写入磁盘成功而标为可分析。

## 边界

- 每个附件最多 **64 MiB**，一条消息最多四个附件。最多两个附件解析任务并行；繁忙时返回 429，普通任务查询不需要等待解析完成。
- 页面和视频图片均最多采样六幅，最长边 1280 像素。文字提取最多一百万字符；模型首轮得到最长 32,000 字节的文字预览以及完整提取文件路径。
- 视频分析覆盖画面，**尚不包含音轨转写**。采样可能遗漏两个采样点之间的事件，因此模型消息会明确注明这一范围。
- 扫描 PDF 的页面会进入视觉模型；当前不会额外生成本地 OCR 全文索引。
- 图片、扫描件、页面图形和视频分析需要支持视觉输入的模型。测试通过本地捕获服务确认实际图片内容进入 OpenAI/Anthropic 请求，不代表已测量真实模型识别准确率。
- SVG 不加载外部文件、网络图片或外部样式。

## 依赖与安装

```powershell
python -m pip install -r backend/tools/requirements-attachments.txt
```

旧版 Office 转换和完整 Word/PPT 页面渲染需要 Windows 的 Word/PowerPoint，或已安装的 LibreOffice。Office 未安装时，DOCX/PPTX 仍可提取 XML 文字和嵌入图片，并明确说明页面渲染不可用；旧版 DOC/PPT 则需要转换器。

可指定程序路径：

- `COA_DOCUMENT_PYTHON`：附件解析使用的 Python。
- `COA_LIBREOFFICE`：LibreOffice/soffice 可执行文件。
- `COA_FFMPEG`：FFmpeg 可执行文件；否则优先 PATH，再使用 imageio-ffmpeg 自带程序。

转换器在只读副本上运行，不改变原文件；Word/PowerPoint 禁用宏。处理进程最多运行 60 秒，Office 解压大小、像素数和输出量均有上限。解析脚本嵌入 C 二进制，安装后的程序不需要源码目录。

## 验证

```powershell
./backend/build.ps1 -Target all
./backend/build/test-review.exe
python backend/tests/test_upload_e2e.py
```

端到端测试生成真实 DOCX/PPTX，并通过 Office 导出 DOC/PPT；生成扫描 PDF、BMP/TIFF/HEIC/SVG/GIF 和 AVI/MP4/MOV/MKV/WebM 视频。检查原始内容、页面/帧及时间标签进入实际模型请求，同时覆盖删除、重启、会话隔离、上传等待、桌面与手机自动滚动，以及解析期间任务查询。

测试在 Windows 上运行。完整日志和机器可读结果位于 `backend/build/review-*.log` 与 `backend/build/review-e2e.json`。本次功能包含在 v0.8.0 中。


最终本地验证：新增端到端套件 **98 项检查全部通过**，包括帧字节比对、四视频的 24 张画面、FFmpeg 回退和解析期间任务查询。专项原生回归 236 个断言通过；场景套件 91 项通过。完整单元套件零失败，具体数量见最新日志。

图片和视频帧模型请求使用独立 HTTP 超时配置：默认 120000 ms，环境变量 `COA_LLM_VISION_TIMEOUT_MS` 可设 1000–600000 ms，并受全局模型超时限制。Windows 的解析、连接、发送和接收阶段各有超时，因此该值不是整次请求的硬性总时长。图片请求输出预算为 8192 token；失败直接保留模型、超时配置及上游错误，不自动重复发送图片，也不另发请求生成失败摘要。用户可以修正接口或模型后手动重试。代码不会根据模型名字猜测视觉能力。

`python backend/tests/test_vision_wait_e2e.py` 覆盖慢模型响应、图片负载、失败后图片重试和普通文本请求；CI 在 Windows、Linux 和两个 macOS 架构执行。

补充“始终卡住”回归：模型完全不返回响应头，以及每30 ms仅返回一个字节且不及时完成响应体。接收阶段使用固定截止时间，持续收到少量数据也不会重置截止时间；超时或接收中断后丢弃不完整响应，不把半份结果当作成功。1000 ms测试配置下，两场景均在5秒内结束为FAILED（包含Windows阶段超时余量），每场景仅一次模型请求；随后图片及文本请求正常完成。
