# Lo que decide, aparte del módulo: las pruebas lo enlazan sin WirePlumber.
# `service.c` necesita GIO —es la interfaz D-Bus— pero tampoco WirePlumber.
equalizer_core = static_library('equalizer-core',
    'presets.c',
    'state.c',
    'service.c',
    dependencies: dependency('gio-2.0'),
    # Se enlaza dentro de un módulo compartido.
    pic: true,
    install: false)

equalizer_core_dep = declare_dependency(
    link_with: equalizer_core,
    dependencies: dependency('gio-2.0'),
    include_directories: include_directories('.'))

equalizer_module = shared_module('wireplumber-module-vasak-equalizer',
    'module.c',
    dependencies: [wireplumber, equalizer_core_dep],
    name_prefix: 'lib',
    install: true,
    install_dir: moduledir)
