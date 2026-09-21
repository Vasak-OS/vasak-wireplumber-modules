# vasak-wireplumber-modules

Módulos de WirePlumber de VasakOS.

## `permisos-de-medios`

Identifica a cada cliente de PipeWire por su **pid**, que es lo único que el
cliente no puede ni falsear ni elegir, para que el permiso de cámara sea por
aplicación.

Le pregunta a `vasak-permissions` por cada cliente que se conecta y le oculta
los objetos de cámara al que no tenga permiso. **Falla cerrando**: mientras la
respuesta no llegue, y si no llega nunca, el cliente no tiene cámara.

Medido con el módulo cargado en un WirePlumber de verdad: un cliente normal ve
**0** nodos de cámara y logra **0 de 5** capturas, y el audio no se toca.

### Por qué existe

`50-vasak-camara.conf`, en `vasak-desktop-settings`, ya no le ofrece la cámara a
quien entra por `pipewire-0`, y el que la quiera la pide por el portal, que
pregunta. Eso deja tres agujeros, y los tres son el mismo agujero: **con
configuración sola no se puede saber quién es el cliente.**

| lo que una regla puede mirar | por qué no alcanza |
|---|---|
| `pipewire.sec.socket` | dice de forma confiable por dónde entró, pero no impide elegir por dónde entrar: los tres sockets son `srw-rw-rw-` y basta `PIPEWIRE_REMOTE=pipewire-0-priv` |
| `application.process.binary` | lo declara el propio cliente |
| `pipewire.sec.label` | el perfil de AppArmor lo pone el kernel, pero cualquier proceso sin confinar se pone el que quiera con `aa-exec` |

Ese último se midió antes de descartarlo: un `gst-launch-1.0` copiado con otro
nombre, reclamando el perfil de Chrome con `aa-exec`, capturó **5 de 5**. Lo que
habría que confinar es al atacante, no a la aplicación permitida.

Queda `pipewire.sec.pid`, que lo fija el servidor desde las credenciales de la
conexión. Es el único.

### Por qué en C y no en Lua

El Lua de WirePlumber **no tiene `io`**: no puede leer `/proc` en absoluto, y
hace falta para el instante de arranque del proceso.

### Qué no hace, ni va a hacer

**No resuelve `/proc/<pid>/exe`.** Eso lo hace `vasak-permissions`, como root y
fijando el pid contra la reutilización, que es exactamente lo que ya hace para
identificar a quien lo llama. Acá se juntan los dos números que ese servicio
necesita —pid e instante de arranque— y se le pasan por
`CheckPermissionFor`. Duplicar la resolución sería una copia que se va a
separar de la original.

Para eso `/usr/bin/wireplumber` tiene que entrar en `DELEGATE_BINARIES`, en el
crate del protocolo de `vasak-permissions`. La lista es de rutas absolutas bajo
`/usr/bin` justamente porque un programa que el usuario pueda escribir no puede
estar ahí, y por lo tanto no puede decir que pregunta por otro.

### Lo que el micrófono no va a poder

`pipewire-pulse` **borra la identidad de quien pasa por él**: todo lo que habla
la API de PulseAudio llega a PipeWire con el pid del demonio de pulse. Se ve en
el propio registro de este módulo:

```
identificado 'pactl': pid 99425 …      ← 99425 es pipewire-pulse
identificado 'pw-cli': pid 411476 …    ← éste sí es el suyo
```

La cámara no tiene ese problema —no hay camino de pulse para video— pero el
micrófono del navegador sí. Hacerlo cumplir es un trabajo dentro de
`pipewire-pulse`, otro componente y otro problema.

### Tres trampas que costaron encontrar

**El orden de los enganches se declara entero o no sirve.** Todos los que
reparten acceso se declaran «antes de `client/apply-access`», así que nombrar
sólo a ése deja al nuestro sin orden respecto de
`client/find-default-access` — que reparte el gestor por omisión— y éste gana
la carrera. Cuando el nuestro corre ya hay un gestor puesto, se aparta como
corresponde, y la cámara queda abierta **sin una sola queja en el registro**.
Hay una prueba que lo comprueba, porque el compilador no puede.

**El closure de un enganche recibe un solo parámetro.** El encabezado de
WirePlumber dice «the closure should accept two parameters: the event
dispatcher and the event» y es falso en 0.5.17: `wp_simple_event_hook_run`
invoca con aridad uno. Creerle no da error de compilación ni caída — el segundo
argumento pasa a ser el `user_data`, y el enganche **se calla**. Parece que no
dispara nunca.

**El nombre del ejecutable en `/proc/<pid>/stat` puede traer espacios y
paréntesis**, y el kernel no los escapa. Hay que buscar el **último** `)`.
Equivocarse tampoco falla: devuelve otro número, el servicio rechaza el proceso
como si hubiera muerto, y la cámara queda negada sin explicación.

### Probar

```bash
meson setup build
ninja -C build
meson test -C build
```

Las pruebas no necesitan WirePlumber andando ni una sesión: lo que decide algo
—qué cuenta como cámara, y cómo se lee el instante de arranque— vive aparte del
enganche, en `camara.c` e `identidad.c`, y el enganche no decide nada.

### Cargarlo sin instalarlo

`WIREPLUMBER_MODULE_DIR` es el directorio entero, no una ruta de búsqueda, así
que hay que armar uno con enlaces a los de fábrica más el nuestro:

```bash
mkdir modulos && ln -s /usr/lib/wireplumber-0.5/*.so modulos/
cp build/src/permisos-de-medios/*.so modulos/
systemctl --user stop wireplumber
WIREPLUMBER_MODULE_DIR=$PWD/modulos WIREPLUMBER_DEBUG=m-vasak-medios:4 wireplumber
```

Y un archivo en `~/.config/wireplumber/wireplumber.conf.d/` que lo cargue:

```
wireplumber.components = [
  { name = libwireplumber-module-vasak-permisos-de-medios, type = module
    provides = custom.vasak-permisos-de-medios }
]

wireplumber.profiles = { main = { custom.vasak-permisos-de-medios = required } }
```

## Licencia

GPL-3.0-or-later.
