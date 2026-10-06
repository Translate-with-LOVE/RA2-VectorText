# 游戏内崩溃排查手册（M1）

> 第一次开 `Mode=draw` 时，游戏在画出第一个字形后（日志 `drawn=1`）以
> `0xC0000005 at 0x1F0BDEB4` 崩溃。本文记录**已排除的假设**、**已加入的自证设施**，
> 以及**下一步的安全二分流程**。

## 1. 已获得的实证（这一局并非全无收获）

探针在真机上跑通，一次性验证了 M1 依赖的全部运行时假设：

```
PROBE bf=0x0E9D2E10 internal=0x0E9D2D40 base=0x23D5D020 pitch=1920 color=0xFFFF
      bounds=0,0,1919,1079 lines=16 symBytes=49 firstCh=U+519B idx=2005 advance=14
```

* 内部渲染分辨率 **1920×1080**，`pitch=1920`（像素单位）✓
* `lines=16`、`symBytes=49`、`bounds` 为整屏 ✓ —— 与 `game.fnt` 文件头完全一致
* `advance=14` 等于 `game.fnt` 里 `军` 的宽度字节 ✓ → **Metrics=game 成立**
* 说明"读 `BitFont` 字段来定位像素缓冲"这条路是通的

## 2. 已排除

| 假设 | 排除依据 |
|---|---|
| 偏移猜错（缓冲/pitch/边界） | 探针实测值全部自洽，且缓冲写入偏移 ~1MB < 1920×1080×2 ✓ |
| 钩子冲突（Ares/Phobos） | 解析两者 `.syhks00`：Ares 1448、Phobos 1314 条记录，**在我们整段文字地址（0x433C00–0x435400、0x4A5800–0x4A6300）里 0 条** |
| 跳过被调函数的 ESP 约定写错 | Ares 自己的代码就是同一约定：`add dword [regs+0x14], 4/8` → `mov eax,[regs+0x14]` → `mov eax,[eax-4]`（即 `esp` 指向返回地址，+4+参数个数，然后把原返回地址当跳转目标）✓ 与我们的 `+4+0x10` 完全一致 |
| 越界写 | 裁剪框 = `bounds ∩ [0, pitch-1]`，本次 x=570 y=250，写点全部在屏内 |

## 3. 已加入的自证设施（新版本 DLL）

1. **方向标志（DF）**：钩子入口检测 DF，若被置位则**清除并写警告**。
   CRT 的 `memcpy/memset`（字形缓冲清零、压缩重采样）在 DF=1 时会**反向写**，
   正好会在"第一次光栅化"时破坏栈 —— 与本次崩溃的时机高度吻合。
   日志出现 `WARNING direction flag (DF) was SET at hook entry` 即命中此因。
2. **阶段标记**：接管过程分 10 个阶段，`FINAL` 行会写
   `M1 diagnostics: lastStage=… sawDF=… skipped=… skipOriginal=…`。
   即使进程硬崩，退出时 DLL 仍会写这行 → **崩溃前走到哪一步一目了然**。
3. **跳过前 3 次完整记录**：
   `SKIP #1 ch=U+4E2D entryESP=… retAddr=… newESP=… (delta=0x14) newX=…`
4. **安全二分开关 `SkipOriginal`**：
   * `1`（默认）= 正常：我们画完就跳过引擎的 `Blit`；
   * `0` = 我们照样画，但**不碰调用流程**，让引擎再画一遍自己的点阵。
     画面几乎等于原版（会露出一点衬线边缘，正好证明我们的像素确实画进去了），
     但**完全不改栈/控制流** —— 用来把"我们的绘制"与"我们的栈操作"分开。

## 4. 下一步：两局二分（都不危险）

```ini
; 第 2 局：只验证"我们的绘制"是否安全（不改控制流）
Mode=draw
SkipOriginal=0
```

* 若**正常进入游戏**（应当看到文字边缘多了一点衬线）+ 日志里 `drawn` 随文字增长、
  `M1 diagnostics: … skipOriginal=0` → 绘制与写入在真机上安全，问题在跳过环节。
* 若**仍崩溃** → 看 `M1 diagnostics` 的 `lastStage`：
  `rasterised(5)` 前崩 = 取数据/光栅化；`written(6)` 前崩 = 写像素；同时看有无 DF 警告。

```ini
; 第 3 局（第 2 局通过后再做）：打开跳过
SkipOriginal=1
```

* 崩溃则把日志的 `SKIP #1…#3`、`M1 diagnostics`、`M1 refusals` 三行发我；
  这三个数字能唯一确定是"返回地址/栈"还是"我们的绘制"。

随时可回退：`Mode=observe`（本手册写作时游戏目录已置为该值）或删除
`VectorText.dll` / `VectorText.ini`。

---

## 5. 结论（用户实测）：崩溃只在"跳过引擎 Blit"时发生

实测二分结果：

| 配置 | 结果 |
|---|---|
| `Mode=draw, SkipOriginal=0`（我们画像素，**不改调用流**） | **不崩**，一切正常 |
| `Mode=draw, SkipOriginal=1`（我们画像素 + 跳过引擎的 Blit） | 崩 |

→ **我们的光栅化与像素写入在真机上完全安全**；问题被唯一地锁定在"跳过被调函数"
（修改 `REGISTERS.esp` + 返回调用者返回地址）这一步。

## 6. 因此 M1 换用更安全的接管方式：`Mode=swap`

不再跳过 `Blit`，而是**把我们的字形写进引擎自己的字形数据里**：

```c
slot = Bitmaps + (SymbolTable[ch] - 1) * SymbolBytes;   // 引擎读的那 49 字节
slot[0] = width;                                        // 与原版相同的步进字节
memcpy(slot + 1, ourCell.bits, SymbolBytes - 1);         // 1bpp、MSB 在前，格式完全相同
return 0;                                               // 让引擎自己画（画的是我们的字形）
```

* **零控制流改动、零栈操作** → 不可能像 skip 那样翻车；
* 引擎继续负责寻址、裁剪、颜色、阴影、逐字渐变；
* 步进字节保持原值 → 布局仍与原版逐像素一致（Metrics=game 天然成立）；
* 幂等：先比较再写，字体被重新加载后会自动重新替换；
* 代价：引擎以不透明 1bpp 写像素，**暂时没有抗锯齿**（AA 需要"自己写像素"那条路，
  留待修好 skip 之后再作为 `Mode=draw` 提供；M2 的 32 位合成也能提供 AA）。

离线验证（`hooktest_draw.bat --mode swap`）：不跳过（返回 0、ESP 不变）、字形数据确实被替换、
步进字节不变、二次调用幂等、替换后的格子有墨迹 —— 全部通过。
---

## 7. 崩溃签名（来自 Syringe/Ares 的 dump，`debug/snapshot-20261006-142534/`）

```
Exception code: C0000005 at 23755EB4      ← EIP 落在"表面对象+4"（堆地址，非代码）
Access: WRITE, faulting address = 0x43464D ← 试图写入 gamemd 的代码段（只读）
Registers: EDI=0043464D  ECX=00434BCF  ESI=0E9B519B(=字体指针高位+首字"军")
Stack:     [ESP]=001AB782 [ESP+4]=570(X) [ESP+8]=250(Y) ...
Ares version: 20.333.289
```

解读：**控制流被劫持到堆上的表面对象里执行**（EIP 落在 `0x…5EB4`，正是 `0x…5EB0` 这
个表面对象的 +4），随后那条"垃圾指令"试图写 `0x43464D`（恰好也是 EDI 里的值 =
`call BitFont::Blit` 的下一条指令地址）→ 只读页写入失败。

* 该崩溃**只出现在跳过引擎 `Blit` 的配置**（用户实测：`SkipOriginal=0` 不崩、`=1` 崩）；
* `skipOriginal=0` 的运行里 `skipped=0`、`drawn=132114`、`failed=0` → **我们的绘制在真机上
  连续工作十几万个字形无误**，并能在画面上看到（用户描述"两层字体叠在一起" = 我们的衬线
  字形 + 引擎叠画在上面的点阵字形，正是二分模式的预期现象）；
* 结论：故障精确定位在"**跳过被调函数**"（修改 `REGISTERS.esp` 并返回调用者返回地址）这一步。
  注意 Phobos.dll 的 1314 个钩子里**没有任何一处**修改保存的 ESP（`83 4? 14` 零命中），
  而 Ares 的 22 处更像"读取返回地址"的辅助代码 —— 提示 Syringe 是否接受 ESP 写回需要
  进一步验证（需要 windbg 打开那份 465 MB 的 dump，本地暂无）。

### 本安装的既有崩溃（在我们 DLL 出现之前）

`%LOCALAPPDATA%\CrashDumps\gamemd.exe.*.dmp` 里有 5 份 8–9 月的崩溃，异常记录各不相同
（`0x46000161` 执行、`0x48CFCA`、`0xD902EB1`、`0x6A7D184A`、`0x4C1D5492` 读取），
说明这个整合包本身就有崩溃史 —— 分析新崩溃时应先排除这一背景噪声。