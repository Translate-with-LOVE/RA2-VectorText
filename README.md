# VectorText

由 Syringe 加载的 32 位矢量文字补丁，直接挂接 `gamemd.exe 1.001` 的原生文字函数。
不依赖 Phobos、Ares 或 cnc-ddraw，FreeType 静态链接进 DLL。

当前使用单行自然 X 布局：游戏继续决定换行、Y、显现顺序和颜色，补丁计算每行字距，
有 cnc-ddraw 时，文字灰度覆盖率在 BGRA8 呈现阶段合成；否则使用原 RGB565 表面。
中文、英文、数字和符号共享基线，保留完整纵向轴承。
字体缺失的图标使用 game.fnt 原位图，后面的文字继续自然排版。
任务加载目标框和战役消息背景按自然文字宽度留余量；不修改文本或伪造字符数量。
tooltip 也按自然宽度测量，保留原生内边距和右边界限制。
tooltip、任务消息和侧栏“就绪”等状态标签的背景高度按实际笔画范围留上下边距，多行提示保留原行距。
任务消息的高度调整在 Phobos 半透明背景 hook 之前完成，兼容安装、关闭或未安装 Phobos 的路径。

配置、实现边界、hook 约定及验证方法见 [最终渲染说明](docs/rendering.md)。

## 构建与安装

需要带 x86 C++ 工具的 Visual Studio / Build Tools、CMake 3.21+、Ninja，以及 `third_party/freetype` 源码。
首次获取源码后初始化固定版本的 FreeType，再在本目录执行：

```bat
git submodule update --init --recursive
build.bat
```

入口脚本自动定位 MSVC x86 与 CMake/Ninja（可使用 Visual Studio 附带的版本），
通过 CMake 增量构建静态 FreeType、MinHook 和 DLL，然后部署到游戏目录。
已有 `VectorText.ini` 会保留；不存在时复制默认配置。可设置 `VT_VCVARS`、`VT_CMAKE` 指定工具路径。
可分发文件集中在 `build/cmake/bin/`：DLL、示例 INI、最终说明、GPL 协议和第三方许可。
部署另外将许可放入游戏目录的 `VectorText-licenses/`，避免覆盖游戏自身的许可文件。
在 x86 Native Tools 命令行中，也可以直接使用：

```bat
cmake --preset x86-release
cmake --build --preset x86-release
ctest --preset offline
cmake --build --preset deploy
```

离线 CTest 覆盖全部非显示测试；`cnc_present_test.bat` 验证实际显示后端，需桌面环境。
每个测试 `.bat` 仍可单独执行，编译均调用同一套 CMake 目标。
原生字体、机器码及 Phobos 兼容测试读取本机游戏文件；仓库不附带这些文件。
默认游戏目录为仓库上级目录，可用 CMake 的 `VT_GAME_DIR` 指定其他安装位置。
通过正常 MO 启动器或 Syringe 启动游戏加载 DLL；修改配置或重新构建后重启游戏。

## 当前配置

- 中文：思源黑体 `NotoSansSC-VF.ttf`，16px，可变字重 450。
- 英文：`arial.ttf`，13px，常规体。
- `Mode` 仅支持 `off`（关闭）、`observe`（仅观测）、`draw`（绘制）；其他值按 `observe` 处理。
- `Mode=draw`、`LineRender=true`：启用单行 X 覆盖；`LineRender=false` 回到逐字绘制。
- `Present32=true`：检测 cnc-ddraw 并启用 32 位呈现；设为 `false` 可恢复 RGB565。
- `HiDPI=true`：依据 cnc-ddraw 实际视口自动选择；D3D9 等比 2× 输出时按双倍分辨率栅格化文字，原生全屏与其他倍率保留 1× 路径。设为 `false` 使用 1× 文字合成。
- `LegacyCodepage1252=true`：将 `U+0080～U+009F` 中 27 个旧码位映射为 Windows-1252 对应的 Unicode 字形，兼容原版 game.fnt 的符号。设为 `false` 按原始 Unicode 解释。
- `Metrics=game`、`AdvanceScale=1.0`：返回给游戏的步进保留原度量。
- `DynamicTextWidth=true`、`LineWidthPadding=4`：动态背景框预留 4px。
- `BaselineRow=13`、`Supersample=1`、`Hinting=0`、`Subpixel=true`：统一基线，轻量纵向 hinting 和四分之一像素灰度定位，保留自然字距。

配置完整说明及回退方法见最终渲染说明；字体文件需存在于本机。
仓库中的 [VectorText.ini](VectorText.ini) 是完整推荐示例。关闭矢量接管使用 `Mode=off`；
`Enabled` 控制日志和 32 位呈现启动，不能替代 `Mode` 关闭 RGB565 绘制。
所有布尔配置支持不区分大小写的 `true/false`、`yes/no`、`on/off`，并兼容 `1/0`；整数和枚举配置保留原语义。
不使用横向强制拉伸。过长单行先收紧空白，再最多等比缩至 90%；无法容纳时整行回退。

## 验证与诊断

先运行 `build.bat`，再执行需要的检查：

| 入口 | 实际用途 |
| --- | --- |
| `line_test.bat` | 单行布局、混排、宽度、对齐、显现、线程隔离；真实游戏测量机器码及已安装 DLL hook；本机 Phobos 背景处理机器码的透明度与回退；无 Phobos/Ares 的图标计数器 |
| `baseline_test.bat` | 与 FreeType 独立固定基线位图比较，验证中英文、数字、符号、相位、缩放及裁剪 |
| `render_quality_test.bat` | 当前生产配置的黑底和纹理背景预览，以及覆盖率、缓存、度量与字体数据检查 |
| `present32_test.bat` | 32 位覆盖率精度、稀疏文字层、裁剪、复制、拉伸及重复重绘 |
| `cnc_present_test.bat` | 三后端实际像素、复制、翻页、清除；2× 独立采样与线性混合、切回 1×、无边框/独占全屏及关闭选项回退 |
| `takeover_test.bat` | 单行拒绝后的逐字接管、颜色、抗锯齿、裁剪和缺字回退 |
| `hooktest_draw.bat --mode draw` | 逐字 hook 的 ESP/EAX、返回跳板与拒绝路径；仅支持 `off`、`observe`、`draw` |
| `python tools\preview_guides.py` | 显示上、中、下辅助线与实际墨迹范围，完整行按最近邻放大 |
| `python tools\verify_dll.py` | DLL 导出、15 条钩子记录、重定位、Phobos/Ares 导入依赖和本机 Phobos hook 区间冲突检查 |

预览图输出到 `build/render-qa/`。离线通过仍需结合实际游戏画面验收。
当前 19 项离线 CTest 通过；实际显示测试仍有两项 D3D9 屏幕像素采样失败，原因待核实，不能视为全后端验收通过。
游戏目录的 `VectorText.log` 记录 `LINE ready`、`LINE fallback`、`LINE native icon`
和 `DYNAMIC width`，可确认实际调用是否命中。

保留的诊断工具：`tools/analyze_log.py` 汇总实际日志，`tools/find_callers.py` 扫描游戏原生调用点，
`tools/screenshot_diff.py` 比较同一静态画面的截图。各工具支持 `--help`。
`tools/find_vcvars.bat` 与 `tools/cmake_build.bat` 负责定位工具链和调用 CMake；所有编译目标统一定义在 `CMakeLists.txt`。

`cnc_present_test.bat` 会短暂显示测试窗口，仅改动 `build/cnc-test/` 中的测试配置。
32 位路径使用静态链接的 [MinHook](third_party/minhook/README.md)，不替换游戏的 ddraw.dll。

## 开源协议

项目自有代码、构建脚本、文字文档及示例配置使用 **GPL-3.0-only**，完整协议见 [LICENSE](LICENSE)。
可按 GPL 第 3 版复制、修改和分发本项目；本项目不提供任何担保。
FreeType 选择与 GPLv3 兼容的 FTL，MinHook/HDE 保留 BSD-2-Clause，其他内含组件保留各自许可。
版权声明、完整第三方协议及发布源码要求见 [第三方声明](THIRD_PARTY_NOTICES.md)。
字体文件和游戏资产不包含在本项目许可授权中。
