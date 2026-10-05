# VectorText M0 log analysis

log      : F:\Mental Omega\VectorText.log
process  : PROC  pid=48392  moduleBase=0x00400000  F:\Mental Omega\gamemd.exe
exe      : EXE   size=0x00497FE0 timestamp=0x3BDF544E crc32=0x1B499086   (expected 0x00497FE0/0x3BDF544E/0x1B499086)
NEW lines: 4000 ; distinct strings: 4000

## per hook

| hook | calls (final) | distinct strings | call sites | plausible NEW | false positives |
|---|---|---|---|---|---|
| `BitFont::Blit(0x434120)` | 774555 | 0 | 0 | 0 | 0 |
| `BitFont::GetTextDimension(0x433CF0)` | 67894 | 2017 | 7 | 2002 | 15 |
| `BitText::DrawText(0x434CD0)` | 688 | 14 | 2 | 6 | 8 |
| `BitText::Print(0x434B90)` | 42209 | 1821 | 2 | 1820 | 1 |
| `Drawing::GetTextDimensions(0x4A59E0)` | 6593 | 15 | 4 | 14 | 1 |
| `Drawing::PrintUnicode(0x4A61C0)` | 8680 | 133 | 3 | 133 | 0 |

## string argument offset (resolves the real signature)

* `BitFont::GetTextDimension(0x433CF0)`: esp+0x4 x2017
* `BitText::DrawText(0x434CD0)`: esp+0xC x14
* `BitText::Print(0x434B90)`: esp+0xC x1821
* `Drawing::PrintUnicode(0x4A61C0)`: esp+0x8 x133

## call sites (UI contexts)

### BitFont::GetTextDimension(0x433CF0)

| caller | NEW | plausible | offsets | sample strings |
|---|---|---|---|---|
| 0x004a5ef1 | 1659 | 1659 | 0x4(1659) | 军事行动： 风暴使者 - 地点： 维尔京群岛 / 143 / 286 |
| 0x004346a2 | 180 | 175 | 0x4(180) | 距离特内里费岛战役发生的几周后 - / ： 普通 / 离特内里费岛战役发生的几周后 - |
| 0x006d4c75 | 131 | 131 | 0x4(131) | 10:29 / 10:28 / 10:27 |
| 0x00433ee6 | 32 | 31 | 0x4(32) | WWWWWWWWWWWWWWW / 等待 / Phobos development build #48. Please tes |
| 0x00478f0b | 10 | 2 | 0x4(10) | 发电厂升级：动力涡轮
---------------------
电力+150
 / 地平线驱逐舰
---------------
反步兵:1 反装甲:2 反建筑:1 |
| 0x00478c3b | 4 | 3 | 0x4(4) | 未探索的区域 / 电力=2160
负载=450 / 电力=2160
负载=600 |
| 0x00553199 | 1 | 1 | 0x4(1) | 任务目标一： 保护天气控制机
任务目标二： 消灭敌军部队 |

### BitText::DrawText(0x434CD0)

| caller | NEW | plausible | offsets | sample strings |
|---|---|---|---|---|
| 0x00479041 | 13 | 5 | 0xC(13) | 发电厂升级：动力涡轮
---------------------
电力+150
 / 未探索的区域 / 电力=2160
负载=450 |
| 0x00621144 | 1 | 1 | 0xC(1) | 任务目标一： 保护天气控制机
任务目标二： 消灭敌军部队 |

### BitText::Print(0x434B90)

| caller | NEW | plausible | offsets | sample strings |
|---|---|---|---|---|
| 0x004a5ff2 | 1805 | 1804 | 0xC(1805) | 军事行动： 风暴使者 - 地点： 维尔京群岛 / 143 / 等待 |
| 0x00621389 | 16 | 16 | 0xC(16) | - 距离特内里费岛战役发生的几周后 - / 难度： 普通 / 再一次踏上我们的国土感觉真好， 即使是在这种紧急状况下。 |

### Drawing::GetTextDimensions(0x4A59E0)

| caller | NEW | plausible | offsets | sample strings |
|---|---|---|---|---|
| 0x006a9d11 | 11 | 10 | - | 10 / 15 / 20 |
| 0x006a9dd1 | 2 | 2 | - | 等待 / 就绪 |
| 0x7bf7475b | 1 | 1 | - | Phobos development build #48. Please tes |
| 0x7bde60c0 | 1 | 1 | - | 主要 |

### Drawing::PrintUnicode(0x4A61C0)

| caller | NEW | plausible | offsets | sample strings |
|---|---|---|---|---|
| 0x006d4d9f | 131 | 131 | 0x8(131) | 10:29 / 10:28 / 10:27 |
| 0x0055309b | 1 | 1 | 0x8(1) | 军事行动： 风暴使者 - 地点： 维尔京群岛 |
| 0x006d4d47 | 1 | 1 | 0x8(1) | 距离敌军到达时间：   |

## top strings by draw count

| x | hook | first caller | text |
|---|---|---|---|
| 9392 | `BitFont::GetTextDimension(0x433CF0)` | 0x00433ee6 | Phobos development build #48. Please test the build before shipping. |
| 4696 | `Drawing::GetTextDimensions(0x4A59E0)` | 0x7bf7475b | Phobos development build #48. Please test the build before shipping. |
| 4696 | `BitText::Print(0x434B90)` | 0x004a5ff2 | Phobos development build #48. Please test the build before shipping. |
| 4319 | `Drawing::PrintUnicode(0x4A61C0)` | 0x006d4d47 | 距离敌军到达时间：   |
| 4319 | `BitText::Print(0x434B90)` | 0x004a5ff2 | 距离敌军到达时间：   |
| 4319 | `BitFont::GetTextDimension(0x433CF0)` | 0x004a5ef1 | 距离敌军到达时间：   |
| 3660 | `BitFont::GetTextDimension(0x433CF0)` | 0x00433ee6 | 选取部队横越画面 |
| 2440 | `BitFont::GetTextDimension(0x433CF0)` | 0x00433ee6 | 选取部队横越地图 |
| 1830 | `BitText::Print(0x434B90)` | 0x00621389 | 选取部队横越画面 |
| 1260 | `BitFont::GetTextDimension(0x433CF0)` | 0x00433ee6 | 提示 - 猎杀无人机会自动搜寻敌人的单位和建筑并摧毁它们。 |
| 1260 | `BitFont::GetTextDimension(0x433CF0)` | 0x00433ee6 | 指挥官， 太平洋阵线的精锐飞行士兵友川纪夫已经前来协助你。 |
| 1260 | `BitFont::GetTextDimension(0x433CF0)` | 0x00433ee6 | 报告提及， 被心灵军团控制的苏联基地就在这附近， 并正准备发动攻击。 |
| 1260 | `BitFont::GetTextDimension(0x433CF0)` | 0x00433ee6 | 当你确认发射以后， 它们会从战争工厂中飞出， 最多可以释放五只。 |
| 1260 | `BitFont::GetTextDimension(0x433CF0)` | 0x00433ee6 | 开始参与研发这个装置。 他们成功完成了第一台原型机。 |
| 1260 | `BitFont::GetTextDimension(0x433CF0)` | 0x00433ee6 | 在欧洲的气象晶体都被摧毁后， 太平洋阵线的金川工业 |
| 1260 | `BitFont::GetTextDimension(0x433CF0)` | 0x00433ee6 | 再一次踏上我们的国土感觉真好， 即使是在这种紧急状况下。 |
| 1260 | `BitFont::GetTextDimension(0x433CF0)` | 0x00433ee6 | 你会发现他的冷冻枪和导弹巢都是非常有效的快速反应武器。 |
| 1260 | `BitFont::GetTextDimension(0x433CF0)` | 0x00433ee6 | 任务目标一： 在敌军的进攻下保护天气控制机 |
| 1260 | `BitFont::GetTextDimension(0x433CF0)` | 0x00433ee6 | 他们呼叫我们的援助。 虽然悖论引擎无法赶到这里， 我们还是传送了一小支部队。 |
| 1258 | `BitFont::GetTextDimension(0x433CF0)` | 0x00433ee6 | 难度： 普通 |
| 1220 | `BitText::Print(0x434B90)` | 0x00621389 | 选取部队横越地图 |
| 990 | `BitFont::GetTextDimension(0x433CF0)` | 0x00433ee6 | - 距离特内里费岛战役发生的几周后 - |

## calls with no readable string (MISS) -- signature evidence

* `BitFont::GetTextDimension(0x433CF0)` caller=0x00433ee6  x2 logged
  * ecx=0x0E902E10 edx=0x001ACE48 esp+0x4=0x001ACE64 esp+0x8=0x001ACE48 esp+0xC=0x00000000 esp+0x10=0x00000000  expect=0x001ACE64 [00 00 01 01 24 CF 1A 00 01 00 00 00 F6 FF FF FF ] w="" arg1=0x001ACE64 [00 00 01 01 24 CF 1A 
  * ecx=0x0E902E10 edx=0x001ACE48 esp+0x4=0x001ACE64 esp+0x8=0x001ACE48 esp+0xC=0x00000000 esp+0x10=0x00000000  expect=0x001ACE64 [00 00 00 00 58 A1 71 34 58 A1 71 34 00 00 00 00 ] w="" arg1=0x001ACE64 [00 00 00 00 58 A1 71 
* `BitFont::GetTextDimension(0x433CF0)` caller=0x004346a2  x2 logged
  * ecx=0x0E902E10 edx=0x001AC1E4 esp+0x4=0x001AC1E4 esp+0x8=0x001AC108 esp+0xC=0x00000000 esp+0x10=0x00000000  expect=0x001AC1E4 [00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ] w="" arg1=0x001AC1E4 [00 00 00 00 00 00 00 
  * ecx=0x0E902E10 edx=0x001AC1FE esp+0x4=0x001AC1FE esp+0x8=0x001AC108 esp+0xC=0x00000000 esp+0x10=0x00000000  expect=0x001AC1FE [00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ] w="" arg1=0x001AC1FE [00 00 00 00 00 00 00 
* `BitFont::GetTextDimension(0x433CF0)` caller=0x004a5ef1  x2 logged
  * ecx=0x0E902E10 edx=0x001AC6D8 esp+0x4=0x001AC6D8 esp+0x8=0x001AC6B4 esp+0xC=0x001AC6B0 esp+0x10=0x00000571  expect=0x001AC6D8 [00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ] w="" arg1=0x001AC6D8 [00 00 00 00 00 00 00 
  * ecx=0x0E902E10 edx=0x001AC668 esp+0x4=0x001AC668 esp+0x8=0x001AC644 esp+0xC=0x001AC640 esp+0x10=0x00000571  expect=0x001AC668 [00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ] w="" arg1=0x001AC668 [00 00 00 00 00 00 00 
* `BitText::Print(0x434B90)` caller=0x004a5ff2  x2 logged
  * ecx=0x0E902E60 edx=0x001AC6D8 esp+0x4=0x0E902E10 esp+0x8=0x0E906190 esp+0xC=0x001AC6D8 esp+0x10=0x000004B1 esp+0x14=0x0000032E esp+0x18=0x00000000 esp+0x1C=0x00000000  expect=0x001AC6D8 [00 00 00 00 00 00 00 00 00 00 00 
  * ecx=0x0E902E60 edx=0x001AC668 esp+0x4=0x0E902E10 esp+0x8=0x0E906190 esp+0xC=0x001AC668 esp+0x10=0x000004B1 esp+0x14=0x0000032E esp+0x18=0x00000000 esp+0x1C=0x00000000  expect=0x001AC668 [00 00 00 00 00 00 00 00 00 00 00 
* `Drawing::PrintUnicode(0x4A61C0)` caller=0x00643707  x2 logged
  * ecx=0x0E906190 edx=0x00887734 esp+0x4=0x001ACB08 esp+0x8=0x00887734 esp+0xC=0x0E906190 esp+0x10=0x001ACB10 esp+0x14=0x001ACB44 esp+0x18=0x240574C0  expect=0x00887734 [00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ] w="
  * ecx=0x0E906190 edx=0x00887734 esp+0x4=0x001ACA98 esp+0x8=0x00887734 esp+0xC=0x0E906190 esp+0x10=0x001ACAA0 esp+0x14=0x001ACAD4 esp+0x18=0x240574C0  expect=0x00887734 [00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ] w="

## false positives of the string heuristic (tune LooksLikeText with these)

* `BitFont::GetTextDimension(0x433CF0)` caller=0x004346a2 text='通'  ecx=0x0E902E10 edx=0x001AC1E2 text@esp+0x4 pWidth=0x001AC108 pHeight=0x00000000 maxW=0  esp+0x4=0x00
* `BitFont::GetTextDimension(0x433CF0)` caller=0x004346a2 text='-'  ecx=0x0E902E10 edx=0x001AC1FC text@esp+0x4 pWidth=0x001AC108 pHeight=0x00000000 maxW=0  esp+0x4=0x00
* `BitFont::GetTextDimension(0x433CF0)` caller=0x004346a2 text='业'  ecx=0x0E902E10 edx=0x001AC208 text@esp+0x4 pWidth=0x001AC108 pHeight=0x00000000 maxW=0  esp+0x4=0x00
* `BitFont::GetTextDimension(0x433CF0)` caller=0x004346a2 text='。'  ecx=0x0E902E10 edx=0x001AC222 text@esp+0x4 pWidth=0x001AC108 pHeight=0x00000000 maxW=0  esp+0x4=0x00
* `BitFont::GetTextDimension(0x433CF0)` caller=0x00478c3b text='X'  ecx=0x0E902E10 edx=0x001AD1E4 text@esp+0x4 pWidth=0x001AD1E4 pHeight=0x001AD204 maxW=168  esp+0x4=0x
* `BitFont::GetTextDimension(0x433CF0)` caller=0x00478f0b text='机器人控制中心\n-----------------\n*太平洋阵线*\n解锁科技\n技能:西风定位\n用法:放置信标\n命令西风火炮攻击\n$1500 ⌚01:00 ⚡-150'  ecx=0x0E902E10 edx=0x007E85D4 text@esp+0x4 pWidth=0x001AD1C8 pHeight=0x001AD198 maxW=1752  esp+0x4=0
* `BitText::DrawText(0x434CD0)` caller=0x00479041 text='机器人控制中心\n-----------------\n*太平洋阵线*\n解锁科技\n技能:西风定位\n用法:放置信标\n命令西风火炮攻击\n$1500 ⌚01:00 ⚡-150'  ecx=0x0E902E60 edx=0x00000085 text@esp+0xC  esp+0x4=0x0E902E10 esp+0x8=0x0E906240 esp+0xC=0x27008F40
* `BitFont::GetTextDimension(0x433CF0)` caller=0x00478f0b text='盟军空军指挥部\n-----------------\n提供雷达\n解锁科技\n技能:侦察卫星\n$1500 ⌚01:00 ⚡-150'  ecx=0x0E902E10 edx=0x007E85D4 text@esp+0x4 pWidth=0x001AD1C8 pHeight=0x001AD198 maxW=1752  esp+0x4=0
* `BitText::DrawText(0x434CD0)` caller=0x00479041 text='盟军空军指挥部\n-----------------\n提供雷达\n解锁科技\n技能:侦察卫星\n$1500 ⌚01:00 ⚡-150'  ecx=0x0E902E60 edx=0x00000075 text@esp+0xC  esp+0x4=0x0E902E10 esp+0x8=0x0E906240 esp+0xC=0x0F88A480
* `BitFont::GetTextDimension(0x433CF0)` caller=0x004346a2 text='机'  ecx=0x0E902E10 edx=0x001AC200 text@esp+0x4 pWidth=0x001AC108 pHeight=0x00000000 maxW=0  esp+0x4=0x00
