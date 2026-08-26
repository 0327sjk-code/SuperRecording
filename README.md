[README.md](https://github.com/user-attachments/files/31455743/README.md)
# SuperRecording

`v1.1.0`。常驻 Windows 系统托盘的原生 C++ 区域录屏工具。单击托盘图标或按全局快捷键直接框选；默认快捷键为 `F3`，可从托盘右键菜单自定义。录制结束后可在深色编辑器中预览、裁切首尾，并导出 MP4 或 GIF。

## 使用

1. 运行 `SuperRecording.exe`。程序不显示主窗口，图标常驻系统托盘；首次运行默认注册为当前用户开机自启。
2. 单击托盘图标或按当前全局快捷键，拖拽选择录制区域；按 `Esc` 取消。
3. 选框靠近显示器边缘时自动吸附；单击而不拖动时选择光标所在显示器全屏。
4. 录制时选区四周持续显示紧凑的红色边界，选区外保持与框选阶段一致的压暗效果；浮条可拖动、暂停、继续或结束。边界、压暗层与浮条均使用 Windows 捕获排除策略，不进入成片。
5. 在编辑窗口预览录屏、连续拖动起点、终点或播放头，选择 `MP4` 或 `GIF`。裁切范围稳定 350 ms 后软件会在后台准备成片；准备完成后，保存与复制直接复用同一文件。

托盘右键菜单可查看并设置录制快捷键、切换 `30/60 FPS`（默认 60）、设置默认保存目录，并随时关闭或重新启用当前用户开机自启。快捷键支持单独使用 `F1–F24`（系统保留的 `F12` 除外），或使用 `Ctrl / Alt / Shift / Win` 搭配字母、数字和功能键。复制功能写入 `CF_HDROP` 文件剪贴板，可在资源管理器、聊天软件或文档软件中直接粘贴。

录制过程中已经持续编码 H.264；停止录制只排空编码器并完成 MP4 容器，不会重新编码整段视频。完整且未裁切的 MP4 采用快速交付路径：复制时直接引用已完成的源文件；保存到同卷目录时使用毫秒级硬链接提交。

编辑后的媒体采用分级交付：

- MP4 仅修改终点，或起点正好位于 H.264 关键帧时，直接重封装压缩样本，无损且不二次编码。
- 任意非关键帧精确起点在滑块稳定 350 ms 后后台编码；成片按“源文件身份 + 起止点 + 格式 + 管线版本”缓存。
- GIF 同样后台生成并缓存。
- 同一编辑结果先复制再保存、或先保存再复制，只生成一次；后续操作使用同卷硬链接，跨卷时仅复制文件。

时间轴使用二维命中区域：播放头三角与中心线只移动预览位置，端点手柄两侧才修改裁切范围，避免预览拖动误把完整原片变成转码任务。

## 正式版界面

- 中文及中英混排显式使用 `Microsoft YaHei UI`，纯数字计时使用 `Segoe UI`。
- 正文和普通控件使用 400 字重；页内标题与主操作使用 600 字重。
- 文本使用标准 ClearType 网格拟合；圆角、图标、时间轴、手柄和状态点由共享 GDI+ 抗锯齿层绘制。
- 编辑窗口采用沉浸式深色标题栏、近黑画布、分层深灰控制面与更深的视频舞台；默认客户区 `1040×720`，最小客户区 `760×590`。
- 深色编辑器保持高对比正文、次级文字、焦点环、时间轴端点和播放头；保存与 MP4 选中保留绿色语义，GIF 使用中性选中态。
- 录制浮条固定为紧凑的 `250×40`，统一中文与数字基线，并保持 `WDA_EXCLUDEFROMCAPTURE`。
- 录制区域使用四个独立的 `2 DIP` 红色边窗和四个选区外压暗窗；所有边窗、压暗窗和浮条无法确认捕获排除时拒绝开始录制，避免 UI 或遮罩意外进入成片。
- 悬停、按下、焦点、格式选择与时间轴状态采用克制的短时过渡；只有正在过渡的控件维持 `16 ms` 动画计时器，静止后立即停止。Windows 关闭客户端区域动画时，各状态立即切换。

### 编辑器键盘操作

- `Tab` / `Shift+Tab`：按时间轴、播放、MP4、GIF、复制、保存的顺序移动焦点。
- `Space` / `Enter`：激活当前按钮；自动重复按键不会重复切换播放状态。
- MP4/GIF 聚焦时使用方向键切换格式。
- 时间轴聚焦时：`Up` / `Home` 选择起点，`Down` / `End` 选择终点，`Left` / `Right` 每次微调 100 ms，按住 `Ctrl` 时每次微调 1 秒。
- `Esc`：关闭编辑窗口。

## 画质与文件大小

- MP4：H.264 High Profile、峰值约束 VBR、优先硬件编码。
- 自适应码率：1080p60 约 20 Mbps，4K60 约 80 Mbps；静止画面由 VBR 自动降低实际占用。
- 录制像素不缩放；选区仅为满足 H.264 要求向内规整到偶数尺寸。
- GIF：为控制体积自动限制到 1280×720、最高 15 FPS；MP4 保留源分辨率与源帧率。

## 构建

依赖：

- Windows 10 SDK 10.0.22621 或更新版本
- Visual Studio 2022 C++ Build Tools（MSVC v143）
- 系统组件：DXGI 1.2、Direct3D 11、Media Foundation、WIC、MFPlay、GDI+

在 PowerShell 中执行：

```powershell
cd 'F:\CodexTool\Video\Project'
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Configuration Release -Rebuild
```

输出：`F:\CodexTool\Video\Project\build\Release\SuperRecording.exe`。Release 使用静态 MSVC 运行库，不需要随包携带 VC Runtime 或第三方 DLL。

## 工程结构

```text
src/app        单实例、托盘、自定义全局快捷键、当前用户开机自启、菜单、应用状态机
src/selection  虚拟桌面选区与显示器边缘吸附
src/overlay    排除捕获的录制区域边框与控制浮条
src/capture    DXGI Desktop Duplication 采集与帧调度
src/media      H.264 写入、无损快裁、精确转码、成片缓存、GIF 与剪贴板
src/editor     MFPlay 预览、双端点时间轴、导出窗口
src/ui         主题令牌与共享抗锯齿绘制层
src/config     用户设置持久化
src/common     公共类型、日志、Win32 RAII 辅助
```

详细数据流与线程约束见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 运行数据

- 配置：`%LOCALAPPDATA%\SuperRecording\settings.ini`；首次启动会在新配置不存在时安全迁移旧版 `%LOCALAPPDATA%\QingRecorder\settings.ini`
- 日志：`%LOCALAPPDATA%\SuperRecording\Logs`
- 原始录制优先写入当前保存目录的隐藏文件夹 `.SuperRecording-cache`，以保证完整 MP4 可快速保存；目标卷不支持硬链接时回退 `%LOCALAPPDATA%\SuperRecording\Temp`
- 后台成片缓存：优先与源录屏同卷的 `.SuperRecording-cache`；不可写时回退 `%LOCALAPPDATA%\SuperRecording\ExportCache`
- 剪贴板交付文件：`%LOCALAPPDATA%\SuperRecording\Clipboard`
- 默认成片：Windows“视频”目录下的 `SuperRecording` 文件夹

程序启动时只清理上述私有缓存目录中的过期文件：未完成导出保留 24 小时，完整缓存和剪贴板文件保留 7 天；不会递归删除目录，也不会触碰正式保存文件。

## 当前边界

- 支持任意单个显示器及负坐标副屏；跨越两个显示器的单个选区会提示重新框选。
- 当前版本录制画面与鼠标，不录系统声音或麦克风。
- 当前版本不支持旋转显示器录制。
- 捕获排除需要 Windows 10 2004 或更新版本；建议使用 Windows 11。
- “500 ms 内完成”适用于完整 MP4、可无损快裁的 MP4、以及已经后台准备完成的 MP4/GIF。同卷通常为硬链接提交；跨卷、FAT/exFAT、离线网络位置仍受实际复制速度限制。
- 用户刚设置任意非关键帧精确起点便立刻点击导出时，需要等待后台任务的剩余编码时间；H.264 帧间压缩无法在未知裁切点上同时保证逐帧精确、零等待和小文件。界面会提前预生成并复用结果，不会重复编码。
