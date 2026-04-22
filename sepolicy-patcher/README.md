# sepolicy patcher

KernelSU/Magisk-style module that:
- loads `sepolicy.rule` at boot
- mirrors Frida's Android SELinux patch as closely as static `sepolicy.rule` allows
- adds `frida_file` and `frida_memfd` types
- adds broad Frida rules like `allow domain domain process execmem`
- keeps awesomeCAM-specific `cameraserver` -> `/data/camera` / `system_lib_file` allowances
- relabels `/data/camera/*.so` to `u:object_r:system_lib_file:s0` in `post-fs-data.sh`

## Install
- zip and install in KernelSU Manager, or
- copy folder to `/data/adb/modules/sepolicy-patcher`


## Notes
- This module copies Frida's patch from `frida-core/lib/selinux/patch.c` as closely as possible.
- Frida's runtime uses a special target `$self` for some rules. Static KernelSU `sepolicy.rule`
  cannot express `$self`, so this module uses `self` as the closest approximation.
- Frida also adds some rules only if the type exists on the running device. Those lines are
  commented out in `sepolicy.rule`.
- `execmem` is broad. Keep only if you still need Frida/shadowhook path.
- If AVCs still appear, inspect with:
  - `su -c 'dmesg | grep -i avc | tail -n 100'`
