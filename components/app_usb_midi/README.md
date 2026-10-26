# app_usb_midi: exclusive Host / Iris device owner

Vendor means the existing ESP-Iris dual CDC ACM management device, not a new
USB Vendor Class interface. MIDI IO means this product is the USB Host for a
class-compliant USB-MIDI 1.0 keyboard. S31 has one controller and one UTMI PHY;
the roles are exclusive. Vibe Mode's retained USB/partition/update behavior is
unchanged. The board supplies OTG power automatically; this component does not
claim to measure VBUS or control a separate power switch.

## Ownership and startup

The application owns the synth and note router. Start Iris management, create
the router, then call `app_usb_midi_create(router, &usb)`. The component borrows
the router until successful shutdown. It creates one 6144-byte owner task,
bounded TX queue and short snapshot mutex. Only one device owner may exist.
Vendor is selected on every boot; a transient Host selection is not persisted.

The public status header has no ESP-IDF types and can be used by the portable UI.
Mode requests are asynchronous and may come from UI/tasks. Only Vendor and Host
are requestable. Status separately reports requested mode, actual owner, state,
connection/directions, VID/PID, counters and ESP error code. `MODE_NONE` means a
failed transition left neither driver confirmed. Successful queueing is not
evidence of a connected or functioning keyboard.

## Role transitions

The owner uses the public `esp_iris_usb_set_enabled(false, timeout_ms)` contract.
Only ESP_OK permits Host installation; timeout/error never permits PHY stealing.
The public Iris extension suspends both console/data USB users while keeping
UART/TCP, workers, logs and services alive. This app never calls TinyUSB
uninstall itself or accesses Iris private structures. The consuming product must
retain an available non-USB Iris transport while Host owns USB; compiled TCP
without a live network interface is not an available transport.

Host uses exactly `espressif/usb ==1.6.0`, a managed Host Library supporting S31
on the pinned SDK. It supplies the daemon/client event APIs but creates no tasks;
this component's single owner drives both handlers. That task claims a standard
alternate-zero MIDI 1.0 Streaming interface, one bulk IN and/or OUT endpoint, and
fixed transfer objects sized to the descriptor's packet size. Configurations
over 4096 bytes and invalid/duplicate endpoints are rejected. Virtual cable zero
is routed; MIDI 2.0 UMP, vendor-specific drivers and multiple attached MIDI
devices are outside this first implementation.

Callbacks only publish completion flags; routing, resubmission and cleanup run
after event dispatch returns. Incoming bulk completions are limited to 512 bytes,
decoded in four-byte events, and never echoed. Touch output is queued separately.
TX overflow schedules external channel-zero All Sound Off after older in-flight
transfers complete, and discards pending TX so a lost release cannot leave a key.

Returning to Vendor disables new TX, withdraws MIDI holds, attempts an external
panic, halts/flushes transfers and pumps all completion callbacks before freeing
buffers. It releases interfaces, closes devices, deregisters the client, waits
for all devices to be freed, uninstalls Host, then resumes Iris USB. No in-flight
transfer or callback context is freed on timeout. Transition failures attempt
Vendor rollback and expose the actual result. If an Iris pause times out while
executing, the owner observes its public pending status and waits for settlement,
then automatically restores Vendor. It never starts Host based on a timed-out
pause or requires another user gesture to recover the management port. Original
errors remain visible, and a repeated mode request can retry.
USB disconnect and protocol re-handshake during a role change are expected.

The pinned S31 CPU reset does not reset HS DMA. A small shutdown handler checks
the controller ownership flag, disconnects its CPU interrupt source routes,
checks the always-on clock status, then disables global interrupts and resets
UTMI/OTG without waiting or freeing. It deliberately leaves the reset controller
clocked until CPU reset so an already entered/pending HCD ISR can safely finish
reading reset MMIO. A terminal shutdown flag prevents further event dispatch,
role changes and transfer submission. Normal transitions use
the complete Host lifecycle, not that emergency reset. When Iris owns USB, its
own shutdown handler remains responsible for quiescence.

## Shutdown and native preview

`app_usb_midi_shutdown(&usb, timeout_ms)` is for an external control task after
UI producers stop. It requests Vendor, waits for teardown/resume, removes the
router's TX sink and frees its resources. On false, the non-NULL handle and
borrowed router/synth must remain alive for another attempt. Never call shutdown
from a USB/Iris callback or while holding a lock needed by the owner.

Native preview builds `usb_midi_native.c` instead of the physical owner. It keeps
Vendor available and explicitly rejects Host with NOT_SUPPORTED; it never
reports a connected keyboard. Native status/requests are serialized by the UI
owner. `python tools/test_midi.py` compiles the real router, packet/descriptor
parsers and synth with the native adapter. Hardware acceptance additionally
requires actual keyboard enumeration/notes/sustain/hotplug, both role directions,
Iris return, DMA-safe reboot and preservation of the retained Vibe Mode contract.

The emergency reset checks cover a stable Host owner. Planned product restart
must first complete normal `app_usb_midi_shutdown()` / Vendor handoff. Concurrent
USB installation/HAL initialization and forced restart remain unvalidated;
source-level guard tests do not establish that overlapping hardware case.

The same runner also compiles `tests/midi_host_lifecycle.c` around the real
production owner with fake USB/FreeRTOS backends. It checks staged startup
rollback, ownership transfer order, receive/transmit routing, TX overflow panic,
in-flight teardown timeouts (buffers stay allocated), successful retry, truthful
Iris-resume failure, late executing-pause timeout recovery, clock-off shutdown
windows and bounded S31 shutdown ordering. Those deterministic host
checks do not substitute for compilation against the pinned official Host Library
or physical USB-role acceptance.
