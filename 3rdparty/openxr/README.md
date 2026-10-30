OpenXR headers, vendored
========================

`include/openxr/` holds `openxr.h`, `openxr_platform.h` and `openxr_platform_defines.h`
from the Khronos OpenXR-SDK (https://github.com/KhronosGroup/OpenXR-SDK), release
1.1.65, unmodified. Apache-2.0 / MIT dual licence; see LICENSE.

Only the headers are needed: android/src/xr/ loads `libopenxr_loader.so` at run time
through `xrGetInstanceProcAddr` and never links the loader. The loader itself reaches the
APK from Maven (`org.khronos.openxr:openxr_loader_for_android`, see
android/armsx3-ui/gradle/libs.versions.toml).

To update: copy the three headers from a newer SDK release and bump the version here.
