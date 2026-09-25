# 设计说明

这份文档只记录**怎么做的**与**凭什么这么做**。所有结论都标了证据来源，没有把握的地方明确写「未复测」。

## 0. 纪律

1. **不许猜方法名和对象名。** 每一个用到的游戏对象 / 方法 / 字段，都要有实机探针转储或反汇编依据。
   别名表里没把握的层名就照念层名本身，绝不编一个中文词冒充。
2. **不按时钟反复重扫。** 扫描只在「进入导航 / 激活之后」触发。
3. **清单变了才播报。** 没变就不出声，免得每点一下都念一遍。
4. **破坏性控件要二次确认。** （读档 / 删除存档这类尚未实现确认，见「已知未完成」。）
5. **未验证就说未验证。**
6. **能用数据文件就别写死代码** —— 所以标签表放在 `plugin\a11y_labels.ini`。

## 1. 为什么是「进程内插件」而不是伴随程序

最早试过宿主进程 + 管道 + 伴随窗口：要跨进程抢焦点，读屏软件念的是伴随窗口而不是游戏，
操作也永远和游戏对不上。改成一个由游戏自己加载的 V2Link 插件后：

- 文字从**游戏进程内**直接交给读屏 DLL，没有中间商；
- 按键在**游戏自己的窗口过程**里接管，翻译成游戏自己的鼠标点击派发；
- 没有第二份焦点、第二个窗口，读屏软件看到的就是唯一那个界面。

## 2. 插件怎么被加载

本作**不会自动加载 `.tpm`**（`Plugins.link` 之外的自动加载路径实测无效），
所以是把启动脚本里的一个槽位替换成 `Plugins.link("a11y.dll")`。

导出三个符号，符合 krkrz 的 V2Link 约定：

```
V2Link(iTVPFunctionExporter*) -> HRESULT
V2Unlink()
GetModuleThreadModel()
```

`iTVPFunctionExporter` 的虚表第 2 项（下标 1）是 `QueryFunctionsByNarrowString`，
用它按**完整签名**取引擎函数，取到的地址实测可用：

```
iTJSDispatch2 * ::TVPGetScriptDispatch()
void ::TVPExecuteExpression(const ttstr &, tTJSVariant *)
void ::TVPExecuteScript(const ttstr &, const ttstr &, tjs_int, tTJSVariant *)
tTJSVariantString * ::TJSAllocVariantString(const tjs_char *)
void ::TVPDoTryBlock(tTVPTryBlockFunction, tTVPCatchBlockFunction, tTVPFinallyBlockFunction, void *)
```

所有与引擎的交互都走 `TVPDoTryBlock` 包一层，脚本异常不往外抛。

## 3. 文本与选项

| 目的 | 取的字段 | 依据 |
|---|---|---|
| 说话人 / 台词 | `kag.historyLayer.currentInfo` 的 `.disp` / `.text` / `.plaintext` | 实机探针 |
| 是否正在显示选项 | `kag.selectShowing` | 实机探针（非选项画面基准值 `0`） |
| 选项清单 | `kag.selectLayer.selects[i]` 的 `name` / `left` / `top` / `width` / `height` | 实机探针 |
| 当前标签 | `kag.currentLabel` | 实机探针 |

**字段含义会翻转**：普通台词 `.text` 是译文、`.plaintext` 是原文；**选项行正好相反**。
所以判据不能用字段名，用「有没有假名」（中文界面里正常不会出现平假名 / 片假名）。

`kag.current` 在**读档画面**上仍然停在旧的消息层，所以它**不能**用来判断「现在是不是在剧情里」；
`kag.selectShowing` 可以。

## 4. 界面控件发现

控件不是 Win32 控件，是 KAG 的图层。做法：

1. 对逻辑分辨率 1920x1080 打 **40px 网格**，逐点问 `kag.getPrimaryLayerAt(x,y)`「这一点上是哪一层」，按层名去重。
   **不要按尺寸过滤掉整屏容器** —— 设置界面的选项全是宽 1928 的容器的子控件，过滤掉容器等于把整页控件丢掉。
2. 对每个收集到的层，枚举它的 `children`：
   - 子控件坐标是**相对父层**的，绝对坐标 = 父层 `left/top` + 子控件 `left/top`；
   - 尺寸过滤：`>= 20x20` 且 `<= 1600x1000`（整屏底图、背景层不是控件）；
   - 全局按名字去重，**先发子控件**（rect 精确）、后补叶子层。
3. 结果按「先上后下、先左后右」排序 = 阅读顺序。
4. 激活 = 对该控件一个「确实命中过」的点发真的鼠标点击（`WM_MOUSEMOVE` + `WM_LBUTTONDOWN/UP`），
   走游戏自己的派发。实测：点标题菜单进入正片、点 `log` 打开回想面板、点存档位选中该槽。

### 两个坑（都真的踩过）

- **坐标必须按浮点解析。** 扫描器发的是控件中心；宽高为奇数时就是 `167.5` 这种半像素值。
  用 `"%d,%d,%d,%d"` 解析，`%d` 读到 `167` 撞上 `.` 就整个中止，后面 `y/w/h` 全部留 0。
  后果不止排序：**激活时点到 `(x,0)`——屏幕最顶端**，看起来就是「点了没反应」。
  现在四段都用 `%lf` 解析再四舍五入。
- **拆字段时必须先把制表符打成字符串结尾**（`*tab = 0;`）。少了它，层名会连父层名一起带走
  （`"helpbase\t1384,99,…,表メッセージレイヤ0"`），而噪声过滤规则是「名字里含 `レイヤ` 就丢」——
  父层名里的 `レイヤ` 被带进来，**每一项都会被丢掉**：扫描恒返回 0 项，
  导航模式里按方向键一片死寂（`g_itemCount == 0` 时方向键分支直接 `return`，什么都不念）。
- **绝不要对这类 TJS 对象用 `for-in`。** 实测会把整个脚本打死，而且 `catch` 不住
  （层对象、`kag.menu.children`、连 `global` 都一样）。只能按索引访问 + 逐个属性 `try/catch`。
- **每一次属性访问都要单独 `try/catch`。** 有些网格点上 `getPrimaryLayerAt` 返回的是一个
  「半个 null」的包装对象，读 `.name` 会抛 `Accessing to null object`；不逐个包住，
  整个扫描脚本就废了（表现为日志里的「探针脚本没有返回」）。

## 5. 键盘

**本作收真键盘**：走 DirectInput / 键盘状态轮询，**不经过窗口消息**。这条有两个直接后果：

- `PostMessage` 送进去的键**游戏看不见**（模式自己的窗口子类化照样收得到）；
  早期"游戏不吃键盘"的结论就是用 `PostMessage` 测出来的，是错的。
- 在窗口过程里 `return 0` 也**拦不住**游戏读到的物理按键。

原生键位（用户实测，并用 `keybd_event` 复验）：空格 / 回车推进、`R` 历史、按住 `Ctrl` 快进。

模式的处理：

- `Tab` / 方向键 / `Home` / `End` / `PgUp` / `PgDn` / `Esc` / `Backspace` 自己接管，不进游戏。
- `Enter` / `Space` 在非导航模式下补一下消息区点击，让「送进游戏窗口的空格」也能推进。
- 测试时要注意：**`keybd_event` 必须先把游戏窗口切到前台**，否则键打在别的窗口上。

### 快进的实测结论

引擎把推进**节流在 ~2.5 句/秒**（约 400ms/句）。三条测量：

| 方式 | 点击频率 | 实际推进 |
|---|---|---|
| 游戏原生 `Ctrl` | — | 1.5–2.5 句/秒 |
| 模式补点击（200ms 一拍） | 5/秒 | 1.75 句/秒 |
| 外部连点（100ms × 2） | 17.8/秒 | 2.3 句/秒 |

点击量翻 3.5 倍，速度只从 1.75 涨到 2.3；而且采样器本身是 5Hz —— 引擎若真更快，
每次采样都会看到新行、计数就该是 5/秒。**所以不要多发点击**，一拍一下刚好。
模式补点击的价值在于「停得准、松手能读回当前句」，不在于更快。

## 6. 设置界面（页面模型）

设置界面不是坐标模型，是**具名项模型**：`option.tjs` 里用 `addCheckItem` / `addUpdate`
动态生成带 getter/setter 的属性，形如

```
property <name> {
  setter(v) { <基础表达式> = (v == 'toggle') ? !(<基础表达式>) : !!v; itemUpdates.<name>(); }
  getter { return !!<基础表达式>; }
}
```

并且游戏自己会生成链接定义，每条选项的命令就是 `Current.cmd("...")`，例如
`fullscreen=false` / `squareMode=true` / `noEffect=false` / `bgAnim=true` /
`setEscape/0` / `setScramble/0..3` / `topMost=true` / `asIcon=toggle` /
`showProg=toggle` / `showMenu=toggle` / `touchMode=toggle` / `faceMode=true` /
`init`（恢复默认），页签是 `Current.page=0..7`，返回标题是 `SystemAction._title()`。

**这意味着以后改设置可以直接调游戏自己的命令，一格坐标都不用点。**（尚未实装。）

每项能读到的值：只有 `kag.afterskip` `afterauto` `voicecut` `voicecutpage` `bgmdown`
`noeffect` `sysnoeffect` 这 7 个。显示类的值试过 `global.SystemConfig.*`、
`System.getArgument(*)`、`global.Current.*`（只有 `cmd` / `func` / `page`）都取不到 ——
值应该在运行时生成的对象上，那个对象还没定位到。

## 7. 其它可用的运行时入口

| 入口 | 用途 | 实测 |
|---|---|---|
| `global.GetSysLangText(id)` | 直接取游戏自己的本地化文本 | `'FontDialogLicenseInfo'` → 中文版权提示 |
| `kag.menu.children[i].caption` | 游戏自己的菜单控件模型（带 `group` / `radio` / `visible`） | `文件(&F)` / `显示(&S)` / `操作(&M)` |

比手写标签表更权威，后续标签应当逐步改成从这两个入口取。

## 8. 第三方依赖

| 组件 | 许可 | 本仓库是否分发 |
|---|---|---|
| TJS2 / 吉里吉里 接口头文件（`tjsCommHead.h` 等 11 个） | 吉里吉里自有许可 或 GNU GPL（双许可） | **是**（`third_party/tjs2`），逐字节原样 + 许可证原文 |
| `nvdaControllerClient.dll` | NV Access（LGPL） | **否**，安装时单独放入 |
| Tolk（可选） | LGPL | **否**，可选放置 |

引擎头文件必须随仓库走，否则「拿到源码就能编出一样的产物」不成立。它们不属于柚子社，
也不含游戏内容；吉里吉里许可的「流用」条款明确允许嵌入别的软件，义务只是在文档里注明 ——
注明放在 `third_party/README.md` 与本文件。

## 9. 已知未完成

- 设置界面显示类选项的**当前值**读不出来（念得出两个选项，念不出当前选的是哪个）。
- 设置界面两条时间滑条只按百分比点击，没有念出游戏实际显示的值。
- 读档界面缺「下一頁」（`page_add`）。
- 破坏性控件（读档 / 删除存档）尚未加二次确认。
- 尚未把标签改成从 `GetSysLangText` 取值。
