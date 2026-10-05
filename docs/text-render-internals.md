# gamemd.exe 文字渲染内部结构（M1 实现依据）

> 全部结论来自对本地 `gamemd.exe` 1.001 的反汇编（原始输出见 `build/disasm/*.txt`，
> 该目录被 git 忽略）。地址均为 VA。**M1 的接管与像素写入直接按本文实现。**

---

## 1. 调用层次（自下而上）

```
BitFont::Lock(Surface*)      0x4348F0   锁定表面, 把缓冲/行距写进 BitFont 对象
BitFont::Blit(wchar, X, Y, color)  0x434120   画一个字形, 返回新的 X（= X + 字宽）
BitFont::UnLock(Surface*)    0x434990   解锁（内部调 Surface vtable[+0x60]）
BitFont::DrawString          0x434500   字符串循环（Core）: 5 个栈参数, ret 0x14
BitText::Print               0x434B90   7 个栈参数, ret 0x1C  -> Lock / SetX / Core / UnLock
BitText::DrawText            0x434CD0   10 个栈参数, ret 0x28（带颜色/对齐的富路径）
Drawing::GetTextDimensions   0x4A59E0   测量（ECX=out rect, EDX=文本）
BitFont::GetTextDimension    0x433CF0   测量（esp+4=文本, +8=w, +0xC=h, +0x10=maxW）
```

## 2. BitFont 对象字段（实测，非 YRpp 猜测）

| 偏移 | 含义 | 证据 |
|---|---|---|
| `+0x04` | `InternalData*`（game.fnt 解析结果） | Blit 0x43416E 起用它取 SymbolTable/Bitmaps |
| `+0x08` | 图缓冲指针（pGraphBuffer） | Blit 0x4341A8 |
| `+0x0C` | **已锁定表面的像素缓冲基址** | `lea (%esi,%eax,2),%esi` @0x4342A2；另一路径 0x4343FF/0x434410 |
| `+0x10` | **行距 pitch（单位：像素）** | `lea (%eax,%ecx,2),%esi` @0x4343DD；另一路径 0x4343FC `imul` |
| `+0x18` | 每行累加量（同一 pitch 的副本/行计数用） | 0x4343D1-0x4343D4 |
| `+0x20` | 文本原点 X（`BitFont::SetX` 0x434110 写入） | tab 停位计算用它 |
| `+0x24` | **当前颜色（WORD，16 位色字）** | Blit 0x434158；`SetColor` 0x433C70 写入并返回旧值 |
| `+0x26` | 第二个颜色字（`SetColor2` 0x433C80 写入） | Blit **不读**它（阴影不走这里，见 §5） |
| `+0x28` | tab 宽度 | tab 路径 0x434137 |
| `+0x30..0x3C` | LTRB 裁剪框（`SetBounds` 0x433CA0 写入 4 个 dword） | 与字形矩形求交 |
| `+0x41` | 绘制路径开关（0x433C90 写入） | Blit 0x4341E0 二选一：带逐像素裁剪 / 不带 |

## 3. 像素格式与写入（Q1/Q2 已确认）

* **文字表面是 16 位色**：所有写像素都是 `mov %bp,(%esi)` + `add $0x2,%esi` ——
  直接写 16 位色字（`asm` 证据：0x4342F4 / 0x434342 ...）。
* 颜色不需要调色板查找；`Drawing::RedShiftLeft/Right`（`0x8A0DD0/0x8A0DD4` 等 6 个全局）
  描述的是这种 16 位色的打包格式，`RGBClass::Adjust`（`0x6612C0`）用它拆/合颜色。
* **目标地址公式（两条独立路径互相验证）**：

  ```
  dst = (uint16_t*)(BitFont[+0x0C]) + y * BitFont[+0x10] + x;
  ```

  * 路径 A（`+0x41 == 0`，0x4343F8）：`ecx = pitch; ecx *= y; ecx += X; dst = base + ecx*2`
  * 路径 B（`+0x41 != 0`，0x4342A2 + 行推进 0x4343C5）：
    `dst = base + (...)*2`，行推进 `dst += pitch*2`
* 字形掩码：**1bpp、MSB 在前、1 = 有墨**（`test $0x80/%0x40/.../%0x01`），
  逐字节消费；空字节走快路径（`add $0x10,%esi` 跳过 8 像素）。
* 字形数据来源（运行时可直接读，不必自己解析 game.fnt）：
  `InternalData[+0x18]` = `uint16 SymbolTable[65536]`；`InternalData[+0x1C]` = 位图基址；
  `InternalData[+0x14]` = 每个字形字节数。索引 = `SymbolTable[ch] - 1`；
  字形首字节 = **宽度（advance）**，其后 `Stride*Lines` 字节为点阵。

## 4. 字符串循环（Q4 已确认）

`BitFont::DrawString(BitFont* this, const wchar_t* s, int X, int Y, int maxChars, int reveal)` — `0x434500`

* `maxChars == 0` → 不限制；否则最多画这么多个字符，**剩下的只测量不绘制**（0x43468D 调 `GetTextDimension`）。
* `reveal == 0` → 直接画，**颜色恒定**（0x434545 `test ecx,ecx; je 画`）。
* `reveal ∈ [1..8]` → **逐字符颜色渐变**：每画一个字符，颜色向白混合一次，
  `ratio = ((9 - reveal) * 31 + 31 * i) & 0xFF`（`RGBClass::Adjust(ratio, RGBClass::White /*0xA80220*/)`），
  画完 `reveal` 个字符后停止绘制、只测量余下部分。这是"消息逐字浮现 + 边缘发亮"的效果。
* `reveal >= 9` → 走 0x434618 跳过渐变 → 纯色。
* `\r` `\n` 只被跳过（不推进 X、不换行）→ **换行由调用者负责**（或由 `DrawText` 富路径处理，见 §5）。
* 返回值为**结束时的 X**（= 起始 X + 所有已绘字符的 advance 之和）。
* 颜色在函数退出时恢复为进入时的值（`mov %bx,0x24(%edi)`）。
* 特殊字符 TAB（`0x09`）在 `Blit` 里处理：`X' = X + tabW - ((X + tabW - originX) % tabW)`。

## 5. 仍未确认（M1.3 之前补齐）

| 项 | 说明 | 计划 |
|---|---|---|
| Q3 阴影/描边 | `Blit` 只写一种颜色，`+0x26` 不被读 → 阴影应是**调用者再画一遍**（偏移+暗色），或由 `BitText::DrawText` 内部完成 | 反汇编 `0x434CD0`（10 参数富路径）与消息/提示调用点 |
| Q4b `DrawText` 的换行/对齐 | `0x434CD0` 有 10 个栈参数（含颜色/对齐），多行文本（含 `\n` 的字符串）应该走它 | 同上 |
| Q5 旁路路径 | `0x434E00–0x435000` 有函数调用 `GetCharacterBitmap`(0x4346C0)，疑似描边路径 | 找入口与调用者，纳入 LEAK 清单 |

## 6. M1.3 的实现配方（方案 B：自己写像素）

```cpp
// 1) 跳过 BitText::Print / DrawText（Syringe: 弹栈 + 跳到返回地址）
// 2) 自己排版（按 §4 的规则 + Metrics=game 的宽度字节）
// 3) 锁定表面并取缓冲/行距——直接复用引擎的两个函数，避免猜 Surface 布局：
using LockFn   = bool(__thiscall*)(void* font, void* surface);   // 0x4348F0
using UnLockFn = bool(__thiscall*)(void* font, void* surface);   // 0x434990
LockFn(0x4348F0)(font, surface);
uint16_t* base  = *(uint16_t**)((char*)font + 0x0C);
int       pitch = *(int*)     ((char*)font + 0x10);
// 4) 逐字形：FreeType(wght=400) -> 1bpp 掩码 -> 写像素（含 §4 的颜色渐变）
//    裁剪：字形矩形 = (X, Y, X+width-1, Y+lines-1)，再与 font[+0x30..0x3C] 及表面求交
// 5) UnLockFn(0x4348F0 的反向)(font, surface);
```

要点：
* **advance 取原版宽度字节**（`Metrics=game`）→ 布局与原版逐像素一致（A2/A4）。
* 颜色 = `font[+0x24]`（或 `DrawText` 传入的颜色参数），按 §4 的 `reveal` 规则逐字符混合。
* 空字符（`SymbolTable[ch]==0`）时引擎会 `xor color,0x5555` 后仍尝试绘制 —— 我们保持"不画"并记录。
* 想暂时回退：`Mode=observe` → 钩子 `return 0`，引擎照常绘制。
