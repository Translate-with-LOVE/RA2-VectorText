# VectorText M1 设计文档：用矢量光栅化接管点阵文字

> 前置：M0 已完成（观测钩子 + 两轮真实运行），结论见 [../VectorText-analysis.md](../VectorText-analysis.md)
> 与 [../README.md](../README.md)。本文只描述 M1 做什么、怎么做、怎么证明做对了。
>
> **已定决策（本轮）**：像素写入走 **方案 B（自己写像素）**；矢量字体用 **Noto Serif**。

---

## 0. 范围

**M1 = 在游戏自己的 8bpp 表面上，用我们自己光栅化并自己写入的像素替换 `game.fnt` 的点阵字形，
且坐标、颜色、度量、换行与原版一致。**

M1 **不做**：抗锯齿（8bpp 调色板装不下）、亚像素定位、32 位合成、多字号、CJK 断行优化。
这些是 M2+（§11）。但选方案 B 的收益在这里：**像素写入器在 M1 就变成我们的**，M2 只需要换目标缓冲
（32 位）与混合函数，不再需要动接管与排版逻辑。

---

## 1. 目标与验收标准

| # | 验收项 | 判定方式 |
|---|---|---|
| A1 | 所有经 `BitText::Print`(0x434B90) 与 `BitText::DrawText`(0x434CD0) 的文字由我们绘制 | 日志：接管计数 ≈ M0 基线 42209 / 688，逐条可查 |
| A2 | 文字位置逐像素一致 | 固定流程截图逐像素比对，差异只允许出现在字形墨迹内部 |
| A3 | 颜色/阴影与原版一致 | 同上；重点 `TextPrintType` 的阴影/描边/渐变文字 |
| A4 | 度量一致 | `Metrics=game` 下 `BitFont::GetTextDimension`(0x433CF0) 返回值与 M0 完全一致，换行不变 |
| A5 | 无遗漏 | 任何绕过接管点的点阵绘制都写 `LEAK` 日志；目标 0 条 |
| A6 | 可一键回退 | `Mode=observe` 回到 M0 行为；`Mode=off` 零开销 |
| A7 | 外部调用者不受影响 | Phobos 等 DLL 的文本调用（M0 捕获到 `0x7BF7475B`、`0x7BDE60C0`）仍正常 |

---

## 2. 现状：M0 已确认的事实

### 2.1 绘制链与调用规模（一局 ≈ 5 分钟）

```
调用者（简报/字幕/提示/消息/时钟/属性面板…）
  ├─ Drawing::GetTextDimensions (0x4A59E0, ECX=out rect, EDX=文本)      6593
  ├─ Drawing::PrintUnicode      (0x4A61C0, ECX=Surface*, esp+8=文本)   8680  → 内部走 BitText::Print
  ├─ BitText::Print   (0x434B90)  42209   ret 0x1C（7 个栈参数）
  │     arg1=BitFont*(0x0E902E10) arg2=Surface* arg3=文本(esp+0xC) arg4=X arg5=Y arg6=W arg7=H
  ├─ BitText::DrawText(0x434CD0)    688   ret 0x28（10 个栈参数，arg3=文本）
  └─ BitFont::Blit    (0x434120) 774555   逐字形写像素（M1 不再调用它，只用于漏检）
  BitFont::GetTextDimension (0x433CF0, esp+4=文本)                     67894
```

### 2.2 已见 surface

| surface | 出现位置 | 推断 |
|---|---|---|
| `0x0E906240` | Print 0x4A5FF2(1670)、DrawText 0x479041 | 主战场/简报 |
| `0x0E9062F0` | Print 0x4A5FF2(57)/0x621389(8)、DrawText 0x479041(1) | 侧栏/提示面板 |
| `0x0E9063A0` | Print 0x4A5FF2(77)/0x621389(8) | 消息框 |
| `0x24A95EB0` | DrawText 0x621144 | 目标面板（另一族地址） |
| `0x0E902E10` | 所有 Print/DrawText 的 arg1 | BitFont 对象（`[0x89C4D0]` 指向它） |

### 2.3 选方案 B 后新增的必查项

| 编号 | 待确认 | 状态 |
|---|---|---|
| Q1 | surface 像素格式：位深、pitch、锁定缓冲 | **✅ 已解决**：文字表面是 **16 位色**，颜色字直接写入；缓冲=`BitFont+0x0C`、pitch(像素)=`BitFont+0x10`，`dst = base + (pitch·y + x)·2`（两条独立路径互证）。详见 [text-render-internals.md](text-render-internals.md) §3 |
| Q2 | 颜色来源与语义 | **✅ 已解决**：`Blit` 的 arg4（`-1` 表示用 `BitFont+0x24`）；`DrawString` 在 `reveal∈[1..8]` 时逐字符向白混合 `ratio=((9-reveal)*31+31i)&0xFF`。详见 §4 |
| Q3 | 阴影/描边的实现 | **⏳ 部分**：`Blit` 只写一种颜色、不读 `+0x26` → 阴影应是调用者再画一遍或 `DrawText` 内部完成 |
| Q4 | 排版规则（推进/换行/居中/裁剪） | **✅ 主体已解决**：`DrawString(str,X,Y,maxChars,reveal)`，advance=字形宽度字节；`\r\n` 只跳过、不换行（换行由调用者或 `DrawText` 负责）；返回结束 X。`DrawText` 的 10 参数细节待补 |
| Q5 | 旁路路径（`0x434E00–0x435000` 调用 `GetCharacterBitmap`） | **⏳ 待办**：影响 A5 漏检清单 |

---

## 3. 接管策略

### 3.1 主接管点：字符串级 + 跳过原实现

在 `BitText::Print` / `BitText::DrawText` 入口跳过原实现，由我们完成：
取参数 → 排版（Q4 规则）→ 光栅化 → 写像素（§4）。

**跳过被调函数的确切做法（Syringe）：**

```
retAddr = *(DWORD*)R->ESP();          // 返回地址
R->ESP(R->ESP() + 4 + argBytes);      // 弹返回地址 + 弹全部参数
R->EAX(原函数应有的返回值);             // Print/DrawText 的返回值语义先测后定
return retAddr;                        // 回到调用者下一条指令
```

| 函数 | 栈参数 | argBytes | 依据 |
|---|---|---|---|
| `BitText::Print` | 7 | `0x1C` | 实测 `ret 0x1C` |
| `BitText::DrawText` | 10 | `0x28` | 实测 `ret 0x28` |

先以 `Mode=observe`（`return 0`，原版照画）跑通参数读取，再切 `Mode=draw`。

### 3.2 兜底：`BitFont::Blit` 只做漏检

M1 不调用 `Blit`，但继续挂钩统计：若在"我们没有接管"的情况下仍有点阵绘制发生 → 写 `LEAK`。
清单归零后再考虑把 `Blit` 改成空操作（可选，仍留 INI 开关）。

### 3.3 外部调用者

Phobos 等 DLL 走的是 `GetTextDimensions` / `PrintUnicode` 等同一批 API；我们只替换"绘制"，
不改 API 契约，度量照常返回正确值（A7）。

---

## 4. 像素写入（方案 B，已选）

### 4.1 组件

```
Rasterizer   : Noto Serif (SC) → 每个码位的覆盖率位图（8bit 灰度，FreeType 输出）
GlyphCache   : (字体, 字号, 码位) → 定长字形块 { width, 灰度位图 }；热路径零分配
Layout       : 按 Q4 复刻的推进/换行/居中/裁剪
PixelWriter  : 目标 surface 的锁定缓冲 + pitch + 位深 → 写像素（含阴影遍）
```

### 4.2 位深与颜色

| 情况 | 写入方式 |
|---|---|
| 8bpp 调色板表面（预期是绝大多数） | 直接写调色板索引字节（原版 `Blit` 收的就是 WORD 颜色值，M1 先按原值写） |
| 16bpp 表面（若有） | 需要时用 `ConvertClass`/RGB565 转换；M1 先探测、遇到即 `FALLBACK` 并记录 |
| M1 的覆盖 → 像素 | 8bpp 无法表达中间灰阶：M1 用**阈值/覆盖度分级**（例如覆盖率 ≥50% 写前景色），
只有在 M2 的 32 位层才做真正的 alpha 混合 |

阴影/描边：按 Q3 复刻成"同一掩码 + 偏移 + 另一种颜色"的第二遍写入。

### 4.3 为什么这条路对 M2 友好

M2 只需要：把 `PixelWriter` 的目标从 8bpp surface 换成 32 位呈现缓冲，
把"阈值写索引"换成"按覆盖率做 RGBA 混合"，其余（接管点、排版、缓存）不动。

---

## 5. 度量对齐

`BitFont::GetTextDimension`(0x433CF0) 一局 67894 次，是所有排版的唯一依据。两种模式：

| 模式 | 宽度来源 | 用途 |
|---|---|---|
| `Metrics=game`（M1 默认） | `game.fnt` 每个码位的宽度字节 | 布局与原版逐像素一致 → 截图差异只可能来自字形本身 |
| `Metrics=freetype` | Noto Serif 的 advance（可含 kerning），并让 `GetTextDimension` 返回同一结果 | 真正的矢量排版，M1 后期作为开关验证 |

### 5.1 实测：矢量字体的宽度代价（`tools/font_preview.py --measure`）

以游戏真实字符串为样本，把 Noto Serif SC 的总 advance 与原版点阵相比（1.00 = 与原版等宽）：

| 字号 | 12 | 13 | 14 | 15 | 16 | 17 |
|---|---|---|---|---|---|---|
| 全部样本总宽比 | **0.97** | 1.06 | 1.14 | 1.22 | 1.30 | 1.38 |
| CJK 为主的字符串 | 0.92 | **1.00** | 1.07 | 1.15 | 1.22 | 1.30 |
| 拉丁为主的字符串 | 1.25 | 1.35 | 1.46 | 1.56 | 1.66 | 1.77 |

结论：
* **只要走矢量 advance，UI 的固定框就会溢出 30%–80%**（拉丁尤其严重）——因为原版 ASCII 极窄（`A` 的 advance 只有 7px）；
* 因此 M1 必须用 `Metrics=game`：**逐字符沿用原版 advance，把矢量字形压缩进原格**（13–14px 时拉丁只需 ~20% 横向压缩）；
* 真·矢量排版（`Metrics=freetype`）需要逐个界面审计框宽，属于 M3，而不是 M1。
* 原版的墨迹高度其实只有约 9–11px（16 行的字符格内留白很大），所以 13–14px 的 Noto **墨迹高度与原版相当**，
  不会出现"字号突然变大"的观感问题。

---

## 6. 光栅化器与字体

### 6.1 本机现状（已确认）

| 资源 | 位置 / 状态 |
|---|---|
| `NotoSerifSC-VF.ttf` | `C:\Windows\Fonts\`，24 MB，**简体中文可变字体**（默认实例即 Regular） |
| `NotoSerif-Regular.ttf` | `C:\Windows\Fonts\`，0.7 MB，仅拉丁 |
| Pillow | 已安装（**自带 FreeType**）→ 可离线做光栅化对照实验，不必先编译任何东西 |
| fontTools | 未安装（M3 做字形子集化时再装） |

### 6.2 游戏内光栅化器：FreeType 静态编进 DLL（**已构建并验证**）

* 来源：`third_party/freetype` git submodule，pin 在 **VER-2-14-3**（commit `0a0221a`）。
* 构建：`third_party/build_freetype.bat` → `build/freetype/freetype.lib`（约 690 KB）。
  采用 FreeType 的“单目标文件”构建，模块表 `third_party/ftmodule.min.h` 只注册
  `autofit / tt / sfnt / psnames / smooth / raster1`（不含 CFF/Type1/SDF/SVG），
  通过 `-DFT_CONFIG_MODULES_H="ftmodule.min.h"` 生效。
* 验证：`ft_smoke.bat --size 13 --wght 400` 已跑通 —— FreeType 2.14.3（32 位静态），
  Noto Serif SC 31058 字形，mono(1bpp) 与 coverage(8bpp) 两种渲染都正常。
* **可变字体**：该字体默认实例是 ExtraLight(`wght=200`)，M1 必须显式设置 `wght=400`
  （`FT_Set_Var_Design_Coordinates`），否则笔画过细。
* 13px 实测度量：`U+4E2D` advance=13、mono 点阵 11×12、`U+0041` advance=9 ——
  与原版 `game.fnt`（CJK 步进 13–15、墨迹约 9–11 行）吻合，**字号取 13 是合适的**。

### 6.4 字形对齐实测（`glyph_test.bat`，离线，不需要启动游戏）

`src/GlyphSource.*` 把 FreeType 位图组织成**引擎自己的字形格**（首字节=advance，其后
`stride×lines` 字节 1bpp、MSB 在前、1=墨），因此可以直接和 `game.fnt` 的原字形逐像素对比：

| 字符 | game.fnt 行/列 | 我们（拉丁13 / 中文16，基线13） |
|---|---|---|
| `A` | 4..12 / 0..6 | 3..12 / 0..6（大写高 1 行） |
| `a` | 6..12 / 0..4 | **6..12 / 0..4（完全一致）** |
| `g` | 6..15 / 0..5 | **6..15 / 0..5（完全一致）** |
| `中` | 0..15 / 0..11 | 0..13 / 0..11 |
| `文` | 0..14 / 0..14 | 0..14 / 1..14 |
| `电` `力` | 0..14、0..15 / 0..12 | 0..13 / 0..12 |

结论：
* **advance 全部与原版逐像素一致**（Metrics=game 用原版宽度字节），列宽都在步进内 → 相邻字形不会重叠；
* 小写/数字基线与降部完全吻合；大写与 CJK 各差 1–2 行（观感可接受，M1.4 可继续微调）；
* 两个坑已修：① 最近列采样压缩会**整根丢掉一像素宽的竖笔**（`中` 的竖笔），改成区间 OR 合并；
  ② 该字体可变实例默认 ExtraLight，必须设 `wght=400`。
* 备选（若想先零依赖验证）：GDI `GetGlyphOutlineW`，接口相同（都产出覆盖率位图），切换成本低。

### 6.3 字体打包

* M1 可直接用系统字体（不随包分发，先验证管线）；
* 正式分发需要把字体放进 Mod 包：Noto 系列为 **SIL OFL 1.1**，可再分发（需附许可证与保留名称声明）；
  24 MB 的 VF 在 M3 做子集化（只保留游戏实际用到的码位，预计可降到 1–3 MB）。

---

## 7. 配置与安全开关

```ini
[VectorText]
Mode=observe            ; off | observe（M0 行为）| draw（M1 接管）
Metrics=game            ; game | freetype
FontFile=C:\Windows\Fonts\NotoSerifSC-VF.ttf
FontWeight=400          ; 该可变字体默认实例是 ExtraLight(200)，必须显式指定
FontSizeLatin=13        ; 实测：与原版拉丁/数字/小写墨迹行高一致（见 §6.4）
FontSizeCJK=16          ; 实测：原版中日韩字形占满 16 行格，13px 会明显偏小
BaselineRow=13          ; 格子内的基线行（16 行格）
FitToAdvance=1          ; 超出原版步进的字形横向压缩（区间 OR 合并，不丢细笔画）
PixelMode=mono          ; mono(=M1, 1bpp) | gray(=M2, 抗锯齿)
CoverageThreshold=128   ; gray 模式下的覆盖率阈值
SkipPrint=1
SkipDrawText=1
BackstopBlit=1          ; 只做漏检
LeakDetect=1
FallbackOnError=1       ; 任何异常/未知格式 → 回退原版并记 FALLBACK
```

**失败即回退**：任何一步失败（surface 未上锁、位深未知、字体缺失、参数越界、异常）都 `return 0`
让原版继续，并写 `FALLBACK`。绝不出现静默的文字消失。

---

## 8. 验证与回归

| 层级 | 方法 |
|---|---|
| 离线对照 | Pillow(FreeType) 生成同一码位/字号的掩码，与 DLL 内 FreeType 输出比对（保证两边一致） |
| 字形尺寸 | `Metrics=game` 时我们生成的 width 必须等于 `game.fnt` 的 width 字节 |
| 覆盖率 | 固定流程跑一局，`LEAK`/`FALLBACK` 计数目标 0；接管计数与 M0 基线对照 |
| 视觉 | 同存档同界面截图 + Python 逐像素 diff（差异只允许在墨迹内） |
| 度量 | 日志对比 `GetTextDimension` 返回值（`Metrics=game` 下应完全一致） |

---

## 9. 任务拆解

| 阶段 | 内容 | 产出 | 预估 |
|---|---|---|---|
| M1.0 | surface 探测（Lock/UnLock + 字段快照），回答 Q1 | surface 清单与像素格式表 | ½ 天 |
| M1.1 | 反汇编绘制核心 0x434500 + `Blit`，回答 Q2/Q3/Q4/Q5 | **✅ 已完成（Q3/Q5 部分）** → [text-render-internals.md](text-render-internals.md) | 1–1.5 天 |
| M1.2 | FreeType 接入（源码 + 单目标文件构建）+ 字形缓存 + Noto Serif SC | `src/Rasterizer.*`、`src/GlyphCache.*`、`third_party/freetype` | 1.5–2 天 |
| M1.3 | 接管与写入：跳过 Print/DrawText、复刻排版、PixelWriter（含阴影遍）、FALLBACK/LEAK | `src/Takeover.*`、`src/PixelWriter.*` | 2 天 |
| M1.4 | 验证：离线对照、截图 diff、覆盖率与度量对照；修 Q5 旁路 | `tools/screenshot_diff.py`、M1 报告 | 1 天 |

合计约 **6–8 个工作日**（方案 B 比方案 A 多约 1–2 天，换来 M2 的平滑过渡）。

---

## 10. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| 颜色/阴影语义未完全复刻（方案 B 的主要风险） | 文字观感不一致 | M1.1 先把规则查清；每个调用点单独对照；未知标志 → FALLBACK |
| surface 位深/格式假设错误 | 花屏或崩溃 | M1.0 探测 + 未知格式一律回退；写入前做边界检查 |
| 跳过函数栈清理错误 | 立即崩溃 | 先 observe 后 draw；`selftest` 覆盖两种模式 |
| `W/H` 居中/裁剪规则复刻不全 | 局部文字位置偏移 | 以简报屏/属性面板/消息框三类界面做逐像素对照 |
| 可变字体（VF）在 FreeType 下的实例选择 | 字重不对 | **已确认并解决**：默认是 ExtraLight(200)，M1 显式设 `wght=400`；`ft_smoke.bat` 可复核 |
| 24 MB 字体 + 每帧光栅化 | 启动慢/掉帧 | 字形缓存 + 只缓存用到的码位；M3 子集化 |
| 旁路文本（Q5、MSOFont/得分屏 SHP 字体） | 部分文字仍点阵 | M1 不接管 SHP 字体；用 `LEAK` 输出清单，按需追加接管点 |

---

## 11. 不在 M1 范围（M2+）

* **M2**：把 PixelWriter 的目标换成 32 位呈现缓冲（cnc-ddraw 内或独立覆盖层），FreeType 灰度 +
  亚像素定位 + SDF 描边；此时才涉及整帧合成、鼠标坐标映射与截图一致性。
* **M3**：多字号/动态字号、CJK 断行与标点挤压、kerning/连字、字形子集化、字体族配置、逐上下文开关。

---

## 12. 待你确认的剩余选项

1. **M1 是否先用 GDI 走一遍**（零依赖，1 天内验证接管+写入），再换成 FreeType？还是直接上 FreeType 静态构建？
2. **字体**：`NotoSerifSC-VF.ttf`（可变，系统自带，先不分发）还是另选字重/静态实例？
3. **`Metrics` 默认 `game`**（保证对照干净）是否可以；何时切 `freetype` 由你定。
