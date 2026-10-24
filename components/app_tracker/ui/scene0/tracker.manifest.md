# tracker build manifest

- source: `projects\aryx\components\app_tracker\ui\tracker.json`
- gspc version: 0.6.1
- profile: host_rgb565 (rgb565, resource_alignment=64, id=0x6942ba48)
- content_id: 0xe53ede9d
- screen: 480x480 scene_id=0

## Outputs

- gsb: `tracker.gsb` (17148 bytes)

## Tables

- commands: 168
- state slots: 83
- binds: 76 (public: 47)
- actions: 47
- hits: 36
- visibility groups: 33
- resource refs: 14
- glyph run pool: 1574 bytes
- text defaults pool: 256 bytes
- tile index: 8x8 grid of 64x64px, 10124 refs, avg 158.2 cmds/tile
- templates: 0

## Scene resource budget

- reachable resource blobs: 632192 bytes
- image payloads: 414720 bytes
- font blobs: 217344 bytes

- Resource blobs include their own headers but exclude scene GSB, bundle tables/alignment and firmware code; shared blobs must not be summed across scenes.
- Initial static decode bytes are an estimate, not measured resident or peak RAM. Exceeding the cache budget can cause eviction, not necessarily allocation failure.
- Different variants may be required for size, format or animation policy. Review authored uses before removing them or moving work to runtime.

## Scene image usage

- cache enabled by default: true
- compiler cache budget: 0 bytes (0 = automatic)
- registered images: 1
- images referenced by scene draw commands: 1
- registered static decode bytes: 0
- draw-reference static decode bytes: 0
- largest registered static decode: 0 bytes

- largest scene draw-reference static decode: 0 bytes
- initially visible images: 1
- initial visible static decode estimate: 0 bytes

Registered images are addressable by this scene, including hidden controls and template resources. Draw references include all pages and templates. Initial visibility uses default state, command bounds and viewport clipping before application updates. Preparation and prefetch depend on the runtime budget and cache state. Budget retained caches, runtime images and animation/vector buffers separately; runtime_peak_bytes is measured at runtime.

## Image resources (414720 bytes payload, 414848 bytes container)

- id 1: 480x432 store bytes=414720 (100.0% of images, 1.00x raw) fmt=rgb565 cache=mmap_direct requested=generated selected_by=generated_resource hash=0x3e71e524 `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\assets\canvas_main.png` scene_referenced=true initially_visible=true
  - candidate store: eligible, encoded=414720, decoded=414720, savings=0 (0.0%), reason=selected

## Fonts

- resource_id 0x9ac39459: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans.ttf` @11px
- resource_id 0xa6a13226: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans-Bold.ttf` @12px
- resource_id 0x4cc5746: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans-Bold.ttf` @26px
- resource_id 0x390afea7: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans-Bold.ttf` @32px
- resource_id 0xb051e294: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans-Bold.ttf` @24px
- resource_id 0x21b8f71: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans-Bold.ttf` @28px
- resource_id 0xfa6949de: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans-Bold.ttf` @15px
- resource_id 0xfcefe8cf: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans-Bold.ttf` @13px
- resource_id 0x4061549b: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans.ttf` @12px
- resource_id 0xbdfe52fa: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans-Bold.ttf` @20px
- resource_id 0xa53e62de: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans.ttf` @8px
- resource_id 0xf707902a: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans-Bold.ttf` @22px
- resource_id 0xdbceaf39: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans.ttf` @10px

## Binds

- GSP_BIND_CLIP_OFF = 29
- GSP_BIND_CLIP_ON = 30
- GSP_BIND_DOT_ON_0 = 31
- GSP_BIND_DOT_ON_1 = 32
- GSP_BIND_DOT_ON_2 = 33
- GSP_BIND_DOT_ON_3 = 34
- GSP_BIND_DOT_ON_4 = 35
- GSP_BIND_GRID_CANVAS = 36
- GSP_BIND_MASTER_VALUE = 37
- GSP_BIND_NAV_ICON = 38
- GSP_BIND_ORDER_LIST = 39
- GSP_BIND_PAT_VALUE = 40
- GSP_BIND_PB_POS = 41
- GSP_BIND_PB_SPEED_VALUE = 42
- GSP_BIND_PP_PAUSE_TEXT = 43
- GSP_BIND_PP_PLAY_TEXT = 44
- GSP_BIND_SEQ_POS = 45
- GSP_BIND_SLOT0_DB = 46
- GSP_BIND_SLOT0_NAME = 47
- GSP_BIND_SLOT1_DB = 48
- GSP_BIND_SLOT1_NAME = 49
- GSP_BIND_SLOT2_DB = 50
- GSP_BIND_SLOT2_NAME = 51
- GSP_BIND_SLOT3_DB = 52
- GSP_BIND_SLOT3_NAME = 53
- GSP_BIND_SMN0 = 54
- GSP_BIND_SMN1 = 55
- GSP_BIND_SMN2 = 56
- GSP_BIND_SMN3 = 57
- GSP_BIND_SMO0 = 58
- GSP_BIND_SMO1 = 59
- GSP_BIND_SMO2 = 60
- GSP_BIND_SMO3 = 61
- GSP_BIND_SPEED_05_SEL = 62
- GSP_BIND_SPEED_10_SEL = 63
- GSP_BIND_SPEED_15_SEL = 64
- GSP_BIND_SPEED_20_SEL = 65
- GSP_BIND_SSN0 = 66
- GSP_BIND_SSN1 = 67
- GSP_BIND_SSN2 = 68
- GSP_BIND_SSN3 = 69
- GSP_BIND_SSO0 = 70
- GSP_BIND_SSO1 = 71
- GSP_BIND_SSO2 = 72
- GSP_BIND_SSO3 = 73
- GSP_BIND_STEP_VALUE = 74
- GSP_BIND_WAVES_CANVAS = 75

## Callbacks

- GSP_ACT_ID_APPLY_PATTERN_CB = 0
- GSP_ACT_ID_MASTER_FADER_CB = 1
- GSP_ACT_ID_NAV_CB = 2
- GSP_ACT_ID_ORDER_PAUSE_CB = 3
- GSP_ACT_ID_ORDER_PLAY_CB = 4
- GSP_ACT_ID_ORDER_RESTART_CB = 5
- GSP_ACT_ID_PAT_NEXT_CB = 6
- GSP_ACT_ID_PAT_PREV_CB = 7
- GSP_ACT_ID_PP_CB = 8
- GSP_ACT_ID_REC_CB = 9
- GSP_ACT_ID_SLOT_FADER0_CB = 10
- GSP_ACT_ID_SLOT_FADER1_CB = 11
- GSP_ACT_ID_SLOT_FADER2_CB = 12
- GSP_ACT_ID_SLOT_FADER3_CB = 13
- GSP_ACT_ID_SLOT_MUTE0_CB = 14
- GSP_ACT_ID_SLOT_MUTE1_CB = 15
- GSP_ACT_ID_SLOT_MUTE2_CB = 16
- GSP_ACT_ID_SLOT_MUTE3_CB = 17
- GSP_ACT_ID_SLOT_SOLO0_CB = 18
- GSP_ACT_ID_SLOT_SOLO1_CB = 19
- GSP_ACT_ID_SLOT_SOLO2_CB = 20
- GSP_ACT_ID_SLOT_SOLO3_CB = 21
- GSP_ACT_ID_SPEED_05_CB = 22
- GSP_ACT_ID_SPEED_10_CB = 23
- GSP_ACT_ID_SPEED_15_CB = 24
- GSP_ACT_ID_SPEED_20_CB = 25
- GSP_ACT_ID_STEP_NEXT_CB = 26
- GSP_ACT_ID_STEP_PREV_CB = 27

## Optimization notes

- dead objects removed: 0
- zero-area commands skipped: 0
- occlusion cull removed: 0
- background fills removed: 0
- same-color rects merged: 0
- static bake: none
- animation players: 0
- animation frame bytes: 0
