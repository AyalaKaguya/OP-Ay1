# app_nand

Application-owned mount of the existing external SPI3 NAND at `/nand`.
This component owns its SPI device, layered wear-leveling block device and
LittleFS mount, separately from Vibe Mode's firmware-local owners. It never
formats, grows a filesystem, erases a region, repairs a superblock or changes
NOR partitions. A missing/damaged filesystem remains an explicit error.

`app_nand_mount/info/unmount` belong exclusively to the project IO worker.
`app_nand_status` is an atomic nonblocking readiness/error query and is safe
before mount. `app_project_inbox_ready` is the stronger deferred file-volume
check, because it also confirms the application inbox directory was created.

SPI geometry and SIO initialization follow the same public driver contracts
as retained Vibe Mode. Before registering VFS, a temporary raw LittleFS probe
reads the persisted block count with callbacks that reject write/erase. Its
state is heap-owned and unmounted before VFS ownership begins. Persisted
capacity must fit current recovered device geometry; it is never silently
expanded. Mount uses `format_if_mount_failed=false`, `grow_on_mount=false`.

Requires SPI NAND BDL support, ESP-IDF `esp_blockdev`, and LittleFS 1.23.1 raw
block-device API. The consuming main manifest pins that dependency. BSP remains
the owner of board pins; this component uses only its public headers/constants.
If a BSP raw-NAND handle already exists, mount refuses a second driver owner.

Mount failure releases resources in dependency order. Cleanup failure retains
the relevant handles for retry; it never frees a bus below a live device.
Unregister consumes the LittleFS block device. All VFS users, including Iris
inbox streams, must drain before unmount. Never register the entire NAND as a
writable application project export: the application exposes only its inbox.

The application's project namespace is `/nand/apps/op-ay1`; `/nand/ideas`,
`/nand/system-update` and every unrelated user file remain outside its owner.
Actual total/used bytes are queried from LittleFS only after mount; no fixed
capacity or free-space claim is derived from a chip model or source constant.
