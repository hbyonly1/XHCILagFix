# 状态导出和暂停／恢复

`wakeupctl-x86_64` 适用于 AMD／Intel 的 x86_64 macOS。它只访问 `USBXHCIWakeup` 的 IOKit 控制接口，不要求选择磁盘、不执行读取测速、不启用内核追踪。100 µs 是请求的轮询间隔，200 µs 是事件滞留阈值，实际调度可能更晚。插件默认自动运行，以下 `start` 命令通常不需要执行。

把整个 `collector` 文件夹复制到 card 根目录。在 Recovery 终端执行：

```sh
sh /Volumes/card/collector/collect.sh check /Volumes/card
```

返回的是一份状态快照。完整属性和每个控制器的 `Controllers` 记录都保存在 `status.plist`，便于阅读的内容保存在 `status.txt`。是否得到软件唤醒、是否在调用后出现进展，以这些记录为准；仅有计数不能单独证明性能恢复或进展由唤醒导致。

需要手动暂停或恢复时：

```sh
sh /Volumes/card/collector/collect.sh stop
sh /Volumes/card/collector/collect.sh start
```

也可在 `start`／`stop` 后追加从 `Controllers` 中读到的十进制 `ControllerRegistryID`，只控制一个控制器。正常 macOS 中，控制命令可能需要在前面加 `sudo`；Recovery 终端通常已经是 root。暂停会停止本插件的备用唤醒，不改变原有 MSI 配置。恢复仍需通过插件当前的运行时与生命周期检查；命令成功返回不代表每个控制器都已满足运行条件，请看导出的逐控制器状态。

每次操作都先在临时目录收集，然后复制到明确的导出位置并逐文件校验。`check` 省略导出目录时以及 `start`／`stop`，默认导出到脚本所在目录。看到 `Exported and byte-verified results:` 后，将对应的整个 `usb-xhci-wakeup.XXXXXX` 目录带回来。复制失败会报错，并保留临时目录原件；不会把临时路径冒充为已导出的路径。控制命令失败时也会尽量保存当前属性与错误；非零退出状态应与 `control-errors.txt` 一起阅读。

离线构建和自检：

```sh
sh build.sh
./wakeupctl-x86_64 --selftest
```

自检只检查命令字典和编号解析，不访问 IOKit，也不会暂停或恢复任何真实控制器。`--mock-plist FILE` 只供 `check` 离线测试使用；导出内容会明确记录 `ControlToolMock = true`。
