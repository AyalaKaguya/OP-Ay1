# main: device composition root

Initializes platform services, starts Recovery support, builds the portable
tracker engine from the embedded S3M asset, creates the native synth and shared
audio bus, starts audio and display, loads the GSP bundle and hands the scene
to `app_tracker`. It contains no input handling,
animation or drawing rules and exports no headers.

Private dependencies: `app_audio`, `app_display`, `app_tracker`,
`app_tracker_engine`, `app_synth`, BSP, ESP-Iris, Recovery, bundle loading, screen mirroring, NVS
and logging. Central dependency pins/overrides live in `idf_component.yml`.

`app_main()` runs once in task context and returns after startup; the UI and
audio lifetimes extend until reboot. A missing engine, bundle or display leaves
Recovery reachable instead of blocking installation.
