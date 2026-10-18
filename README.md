# XHCILagFix (Ring 0)

[English](README_EN.md)

用于修复部分 x86_64 macOS 平台上 USB 端口出现的严重延迟问题，表现为鼠标卡顿几乎不可移动、键盘重复输入和 USB 存储器读取极慢的问题。补丁通过 Lilu 接入 AppleUSBXHCI，将四类 USB 传输的完成事件目标统一重定向到 **Interrupter 0 / Event Ring 0**。

本仓库原名 USBXHCIWakeup。新版已用独立的 **XHCILagFix.kext 0.1.0** 完整替换旧轮询补偿实现：不再等待事件积压后手动唤醒驱动，而是在请求提交前改变硬件将采用的目标环。

## 测试状态

- **macOS 15 Sequoia 已测试**：AMD Ryzen 9 8945HS / ROG Zephyrus G14 GA403UV，此前严重的 USB 输入延迟问题已改善、设备恢复可用。
- **macOS 26 Tahoe 已测试**。

理论上来说较新系统应全部支持，旧系统未测试，尚不知道苹果何时对 XHCI 进行了较大改动。

## 问题机制

经过反复抓取日志观察到：USB Transfer Event 已经在 Ring 1 中有效，但对应 XNU 中断入口往往约 50 ms 后才执行，部分样本更长；进入 XNU 中断入口后，AppleUSBXHCI 的 Filter、Action 和事件消费通常只需微秒级。

```text
USB 传输完成，Ring 1 中事件已经有效
                 ↓
          数十毫秒等待（罪魁祸首！！）
                 ↓
         XNU 硬件中断入口
                 ↓
       Filter / Action / 事件消费
             通常为微秒级
```

因此，本项目处理完成事件产生之后的通知路径。Event 有效无法说明 MSI 写事务已发送，无法仅凭已有日志给 MSI、APIC 或 CPU 定责。

## 我的猜测：虽能启用多个 Interrupter，但不代表 Interrupter Mapping 可用

这一思路受到 QEMU 的 xHCI 故障及其修复启发。[QEMU 的 MSI 映射补丁](https://lists.nongnu.org/archive/html/qemu-devel/2024-12/msg03236.html)修正了辅助 Interrupter 到已启用 MSI 消息的取模映射，解决了与 macOS 驱动配合时的问题。该修复说明，多 Interrupter 与少量 MSI 消息之间的兼容存在问题。

[xHCI 1.2b 规范 §4.17.1 Interrupter Mapping](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf#page=263)写道：

> If the Number of Interrupters (MaxIntrs) field is greater than 1, then Interrupter Mapping shall be supported.

以及：

> If Interrupter Mapping is not supported, the Interrupter Target field shall be ignored by the xHC and all Events targeted at Interrupter 0.

我的观点与原作者几乎相同：**在受影响的平台与系统组合上，不能只根据 MaxIntrs > 1，就确定 Interrupter Mapping 及辅助 Interrupter 的通知路径实际可用。** 

`shall be supported` 是规范对实现的要求，而具体实现应根据硬件的实际情况判断。尤其需要区分：

- **事件映射到哪个 Ring**：本机已观察到 Transfer Event 写入 Ring 1，不能说环内存完全不可用。
- **该 Interrupter 能否及时通知 CPU**：这是长时间等待所指向的可疑环节。

据此把 **辅助 Interrupter 1/2 的通知路径不能使用** 作为结论（具体机制尚不清楚）。Ring 2 尚不足以单独判定故障。根据 Linux 和 Windows 仅启用 Ring 0 的行为，修复的目标是绕过 Ring 1 和 Ring 2 。

单条 MSI 消息也不是错误原因：规范允许多个 Interrupter 映射到较少的 MSI 消息，但 **前提是映射机制工作**。QEMU 的取模修复改变的是通知消息选择，事件仍留在原来的 Ring。因为我们无法观测真机硬件内部行为，即使观测到也无法修改，因此本补丁则直接改变传输的 Target，使这些事件走 Ring 0。

## 为什么不直接将 MaxIntrs 改成1

在已反汇编审计的 AppleUSBXHCI 版本中，驱动要求至少三个 Interrupter，并固定初始化三组用途：

| Interrupter / Ring | 原驱动用途 |
| --- | --- |
| 0 | 命令完成、端口状态变化、MFINDEX Wrap 等主事件 |
| 1 | Control、Bulk、Interrupt 等非等时端点的传输事件 |
| 2 | Isochronous 等时端点的传输事件 |

因此，直接伪造 MaxIntrs=1 会与该驱动的启动要求冲突。我们的解决方法是：**保留驱动创建三个 Interrupter 的流程，把能够改写的传输目标统一设为0。** Ring 1/2 无需被关闭，控制器报告的 MaxIntrs 和 PCI MSI 配置也不需要修改。

## 具体改动：三个 Hook

### 1. 普通 TransferRing::enqueueRequest

覆盖 Control、Bulk、Interrupt 请求。在原 enqueueRequest 执行之前，所有 prepare 子类已经完成 TD 构造，但新请求尚未由原函数连接并发布给控制器。

遍历完整请求 TD 队列，将有效 TRB 的 Interrupter Target 清零：

```cpp
trb.dw2 &= ~0xffc00000U; // DW2[31:22] = Interrupter Target = 0
```

保留 DMA 地址、DW2低22位、DW3、IOC、BEI、Cycle 和队列拓扑。Normal、Setup/Data/Status、Link、Event Data、No Op Transfer 等已支持类型均在提交前统一处理。

### 2. IsochronousTransferRing::enqueueRequest

对等时请求采用相同处理，覆盖 Isoch TRB 及关联描述符。因此 USB 四种传输类型都进入本版改写范围，不区分键鼠、U盘、音频设备、VID/PID 或端口。

选择 enqueue 而不是 addTD，是因为最后一个 TD 可能直接入队而绕过 addTD，Control 子类也可能在基类 prepare 之后继续追加描述符。在两个 enqueue 入口处理完整队列，能覆盖已审计路径中的这些情况。

### 3. CommandRing::setAddress

部分缺少可用目标 TRB 的错误事件会使用 Slot Context 中的备用 Target，仅修改传输 TRB 不覆盖这条选择路径。

补丁在 Address Device 提交前制作 32 字节 Slot 输入副本，将副本 Target 设为0，再交给原函数。原驱动同步复制到自己管理的 DMA Input Context，栈副本不会直接作为硬件异步访问的 DMA 内存。原函数对源 DW3 的清理效果保留，源 DW2 不被直接覆盖。

已审计的其他 Configure Endpoint 路径不更新 Slot Target，EP0 Evaluate 路径没有设置更新 Slot 所需的 A0，因此不为它们增加冗余 Hook。未来或第三方额外的 Slot Evaluate A0 路径不在当前覆盖范围。

## 通用性

- 按已加载 AppleUSBXHCI 的运行时符号定位。
- 不检查系统 build、UUID 或入口字节白名单，不限定某个 AMD PCI ID、设备或端口。
- 只监听 AppleUSBXHCI，不要求 IOUSBHostFamily 的 UUID 匹配；请求改写也不依赖端点描述符识别成功。
- Lilu 负责对实际入口建立跳板；插件版本范围覆盖 macOS 26 所用的 Darwin 25，OpenCore 条目的 MinKernel/MaxKernel 留空。需要支持对应系统的 Lilu，本项目最低依赖版本为1.7.3。
- 保留空指针、请求未发布、队列和 TRB 类型检查；整条队列验证后才改写。读取实际 TD 容量，每TD最多256槽、每请求最多256个TD，避免越界或部分改写。

## 安装

1. 将 `Kexts/XHCILagFix.kext` 放入 `EFI/OC/Kexts`。
2. 在 OpenCore `Kernel/Add` 中启用该插件，放在 Lilu 后面。条目参考见 [OpenCore/Kernel-Add.plist](OpenCore/Kernel-Add.plist)，不要用它整份覆盖配置。
3. 禁用旧的 USBXHCIWakeup、 GenericUSBXHCI 或其他同类修复类似问题的插件，避免重复改写。
4. 重启生效。

需要停用时，启动参数使用 `-xhcilagfixoff`，也兼容 `-xhciallrouteoff`。本插件不支持运行时卸载。

## 查看是否生效

```sh
ioreg -r -c XHCILagFix -l -w0
```

| 字段 | 含义 |
| --- | --- |
| Version | 独立插件版本，当前为0.1.0 |
| Active | 三个核心 Hook 已全部安装并启用改写 |
| Failure | 符号解析或跳板安装失败的说明，成功时为空 |

这里只显示加载与 Hook 安装状态。状态仅在服务启动和驱动加载完成时发布；`Active=Yes` 表示补丁已启用，实际效果以设备表现为准。

## 项目内容与验证

```text
src/                    独立补丁源码，221行，含注释和空行
Kexts/XHCILagFix.kext/   预编译插件
OpenCore/               Kernel/Add条目参考
tests/                  离线队列和原函数转发测试
build.sh                构建脚本
validate.py             离线验证脚本
BUILDING.md             构建方法
validation-summary.json 验证摘要
```

离线测试覆盖四类队列改写、不同 TD 容量、位域保留、原函数参数及返回值、Slot 私有副本、符号缺失和部分安装后的转发。

构建方法见 [BUILDING.md](BUILDING.md)。
