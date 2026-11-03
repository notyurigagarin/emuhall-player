# Vulkan headers (Khronos Group)

The C headers EmuHall's player compiles its Vulkan presentation against (`crates/emuhall-core-host/native/vkhost.c`).
No Vulkan library is shipped: the player loads the system's Vulkan loader at run time.

- Upstream: https://github.com/KhronosGroup/Vulkan-Headers, version 1.4.362 (`VK_HEADER_VERSION 362`).
- Copied unchanged from `3rdparty/vulkan/include` of the vendored PCEE2 source archive
  (`third-party/pcee2/pcee2-libretro-5719627731beb90e379ff1be1223391b7f563b3b-source.tar.gz`), which carries them
  unmodified; `vk_mem_alloc.h` (a separate AMD library) was left out.
- Copied: 2026-10-31.

## License

Apache-2.0 OR MIT, as stated in `LICENSE.md` and `LICENSE.txt` (Khronos).

## EmuHall modifications

None.
