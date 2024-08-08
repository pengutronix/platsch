project(
  'platsch',
  'c',
  version : '2024.08.0',
  default_options : [
    'warning_level=2',
  ],
  license : '0BSD',
)

libdrm_dep = dependency('libdrm', version : '>=2.4.112')

install_headers('libplatsch.h')

platsch_lib = both_libraries(
  'platsch',
  version : '0.1',
  sources : ['libplatsch.c'],
  gnu_symbol_visibility : 'hidden',
  dependencies : [libdrm_dep],
  install : true
)

pkg_mod = import('pkgconfig')
pkg_mod.generate(
  name : 'libplatsch',
  version : '0.1',
  filebase : 'libplatsch',
  description : 'A Library to build custom bootsplash applications.',
  libraries : platsch_lib
)

executable(
  'platsch', 
  sources : ['platsch.c'],
  link_with : platsch_lib.get_static_lib(),
  install : true,
  install_dir : get_option('sbindir'),
)
