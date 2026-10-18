# 构建 XHCILagFix

在安装了 Xcode Command Line Tools 的 macOS 上准备 Lilu 和 MacKernelSDK 源码。依赖不提交到本仓库，可通过环境变量指定：

```sh
export XHCI_KERNEL_SDK=/path/to/MacKernelSDK
export XHCI_LILU_SOURCE=/path/to/Lilu
sh build.sh
```

也可分别放在 `deps/MacKernelSDK`、`deps/Lilu`。输出为 `build/XHCILagFix.kext`，仓库内的 `Kexts/XHCILagFix.kext` 是随项目提供的预编译版本。重新构建不会自动覆盖 EFI 或仓库的预编译版本。

```sh
python3 validate.py
```

验证脚本重新构建，并运行 ARM64/x86_64 用户态离线测试；在 Apple Silicon 上运行 x86_64 测试需要 Rosetta。生成的 Kext 本身仅为 x86_64。脚本不加载 Kext、不修改 EFI，也不访问实际 USB 设备。

如需额外核查已授权提取的参考内核镜像与本地 Lilu 二进制的导入符号、IOService虚函数表，可指定：

```sh
export XHCI_REFERENCE_KERNEL=/path/to/com.apple.kernel.macho
export XHCI_LILU_BINARY=/path/to/Lilu.kext/Contents/MacOS/Lilu
python3 validate.py
```

不提供参考镜像时，这两项审计会明确记录为未执行，其余测试照常运行。详细报告输出到 `build/validation.json`；仓库的 `validation-summary.json` 记录打包时的结果。离线检查通过不代表 macOS 26 已经实机兼容。
