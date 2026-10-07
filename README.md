# VectorText

由 Syringe 加载的 32 位矢量文字补丁，直接挂接 `gamemd.exe 1.001` 的原生文字函数。
不依赖 Phobos、Ares 或 cnc-ddraw，FreeType 静态链接进 DLL。

当前使用单行自然 X 布局：游戏继续决定换行、Y、显现顺序和颜色，补丁计算每行字距，
将灰度覆盖率合成到原 RGB565 表面。中文、英文、数字和符号共享基线，保留完整纵向轴承。
字体缺失的图标使用 game.fnt 原位图，后面的文字继续自然排版。
任务加载目标框和战役消息背景按自然文字宽度留余量；不修改文本或伪造字符数量。

配置、实现边界、hook 约定及验证方法见 [最终渲染说明](docs/rendering.md)。

## 构建与安装

需要带 x86 C++ 工具的 Visual Studio / Build Tools，以及 `third_party/freetype` 源码。
在本目录执行：

```bat
build.bat
```

脚本自动定位 MSVC，首次构建静态 FreeType，生成游戏目录的 `VectorText.dll`，
并将项目 `VectorText.ini` 复制到游戏目录。找不到工具链时可设置 `VT_VCVARS`
为本机 `vcvars32.bat` 的路径。
通过正常 MO 启动器或 Syringe 启动游戏加载 DLL；修改配置或重新构建后重启游戏。

## 当前配置

- 中文：思源黑体 `NotoSansSC-VF.ttf`，16px，可变字重 450。
- 英文：`arial.ttf`，13px，常规体。
- `Mode=draw`、`LineRender=1`：启用单行 X 覆盖；`LineRender=0` 回到逐字绘制。
- `Metrics=game`、`AdvanceScale=1.0`：返回给游戏的步进保留原度量。
- `DynamicTextWidth=1`、`LineWidthPadding=4`：动态背景框预留 4px。
- `BaselineRow=13`、`Supersample=1`、`Hinting=0`：统一基线，在目标像素网格进行 LIGHT hinting。

配置完整说明及回退方法见最终渲染说明；字体文件需存在于本机。
不使用横向强制拉伸。过长单行先收紧空白，再最多等比缩至 90%；无法容纳时整行回退。

## 验证与诊断

先运行 `build.bat`，再执行需要的检查：

| 入口 | 实际用途 |
| --- | --- |
| `line_test.bat` | 单行布局、混排、宽度、对齐、显现、线程隔离；真实游戏测量机器码及已安装 DLL hook；无 Phobos/Ares 的图标计数器 |
| `baseline_test.bat` | 与 FreeType 独立固定基线位图比较，验证中英文、数字、符号、相位、缩放及裁剪 |
| `render_quality_test.bat` | 当前生产配置的黑底和纹理背景预览，以及覆盖率、缓存、度量与字体数据检查 |
| `takeover_test.bat` | 单行拒绝后的逐字接管、颜色、抗锯齿、裁剪和缺字回退 |
| `hooktest_draw.bat --mode draw` | 逐字 hook 的 ESP/EAX、返回跳板与拒绝路径；也支持 `aa`、`swap`、`observe` |
| `python tools\verify_dll.py` | DLL 导出、13 条钩子记录、重定位和 Phobos/Ares 导入依赖检查 |

预览图输出到 `build/render-qa/`。离线通过仍需结合实际游戏画面验收。
游戏目录的 `VectorText.log` 记录 `LINE ready`、`LINE fallback`、`LINE native icon`
和 `DYNAMIC width`，可确认实际调用是否命中。

保留的诊断工具：`tools/analyze_log.py` 汇总实际日志，`tools/find_callers.py` 扫描游戏原生调用点，
`tools/screenshot_diff.py` 比较同一静态画面的截图。各工具支持 `--help`。
`tools/find_vcvars.bat` 与 `third_party/build_freetype.bat` 是构建依赖。
