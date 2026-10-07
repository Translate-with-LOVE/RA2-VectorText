# 小字号画质

本轮继续使用游戏自带的 RGB565 表面，不依赖 cnc-ddraw 或其他呈现器。
单行 X 布局、游戏原有 Y、换行、显现和颜色流程保持原有接口。

## 当前配置

- 中文：`NotoSansSC-VF.ttf`，16px，可变字重 450。
- 英文：`arial.ttf`，13px，常规体。固定字体的字重由文件决定，`FontWeight` 不会改变它。
- `Supersample=1`：在实际目标尺寸栅格化，不先生成 64px 字形再缩回 16px。
- `Hinting=0`：FreeType LIGHT，主要沿纵向对齐笔画，同时保留水平比例。
- `Gamma=1.0`、`LinearBlend=1`，继续保留覆盖率并在较高精度的线性空间混合，最后输出 RGB565。

`Hinting=1` 使用 NORMAL，`Hinting=2` 关闭 hinting；两者供字体适配时对比。
不支持的值回到 LIGHT。无抗锯齿模式仍使用 MONO，不受该参数影响。
切换 hinting 会清空字形缓存；单行自然步进改用 FreeType 的 `linearHoriAdvance`，
因此 hinting 和采样方式不会改变设计字距。整行等比缩小仍使用同一比例缩放这些度量。

旧逐字回退路径的显式字宽仍按游戏度量返回；轮廓变换的固定点舍入可能生成极低覆盖率的
边缘溢出，现在仅在启用 FitToAdvance 的显式旧字格中裁掉该部分。
自然单行字形保留轴承和边缘，不按旧字格裁切。

## 共同基线与完整字形

直接绘制路径已经取消按每个字符的轮廓上下边界做纵向补偿，中英文、数字和符号使用同一规则。
以前为了避免 16 行字格裁切，16px 思源黑体中的“我”“们”“个”分别被下移
约 0.36、0.58、0.78px，造成同一行中文字上下跳动。

现在统一使用 `BaselineRow`，直接保留 FreeType 的纵向轴承。
字形缓存记录 `inkY` 和实际位图行数，允许自然笔画超出旧的 16 行存储范围；
写像素时仍严格遵守目标表面和文本框的上下裁剪边界。
字号、游戏提供的 Y、自然步进都不因单个字形的高度而改变。
直接画像素的逐字回退路径也保留共同基线与完整纵向轴承；
只有必须拷贝点阵到 game.fnt 的 swap 模式显式请求旧固定字格，且使用独立缓存键。

`baseline_test.bat` 使用截图中的任务文本，与 FreeType 直接绘制在固定基线上的位图逐像素比较，
覆盖全部大小写英文字母、数字、常见中英文符号、LIGHT/NORMAL/无 hinting、1x/2x/4x、
四种 X 相位、整行等比缩小，以及顶边和文本框裁剪。
逐字回退也检查纵向轴承、完整行数和每行覆盖率；独立固定字格缓存不会混入直接绘制路径。
此检查独立于字格重定位算法，用于防止重新引入逐字基线漂移。

## 对比与验证

执行 `render_quality_test.bat` 构建和检查生产预览，输出
`build/render-qa/raster-after.bmp` 和 `build/render-qa/raster-scene.bmp`。
它使用生产代码、真实 game.fnt 和合成 RGB565 表面，以当前项目 INI 验证黑底和纹理背景。
旧字体组合的批量选型脚本已删除；需要单独比较参数时，修改 QA 目录的 `VectorText.ini`，
在该目录运行 `render_quality.exe comparison.bmp --check --line`。
每次运行启动独立进程，不修改游戏目录配置。

检查包括文字返回步进、原始字体数据不变、缓存相位区分、旧字格边界、混合色道，
以及目标字号 LIGHT/NORMAL 与 4x LIGHT 的自然步进一致性。
`--scene` 可生成深色纹理背景，正常预览仍以黑底验证细节。
字形连续性检查关注内部空白行；最终可读性还需要目测和游戏内验证。

预览中的字号与游戏字号相同，比较时应以 100% 查看；放大图仅用于检查边缘像素。
离线通过不意味着游戏内每种背景、缩放比例都具有相同观感。

## 回退

本轮之前的 DLL 和 INI 保存在 `backups/raster-before-20261006`。
只恢复旧视觉配置也可以使用：

```ini
FontFile=C:\Windows\Fonts\NotoSerifSC-VF.ttf
FontFileLatin=C:\Windows\Fonts\ARIALNB.TTF
FontWeight=500
FontSizeLatin=13
FontSizeCJK=16
Supersample=4
Hinting=0
```

修改配置后重启游戏生效。中文继续保持 16px，没有横向拉伸。
