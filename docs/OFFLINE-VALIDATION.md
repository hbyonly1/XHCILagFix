# Offline validation

The release Kext was built as an x86_64 macOS Kext. The saved AppleUSBXHCI samples 24E263 and 24H32 passed the runtime layout, hook-span, and prepare-boundary checks. Each sample produced 794 layout assertions, 538 hook-span assertions, and 1142 prepare assertions. Policy, reservation, controller-lifecycle, collector, and export tests also passed.

This validation did not load the Kext, access a live controller, modify EFI, or claim a hardware result. The separate `docs/audit-review.json` is the static-review record; current single-machine hardware feedback is summarized in the root README and `validation-summary.json`.
