project('7mm_recorder', ['c', 'cpp'],
  version: '1.0',
  default_options: [
    'warning_level=2',
    'c_std=gnu11',
    'cpp_std=c++17'
  ]
)

inc = include_directories('include')

spdlog_dep = dependency('spdlog')
yaml = dependency('yaml-cpp', required: true)
cfitsio_dep = dependency('cfitsio', required: true)
zmq_dep = dependency('libzmq', required: true)

receiver_lib = static_library('zmq_receiver',
  'src/zmq_receiver.cpp',
  include_directories: inc,
  dependencies: [cfitsio_dep, zmq_dep, spdlog_dep, yaml]
)

executable('7mm_recorder',
  ['src/main.cpp',
  'src/sdfits.cpp',
  'src/ContinuumFits.cpp'
  ],
  include_directories: inc,
  dependencies:  [cfitsio_dep,yaml,spdlog_dep,zmq_dep],
  link_with: receiver_lib,
)

time_utils_test = executable('time_utils_test',
  'tests/time_utils_test.cpp',
  include_directories: inc,
)
test('time_utils', time_utils_test)
