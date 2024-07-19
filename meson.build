project(
  'platsch',
  'c',
  version : '2019.12.0',
  default_options : [
    'warning_level=1',
  ],
  license : '0BSD',
)

libdrm_dep = dependency('libdrm', version : '>=2.4.112')

executable(
  'platsch', 
  sources : ['platsch.c'], 
  dependencies : [libdrm_dep],
  install : true,
  install_dir : get_option('sbindir'),
)
