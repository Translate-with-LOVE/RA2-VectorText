# VectorText — M0 观测原型

一个可被 **Syringe** 加载的 32 位钩子 DLL，用来观测 `gamemd.exe` 的文字绘制/度量管线，
为实现"现代矢量文字渲染"做前置调研（见游戏根目录 `game.fnt-加载机制研究.md` 附录 B）。

> **M0 只观测，不改变任何绘制行为。** 六个钩子全部 `return 0`，Syringe 会执行被覆盖的
> 原始指令再跳回去，游戏表现与没装这个 DLL 完全一致。任何字符串/参数读取都有
> `VirtualQuery` + SEH 双重保护，参数不是字符串时只会少记一条，不会崩。

---

## 1. 它产出什么

运行一局游戏后，游戏根目录的 `VectorText.log` 会给出：

| 内容 | 用途 |
|---|---|
| `NEW` 行：每个**首次出现**的字符串 + 调用者地址 + 原始栈参数 | 看清"哪些 UI 文本走哪个函数"，以及该函数的真实参数布局 |
| `STATS` 行：每 2 秒一次的六个钩子调用计数 | 判断哪个上下文量大、哪个钩子才是瓶颈 |
| `FINAL` / `UNIQ` 行：退出时按次数排序的去重字符串表 | 直接得到"矢量渲染器要覆盖的文本清单" |

`LogBitFontBlitDetails=1` 时还会记录**实际被绘制的字符**（逐字形），可用于统计字库真实使用率。

---

## 2. 目录结构

```
F:\Mental Omega\VectorText\
├─ build.bat              # 一键构建（自动查找 vcvars32）
├─ selftest.bat           # 离线自检：加载 DLL 并调用全部钩子，不启动游戏
├─ run_game.bat           # 用 Syringe 直接启动游戏（等价于 MO 启动器的启动方式）
├─ ft_smoke.bat           # 构建并运行 FreeType 冒烟测试（32 位静态链接）
├─ glyph_test.bat         # 字形对齐测试：game.fnt 原字形 vs 我们的矢量子形
├─ VectorText.ini         # 配置（build 时复制到游戏根目录）
├─ include\
│   ├─ SyringeABI.h       # Syringe 钩子 ABI（REGISTERS / declhook / DEFINE_HOOK / 握手结构）
│   └─ YRAddresses.h      # gamemd.exe 1.001 地址 + 每个钩子的序言字节依据
├─ src\
│   ├─ Logger.h / .cpp    # 统计与日志（去重表、周期 flush、退出汇总、可执行文件校验）
│   ├─ Hooks.cpp          # 六个钩子实现
│   └─ DllMain.cpp        # 入口 + SyringeHandshake
├─ docs\
│   └─ M1-design.md       # M1 设计文档：接管点、跳过函数做法、像素写入方案、度量对齐、验证与风险
├─ third_party\
│   ├─ build_freetype.bat # 静态构建 32 位 FreeType -> build\freetype\freetype.lib
│   ├─ ftmodule.min.h     # 最小模块表（只编 TrueType + autofit + mono/smooth 光栅化器）
│   └─ freetype\          # git submodule: github.com/freetype/freetype @ VER-2-14-3
└─ tools\
    ├─ find_vcvars.bat    # 工具链定位（各脚本共用）
    ├─ selftest.cpp       # 自检程序：合成 REGISTERS 直接调用钩子处理函数
    ├─ ft_smoke.cpp       # FreeType 冒烟测试：字形点阵、覆盖率、度量、可变字重
    ├─ verify_dll.py      # 不启动游戏就能校验 DLL 的钩子表是否正确
    ├─ analyze_log.py     # 把 VectorText.log 变成覆盖率/调用点/签名报告
    └─ font_preview.py    # game.fnt 与矢量字体的对照图 / 宽度量化对比
```

## 3b. FreeType 子模块（M1 用）

```bat
git submodule update --init --recursive          :: 拉取 third_party/freetype (VER-2-14-3)
F:\Mental Omega\VectorText\third_party\build_freetype.bat   :: -> build\freetype\freetype.lib
F:\Mental Omega\VectorText\ft_smoke.bat --size 13 --wght 400 :: 冒烟测试
```

* 采用 FreeType 的“单目标文件”构建：只编 `TrueType + sfnt + autofit + psnames + smooth + raster1`
  （模块表在 `third_party/ftmodule.min.h`，通过 `-DFT_CONFIG_MODULES_H` 指定），产物约 690 KB，静态链接、无运行时依赖。
* 已实测：FreeType **2.14.3**，32 位，Noto Serif SC 可渲染中英文；`U+4E2D` 在 13px 下
  advance=13、点阵 11×12 —— 与原版 `game.fnt` 的 CJK 步进（13–15）吻合，确认 13px 是合适的字号。
* **可变字体陷阱**：`NotoSerifSC-VF.ttf` 的默认实例是 **ExtraLight (wght=200)**，直接渲染会细得没法看；
  必须 `FT_Set_Var_Design_Coordinates(wght=400)`（或选名为 Regular 的 named instance）。
* 本机 Git 默认走 schannel，在受限 shell 里会报 `SEC_E_NO_CREDENTIALS`；
  用 `git -c http.sslBackend=openssl ...` 即可（仓库本地已写入该配置）。
* `git submodule` 子命令会调用 MSYS 的 `sh.exe`，在受限 shell 中会失败；clone/commit 本身正常。

构建产物直接落在游戏根目录：`VectorText.dll`、`VectorText.ini`（Syringe 在工作目录里扫描 DLL）。

> **关于日志何时产生**：`SyringeHandshake` 运行在 **Syringe 自己的进程**里（Syringe 会
> LoadLibrary 这个 DLL 来握手），而钩子处理函数运行在 **gamemd.exe** 里。所以日志**不在**握手时创建，
> 而是由游戏进程在**第一次文字调用**时惰性打开——这样日志里的数据一定来自游戏本体。
> `selftest.bat` 就是用来在不启动游戏的情况下验证这条路径的。

---

## 3. 构建

```bat
F:\Mental Omega\VectorText\build.bat
```

* 需要任意一套含 x86 工具的 MSVC（本机已验证：Visual Studio 18 BuildTools / MSVC 14.50，
  `vcvars32.bat` 自动定位）。找不到时可显式指定：
  `set VT_VCVARS=C:\...\VC\Auxiliary\Build\vcvars32.bat`
* 编译选项：`/LD /MT /O2 /std:c++17`（静态 CRT，无运行时依赖），机器码 x86。
* 唯一的警告 `C4324: hookdecl 由于对齐说明符被填充` 是**预期的**——`.syhks00` 记录必须是
  16 字节对齐（Syringe ABI 如此，Phobos.dll 同样是每 16 字节一条）。

## 4. 自检与校验（都不启动游戏）

**a) DLL 结构校验**（钩子表、导出名、重定位）：

```bat
python F:\Mental Omega\VectorText\tools\verify_dll.py
```

校验：PE 是否 32 位 DLL → 钩子处理函数是否以**未修饰名字**导出 → `.syhks00` 是否存在且
记录格式正确 → 每条记录的 `hookName` 指针能否解析到某个导出函数（Syringe 就是靠这个
把钩子绑定到处理函数的）→ 该指针字段是否有基址重定位（决定 ASLR 下是否安全）。

**b) 运行时自检**（加载 DLL、用合成寄存器调用全部钩子处理函数、检查日志与返回值）：

```bat
F:\Mental Omega\VectorText\selftest.bat
```

它会打印每个钩子的返回值（必须都是 `0` = 透传），并在 `VectorText\build\VectorText.log`
生成一份真实日志（含去重、UTF-8 中文、退出汇总），用来证明日志链路本身是通的。
当前实测：`failures=0`，六个钩子计数 5/5/5/5/5/25。

**c) 构建产物校验输出**（实测）：

```
machine   : 0x014C x86 (ok)
exports   : 7   (SyringeHandshake + 6 个 VT_Hook_*)
.syhks00  : 6 条记录，export=True reloc=True
result    : OK -- ready for Syringe
```

## 5. 安装 / 卸载 / 确认已加载

* **安装**：`build.bat` 已经把 DLL 放到游戏根目录；下次通过 Syringe 启动游戏即生效
  （正常用 MO 启动器启动即可，不需要改任何配置）。
* **确认**：看游戏根目录的 `syringe.log`，应出现

  ```
  SyringeDebugger::FindDLLs: Recognized DLL: "VectorText.dll"
  SyringeDebugger::Handshake: Answers "VectorText M0: observing text hooks (no drawing changes)." (0)
  ```

* **卸载**：把 `VectorText.dll` 改名或删除即可（`VectorText.ini` / `VectorText.log` 可留可删）。

## 6. 运行观察
```bat
F:\Mental Omega\VectorText\run_game.bat
```

或直接用 MO 启动器进游戏，玩一会儿（主菜单 → 遭遇战 → 建造/选中单位/看 tooltip/发消息），
正常退出后打开 `VectorText.log`：

```
[    1234 ms] NEW   Drawing::PrintUnicode(0x4A61C0)   caller=0x004B9874 text="Options"  text@esp+0x8 esp+0x4=0x... 
[    2000 ms] STATS Drawing::GetTextDimensions(0x4A59E0)=812  Drawing::PrintUnicode(0x4A61C0)=1523  ...
```

### 日志分析

```bat
python F:\Mental Omega\VectorText\tools\analyze_log.py --md F:\Mental Omega\VectorText\VectorText-analysis.md
```

产出：每个钩子的调用次数 / 去重字符串数 / **调用点（UI 上下文）清单**及样本文本、
字符串参数的**真实栈偏移**、无字符串调用的证据（`MISS`，用于敲定签名）、按绘制次数排序的字符串表。
首轮真实运行的结果见 [VectorText-analysis.md](VectorText-analysis.md)。

---

## 7. 钩子清单（地址来自对本地 gamemd.exe 的反汇编）

| 钩子名 | 地址 | 覆盖字节 | 约定 | 参数（两轮实测已确认） |
|---|---|---|---|---|
| `VT_Hook_Drawing_GetTextDimensions` | `0x4A59E0` | 10 | `__fastcall` | `ECX`=out rect, `EDX`=文本；工具提示/文本框换行都走这里 |
| `VT_Hook_Drawing_PrintUnicode` | `0x4A61C0` | 10 | `__thiscall` | `ECX`=Surface*, **`esp+8`=文本**（133/133 命中）；`esp+4`=栈上矩形/缓冲, `esp+0xC`=Surface 再出现一次 |
| `VT_Hook_BitFont_GetTextDimension` | `0x433CF0` | 6 | `__thiscall` | `ECX`=BitFont*, **`esp+4`=文本**（2017/2017）, `+8`=int* w, `+C`=int* h, `+10`=maxW；**度量对齐的关键** |
| `VT_Hook_BitText_Print` | `0x434B90` | 5 | `__thiscall`（ECX 未使用） | `esp+4`=BitFont*, `+8`=Surface*, **`+C`=文本**（1821/1821）, `+10`=X, `+14`=Y, `+18`=W, `+1C`=H |
| `VT_Hook_BitText_DrawText` | `0x434CD0` | 5 | `__thiscall` | 同 Print，**`esp+0xC`=文本**（14/14）+ 颜色/对齐参数（共 10 个栈参数） |
| `VT_Hook_BitFont_Blit` | `0x434120` | 5 | `__thiscall` | `ECX`=BitFont*, `esp+4`=字符, `+8`=X, `+C`=Y, `+10`=颜色；**逐字形热路径**，默认只计数 |

### 实测的调用点、目标 surface 与文本规模（一轮约 5 分钟的对局）

| 钩子 / 调用点 | 次数 | 目标 surface | 用途（样本文本） |
|---|---|---|---|
| `GetTextDimension` `0x4A5EF1` | 1659 | – | 任务简报正文/标题/评分（`军事行动： 风暴使者 - 地点： 维尔京群岛`） |
| `GetTextDimension` `0x4346A2` | 180 | – | 对白/字幕包装（`距离特内里费岛战役发生的几周后 -`） |
| `GetTextDimension` `0x6D4C75` + `PrintUnicode` `0x6D4D9F` | 131+131 | – | 游戏时钟（`10:29`） |
| `GetTextDimension` `0x433EE6` | 32 | – | 提示/帮助文本 + 字宽探针（`WWWWWWWWWWWWWWW`、`提示 - 猎杀无人机…`） |
| `GetTextDimension` `0x478F0B` / `0x478C3B` + `DrawText` `0x479041` | 10/4 + 13 | `0x0E906240`, `0x0E9062F0` | 单位/建筑属性提示（`发电厂升级：动力涡轮 / 电力+150`） |
| `BitText::Print` `0x4A5FF2` | 1805 | `0x0E906240`(1670), `0x0E9063A0`(77), `0x0E9062F0`(57) | 简报/字幕主绘制路径，BitFont 恒为 `0x0E902E10` |
| `BitText::Print` `0x621389` + `DrawText` `0x621144` | 16 + 1 | `0x0E9063A0`, `0x24A95EB0` | 战场消息框、任务目标面板 |
| `GetTextDimensions` `0x6A9D11` / `0x6A9DD1` | 11 / 2 | – | 速度/状态标签（`10/15/20`、`等待/就绪`） |
| `GetTextDimensions` `0x7BF7475B` / `0x7BDE60C0` | 1 / 1 | – | **外部 DLL（Phobos 等）也在调用同一批 API** |

密集度最高的字符串是 Phobos 的调试横幅（单轮 9392+4696+4696 次）、倒计时标签 `距离敌军到达时间：`（4319×3）、
帮助文本 `选取部队横越画面/地图` —— 这些是最需要控制每帧成本的对象。`BitFont::Blit` 一轮 **774,555** 次，
是所有点阵字形的最终出口，也是抑制/接管的最佳兜底点。

被覆盖的字节数都落在完整指令边界上（`YRAddresses.h` 里逐条记录了序言字节），
否则 Syringe 回放原始指令时会从指令中间继续执行。

### 与 Ares / Phobos 的共存

已逐条核对：Ares 的 1488 条 hook、Phobos 的 1314 条 hook 与上述 6 个地址**零重叠**
（Ares/Phobos 在字体相关区间只挂了 3 个 `BuildingLightClass_*` 探照灯 hook，位于 `0x435820` 之后，
在我们的代码之外）。DLL 由 Syringe 在它们之后加载，互不影响。

---

## 8. 已知限制

* `Drawing::PrintUnicode` / `BitText::DrawText` 的参数布局没有权威文档，M0 采用
  "扫描前 N 个栈参数，找第一个指向可打印宽字符串的值"的启发式，并把它记为
  `text@esp+0xNN`。**日志本身就是用来确认真实签名的证据**；确认真实签名后再改成精确读取。
* `BitFont::Blit` 每个字形调用一次，默认只计数；开启明细后日志会明显变大（去重按字符，上限受
  `MaxUniqueStrings` 约束）。
* 日志中的"字符串"只覆盖 16 位码元（BMP）；游戏文本本身就是 UTF-16 码元流。
* 只观测**绘制**调用，不包含 CSF 字符串加载、Win32 对话框（`CreateFontIndirectA` 那条 GDI 路径）
  以及得分屏的 SHP 字体。

## 9. 排错

| 现象 | 原因 / 处理 |
|---|---|
| `syringe.log` 里没有 `Recognized DLL: "VectorText.dll"` | DLL 不在游戏根目录（Syringe 在**工作目录**里扫描），或被安全软件拦截 |
| 有 `Recognized`，握手语也打印了，但 `VectorText.log` 不生成 | **已修复的坑**：日志原先在 `SyringeHandshake` 里创建，而握手运行在 *Syringe 的进程*中——游戏进程里的 DLL 实例从未初始化日志。现在日志由游戏进程在第一次文字调用时惰性打开；请确认用的是最新构建（`build.bat` 重新构建，或 `selftest.bat` 通过） |
| 日志只有表头，没有 `NEW`/`STATS` | 钩子没被调用：先用 `verify_dll.py` 确认钩子表；再看日志里的 `EXE` 行——如果 `crc32` 不是 `0x1B499086`，说明游戏不是 1.001 UC 版，`YRAddresses.h` 的地址不适用（日志会打 `WARN`） |
| 想让 DLL 彻底不工作 | `VectorText.ini` 里 `Enabled=0`，或把 `VectorText.dll` 改名/删除 |
| 游戏异常退出 | 把 `VectorText.dll` 改名后重试；同时保留 `VectorText.log` 与 `syringe.log` 以便定位（钩子全部 `return 0`，理论上不改变执行流） |

## 10. 下一步（M1）

1. 跑一局，把这轮日志按 `caller` 地址聚类 → 得到"UI 上下文清单"（哪些上下文直接画到主画面、
   哪些先画到离屏 surface）。
2. 用反汇编把 `PrintUnicode` / `DrawText` 的真实签名定死，替换启发式。
3. M1：FreeType 渲染但**先画回游戏 8bpp 表面**（灰度 → 阈值 1bpp，暂不开 AA），
   同时接管 `0x433CF0` 的度量，验证位置/颜色/换行与原版一致——这一步会暴露绝大多数坑。
4. M2：把合成搬到 32 位、缩放后的呈现层（cnc-ddraw 内或独立覆盖层），开 AA 与亚像素定位。
