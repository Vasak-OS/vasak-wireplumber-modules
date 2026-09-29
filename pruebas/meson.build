test('identidad', executable('pruebas-identidad', 'identidad.c',
    dependencies: decisiones_dep, install: false))

test('camara', executable('pruebas-camara', 'camara.c',
    dependencies: decisiones_dep, install: false))

test('servicio', executable('pruebas-servicio', 'servicio.c',
    dependencies: decisiones_dep, install: false))

# La tabla de decisiones: la línea que decide si un cliente tiene cámara. Va
# después de las otras porque es la que más importa, no porque dependa de ellas.
test('decisiones', executable('pruebas-decisiones', 'decisiones.c',
    dependencies: decisiones_dep, install: false))

# El orden de los enganches. No lo ve el compilador, y cuando está mal el
# módulo no niega nada **y no se queja**.
test('el-orden-del-enganche', find_program('el-orden-del-enganche.sh'))

# Por qué método consulta. Tampoco lo ve el compilador —el enganche vive en
# `modulo.c`, que no compila acá— y el método equivocado no rompe la cámara:
# rompe el aviso y el registro del intento, que es lo que la dejaba sin poder
# desbloquearse.
test('el-metodo-de-la-consulta', find_program('el-metodo-de-la-consulta.sh'))

# Que no vuelva a haber un camino vivo que pregunte con el método que abre un
# diálogo. Complementa al anterior: éste mira el enganche, aquél el archivo.
test('sin-camino-de-dialogo', find_program('sin-camino-de-dialogo.sh'))

# Que el módulo viaje apagado y que el fragmento que lo declara se instale.
test('viene-encendido', find_program('viene-encendido.sh'))
