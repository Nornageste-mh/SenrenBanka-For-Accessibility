# third_party —— 引擎的代码，不是游戏的内容

这里放的是**编译必需**的第三方头文件，为的是让「拿到本仓库源码的人能编出一样的产物」。
它们**不属于柚子社**，也不含任何游戏内容。

## tjs2/ —— TJS2 Script Engine（吉里吉里 / KiriKiri）

```
Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors
```

- **来源**：吉里吉里 / krkrz 发行包中的 TJS2 脚本引擎头文件。
- **内容**：`a11y6.cpp` 编译时真正被 `#include` 到的那 11 个头文件（含传递依赖），
  **逐字节原样，未做任何修改**：
  `tjsCommHead.h` `tjsInterface.h` `tjsVariant.h` `tjsVariantString.h` `tjsTypes.h`
  `tjsString.h` `tjsConfig.h` `tjsErrorDefs.h` `tjsMessage.h` `tjs.h` `targetver.h`
- **许可证**：吉里吉里采用**双许可** —— 吉里吉里自有许可，或 GNU GPL（任选其一）。
  自有许可证原文随本目录提供：`LICENSE-kirikiri-ja.txt`；
  来源页 <https://krkrz.github.io/krkr2doc/>（「吉里吉里および KAG のライセンス」）。
- **为什么可以这样分发**：该许可的「流用・改造とライセンスの変更」条款明确允许把本软件的
  源码或片段**嵌入其他软件**（开源、闭源均可）；「二次配布」条款只要求**随附本许可证文本**，
  且不得就本软件本身的分发收取费用（本仓库免费）。
  该条款另要求「在文档等中注明使用了本软件的源码」——本文件与根目录 `README.md`
  的「第三方组件」一节即为该注明。
  另有「プラグインの作成」条款，对为编写插件而使用引擎源码文件的情形给出更宽的豁免。
- **GPL 分支**：若选择 GPL 分支使用本引擎，则须整体遵循 GPL；本模式按自有许可分支使用。
- 本目录**不含**吉里吉里/KAG 的其余源码（执行核心、KAG 系统、其它插件等）。

## 未随仓库分发的第三方组件

| 组件 | 用途 | 为什么不分发 |
|---|---|---|
| `nvdaControllerClient.dll` | 读屏桥（NV Access 官方 x86 构建） | 有各自的许可，且不应由本仓库转手；安装时自取 |
| Tolk（可选） | 一套接口覆盖多家读屏 | 同上 |

## 与本仓库的边界

本仓库**不含**、且 `.gitignore` 主动拦截：

- 游戏本体与资源：`*.xp3` `*.sig` `*.exe` `*.dll` `savedata/` …
- 游戏脚本与剧情：`*.ks` `*.scn` `*.psb` `*.tjs` `*.func` …
- 音视频与图片：`*.ogg` `*.png` `*.pimg` …、`shots/`
- **解包 / 解密 / 反汇编等分析脚本与其产物**
- 运行日志与探针输出（可能夹带剧情文本）

`third_party/` 是这条线**唯一**的开口，而且只开口给引擎代码。
