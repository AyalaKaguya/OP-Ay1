# main: device composition root

Initializes platform services, starts Recovery support, builds the portable
tracker engine from the embedded S3M asset, creates the native synth and shared
audio bus, starts audio and display, loads the GSP bundle and hands the scene
to `app_tracker`. It contains no input handling,
animation or drawing rules and exports no headers.

Private dependencies: `app_audio`, `app_display`, `app_tracker`,
`app_tracker_engine`, `app_synth`, BSP, ESP-Iris, Recovery, bundle loading, screen mirroring, NVS
and logging. Central dependency pins/overrides live in `idf_component.yml`.

The composition also creates the shared sampler and global project service,
binds sampler output into the audio bus, and passes the same project/sampler
handles to the workstation UI. Project IO owns its background worker and NAND
mount; `main` does not read/write project files in UI callbacks. The deferred
Iris inbox volume exposes only `/nand/apps/op-ay1/inbox`, with readiness published
after the project worker mounts storage and creates that directory.

`app_main()` runs once in task context and returns after startup; the UI and
audio lifetimes extend until reboot. A missing engine, bundle or display leaves
Recovery reachable instead of blocking installation.
