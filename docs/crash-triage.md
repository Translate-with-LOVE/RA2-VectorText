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
