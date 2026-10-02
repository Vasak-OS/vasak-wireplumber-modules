# Las pruebas del ecualizador. Las cuatro primeras no necesitan WirePlumber ni
# PipeWire andando: `presets`, `state` y `config` son glib, y `service` levanta
# su propio bus con `GTestDBus`.
test('equalizer-presets', executable('tests-equalizer-presets', 'presets.c',
    dependencies: equalizer_core_dep, install: false))

test('equalizer-state', executable('tests-equalizer-state', 'state.c',
    dependencies: equalizer_core_dep, install: false))

test('equalizer-service', executable('tests-equalizer-service', 'service.c',
    dependencies: equalizer_core_dep, install: false))

test('equalizer-config', executable('tests-equalizer-config', 'config.c',
    dependencies: equalizer_core_dep, install: false),
    env: {
        'EQ_PIPEWIRE_CONF': meson.project_source_root() / 'data' / '60-vasak-equalizer.conf',
        'EQ_WIREPLUMBER_CONF': meson.project_source_root() / 'data' / '50-vasak-equalizer.conf',
        'EQ_MESON_BUILD': meson.project_source_root() / 'src' / 'equalizer' / 'meson.build',
    })

# La de punta a punta: una pila de audio aislada, el módulo compilado, y que el
# filtro siga a la salida, se apague y vuelva con lo guardado. Necesita los dos
# demonios instalados; donde no están —el runner de Ubuntu— no se registra, y
# lo dice. Ver la opción `integration-tests`.
integration_tools = ['pipewire', 'wireplumber', 'pw-dump', 'wpctl', 'pw-cat',
                     'pw-record', 'pw-link', 'jq', 'gdbus', 'dbus-run-session',
                     'python3']
integration = get_option('integration-tests')
integration_ok = not integration.disabled()
foreach tool : integration_tools
    if integration_ok and not find_program(tool, required: integration).found()
        integration_ok = false
    endif
endforeach

if integration_ok
    test('equalizer-follows-output', find_program('follows-output.sh'),
        env: {'ISOLATED_MODULE': equalizer_module.full_path()},
        depends: equalizer_module,
        timeout: 120,
        is_parallel: false)

    # Y que lo que el nodo dice se oiga: tonos grabados a la salida contra la
    # respuesta calculada de los biquads.
    test('equalizer-response', find_program('response.sh'),
        env: {'ISOLATED_MODULE': equalizer_module.full_path()},
        depends: equalizer_module,
        timeout: 120,
        is_parallel: false)
else
    message('Sin PipeWire, WirePlumber o sus herramientas: no se registran equalizer-follows-output ni equalizer-response.')
endif
