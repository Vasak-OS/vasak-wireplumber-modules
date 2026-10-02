# vasak-wireplumber-modules

Módulos de WirePlumber de VasakOS: los permisos de medios por aplicación y el
ecualizador de sistema.

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
necesita —pid e instante de arranque— y se le pasan por `QueryPermissionFor`, que
es el único de los dos métodos que **anota el intento y avisa** cuando todavía no
hay decisión; el otro abre un diálogo, y de por qué acá no puede haberlo está en
[«Cuatro trampas que costaron encontrar»](#cuatro-trampas-que-costaron-encontrar).
Duplicar la resolución sería una copia que se va a separar de la original.

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

### Cuatro trampas que costaron encontrar

**Hay dos métodos para consultar, y el equivocado no rompe la cámara: rompe el
aviso.** `vasak-permissions` expone `QueryPermissionFor` y `CheckPermissionFor`,
y los dos dicen lo mismo sobre el permiso —el módulo sigue negando igual, falla
cerrando, el agujero del socket privilegiado sigue cerrado—. Lo que cambia es
qué pasa cuando **nadie decidió todavía**, que es el caso de todo cliente nuevo,
que es el caso constante.

`QueryPermissionFor` llama a `anotar_y_avisar()`: **anota el intento** y **avisa
una vez**. Con eso la aplicación aparece en Privacidad y seguridad y hay dónde
darle permiso.

`CheckPermissionFor` no anota nada. Ante una decisión que no conoce abre un
diálogo con `agent::ask()` y, si nadie contesta, devuelve un «no» en silencio.
Ese método es para acciones de la persona —«estás compartiendo pantalla, ¿seguí?»—
, donde el diálogo tiene quién lo conteste. Este módulo en cambio ve a cada
cliente **al conectarse**: «¿le permitís la cámara a pactl?» no es una pregunta
que alguien pueda contestar, porque no hay nadie mirando la pantalla de permisos
en el momento en que arranca el navegador.

El resultado de usar el equivocado es peor que no avisar: bloquea la cámara
**sin dejar registro**, o sea que la aplicación no figura en ningún lado y no hay
forma de desbloquearla. Es exactamente el callejón sin salida por el que este
módulo viajó apagado desde su primer día, y la razón de que el paquete exija
`vasak-permissions>=0.14.0` es la que acabo de describir. Encendió el método
equivocado y se apagó la mitad que lo hacía desbloqueable.

Dos pruebas lo atan, y son de fuente a propósito: el enganche vive en
`modulo.c`, que no compila en CI, así que no hay forma de observarlo desde C.
`el-metodo-de-la-consulta.sh` comprueba que la cadena que va del enganche a la
llamada por D-Bus termine en `QueryPermissionFor`, con su firma; y
`sin-camino-de-dialogo.sh` que el otro no aparezca en ninguna línea de **código**,
para que no quede ni el camino ni la constante a mano «por si acaso».

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

### Viene encendido, y por qué recién ahora

El paquete instala el módulo y un fragmento que lo declara **en `required`**,
así que instalarlo hace cumplir el permiso de cámara.

Viajó apagado desde que se empaquetó, y no por prudencia genérica: faltaba la
otra mitad de la regla del escritorio — todo lo que se bloquea se tiene que
poder desbloquear. El módulo niega a quien no tenga decisión guardada, y una
aplicación que nunca preguntó no figura en Privacidad y seguridad: no había
interruptor que mover, y la negación no generaba aviso, así que tampoco llegaba
la oferta de permitirla.

Desde `vasak-permissions` 0.14.0 esa mitad existe. Una consulta sin decisión
**anota el intento** —con lo que la aplicación aparece en la pantalla de
permisos— y **avisa una vez**, en el momento en que la persona sabe qué estaba
haciendo. Eso es lo que hace falta para que este módulo se pueda encender: sin el
registro, la aplicación bloqueada no figuraría en Privacidad y seguridad y no
habría dónde darle permiso. Por eso el paquete pide esa versión y no una
anterior: contra el servicio viejo, esto volvería a bloquear sin decir nada.

Para apagarlo en un equipo, sin desinstalar nada, un archivo propio en
`/etc/wireplumber/wireplumber.conf.d/`:

```
wireplumber.profiles = { main = { custom.vasak-permisos-de-medios = disabled } }
```

Hay una prueba que comprueba que viaje encendido, y que el fragmento documente
cómo apagarlo. Estuvo al revés a propósito mientras faltaba el aviso, y se dio
vuelta en el mismo commit que lo encendió.

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

## `equalizer`

El **ecualizador de sistema**: diez bandas (31, 63, 125, 250 y 500 Hz, 1, 2, 4,
8 y 16 kHz), ocho perfiles de fábrica y uno propio, delante de la salida por
omisión. Vale para todo lo que suena, no para un reproductor.

Son tres piezas:

| pieza | qué hace |
|---|---|
| `data/60-vasak-equalizer.conf` → `pipewire.conf.d` | carga un `filter-chain` **dentro del demonio de PipeWire**: un `bq_highshelf` en `Freq = 0` (ganancia plana, el margen) y diez `bq_peaking` de una octava (Q = 1,41), arrancando en 0 dB |
| `src/equalizer/` → módulo de WirePlumber | le pone al filtro las ganancias guardadas cada vez que aparece, lo apaga y lo prende, guarda lo elegido y publica la interfaz D-Bus |
| `data/50-vasak-equalizer.conf` → `wireplumber.conf.d` | carga el módulo, **encendido** |

### Cómo sigue a la salida: no lo hace este repositorio

El filtro se declara `filter.smart = true` y **sin destino**, que es la política
de filtros inteligentes de WirePlumber 0.5 (`linking/find-filter-target.lua`,
`linking/get-filter-from-target.lua`): los flujos que iban a la salida por
omisión van al filtro, y la salida del filtro va a la salida por omisión. Cuando
ésta cambia —unos auriculares Bluetooth, otra salida elegida en el panel—
WirePlumber reengancha todo. Y `find-best-default-node.lua` nunca elige un
filtro inteligente como salida por omisión, así que el ecualizador no le roba
ese lugar al dispositivo.

Por eso el módulo **no registra ningún enganche de eventos**. Uno que reenlazara
al cambiar de salida competiría con la política por los mismos enlaces, y en
WirePlumber un enganche mal ordenado pierde en silencio — lo aprendimos en el
módulo de arriba.

**Apagado** no es «todo en cero»: es `filter.smart.disabled = true` en el
metadato `filters`, con lo que WirePlumber desengancha el filtro y los flujos
van directo al dispositivo. **Plano**, en cambio, deja el filtro en el camino, y
está medido como transparente (abajo).

Lo que no cubre, a sabiendas:

- Un flujo que **eligió** una salida concreta —movido a mano en un mezclador— no
  pasa por el ecualizador: la política sólo lo pone delante de lo que va «a la
  salida por omisión».
- Es **global**, no por salida: el mismo perfil para auriculares y parlantes.
  Por salida es una extensión posible (un estado por `node.name` de la salida),
  no algo que haya que deshacer.
- Es **estéreo**: una salida 5.1 recibe lo ecualizado mezclado a dos canales.
- **Grabar el monitor de la salida por omisión** graba lo que entra al
  ecualizador, no lo que sale: la política también engancha ahí al filtro. Una
  captura de pantalla con «el audio del escritorio» graba sin ecualizar.
  Comprobado con WirePlumber 0.5.18 por las tres vías —`pw-record` con
  `stream.capture.sink`, `parecord -d @DEFAULT_MONITOR@` y `parecord -d
  <salida>.monitor`, aun nombrando la salida—: las tres quedan enganchadas al
  monitor de `vasak-equalizer`. `follows-output` lo mira, para que si
  WirePlumber cambia esto se entere el README.

### Por qué en el demonio de PipeWire y no en un proceso aparte

Cargado en el demonio, el filtro corre en el mismo hilo de tiempo real que el
resto del grafo: no hay un proceso más que despertar en cada ciclo. Y si
WirePlumber se reinicia, el audio sigue pasando por el filtro con los valores
que tenía.

### El margen

Subir una banda puede saturar: el grafo trabaja en coma flotante, pero la placa
de sonido no. El módulo baja todo lo que suba la banda más alta —«bass» lleva
+6 dB en 31 Hz, así que el resto baja 6 dB— y lo publica como `Preamp`. Ningún
perfil de fábrica pasa de +6 dB por eso mismo: el margen se cobra en volumen.

### Lo elegido, guardado

En `$XDG_STATE_HOME/vasak/equalizer.ini` (por omisión
`~/.local/state/vasak/equalizer.ini`): si está encendido, el perfil elegido y los
valores del propio. Lo escribe el módulo medio segundo después del último
cambio, de una vez (temporal y renombre), y lo lee al arrancar. Un archivo roto
no rompe el audio: lo que no se entiende vuelve a lo de fábrica, campo por
campo.

Es un archivo propio y no `vasak.conf` porque lo lee el módulo, que arranca
antes que cualquier aplicación y no depende de ninguna; y va en
`XDG_STATE_HOME` porque es estado —lo cambia el uso—, como la salida por
omisión que guarda WirePlumber.

### No choca con `permisos-de-medios`

El nodo del filtro es del propio demonio de PipeWire, no de un cliente: el
gestor de permisos no lo ve pasar, ni lo bloquea. Y lo que el módulo escribe —un
`Props` del nodo y una clave de `filters`— lo escribe WirePlumber, que entra por
el socket del gestor. No hay camino nuevo hacia la cámara ni hacia nada.

### La interfaz D-Bus

Es el contrato con la interfaz gráfica: el bloque del reproductor desplegable
de `vasak-desktop` (#131) y la sección Sonido de `vasak-settings`.

| | |
|---|---|
| bus | el de la **sesión** |
| nombre | `org.vasak.Equalizer` |
| objeto | `/org/vasak/Equalizer` |
| interfaz | `org.vasak.Equalizer1` |

El nombre lo tiene WirePlumber mientras corre. Si no está, el ecualizador **no
está disponible** —se dibuja así, no roto—, y se vigila con
`NameOwnerChanged` para que aparezca cuando WirePlumber arranque.

**Propiedades** (todas de lectura; los cambios llegan por
`org.freedesktop.DBus.Properties.PropertiesChanged`, sólo con las que
cambiaron):

| propiedad | tipo | qué es |
|---|---|---|
| `Frequencies` | `ad` | las diez frecuencias, en Hz, en el orden de las bandas: `31, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000`. Fija |
| `GainRange` | `(dd)` | el rango de cada banda, en dB: `(-12, 12)`. Fijo |
| `Presets` | `as` | los perfiles de fábrica, en el orden de la grilla: `flat, bass, treble, vocal, pop, rock, jazz, classic`. Fija |
| `Preset` | `s` | el perfil elegido: uno de `Presets`, o `custom` |
| `Gains` | `ad` | las diez ganancias **que suenan**, en dB: las del perfil elegido, o las propias |
| `CustomGains` | `ad` | las del perfil propio, aunque no sea el elegido |
| `Preamp` | `d` | el margen que se está aplicando, en dB (0 o negativo). Informativo |
| `Enabled` | `b` | si está encendido |
| `Available` | `b` | si el filtro está en PipeWire. Falso hasta reiniciar PipeWire después de instalar el paquete; mientras tanto los cambios se guardan y suenan cuando aparezca |
| `Saved` | `b` | si lo que suena es lo que está en disco. Falso durante el medio segundo que espera el guardado, y **falso si no se pudo escribir** |

**Métodos** (ninguno devuelve nada; un argumento inválido es
`org.freedesktop.DBus.Error.InvalidArgs` y no cambia nada):

| método | qué hace |
|---|---|
| `SetGain(u band, d gain)` | mueve una banda (0–9). Lo que suena pasa a ser el perfil propio: si estaba en `rock`, el propio toma los valores de `rock` y se mueve esa banda. `Preset` pasa a `custom` |
| `SetGains(ad gains)` | las diez de una vez, con la misma regla. Hacen falta exactamente diez |
| `SetPreset(s preset)` | elige un perfil de `Presets`, o `custom` para volver a los valores propios |
| `SetEnabled(b enabled)` | apaga o prende. Apagado, el audio no pasa por el filtro |

Lo que la interfaz hace con esto:

- **Los nombres de los perfiles los traduce la interfaz.** Los identificadores
  son estables y van en inglés; «Plano», «Graves», «Agudos», «Voz», «Pop»,
  «Rock», «Jazz», «Clásica» y «Personalizado» (`custom`) van en los `locales/`
  de cada aplicación.
- **Arrastrar una banda**: `SetGain` mientras se arrastra, limitado a unas 30
  llamadas por segundo. Suena en el acto; el archivo se escribe medio segundo
  después del último cambio, sin que la interfaz tenga que avisar que se soltó
  (no hay método para eso, ni hace falta).
- **Cambiar de perfil**: `SetPreset`, y animar los tiradores hacia los `Gains`
  que llegan en `PropertiesChanged`. El sonido cambia en el acto.
- **El encabezado**: el perfil activo sale de `Preset`; «Guardado» / «Sin
  guardar», de `Saved`.
- **Restablecer** el propio es `SetGains` con diez ceros.
- **El filtro aparece como salida** en `pactl list sinks` («VasakOS
  Equalizer», `Name: vasak-equalizer`). El selector de salida del panel y de
  Ajustes lo tiene que **sacar de la lista**: elegirlo como salida por omisión
  no tiene sentido. Lo que identifica a cualquier filtro, éste o uno futuro, es
  la propiedad `filter.smart = "true"`.

Para probarla a mano:

```bash
gdbus introspect --session -d org.vasak.Equalizer -o /org/vasak/Equalizer
gdbus call --session -d org.vasak.Equalizer -o /org/vasak/Equalizer \
  -m org.vasak.Equalizer1.SetPreset rock
gdbus monitor --session -d org.vasak.Equalizer
```

Y desde Rust, con el `zbus` que ya usa `vasak-desktop`:

```rust
#[zbus::proxy(
    interface = "org.vasak.Equalizer1",
    default_service = "org.vasak.Equalizer",
    default_path = "/org/vasak/Equalizer"
)]
trait Equalizer {
    fn set_gain(&self, band: u32, gain: f64) -> zbus::Result<()>;
    fn set_gains(&self, gains: &[f64]) -> zbus::Result<()>;
    fn set_preset(&self, preset: &str) -> zbus::Result<()>;
    fn set_enabled(&self, enabled: bool) -> zbus::Result<()>;

    #[zbus(property)] fn frequencies(&self) -> zbus::Result<Vec<f64>>;
    #[zbus(property)] fn gain_range(&self) -> zbus::Result<(f64, f64)>;
    #[zbus(property)] fn presets(&self) -> zbus::Result<Vec<String>>;
    #[zbus(property)] fn preset(&self) -> zbus::Result<String>;
    #[zbus(property)] fn gains(&self) -> zbus::Result<Vec<f64>>;
    #[zbus(property)] fn custom_gains(&self) -> zbus::Result<Vec<f64>>;
    #[zbus(property)] fn preamp(&self) -> zbus::Result<f64>;
    #[zbus(property)] fn enabled(&self) -> zbus::Result<bool>;
    #[zbus(property)] fn available(&self) -> zbus::Result<bool>;
    #[zbus(property)] fn saved(&self) -> zbus::Result<bool>;
}
```

### Medido

Con `tests/equalizer/measure.sh`, en la pila aislada, el 02/10/2026: PipeWire
1.6.9, WirePlumber 0.5.18, i7-10510U, 48 kHz estéreo, el ecualizador en «rock»,
treinta segundos de reproducción después de treinta de asentarse.

| | quantum 1024 (21,3 ms) | quantum 256 (5,3 ms) |
|---|---|---|
| latencia agregada, medida con un impulso a la entrada y a la salida del filtro en la misma grabación | **0 muestras** | **0 muestras** |
| CPU del demonio de PipeWire, ecualizador encendido | 1,00–1,03 % de un núcleo | 1,87–1,90 % |
| … y apagado | 0,20 % | 0,63–0,83 % |
| tiempo de proceso del filtro por ciclo (`pw-top`, BUSY de los dos nodos) | 150–180 µs | 42–75 µs |
| errores del filtro (`pw-top`, ERR) | 0 | 0 |

- **No agrega latencia**: el filtro corre en el mismo ciclo del grafo que la
  salida, y los biquads no tienen retardo propio. Lo que entra en un ciclo sale
  en ese mismo ciclo.
- **Cuesta cerca de un 0,8 % de un núcleo** con el quantum de siempre, y algo más
  de un 1 % con uno chico. Sólo mientras suena: la salida del filtro es pasiva,
  y en silencio se suspende con el dispositivo.
- **Plano es transparente**: un tono de 1 kHz da el mismo nivel, al milésimo de
  dB, con el filtro plano y sin el filtro (`equalizer-response`).
- **Encender, apagar o cambiar de salida reengancha**, y eso se oye como un
  corte de unos milisegundos (el reproductor de prueba anotó 4 xruns en el
  cambio, con quantum 256). Cambiar de perfil o mover una banda no reengancha
  ni interrumpe el audio: sólo cambian los coeficientes de los biquads.

La máquina estaba cargada (promedio de carga entre 11 y 24 en 8 núcleos, por
otras compilaciones): los xruns del reproductor de prueba fueron de 0 a 650 por
ventana, sin relación con que el ecualizador estuviera encendido o apagado, y
por eso no se citan como medición. Vale repetir `measure.sh` en un equipo
quieto.

### Probar

```bash
meson setup build && meson test -C build
```

Las unitarias (`equalizer-presets`, `-state`, `-service`, `-config`) no
necesitan nada andando: `service` levanta su propio bus con `GTestDBus` y le
habla con un cliente de verdad. Las de punta a punta —`equalizer-follows-output`
y `equalizer-response`— levantan **una pila de audio aislada**
(`tests/equalizer/isolated-session.sh`): su propio `XDG_RUNTIME_DIR`, su propio
bus de sesión, dos salidas nulas, sin ALSA ni Bluetooth ni el módulo de
permisos. **No tocan la pila de la sesión.** Meson las registra si encuentra
`pipewire`, `wireplumber` y sus herramientas (`-Dintegration-tests=auto`, lo de
siempre); `disabled` las saca y `enabled` falla si falta algo. En CI corren en
el contenedor de Arch, con `enabled`.

- `follows-output`: el reproductor va al filtro y el filtro a la salida por
  omisión; al cambiarla, el filtro se muda y suelta la anterior; apagado, el
  reproductor va directo; y lo elegido vuelve después de reiniciar PipeWire y
  WirePlumber.
- `response`: un tono grabado a la salida contra la respuesta calculada de los
  biquads. Plano y apagado dan lo mismo; `bass` y `rock` dan lo que dicen, a
  ±0,3 dB (medido: a ±0,003).

`tests/equalizer/measure.sh` es la medición de abajo, para repetirla.

## Licencia

GPL-3.0-or-later.
