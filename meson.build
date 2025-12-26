project('wlpinyin', ['c'], default_options : ['warning_level=3', 'c_std=c23'])

add_project_arguments([
  '-Wno-gnu-zero-variadic-macro-arguments', '-Wno-disabled-macro-expansion', '-Wno-strict-prototypes', '-Wno-unsafe-buffer-usage',
  '-Wformat',
  '-D_GNU_SOURCE',
], language: 'c')

wl_client = dependency('wayland-client')
wl_protocols = dependency('wayland-protocols')
pinyin = dependency('rime')
xkbcommon = dependency('xkbcommon')
cc = meson.get_compiler('c')
rt = cc.find_library('rt', required: false)
scanner = find_program('wayland-scanner')
scanner_private_code = generator(scanner, output: '@BASENAME@-protocol.c', arguments: ['private-code', '@INPUT@', '@OUTPUT@'])
scanner_client_header = generator(scanner, output: '@BASENAME@-client-protocol.h', arguments: ['client-header', '@INPUT@', '@OUTPUT@'])

wl_protocols_dir = wl_protocols.get_variable(pkgconfig: 'pkgdatadir')
xdg_shell = wl_protocols_dir + '/stable/xdg-shell/xdg-shell.xml'
text_input_path = wl_protocols_dir + '/unstable/text-input/text-input-unstable-v3.xml'
protocols = ['input-method-unstable-v2.xml', text_input_path, 'virtual-keyboard-unstable-v1.xml', xdg_shell]
protocols_src = scanner_private_code.process(protocols)
protocols_headers = scanner_client_header.process(protocols)
protocols_dep = declare_dependency(sources: [protocols_src, protocols_headers], dependencies: wl_client)

executable('wlpinyin', ['main.c', 'im.c', 'rime_engine.c', 'config.c'], dependencies: [wl_client, pinyin, xkbcommon, protocols_dep, rt], install: true)
