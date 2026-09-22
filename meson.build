project('vasak-wireplumber-modules', 'c',
    version: '0.1.0',
    license: 'GPL-3.0-or-later',
    meson_version: '>=0.60.0',
    default_options: ['c_std=c11', 'warning_level=2'])

# La versión de la API, en un solo lugar: se usa para pedir la dependencia y
# para armar el directorio de módulos, y que se separen sería instalar contra
# unos encabezados y depositar el resultado al lado de otros.
wp_api = '0.5'

wireplumber = dependency('wireplumber-' + wp_api)

# Adónde van los módulos.
#
# WirePlumber **no** lo publica en su pkg-config —sólo trae prefix, includedir
# y libdir—, así que a diferencia de Wayfire no se le puede preguntar y hay que
# armarlo. Si algún día lo publica, esto pasa a ser un get_variable() y se
# borra el comentario.
#
# Se comprueba que exista en vez de confiar: un módulo instalado en el lugar
# equivocado no falla al compilar ni al empaquetar. WirePlumber arranca sin él
# y la cámara queda abierta, que es la forma de fallar que este repositorio
# existe para no tener.
moduledir = join_paths(wireplumber.get_variable(pkgconfig: 'libdir'),
                       'wireplumber-' + wp_api)

if not import('fs').is_dir(moduledir)
    warning('No existe @0@. WirePlumber pudo haber cambiado dónde busca sus módulos: comprobar antes de empaquetar.'.format(moduledir))
endif

# El fragmento que declara el componente. Va al directorio de WirePlumber y no
# al de configuración del sistema: es lo que trae el paquete, y quien quiera
# cambiarlo lo hace en /etc o en ~/.config, que le ganan.
install_data('datos/50-vasak-permisos-de-medios.conf',
    install_dir: get_option('datadir') / 'wireplumber' / 'wireplumber.conf.d')

subdir('src/permisos-de-medios')
subdir('pruebas')
