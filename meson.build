project('7mm_writer', ['c', 'cpp'],
  version: '1.0',
  default_options: ['warning_level=2', 'c_std=gnu11', 'cpp_std=c++20']
)

inc = include_directories('include')

dpdk_dep = dependency('libdpdk', required: true)

spdlog_dep = dependency('spdlog')
yaml = dependency('yaml-cpp', required: true)
cfitsio_dep = dependency('cfitsio', required: true)

dpdk_lib = static_library('dpdk',
  'src/dpdk.cpp',
  include_directories: inc,
  dependencies: dpdk_dep
)

executable('Writer',
  ['src/main.cpp',
  'src/sdfits_writer.cpp'
  ],
  include_directories: inc,
  dependencies:  [cfitsio_dep,yaml,spdlog_dep],
  link_with: dpdk_lib,
)
