# The framegen library inside Gamescope's build; CMakeLists.txt builds the same sources on their own.
framegen_lib = static_library(
  'framegen',
  [
    'src/ComputeImage.cpp',
    'src/Device.cpp',
    'src/FramePacer.cpp',
    'src/LosslessGenerator.cpp',
    'src/LosslessImages.cpp',
    'src/LosslessPipelines.cpp',
    'src/LosslessShaders.cpp',
    'src/PeResources.cpp',
    'src/SpirvInterface.cpp',
  ],
  include_directories: include_directories('include', 'src', '../thirdparty/SPIRV-Headers/include/spirv/unified1'),
  dependencies: vulkan_dep.partial_dependency(compile_args: true, includes: true),
)

framegen_dep = declare_dependency(
  link_with: framegen_lib,
  include_directories: include_directories('include'),
)
