# Mental Omega 矢量文字渲染：M0 → M2 交付说明

> 工程目录 `F:\Mental Omega\VectorText`；产出物是游戏根目录的 `VectorText.dll` + `VectorText.ini`。
> 全部行为可用一个 INI 键回退；删除这两个文件即完全无痕。

---

## 1. 现在能做什么

游戏里原本用 `game.fnt` 点阵字形绘制的文字，现在可以由 FreeType 矢量字形替换：

* **抗锯齿**（1/4 像素级覆盖率混合进游戏的 16 位表面）
* **2× 超采样**光栅化 + 覆盖率**伽马校正** + FreeType **stem darkening**
* **亚像素定位**（小数笔位烘焙进字形位图）
* **可变字体字重**（`FontWeight` 200–900）
* **三档度量**：完全沿用原版步进 / 按比例放宽 / 自然矢量步进
* 排版、换行、居中、阴影、逐字渐变**仍由引擎计算** —— 我们只替换字形像素

## 2. 四种运行模式（INI `Mode=`）

| 模式 | 机制 | 抗锯齿 | 亚像素 | 风险 |
|---|---|---|---|---|
| `aa`（推荐） | 我们把自己的 AA 像素混进 16 位表面；同时把引擎该字符的位图清零，让引擎"画了个空"但照常走完自己的代码 | ✅ | ✅ | 无（真机连续 13 万字形零失败） |
| `swap` | 把矢量子形写进引擎自己的字形数据，引擎照常绘制 | ❌（1bpp） | ❌（拿不到笔位） | 无 |
| `draw` | 我们完全自己写像素，并跳过引擎的 `Blit`（用自生成的栈修复跳板） | ✅ | ✅ | 实验（见 §5） |
| `observe` / `off` | M0 观测模式 / 完全停用 | — | — | 无 |

`Mode=observe` 时 DLL 只记录统计与探针，不改任何绘制行为 —— 一键回退。

## 3. 画质与度量参数（INI）

| 键 | 现在 | 作用 |
|---|---|---|
| `FontFile` | `NotoSerifSC-VF.ttf` | 任意 FreeType 可读字体（同机另有 `NotoSansSC-VF.ttf`） |
| `FontWeight` | `700` | 可变字体字重；NotoSerifSC 默认实例是 ExtraLight(200)，必须显式指定 |
| `FontSizeLatin` / `FontSizeCJK` | `13` / `16` | 实测对齐 `game.fnt`：拉丁墨迹 9 行、中文占满 16 行格 |
| `BaselineRow` | `13` | 16 行格内的基线 |
| `StemDarkening` | `120` | FreeType 光栅化期笔画加粗（治"AA 小字太轻"） |
| `Gamma` | `1.40` | 覆盖率伽马（治"发虚"） |
| `Supersample` | `2` | 2× 光栅化 + 盒式降采样 |
| `Subpixel` | `1` | 1/4 像素相位；需要小数步进（见下） |
| `Metrics` | `scaled` | `game` / `scaled` / `vector` |
| `AdvanceScale` | `1.05` | `scaled` 的放宽比例 |
| `FitToAdvance` | `1` | 字宽超出格子时横向压缩（区间 OR 合并，不丢细笔画） |
| `Probe` | `1` | 每发现一个 BitFont 对象写一行运行时布局探针（只读） |

## 4. 度量三档（离线审计结论）

工具 `tools/audit_metrics.py`：把日志里游戏真实做过的**310 次测量**（含调用者给出的 `maxW` 上限）
逐条重放，分别用引擎宽度与矢量宽度累加：

| 模式 | 总宽 | 超框 |
|---|---|---|
| `game` | 1.000× | 0 |
| **`scaled` 1.05** | **1.031×** | **0 ← 采用** |
| `scaled` 1.10–1.20 | 1.125–1.205× | 1（任务目标框 `caller=0x00553199`，`maxW=400`） |
| `vector`（自然度量） | 1.377× | 1（同上，自然度量下 451px/+13%） |

* 完全自然度量会让文本整体宽 ~38%，对按点阵字体手工摆好的固定 UI 太激进；
* **+5% 是"零溢出"的最大档位**，正好顺带减轻横向压缩 → 笔画更实；
* 要推进到 `vector`，只需处理那一个任务目标框（M2.2）。

## 5. 崩溃根因与修复（重要历史）

第一次开 `Mode=draw` 时，游戏在画出第一个字形后崩在 `0xC0000005`。从 Syringe 留下的完整内存转储
（`debug/snapshot-*/extcrashdump.dmp`）里读出被改写的机器码，顺藤摸出 **Syringe 的钩子桩**：

```asm
pushad / pushfd / push <origin> / push esp / call <handler>
mov  [esp-4], eax        ; 返回值
popfd
popad                    ; ★ x86 的 popad 不加载保存的 ESP
cmp  dword [esp-0x28], 0 ; 非 0 → 跳转到返回值
jmp  dword [esp-0x28]
```

* `REGISTERS` 布局与我们一致（`+0x14` = 入口 ESP，指向返回地址）；
* **但 `R->ESP(...)` 的写入会被 `popad` 静默丢弃** → 我们"跳到返回地址"时栈少了 `0x14`
  → 调用者帧错位 → 下一次 `ret` 弹进堆上的表面对象（`EIP=0x…5EB4`，与 Ares 抓到的签名一致）。

**修复**：不再依赖 `R->ESP`，改为返回一个自生成的 9 字节跳板，由它完成 `ret 0x10` 的栈修复：

```asm
mov edx,[esp] ; add esp,0x14 ; push edx ; ret
```

## 6. 验证证据

| 层级 | 工具 / 证据 | 结果 |
|---|---|---|
| 钩子胶水契约 | `hooktest_draw.bat --mode draw/aa/swap/observe` | 各模式 `failures=0`；跳过路径断言"不改 ESP + 返回跳板 + 跳板字节" |
| 接管路径 | `takeover_test.bat` | 布局/像素/颜色/AA/裁剪/未锁定/缺字形/吞吐 8 项全过；1697 字形/ms |
| 字形对齐 | `glyph_test.bat` | `A`/`a`/`文` 的行列范围与 `game.fnt` **完全一致**；中文 0..13 vs 0..15 |
| 像素公式 | `pixel_test.bat` | 用引擎自己的 16 位寻址公式渲染对照表 |
| 真实文本 | `render_strings.bat` | 日志里 40 条真实游戏字符串：98.6% 由我们绘制；**矢量字体缺字形 0 个** |
| 亚像素 | `render_strings --subpixel 0/1` 逐像素比对 | 353 个像素不同 → 相位确实生效 |
| 度量 | `audit_metrics.py` | 310 次真实测量 × 4 档度量 |
| 游戏内 | 探针 + 用户实测 | 运行时布局 `pitch=1920 bounds=0,0,1919,1079 lines=16 symBytes=49` 全部自洽；`Mode=aa` 连续 13.2 万字形、`failed=0`、`unknownGlyph=0` |
| DLL 完整性 | `verify_dll.py` / `selftest.bat` | 6 条钩子记录 export/reloc 正常；`failures=0` |

## 7. 后续（M2.2 / M3）

1. **M2.2 逐界面适配**：把任务目标框（`0x00553199`）单独处理（给它保留原度量或略小字号），
   即可把 `Metrics` 推到 `vector`（自然字距、不压缩、最佳字形）。
2. **M3 画质层**：在 `Mode=draw` 跑通的前提下，叠加 32 位合成（超采样、真彩 gamma、SDF 描边），
   可彻底摆脱 16 位表面的色深与 1 像素粒度限制。
3. **M2.4 字号体系**：按用途（正文/提示/标题）配置不同的 size/weight/darkening 组合。
