# app_note_router: source-owned synth notes

The router borrows an `app_synth` instance. UI and USB producers call its public
control methods; the audio task remains the sole synth renderer. The router owns
touch hold bits, MIDI key/sustain bits for all 16 channels, and diagnostic counts.
No render callback, board driver or USB stack is part of this component.

Create the synth, create one router, then share that router with the UI and USB
owner. A pitch sounds while any touch or MIDI channel still owns it. Touch
release and page changes call `release_touch`; USB detach calls
`midi_disconnect`. Neither cancels the other source. Duplicate same-source note
ons retrigger; another source acquiring the same held pitch merges ownership.
The existing synth still has four physical voices and its normal stealing law;
the router does not create extra voices or promise that every held pitch sounds.

USB input accepts MIDI 1.0 channel messages on virtual cable zero. Note On with
velocity zero is Note Off. CC64 is sustain per channel, CC123 withdraws physical
keys while honoring sustain, CC121 releases sustain, and CC120 withdraws all
MIDI holds on that channel. Source-local withdrawals use the synth release
envelope; the global `panic` hard-clears all voices and ownership. Other channel
messages and valid System Common/realtime/SysEx packets are ignored; this
component does not change patches, pitch bend or implement a MIDI clock sync.
Malformed CIN/status/data and USB running status are rejected.

Successful note-ons update ownership only after the synth command is accepted.
Rejected first notes do not create phantom holds. Note-off paths use the synth's
guaranteed release fallback even when its command ring is full. Call the shared
bulk packet decoder in `app_usb_midi` to withdraw MIDI holds after a malformed,
short or rejected transfer instead of continuing a partial gesture.

The optional borrowed TX sink receives only touch Note On/Off and global panic,
on virtual cable/channel zero. Incoming MIDI is never echoed. The sink must be
nonblocking and must not call back into this router. Rejected TX increments a
counter without suppressing local sound. The USB owner installs its queue sink
after startup and removes it before destroying the queue/context.

All public methods serialize with a priority-inheriting FreeRTOS mutex on the
device; no audio code takes that lock. Native builds use a C11 atomic lock.
Calls may come from ordinary tasks, not ISRs. Lock holders perform only bounded
128-note scans, enqueue commands and call the nonblocking TX sink. Destroy only
after every producer has stopped and the USB owner has detached its sink; the
borrowed synth must remain alive through destruction. Allocation happens only
at create, which returns NULL on invalid synth/allocation/mutex failure.

`python tools/test_midi.py` checks source/channel overlap, selective detach,
sustain, controller clear, full synth queues, malformed packets and no-echo
against the real portable synth. It does not connect to a USB device.
