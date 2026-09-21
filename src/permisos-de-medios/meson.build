# Lo que decide algo va en su propia biblioteca estática, para que las pruebas
# la puedan enlazar sin arrastrar WirePlumber: correr las pruebas no necesita un
# servidor andando, ni una sesión.
decisiones = static_library('decisiones',
    'camara.c',
    'identidad.c',
    'servicio.c',
    dependencies: dependency('glib-2.0'),
    install: false)

decisiones_dep = declare_dependency(
    link_with: decisiones,
    dependencies: dependency('glib-2.0'),
    include_directories: include_directories('.'))

shared_module('wireplumber-module-vasak-permisos-de-medios',
    'modulo.c',
    dependencies: [wireplumber, decisiones_dep],
    name_prefix: 'lib',
    install: true,
    install_dir: moduledir)
