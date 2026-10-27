# libretro.h

`libretro.h` is taken unchanged from libretro-common (https://github.com/libretro/libretro-common,
at the revision used by melonDS DS v1.4.0, fa8a1b572a444fcb552007019f8caf68281108aa).
License: MIT, Copyright (C) 2010-2024 The RetroArch team; the full license text is in the
header of `libretro.h` and applies only to that header file. The header is used for
compilation only; the core itself is loaded at runtime via QLibrary (no linking).

`libretro_vulkan.h` (Vulkan hardware render interface v5 and context negotiation interface v2) is taken unchanged
from the same libretro-common revision (MIT, see its header comment). It includes `<vulkan/vulkan.h>`, which is not
vendored: Linux uses the system headers (`libvulkan-dev`), Windows the header-only vcpkg port `vulkan-headers`.
Uzume never links the Vulkan loader; `emulation/vk_render.cpp` loads it at runtime (QLibrary).
