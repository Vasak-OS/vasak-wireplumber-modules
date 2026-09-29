# Saca los comentarios de bloque y deja el código, una línea por línea.
#
# Para las pruebas de fuente de `pruebas/`: miran **código**, no prosa. Estas
# dos cosas no se pueden confundir —
#
#   g_dbus_connection_call (…, VASAK_METODO_CHECK, …)   ← una llamada
#   /* `CheckPermissionFor` no anota nada */           ← la explicación
#
# — y una puerta que no distingue las dos se activa con la advertencia escrita al
# lado. El costo de eso no es un falso rojo: es que la próxima vez que alguien
# escriba por qué no hay que hacer algo, lo borre, que es exactamente lo
# contrario de lo que la explicación sirve.
#
# `//` no se quita: este repositorio no los usa, y un recognizador que los
# desarmara sin necesidad sería otra cosa que mantener.
#
# Las líneas que quedan sin código no se imprimen. Un `grep` sobre un archivo
# que el filtro hubiera vaciado tiene que notarse, y no pasar por resultado.
#
# La sangría se conserva a propósito, y no por prolijidad: en el estilo de este
# repositorio la llave que cierra una función es la única de su nivel que está
# en la columna cero, y `el-metodo-de-la-consulta.sh` corta el cuerpo de una
# función justo en esa. Sin sangría, cualquier llave de un `if` dentro del cuerpo
# la cerraría antes de tiempo.
#
# Se usa así:  codigo=$(awk -f pruebas/sin-comentarios.awk src/permisos-de-medios/modulo.c)
{
    resto = $0
    salida = ""
    while (length(resto) > 0) {
        if (dentro) {
            fin = index(resto, "*/")
            if (fin == 0) { resto = ""; break }
            resto = substr(resto, fin + 2)
            dentro = 0
        } else {
            ini = index(resto, "/*")
            if (ini == 0) { salida = salida resto; resto = ""; break }
            salida = salida substr(resto, 1, ini - 1)
            resto = substr(resto, ini + 2)
            dentro = 1
        }
    }
    sub(/[ \t]+$/, "", salida)
    if (salida ~ /[^ \t]/) print salida
}
