## Ember
A small 2D C++ Gamedev Library, with a focus on being lightweight and efficient.

### Building

- Requires C++20 and CMake 3.5+
- At least one **RHI** implementation must be enabled in CMake:
    - [Vulkan](https://github.com/maze7/Ember/blob/main/include/ember/rhi/vulkan) (Default on Linux/MacOS) `EMBER_VULKAN`
    - [DX12](-) (Not currently implemented, but support is planned) `EMBER_DX12`
