# VectorText

由 Syringe 加载的 32 位矢量文字补丁。同一个 `VectorText.dll` 自动识别
原版 RA2 `game.exe 1.006` 和尤里复仇 `gamemd.exe 1.001`，挂接各自的原生文字函数。
不依赖 Phobos、Ares 或 cnc-ddraw，FreeType 静态链接进 DLL。
最低运行系统为 **Windows 7**，DLL 使用 x86 及静态 C/C++ 运行库，也可在 64 位 Windows 的游戏进程中加载。

当前使用单行自然 X 布局：游戏继续决定换行、Y、显现顺序和颜色，补丁计算每行字距，
有 cnc-ddraw 时，文字灰度覆盖率在 BGRA8 呈现阶段合成；否则使用原 RGB565 表面。
中文、英文、数字和符号共享基线，保留完整纵向轴承。
字体缺失的图标使用 game.fnt 原位图，后面的文字继续自然排版。
任务加载目标框和战役消息背景按自然文字宽度留余量；不修改文本或伪造字符数量。
tooltip 也按自然宽度测量，保留原生内边距和右边界限制。
tooltip、任务消息和侧栏“就绪”等状态标签的背景高度按实际笔画范围留上下边距，多行提示保留原行距。
原版过场字幕按实际绘字范围保存下一帧的擦除矩形，避免换句后残留旧颜色的逗号、负向笔画或高清采样边缘。
任务消息的高度调整在 Phobos 半透明背景 hook 之前完成，兼容安装、关闭或未安装 Phobos 的路径。

配置、实现边界、hook 约定及验证方法见 [最终渲染说明](docs/rendering.md)。

## 构建与安装

需要 MSVC **14.44（VS 2022）x86/x64 工具集**、CMake 3.21+、Ninja，以及 FreeType、MinHook 两个 submodule 的源码。
可以使用 VS 2026 的 IDE / Build Tools 安装目录，但须另外安装上述工具集；默认的 14.50+ 已不支持 Win7，
CMake 会拒绝它。[微软平台支持说明](https://learn.microsoft.com/en-us/cpp/overview/supported-platforms-visual-cpp)。
首次获取源码后初始化两个固定版本的 submodule，再在本目录执行：

```bat
git submodule update --init --recursive
build.bat
```

入口脚本自动定位 MSVC x86 与 CMake/Ninja（可使用 Visual Studio 附带的版本），
通过 CMake 增量构建静态 FreeType、MinHook 和 DLL，然后部署到游戏目录。
已有 `VectorText.ini` 会保留；不存在时复制默认配置。可设置 `VT_VCVARS`、`VT_CMAKE` 指定工具路径；
`VT_VCVARS_VER` 指定兼容 Win7 的工具集版本，默认 `14.44`，也允许 `14.29` 或 `14.3x/14.4x`。
可分发文件集中在 `build/cmake/bin/`：DLL、示例 INI、最终说明、GPL 协议和第三方许可。
原版与尤里复仇使用同一份 DLL、配置和三个呈现后端。手动安装时将 `VectorText.dll`
放到对应游戏目录，在该目录运行 `Syringe.exe game.exe` 或 `Syringe.exe gamemd.exe`。
识别结果写入 `VectorText.log` 的 `profile:` 行，不需要切换构建或设置游戏版本。
加载时会核对 PE 身份、共同加载入口及全部 16 个文字入口的原始指令；未知或不匹配的程序拒绝接管。
部署另外将许可放入游戏目录的 `VectorText-licenses/`，避免覆盖游戏自身的许可文件。
在上述兼容工具集的 x86 Native Tools 命令行中，也可以直接使用：

```bat
cmake --preset x86-release
cmake --build --preset x86-release
ctest --preset offline
cmake --build --preset deploy
```

要同时运行原版 RA2 的原生机器码回归，配置游戏路径后运行同一套测试：

```bat
cmake --preset x86-release "-DVT_RA2_GAME_DIR=F:/SteamLibrary/steamapps/common/Command & Conquer Red Alert II"
cmake --build --preset x86-release
ctest --preset offline
```

`line_ra2` 使用同一个测试 EXE 和 DLL，执行原版的测量代码并验证字幕擦除、启动混排与返回跳板；
`unsupported_host` 检查未知程序在安装任何钩子前被拒绝。

离线 CTest 覆盖全部非显示测试；`cnc_present_test.bat` 验证实际显示后端，需桌面环境。
每个测试 `.bat` 仍可单独执行，编译均调用同一套 CMake 目标。
原生字体、机器码及 Phobos 兼容测试读取本机游戏文件；仓库不附带这些文件。
默认游戏目录为仓库上级目录，可用 CMake 的 `VT_GAME_DIR` 指定其他安装位置。
通过正常 MO 启动器或 Syringe 启动游戏加载 DLL；修改配置或重新构建后重启游戏。
编译统一使用 `WINVER/_WIN32_WINNT=0x0601`、`NTDDI_VERSION=0x06010000` 和 PE 6.1 目标。
`python tools\verify_dll.py` 检查普通、延迟导入及 Win7 API 白名单；静态链接无需另装 VC 运行库。
导入检查和本机回归不能代替 Win7 实机测试；Win7 上的实际加载、显示及驱动兼容仍需验收。

## 代码结构

| 入口或模块 | 职责 |
| --- | --- |
| `src/Hooks.cpp`、`src/hooks/` | Syringe ABI、行入口与返回跳板；文本观测、尺寸测量、背景调整分别实现。 |
| `include/GameAddresses.h`、`src/RuntimeHooks.*`、`RuntimeHookStub.cpp` | 自动选择 RA2/YR 地址表，校验并安装文字 hook；保留寄存器、标志和原生栈约定。 |
| `src/Takeover.cpp`、`src/takeover/` | 字体初始化与逐字回退；整行排版、只读测量、诊断统计分别实现。 |
| `src/GlyphSource.*`、`src/glyph/` | 字形缓存入口；字体资源及缓存失效、配置与度量、完整栅格流水线分别实现。 |
| `src/PixelWriter.*`、`src/PixelPlane.*`、`src/plane/` | 像素写入、稀疏文字层及按职责拆分的绘制、复制、同步与合成实现。 |
| `src/Presentation32.cpp`、`src/presentation/` | cnc-ddraw 检测与安装；表面生命周期、GDI/OpenGL/D3D9 后端及 HiDPI 图集分别实现。 |
| `src/Config.*`、`src/Logger.*` | INI 解析与配置访问；调用日志与摘要输出。 |

顶层头文件提供模块接口，子目录中的头文件和 `ConfigState.h` 是私有实现接口。
呈现测试复用生产后端；`presentation/TestAccess.cpp` 只编入测试程序。
具体调用链和状态归属见 [模块边界](docs/rendering.md#模块边界)。

## 配置参数

所有参数放在游戏目录 `VectorText.ini` 的 `[VectorText]` 节下，修改后重启游戏生效。
**默认值**表示代码在该项缺失时使用的值。完整配置文件见 [VectorText.ini](VectorText.ini)。
布尔值不区分大小写，仅支持 `true/false`、`yes/no`、`on/off` 和 `1/0`；其他数字（如 `2`、`-1`）按无效值处理。
数值先按有符号值完整解析，再按各参数范围截断，最后转换为存储类型；整数支持十进制和 `0x` 十六进制，浮点数支持十进制及科学计数法。
缺失、空值、格式错误、解析溢出、过长值和非有限浮点数使用默认值；数值前后的空白会被忽略。
枚举按各自规则解析。
字号、基线及边距使用游戏逻辑像素；三个后端的 HiDPI 均保留独立的 2～8× 字形采样，按实际画面倍率合成，不改变文字的视觉尺寸。
启动画面在首次绘字前按真实客户区预选密度，640×480→4K 使用 6× 字形，1920×1080→4K 使用 2×。
D3D9 的一次性启动图在首次呈现时直接建立高清文字图集，GDI 也保留后端启动前绘制的文字。
`cnc_present_test.bat --startup-only` 检查三后端的 4K 静态启动图，无需再次锁定表面或上传。

| 参数 | 默认值 | 作用与取值 |
| --- | --- | --- |
| `Enabled` | `true` | 控制日志和 32 位呈现启动。关闭全部矢量接管应使用 `Mode=off`，仅设为 `false` 不会关闭 RGB565 绘制。 |
| `Mode` | `observe` | `off` 使用原生文字；`observe` 仅观测；`draw` 矢量绘制。不区分大小写，未知值按 `observe` 处理。 |
| `LineRender` | `false` | 单行自然排版，统一计算字距、kerning 和标点间距。需 `Mode=draw`，且不能使用 `Metrics=vector` 或大于 1 的 `AdvanceScale`；关闭时逐字绘制。 |
| `MixedScriptCenter` | `true` | 自然单行中，中英文混排时按英文大写高度与中文字面居中英文整段。所有英文共用同一基线偏移，保留字体自身的上标和下伸笔画；纯英文和纯中文行不移动。 |
| `LegacyCodepage1252` | `true` | 将 `U+0080～U+009F` 中 27 个旧码位按 Windows-1252 映射为 Unicode 字形；`false` 严格按原始 Unicode 解释。不修改 CSF 或正确的 Unicode 字符。 |
| `Present32` | `true` | 检测 cnc-ddraw，在 BGRA8 呈现阶段合成文字，提高抗锯齿边缘的颜色精度。关闭、无 cnc-ddraw 或接入失败时使用 RGB565。 |
| `HiDPI` | `true` | 三后端均跟随实际 X/Y 放大倍率，按较大倍率向上取整生成独立 2～8× 字形。D3D9/OpenGL 在最终画面用 GPU 合成，GDI 在输出尺寸软件合成。4.8×3.6 开场使用直接栅格化的 5× 字形；两轴等于栅格密度时保留原采样，否则线性过滤。任一轴不放大、资源或驱动能力不足时保留逻辑分辨率合成；`false` 关闭高清文字层。 |
| `DynamicTextWidth` | `true` | 单行布局启用时，按自然文字宽度调整加载目标框、任务消息、tooltip 和侧栏状态背景；同时处理相应墨迹高度与内边距。关闭时保留原矩形度量。 |
| `LineWidthPadding` | `4` | 自然宽度测量的额外余量，限制为 1～32px。任务消息左右合计至少留 4px；tooltip 还会添加原生内边距。 |
| `Metrics` | `scaled` | 逐字路径的度量策略：`game` 使用原字宽；`scaled` 使用原字宽乘 `AdvanceScale`；`vector` 使用矢量字体字宽。后两者可能改写游戏字宽并改变布局；推荐 `game` 配合单行排版。 |
| `AdvanceScale` | `1.05` | 仅 `Metrics=scaled` 时生效的原字宽倍率，限制为 1.0～2.0。大于 1 时停用自然单行排版。 |
| `FitToAdvance` | `true` | 逐字路径按指定字宽限制字形，避免溢出旧字格；自然单行布局保留字体比例。`Metrics=vector` 不启用此限制。 |
| `FontFile` | `C:\Windows\Fonts\NotoSerifSC-VF.ttf` | 主字体文件路径，用于中文及相应标点；未指定独立英文字体时也用于英文。文件需在本机存在。 |
| `FontFileLatin` | 空 | 独立英文、数字及常用半角符号字体；为空或加载失败时使用主字体。 |
| `FontFileSymbol` | 空 | 独立 Unicode 符号字体，如 `C:\Windows\Fonts\seguisym.ttf`。优先用于闪电、矿镐、时钟、箭头、数学及装饰符号；ASCII 和中文标点保留原字体。为空、加载失败或缺字时沿用原字体及点阵回退。 |
| `FontSizeSymbol` | `13` | 独立符号字号，最小 6px；不跟随英文或中文字号。与文字共用基线，HiDPI 同样生成 2～8× 字形。 |
| `FontWeight` | `400` | 主字体 `FontFile` 的可变 `wght` 字重轴值；不影响独立英文或符号字体。大于零时限制在字体支持的轴范围内，非正值保留字体默认字重。固定字体字重由文件决定。 |
| `FontWeightLatin` | `400` | 独立英文字重，规则同 `FontWeight`；`FontFileLatin` 留空或加载失败时，也以此字重加载主字体供英文使用。 |
| `FontWeightSymbol` | `400` | 独立符号字重，规则同 `FontWeight`；仅用于配置成功且含对应字形的符号字体。 |
| `FontSizeLatin` | `13` | 英文字体字号，最小 6px。HiDPI 按独立的 2～8× 字形密度栅格化，逻辑字号不变。 |
| `FontSize` | `16` | 主字体 `FontFile` 的字号，当前用于中文及相应标点，最小 6px。与英文、符号独立设置，共享基线。 |
| `BaselineRow` | `13` | 相对游戏传入 Y 的共同基线位置，限制为 1～32；增大会整体下移文字，不会逐字按轮廓对齐。 |
| `Supersample` | `2` | 1～4 倍栅格化后降采样；`1` 直接在目标像素网格栅格化。较高值会改变 hinting 网格并增加开销，与 HiDPI 输出倍率是独立选项。 |
| `Hinting` | `0` | 灰度栅格对齐：`0` 轻量纵向对齐（LIGHT），`1` 完整对齐（NORMAL），`2` 不对齐；其他值回到 LIGHT。关闭抗锯齿时使用 MONO。 |
| `Subpixel` | `true` | 字形使用四分之一像素 X 相位；`false` 将可见原点取最近整数像素。自然单行布局的字距和测量仍保留小数。不是 RGB 子像素彩色抗锯齿。 |
| `AntiAlias` | `true` | 启用灰度抗锯齿；`false` 使用单色字形，灰度混合及描边选项不生效。 |
| `Gamma` | `1.0` | 调整灰度覆盖率，限制为 0.5～3.0；`1` 保持原覆盖率，大于 1 会加重边缘，小于 1 会减弱边缘。 |
| `StemDarkening` | `0` | `0` 关闭；正数尝试启用 FreeType 自动 hinting 的笔画加粗属性。当前不是可调加粗量，效果取决于字体及实际使用的驱动。 |
| `LinearBlend` | `true` | 在线性光空间混合文字和背景，避免直接在 sRGB 数值中混合造成边缘过暗；RGB565 与 BGRA8 路径均使用。 |
| `Dither` | `true` | RGB565 回写时对量化阈值加入轻微 Bayer 抖动，减轻色阶；BGRA8 最终文字合成不需要该抖动。 |
| `Outline` | `0` | 灰度文字描边，`0` 关闭，非零开启；值限制为 0～2。描边需 `AntiAlias=true`。 |
| `OutlineColor` | `0x0000` | 描边颜色，按 16 位 RGB565 色值解析；支持十六进制，`0x0000` 为黑色。仅开启描边时生效。 |
| `SubtitleOutline` | `true` | 过场字幕保留原色；亮字用约半像素黑边，暗字用白内缘和黑外缘各约四分之一像素（至少一个高清采样像素），避免白边过重。边缘颜色保持稳定，不随背景像素切换；RGB565 回退为 1px。 |
| `Detailed` | `true` | 收集独特字符串和详细调用信息；关闭时以计数统计为主，减少动态金额、计时文本持续建表和写盘的开销。 |
| `LogBitFontBlitDetails` | `false` | 逐字 Blit 详细日志，需同时启用 `Enabled` 和 `Detailed`；正常游戏建议关闭。 |
| `MaxUniqueStrings` | `4000` | 详细日志收集的独特字符串数量上限，最小 16；达到上限后继续累计调用统计，不再加入新字符串。 |
| `FlushIntervalMs` | `2000` | 调用过程中写入统计摘要的间隔，单位毫秒，范围 250～2147483647；负数截断为 250，格式错误或解析溢出回到 2000。不是每次字符绘制的写盘间隔。 |
| `LogFileName` | `VectorText.log` | 相对游戏目录的日志文件名，启动时重建日志。 |
| `Probe` | `true` | 对首次使用的 BitFont 对象记录布局探针，帮助检查字体、表面、pitch 和裁剪框。 |
| `PresentProfile` | `false` | 32 位呈现路径每 300 帧记录绘字、复制、上传和叠加的 CPU 耗时。诊断性能时临时开启；不是 GPU 完成时间或整局 FPS。 |

过长单行先收紧空白，再最多等比缩至 90%；仍无法容纳时整行回退，不横向强制拉伸。
`PresentTextScale`、旧模式 `aa/swap` 及无实际作用的 `FitMode`、`ClassAlign`、`FallbackOnError`、`MetricsExcept` 已移除，不应加入新配置。
配置边界、回退规则和 hook 约定见 [最终渲染说明](docs/rendering.md)。

## 验证与诊断

先运行 `build.bat`，再执行需要的检查：

| 入口 | 实际用途 |
| --- | --- |
| `line_test.bat` | 单行布局、混排、宽度、对齐、显现、线程隔离；真实游戏测量机器码及已安装 DLL hook；本机 Phobos 背景处理机器码的透明度与回退；无 Phobos/Ares 的图标计数器 |
| `baseline_test.bat` | 与 FreeType 独立固定基线位图比较，验证中英文、数字、符号、相位、缩放及裁剪 |
| `render_quality_test.bat` | 当前生产配置的黑底、纹理背景和加载界面 2× 字形预览，以及覆盖率、缓存、度量与字体数据检查 |
| `present32_test.bat` | 32 位覆盖率精度、稀疏文字层、裁剪、复制、拉伸及重复重绘 |
| `cnc_present_test.bat` | 三后端实际像素、复制、翻页、清除；2× 独立采样、线性/非线性混合、4.8×3.6 字幕、连续缩放和关闭 HiDPI；另含 D3D9 无边框/独占全屏及回退。`--parity-only` 专测三后端能力一致性。 |
| CTest `scaled_composition` | GDI 稀疏高密度文字与独立连续图像采样对照：2/5/8× 字形、裁剪、接缝、非等比输出和两种混合模式 |
| `takeover_test.bat` | 单行拒绝后的逐字接管、颜色、抗锯齿、裁剪和缺字回退 |
| `hooktest_draw.bat --mode draw` | 逐字 hook 的 ESP/EAX、返回跳板与拒绝路径；仅支持 `off`、`observe`、`draw` |
| `python tools\preview_guides.py` | 显示上、中、下辅助线与实际墨迹范围，完整行按最近邻放大 |
| `python tools\verify_dll.py [DLL] [游戏 EXE]` | DLL 导出、共同加载 hook、16 个运行时文字入口、字幕擦除机器码、重定位、Phobos/Ares 导入依赖和本机 Phobos hook 区间冲突检查 |

预览图输出到 `build/render-qa/`。离线通过仍需结合实际游戏画面验收。
当前 31 项离线 CTest 通过，包含 RA2/YR 原生入口、未知程序拒绝、首帧 HiDPI、字幕换色后的完整擦除、高清采样边缘、加载界面内存表面的文字复制、重复重绘和销毁复用，以及小数和非等比倍率采样对照。
本机三后端的精确 2×、4.8×3.6、连续窗口缩放、字幕密度切换、两种混合模式及关闭 HiDPI 检查均通过。
三后端共享字号、混排基线、字幕描边和独立高清采样；GDI 使用 CPU 合成，性能不与 GPU 后端等同，驱动的颜色舍入也可能有细小差异。其他机器、驱动和 cnc-ddraw 版本仍需验证。
游戏目录的 `VectorText.log` 记录 `LINE ready`、`LINE fallback`、`LINE native icon`
和 `DYNAMIC width`，可确认实际调用是否命中。
加载界面的原生 `BSurface` 内存目标也接入高精度文字层，复制到显示表面时保留独立的高密度采样。
`output text scale=2x2` 只说明呈现器启用了 2×，不能证明所有绘字目标均已接管；
`CPU text surface registered ... hi-raster=1` 可确认加载内存表面已登记。最终效果仍需重启游戏后检查。
原版 800×600 拉伸到 3840×2160 时，日志应显示 `output text scale=4.8x3.6`；
`SUBTITLE erase` 记录原擦除框、实际笔画和扩大的擦除框。开场图片、Logo 和图片内的预制文字仍使用原素材。

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
