# tracker build manifest

- source: `projects\aryx\components\app_tracker\ui\tracker.json`
- gspc version: 0.6.1
- profile: host_rgb565 (rgb565, resource_alignment=64, id=0x6942ba48)
- content_id: 0x2530f7ba
- screen: 480x480 scene_id=0

## Outputs

- gsb: `tracker.gsb` (28108 bytes)

## Tables

- commands: 273
- state slots: 143
- binds: 136 (public: 107)
- actions: 103
- hits: 92
- visibility groups: 97
- resource refs: 13
- glyph run pool: 1154 bytes
- text defaults pool: 192 bytes
- tile index: 8x8 grid of 64x64px, 16844 refs, avg 263.2 cmds/tile
- templates: 0

## Scene resource budget

- reachable resource blobs: 595776 bytes
- image payloads: 414720 bytes
- font blobs: 180928 bytes

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

- resource_id 0x6bdd6edc: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans-Bold.ttf` @22px
- resource_id 0x9ac39459: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans.ttf` @11px
- resource_id 0xa6a13226: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans-Bold.ttf` @12px
- resource_id 0x4cc5746: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans-Bold.ttf` @26px
- resource_id 0x390afea7: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans-Bold.ttf` @32px
- resource_id 0xa1e1a17: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans-Bold.ttf` @24px
- resource_id 0x21b8f71: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans-Bold.ttf` @28px
- resource_id 0x533cb5aa: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans-Bold.ttf` @15px
- resource_id 0xfcefe8cf: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans-Bold.ttf` @13px
- resource_id 0x1380dd93: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans.ttf` @12px
- resource_id 0xbdfe52fa: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans-Bold.ttf` @20px
- resource_id 0xa53e62de: `\\?\C:\Users\ayala\Desktop\OP-Ay1\projects\aryx\components\app_tracker\ui\fonts\DejaVuSans.ttf` @8px

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
- GSP_BIND_PAD0_DIM = 39
- GSP_BIND_PAD0_NEXT = 40
- GSP_BIND_PAD0_OFF = 41
- GSP_BIND_PAD0_PLAY = 42
- GSP_BIND_PAD10_DIM = 43
- GSP_BIND_PAD10_NEXT = 44
- GSP_BIND_PAD10_OFF = 45
- GSP_BIND_PAD10_PLAY = 46
- GSP_BIND_PAD11_DIM = 47
- GSP_BIND_PAD11_NEXT = 48
- GSP_BIND_PAD11_OFF = 49
- GSP_BIND_PAD11_PLAY = 50
- GSP_BIND_PAD12_DIM = 51
- GSP_BIND_PAD12_NEXT = 52
- GSP_BIND_PAD12_OFF = 53
- GSP_BIND_PAD12_PLAY = 54
- GSP_BIND_PAD13_DIM = 55
- GSP_BIND_PAD13_NEXT = 56
- GSP_BIND_PAD13_OFF = 57
- GSP_BIND_PAD13_PLAY = 58
- GSP_BIND_PAD14_DIM = 59
- GSP_BIND_PAD14_NEXT = 60
- GSP_BIND_PAD14_OFF = 61
- GSP_BIND_PAD14_PLAY = 62
- GSP_BIND_PAD15_DIM = 63
- GSP_BIND_PAD15_NEXT = 64
- GSP_BIND_PAD15_OFF = 65
- GSP_BIND_PAD15_PLAY = 66
- GSP_BIND_PAD1_DIM = 67
- GSP_BIND_PAD1_NEXT = 68
- GSP_BIND_PAD1_OFF = 69
- GSP_BIND_PAD1_PLAY = 70
- GSP_BIND_PAD2_DIM = 71
- GSP_BIND_PAD2_NEXT = 72
- GSP_BIND_PAD2_OFF = 73
- GSP_BIND_PAD2_PLAY = 74
- GSP_BIND_PAD3_DIM = 75
- GSP_BIND_PAD3_NEXT = 76
- GSP_BIND_PAD3_OFF = 77
- GSP_BIND_PAD3_PLAY = 78
- GSP_BIND_PAD4_DIM = 79
- GSP_BIND_PAD4_NEXT = 80
- GSP_BIND_PAD4_OFF = 81
- GSP_BIND_PAD4_PLAY = 82
- GSP_BIND_PAD5_DIM = 83
- GSP_BIND_PAD5_NEXT = 84
- GSP_BIND_PAD5_OFF = 85
- GSP_BIND_PAD5_PLAY = 86
- GSP_BIND_PAD6_DIM = 87
- GSP_BIND_PAD6_NEXT = 88
- GSP_BIND_PAD6_OFF = 89
- GSP_BIND_PAD6_PLAY = 90
- GSP_BIND_PAD7_DIM = 91
- GSP_BIND_PAD7_NEXT = 92
- GSP_BIND_PAD7_OFF = 93
- GSP_BIND_PAD7_PLAY = 94
- GSP_BIND_PAD8_DIM = 95
- GSP_BIND_PAD8_NEXT = 96
- GSP_BIND_PAD8_OFF = 97
- GSP_BIND_PAD8_PLAY = 98
- GSP_BIND_PAD9_DIM = 99
- GSP_BIND_PAD9_NEXT = 100
- GSP_BIND_PAD9_OFF = 101
- GSP_BIND_PAD9_PLAY = 102
- GSP_BIND_PB_POS = 103
- GSP_BIND_PB_SPEED_VALUE = 104
- GSP_BIND_PP_PAUSE_TEXT = 105
- GSP_BIND_PP_PLAY_TEXT = 106
- GSP_BIND_SLOT0_DB = 107
- GSP_BIND_SLOT0_NAME = 108
- GSP_BIND_SLOT1_DB = 109
- GSP_BIND_SLOT1_NAME = 110
- GSP_BIND_SLOT2_DB = 111
- GSP_BIND_SLOT2_NAME = 112
- GSP_BIND_SLOT3_DB = 113
- GSP_BIND_SLOT3_NAME = 114
- GSP_BIND_SMN0 = 115
- GSP_BIND_SMN1 = 116
- GSP_BIND_SMN2 = 117
- GSP_BIND_SMN3 = 118
- GSP_BIND_SMO0 = 119
- GSP_BIND_SMO1 = 120
- GSP_BIND_SMO2 = 121
- GSP_BIND_SMO3 = 122
- GSP_BIND_SPEED_05_SEL = 123
- GSP_BIND_SPEED_10_SEL = 124
- GSP_BIND_SPEED_15_SEL = 125
- GSP_BIND_SPEED_20_SEL = 126
- GSP_BIND_SSN0 = 127
- GSP_BIND_SSN1 = 128
- GSP_BIND_SSN2 = 129
- GSP_BIND_SSN3 = 130
- GSP_BIND_SSO0 = 131
- GSP_BIND_SSO1 = 132
- GSP_BIND_SSO2 = 133
- GSP_BIND_SSO3 = 134
- GSP_BIND_WAVES_CANVAS = 135

## Callbacks

- GSP_ACT_ID_MASTER_FADER_CB = 0
- GSP_ACT_ID_NAV_CB = 1
- GSP_ACT_ID_PAD0_CB = 2
- GSP_ACT_ID_PAD10_CB = 3
- GSP_ACT_ID_PAD11_CB = 4
- GSP_ACT_ID_PAD12_CB = 5
- GSP_ACT_ID_PAD13_CB = 6
- GSP_ACT_ID_PAD14_CB = 7
- GSP_ACT_ID_PAD15_CB = 8
- GSP_ACT_ID_PAD1_CB = 9
- GSP_ACT_ID_PAD2_CB = 10
- GSP_ACT_ID_PAD3_CB = 11
- GSP_ACT_ID_PAD4_CB = 12
- GSP_ACT_ID_PAD5_CB = 13
- GSP_ACT_ID_PAD6_CB = 14
- GSP_ACT_ID_PAD7_CB = 15
- GSP_ACT_ID_PAD8_CB = 16
- GSP_ACT_ID_PAD9_CB = 17
- GSP_ACT_ID_PP_CB = 18
- GSP_ACT_ID_REC_CB = 19
- GSP_ACT_ID_SLOT_FADER0_CB = 20
- GSP_ACT_ID_SLOT_FADER1_CB = 21
- GSP_ACT_ID_SLOT_FADER2_CB = 22
- GSP_ACT_ID_SLOT_FADER3_CB = 23
- GSP_ACT_ID_SLOT_MUTE0_CB = 24
- GSP_ACT_ID_SLOT_MUTE1_CB = 25
- GSP_ACT_ID_SLOT_MUTE2_CB = 26
- GSP_ACT_ID_SLOT_MUTE3_CB = 27
- GSP_ACT_ID_SLOT_SOLO0_CB = 28
- GSP_ACT_ID_SLOT_SOLO1_CB = 29
- GSP_ACT_ID_SLOT_SOLO2_CB = 30
- GSP_ACT_ID_SLOT_SOLO3_CB = 31
- GSP_ACT_ID_SPEED_05_CB = 32
- GSP_ACT_ID_SPEED_10_CB = 33
- GSP_ACT_ID_SPEED_15_CB = 34
- GSP_ACT_ID_SPEED_20_CB = 35

## Optimization notes

- dead objects removed: 0
- zero-area commands skipped: 0
- occlusion cull removed: 0
- background fills removed: 0
- same-color rects merged: 0
- static bake: none
- animation players: 0
- animation frame bytes: 0
