<h1 align="center">USBXHCIWakeup</h1>

<p align="center">用于 macOS AppleUSBXHCI 中断通知延迟的实验性 USB 事件补偿补丁</p>

<p align="center"><a href="README.md">简体中文</a> · <a href="README_EN.md">English</a></p>

---

这是一个针对 macOS AppleUSBXHCI 的实验性补偿补丁，面向一类“USB 设备已经枚举，但存储读取和输入设备响应都周期性停顿”的问题。根本机制尚不清楚，使用者应保留可启动的原 EFI，并自行承担测试风险。

## 前置条件

| 项目 | 本次测试环境 |
|---|---|
| 机型 | ROG Zephyrus G14（2024）GA403UV |
| CPU / iGPU | AMD Ryzen 9 8945HS；Radeon 780M |
| 独立显卡 | NVIDIA GeForce RTX 4060 Laptop GPU |
| macOS 目标 | x86_64 macOS；本次主要在 Sequoia Recovery / 安装器环境测试 |
| USB 接口 | 2× USB 3.2 Gen 2 Type-A（最高 10 Gbps）；1× USB 3.2 Gen 2 Type-C（最高 10 Gbps、DisplayPort）；1× USB4 Type-C（最高 40 Gbps、DisplayPort / PD）；另有 microSD、HDMI 2.1 和 3.5 mm 音频口 |
| 设备映射 | XHC0/XHC1/XHC3/XHC4 使用定制 UTBMap；XHC3、XHC4 的 Type-C 端口按 Type-C + Switch 映射，XHC0 保留外部 USB3 和内部设备端口 |

接口能力参考 [ASUS GA403 官方规格](https://rog.asus.com/us/laptops/rog-zephyrus/rog-zephyrus-g14-2024/spec/)。官方规格说明的是产品型号能力；实际连接关系以本机 IORegistry 和 UTBMap 为准。

### 已启用的关键 Kext 与参数

| 项目 | 版本 / 作用 |
|---|---|
| Lilu | 1.7.3；其他插件的基础 |
| GenericUSBXHCI | 1.3.0b2，GUX-RyzenXHCIFix 的 Ryzen 兼容分支；用于早期 xHCI 探测路径 |
| USBToolBox + UTBMap | USBToolBox 1.2.0、UTBMap 1.1；UTBMap 为本机定制端口映射 |
| USBXHCIWakeup | 0.2.0；本项目的备用事件唤醒路径 |
| AMDRyzenCPUPowerManagement | 17.0.0 |
| SMCAMDProcessor | 17.0.0 |
| VoodooI2C / VoodooI2CHID | 触控板和 I2C 输入链 |
| VoodooPS2Controller | 键盘、鼠标和兼容输入链 |
| AsusSMC、WhateverGreen、AppleALC、AMFIPass、RestrictEvents | 平台、显示、音频和系统兼容 |

本次 EFI 的关键启动参数为：

```text
-v keepsyms=1 debug=0x100 -wegnoegpu -vi2c-force-polling
amfi=0x80 ipc_control_port_options=0 alcid=11
```

同时保留 `XhciPortLimit=false`。固件 IOMMU 关闭。

## 问题表现

在我的 ROG Zephyrus G14 GA403UV 笔记本上，问题表现为：

- 键盘随着重启会随机性的出现输入延迟、重复字符，2.4 GHz 无线鼠标几乎无法移动和点击，USB-C 有线网卡也可以枚举，但延迟极高几乎不可用，只有触控板可以正常使用；
- 离线安装盘读取极慢，实测经过三个多小时才能进入系统，有用户也报道过和我几乎一样的时间；
- 设备列举正常，协议正常，IORegistry 可以看到控制器和设备，链路也可能显示为 USB 3、5 Gbps，但实际请求仍然长时间停顿。

这与一些 AMD OS X 社区报告的现象相似。第一篇是 Ryzen 7 6800H / Radeon RX 6850M XT 笔记本，报告了安装 Sonoma 时键盘鼠标极慢、USB 存储几乎不可用但设备仍能被识别；讨论中有用户提到有极大可能与 USB4 相关控制器兼容性有关。第二篇是同为 Ryzen 9 8945HS 的 Minisforum UM890 Pro，报告 Recovery 中无线键盘鼠标响应极慢。同样配有 USB4 接口，它们说明问题可能集中出现在 AMD Ryzen、USB4/XHCI 和 macOS 安装环境的组合上，截至目前仍未确认与“USB4”的联系：

- [R7 6800H / RX 6850M USB installation issue](https://forum.amd-osx.com/threads/r7-6800h-rx6850m-laptops-having-usb-issues-while-installing-sonoma.4726/)
- [Minisforum UM890 Pro / Ryzen 9 8945HS slow wireless input](https://forum.amd-osx.com/threads/minisforum-um890-pro-amd-ryzen-9-8945hs-w-radeon-780m-graphics.5900/)

## 调查过程与结论

调查通过多轮日志、IORegistry、驱动反汇编和有限的事件采样，把问题从“USB 设备整体变慢”缩小到一次完成事件的中断通知链。日志确认存储设备使用 BOT 协议，但 BOT 的 CBW、数据和 CSW 不是停顿的原因。

一次读取所涉及的边界可以写成：

```text
块层请求 / SCSI READ
        ↓
BOT：CBW → Data → CSW
        ↓
xHCI Transfer TRB / Event Ring
        ↓
MSI 或其他中断通知
        ↓
XNU 中断入口
        ↓
AppleUSBXHCI Filter / Action / Event 消费
```

最关键的观测时间线是：

```
USB Transfer Event 已写入 Event Ring
        ↓ 约 40–50 ms（导致一切延迟的罪魁祸首！！）
XNU 硬件中断入口
        ↓ 微秒级
AppleUSBXHCI Filter / Action / Event 消费
```

也就是说，完成事件已经存在，原驱动的软件处理也很快；主要空白位于事件有效到 XNU 中断路径开始之间。仍然不能单独确认延迟发生在 xHCI 生成 MSI 之前、PCIe/SoC 投递过程中、Local APIC 到 CPU 接受之间，还是共享中断后来由其他 Event Ring 活动顺带唤醒。

### 与 QEMU Issue #2705 的关系

[QEMU Issue #2705](https://gitlab.com/qemu-project/qemu/-/issues/2705) 讨论的是 macOS 客户机在 xHCI 中断映射不正确时，非 Ring 0 的 Event Ring 事件无法及时通过 MSI/MSI-X 或引脚中断送达；相关修复讨论包括禁用不支持的 Interrupter，或在映射不支持时把事件统一放到 Ring 0。

本机和该问题有一个相似点：均为中断异常导致的 USB 设备延迟。但后来调查确认，本机三个 Interrupter（Ring 0、1、2）按照 xHCI 规范通过单个 MSI 消息共享 CPU Vector 116，Ring 1 的 Target=1，映射本身是正确的；真正发现的问题是 Ring 1 的 Transfer Event 已经有效，却在进入 XNU 中断入口前延迟约 40–50 ms，而 Filter、Action 和事件消费仅需微秒级。因此，正确原因应表述为 Ring 1 完成事件的中断通知或投递路径异常延迟，而不是 Interrupter 映射错误；MSI 生成与投递的具体环节尚未直接观测。

经过多轮日志、反汇编和重启，原因只能粗略定位于此；继续采集仍缺少“MSI 写事务实际发生时刻”的现成观测点，且和 AI 沟通已经不能取得太多新进展。调查因此停止，补丁定位为绕过症状的实验方案，而不是针对硬件机制的根本修复。

## 当前修复思路

因只有触摸板正常工作，受其方案（`-vi2c-force-polling`）的启发，我决定也采取轮询的思想来绕过中断通知问题；
该补丁保留正常 MSI 路径，并增加一个备用唤醒路径：

1. 在每个控制器自己的原工作循环上检查实际事件环；
2. 对稳定有效且持续未消费的事件进行确认；
3. 默认每 **100 µs** 请求一次检查，事件稳定滞留 **200 µs** 后才触发补偿；
4. 在原工作循环内调用 AppleUSBXHCI 自己的 `manuallyProcessInterrupts()`；
5. 由原驱动消费事件并完成正常的 ERDP/EHB 更新。

100 µs 的检查时间和 200 µs 的滞留时间是根据当前测试定下的工作参数，经测试此参数工作良好。
补丁本身不更改 Interrupter Target、MSI/MSI-X、IMAN、IMOD 或中断配置，也不移动 Ring consumer。未知的私有 ABI、对象关系或生命周期覆写会拒绝启用。

## 未知影响

目前尚不能证明：

- 100 µs 检查在所有机器上的 CPU 占用和功耗；
- 睡眠、唤醒、热插拔和多控制器同时工作时的稳定性；
- 手动补偿是否会触发某些未发现的驱动锁交错；
- 长时间运行或异常掉电时是否存在数据一致性风险。

当前大文件读取的 MD5 多次一致，说明 USB 读取结果稳定可重复。原生驱动仍负责消费 Event Ring，补丁不直接伪造事件或改写 MSI 配置。

## 当前测试反馈

启用补偿路径后，据我实测报告（仅测试了我的机器）：

- USB 读取速度恢复正常，在 USB 3.0 的实际链路速度下，离线安装镜像加载时间从三个多小时降到三分钟左右，且成功安装并进入了系统；
- 2.4 GHz 无线鼠标和键盘恢复正常响应，USB-C 有线网卡可用，ping 延迟正常；
- USB-A 端口和 USB-C 端口似乎正常工作；
- 之前用于启动的 USB 3.0 SG Flash 会直接异常退出，现在也可以读取；

## 使用范围

- macOS 11 及以后、x86_64、AppleUSBXHCI；
- 需要 Lilu 1.7.3 或兼容版本，并放在本 Kext 之前；
- 不保证所有 AMD/Intel 控制器和所有 macOS 版本都能启用；未知私有 ABI 会安全拒绝启用；
- 100 µs 是请求的检查周期，不是硬实时保证；CPU 占用、睡眠唤醒、热插拔和多控制器压力仍需更多实机测试。

默认自动运行。遇到异常时可以使用启动参数 `-xhciwakeupoff` 禁用，或回滚到原 EFI。

## 应用方法

1. 备份当前 EFI，把 `Kexts/USBXHCIWakeup.kext` 复制到 EFI 的 `EFI/OC/Kexts/`。
2. 将 `OpenCore/Kernel-Add.plist` 中的条目加入 `config.plist → Kernel → Add`，确保它位于 Lilu 之后并启用。

## 目录内容

- `Kexts/USBXHCIWakeup.kext`：已构建的 x86_64 Kext；
- `OpenCore/Kernel-Add.plist`：Kernel → Add 条目片段；
- `collector/`：只读状态导出和 start/stop 控制工具；
- `src/`、`tests/`、`build.sh`：源码和离线验证材料；
- `licenses/`：Lilu、MacKernelSDK、HDE64 许可文本；
- `validation-summary.json`：构建和离线检查摘要。

安装时不需要运行 collector 。collector 只用于确认逐控制器状态、唤醒调用和进展计数。

## 验证边界

`manuallyProcessInterrupts()` 会走原驱动的完成路径，因此原驱动可能正常写入硬件状态寄存器。补丁没有主动重写这些寄存器。原生完成回调在极端生命周期交错下的锁行为尚未穷尽验证，发布前请保留可启动的原 EFI 和回滚选项。

