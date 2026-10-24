# ESP-Mosaico aryx

Created with `python mosaico.py project init my_app`.
The same GSP 1.5.1 scene and portable C UI run on PC and ESP-Mosaico.

From the workspace root:

```sh
python mosaico.py project sim --project projects/aryx --interactive
python mosaico.py recover
python mosaico.py iris system-update --project projects/aryx
```

Run `recover` before the first install on a blank or unverified device.
Use `system-update` for new apps or changed resources/layouts. Use `app-update`
only for code changes with an identical full partition table and resources.

Application components:

- `components/app_hello`: portable UI behavior and private controller state.
- `components/app_display`: BSP-to-GSP display and optional touch policy.
- `main`: device startup and dependency composition only.
- `components/app_hello/host`: desktop adapter and its separate CMake build,
  reusing the same controller and public header; excluded from the ESP32 build.
- `components/app_hello/ui/main.json`: authored scene; generated bindings and bundles stay in the build.

`project sim` discovers a unique scene under `components/*/ui/main.json` (or the
older root `ui/main.json`). With multiple scenes, select one using `--scene PATH`,
relative to the project. This previews only that scene; omit `--scene` to run the
PC backend and its compiled application behavior.

Dependency direction is `main -> app_hello -> ESP-GSP` and
`main -> app_display -> BSP/ESP-GSP`. Neither application component depends on
`main` or on its sibling. Public headers live in `include/`; private state stays
in implementation files. Each component README includes its contract and usage.

When adding behavior, keep `main` as a composition root. Add cohesive `app_*`
components only for independent responsibilities, declare public/private CMake
dependencies explicitly, and keep the graph acyclic. Avoid cross-module globals;
pass handles, context and value snapshots. Keep each public API comment within
three lines, describing its purpose and key preconditions/results. Put detailed
ownership, lifecycle, threading and timeout contracts in the component README;
document units and ranges beside fields.
Third-party dependencies retain upstream names. Pins and workspace overrides live
in `main/idf_component.yml` and the root CMake configuration. Apply local patches
in a dedicated dependency checkout and select it with `override_path`; never edit
`managed_components/` in place.

Git, editor and hook entry files stay at the root; format rules, project policy
and setup instructions live in `tools/quality/`. See its README to enable hooks.
The `tools/quality/policy.json` prefix rule checks local application component names.
Architecture and API semantics also require code review; formatter/static-analysis
success alone does not establish those contracts.
