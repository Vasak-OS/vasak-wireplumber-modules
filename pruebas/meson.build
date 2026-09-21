test('identidad', executable('pruebas-identidad', 'identidad.c',
    dependencies: decisiones_dep, install: false))

test('camara', executable('pruebas-camara', 'camara.c',
    dependencies: decisiones_dep, install: false))

test('servicio', executable('pruebas-servicio', 'servicio.c',
    dependencies: decisiones_dep, install: false))

# El orden de los enganches. No lo ve el compilador, y cuando está mal el
# módulo no niega nada **y no se queja**.
test('el-orden-del-enganche', find_program('el-orden-del-enganche.sh'))
