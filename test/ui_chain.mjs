/*
 * Mono Voice — chain UI harness.
 *
 * src/ui_chain.js is the surface you actually touch when Mono Voice sits in a
 * Move synth slot, and until now it had no coverage at all. In the sibling
 * module Work, three of the four bugs reported from hardware lived in exactly
 * that file while the engine suite passed 445 checks — they were not engine
 * bugs, they were "does this behave like an instrument" bugs.
 *
 * What this pins:
 *   - host_module_get_param is a BLOCKING round-trip to the shim, serviced once
 *     per SPI frame (~23 ms) and abandoned after 100 ms. The channel serves
 *     about 44 a second in total, so round-trips are counted, not just values.
 *   - A read that times out returns null. Folding that into a default zeroes
 *     the mirror, and the next knob turn writes the zero back to the DSP.
 *   - decodeDelta reports ACCUMULATED movement, so a raw delta on a short range
 *     lands on an end stop every time.
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = fs.readFileSync(path.join(root, 'src/ui_chain.js'), 'utf8');

const MoveKnob1 = 71, MoveShift = 1, MoveMainKnob = 2;
const MoveLeft = 5, MoveRight = 6, MoveRec = 86;

const constants = {
    MoveKnob1, MoveShift, MoveMainKnob, MoveLeft, MoveRight, MoveRec,
    MoveMainButton: 3, MoveBack: 4
};

const params = new Map([
    ['machine', '0'], ['record', '0'], ['page', '0'],
    ['arp_enabled', '0'], ['arp_latch', '0'], ['arp_mode', '0'],
    ['arp_rate', '3'], ['arp_octaves', '1'], ['arp_gate', '92'],
    ['arp_length', '16'], ['arp_velocity', '0'],
    ['arp_offsets', new Array(16).fill(0).join(',')],
    ['user_wave_mask', '0']
]);
for (let i = 1; i <= 8; i++) { params.set(`p${i}`, '64'); params.set(`alt${i}`, '64'); }

let roundTrips = 0;
let readFailures = 0;
const announcements = [];
const writes = [];
const printed = [];
const keyReads = new Map();

/* The engine's external-CC counter. mono_cc_param bumps it; mono_set_param
 * (this editor's own knob writes) deliberately does NOT — that asymmetry is
 * the whole mechanism, so the mock has to model it rather than smooth it. */
let ccRevision = 0;

/* Simulate what an external MIDI CC does inside the DSP: write the parameter
 * behind the editor's back and bump the counter. The editor cannot see the CC
 * itself — schwung delivers cable-2 MIDI to the DSP, and onMidiMessageExternal
 * is an overtake-only hook, so a chain UI has no external-MIDI path at all. */
function externalCC(slot, value, bank = 'p') {
    params.set(`${bank}${slot + 1}`, String(value));
    ccRevision++;
}

const context = vm.createContext({
    console, Math, Number, JSON, String, Array, parseInt, parseFloat, isFinite,
    clear_screen() {}, print(x, y, text) { printed.push(String(text)); },
    fill_rect() {}, draw_rect() {},
    text_width(t) { return String(t).length * 6; },
    move_midi_internal_send() {},
    host_module_get_param(key) {
        roundTrips++;
        keyReads.set(key, (keyReads.get(key) ?? 0) + 1);
        if (readFailures > 0) { readFailures--; return null; }
        /* Served by mono_get_param: one read carrying the external-CC counter
         * and the record-arm flag. */
        if (key === 'ui_poll') return `${ccRevision}:${params.get('record') ?? '0'}`;
        /* Served by mono_get_param: the selected page's eight primary values
         * and eight Shift-bank values, "p1,..,p8|alt1,..,alt8". */
        if (key === 'page_values') {
            const bank = (prefix) => Array.from({ length: 8 },
                (_, i) => params.get(`${prefix}${i + 1}`) ?? '0').join(',');
            return `${bank('p')}|${bank('alt')}`;
        }
        return params.get(key) ?? '0';
    },
    host_module_set_param(key, value) {
        writes.push({ key, value: String(value) });
        params.set(key, String(value));
    }
});

function synthetic(exports) {
    return new vm.SyntheticModule(Object.keys(exports), function initialize() {
        for (const [name, value] of Object.entries(exports)) this.setExport(name, value);
    }, { context });
}

const modules = new Map([
    ['/data/UserData/schwung/shared/constants.mjs', synthetic(constants)],
    ['/data/UserData/schwung/shared/input_filter.mjs', synthetic({
        /* Transcribed from schwung's src/shared/input_filter.mjs. The
         * accumulated-count contract is the whole reason the knob maths needed
         * fixing, so it must not be simplified here. */
        decodeDelta(v) {
            if (v === 0) return 0;
            if (v >= 1 && v <= 63) return v;
            if (v >= 65 && v <= 127) return -(128 - v);
            return 0;
        }
    })],
    ['/data/UserData/schwung/shared/menu_layout.mjs', synthetic({
        drawMenuHeader(text) { printed.push(String(text)); },
        drawMenuFooter(f) { printed.push(`${f?.left ?? ''} ${f?.right ?? ''}`); }
    })],
    ['/data/UserData/schwung/shared/screen_reader.mjs', synthetic({
        announce(text) { announcements.push(String(text)); },
        announceParameter(label, value) { announcements.push(`${label} ${value}`); },
        announceView(text) { announcements.push(String(text)); }
    })]
]);

const module_ = new vm.SourceTextModule(source, { context, identifier: 'ui_chain.js' });
await module_.link((specifier) => {
    const found = modules.get(specifier);
    assert(found, `unexpected import: ${specifier}`);
    return found;
});
await module_.evaluate();

const ui = context;
const cc = (num, value) => ui.onMidiMessageInternal([0xb0, num, value]);
const settle = (n = 20) => { for (let i = 0; i < n; i++) ui.tick(); };

/* ------------------------------------------------------------------ tests */

ui.init();

/* A jog turn through the pages used to call fetchAll() inline — about twenty
 * blocking round-trips, most of a second of frozen UI, per detent. */
roundTrips = 0;
cc(MoveMainKnob, 1);
assert(roundTrips === 0,
    `changing page cost ${roundTrips} blocking round-trips on the input path; ` +
    'it must defer the refresh to a later tick');

roundTrips = 0;
cc(MoveKnob1, 1);
assert(roundTrips === 0,
    `turning a knob cost ${roundTrips} blocking round-trips`);

/* ...and the deferred refresh must actually happen. */
settle(4);
assert(roundTrips > 0, 'the deferred refresh never ran');

/* Steady state must stay inside what the param channel can serve. The arp
 * pages used to run a full fetchAll six times a second — three times the
 * budget, which is what made reads elsewhere time out. */
cc(MoveMainKnob, 1);                       /* SYNTH -> AMP */
settle(10);
roundTrips = 0;
settle(44);                                /* one second */
assert(roundTrips <= 20,
    `a sound page costs ${roundTrips} round-trips per second at idle`);

for (let i = 0; i < 7; i++) { cc(MoveMainKnob, 1); settle(6); }   /* onto ARP */
roundTrips = 0;
settle(44);
assert(roundTrips <= 44,
    `the arp page costs ${roundTrips} round-trips per second — more than the ` +
    'channel can serve, so other reads start timing out');

/* decodeDelta is accumulated. One detent moves one; a fast spin moves a
 * quarter of the range, never straight to an end stop. */
params.set('p1', '64');
ui.init();
settle(6);
writes.length = 0;
cc(MoveKnob1, 1);                          /* SYNTH knob 1, range 0-127 */
let write = writes.find(w => w.key === 'p1');
assert.equal(write?.value, '65', `one detent moved p1 to ${write?.value}, expected 65`);

params.set('p1', '64');
ui.init();
settle(6);
writes.length = 0;
cc(MoveKnob1, 40);                         /* a fast spin */
write = writes.find(w => w.key === 'p1');
assert.equal(write?.value, '96',
    `a fast spin moved p1 to ${write?.value}, expected 96 (a quarter of 0-127)`);

/* The jog steers a six-entry machine list under Shift and a nine-entry page
 * list otherwise. A raw accumulated delta lands on an end stop; one detent
 * must move one. */
ui.init();
settle(6);
writes.length = 0;
cc(MoveShift, 127);
cc(MoveMainKnob, 20);                      /* a fast spin over 6 machines */
cc(MoveShift, 0);
write = writes.find(w => w.key === 'machine');
assert.equal(write?.value, '2',
    `a fast jog spin set machine to ${write?.value}, expected 2 of 0-5`);

/* A read that times out must leave the mirror alone. Folding null into 0 meant
 * the next knob turn wrote that 0 straight back to the DSP. */
params.set('p1', '100');
ui.init();
settle(8);
readFailures = 400;
settle(40);
readFailures = 0;
writes.length = 0;
cc(MoveKnob1, 1);
write = writes.find(w => w.key === 'p1');
assert.equal(write?.value, '101',
    `after a dead param channel the UI wrote p1=${write?.value}; it must continue ` +
    'from 100, not from a zeroed mirror');
assert(!writes.some(w => w.key === 'p1' && w.value === '0'),
    'a failed read produced a write of 0 — that is the silent patch-corruption bug');

/* ------------------------------------------- external MIDI CC follow-through
 *
 * Reported from hardware on mono-voice, 2026-09-01: "midi CCs are being heard
 * — but I can't see the values change on the screen, and if I set a value by
 * CC then turn the Move knob, it starts from the old value."
 *
 * Both are one bug. An external CC is applied inside the DSP; this editor
 * mirrors parameters in values[]/altValues[] and, on the sound pages, had no
 * refresh path at all — not since MIDI CC shipped in v0.4.0. So the screen
 * kept drawing the stale mirror, and adjust() sent stale+delta, undoing the CC.
 */

/* Reset to a known state. Machine matters: an earlier test leaves machine 2
 * selected, whose SYNTH knob 1 draws as a semitone offset rather than the raw
 * value, and page 0 machine 0 is the plain numeric case this block reasons
 * about. */
for (let i = 1; i <= 8; i++) { params.set(`p${i}`, '64'); params.set(`alt${i}`, '64'); }
params.set('record', '0');
params.set('machine', '0');
ui.init();
settle(8);

/* 1. The screen must show a CC edit. */
printed.length = 0;
externalCC(0, 100);
settle(12);
assert(printed.includes('100'),
    'an external CC moved SYNTH knob 1 to 100 and the screen never drew it — ' +
    'the editor is still showing its stale mirror');

/* 2. ...and the next Move knob turn must continue from the CC value, not
 *    overwrite it with the old one. This is the half that loses your edit. */
writes.length = 0;
cc(MoveKnob1, 1);
let ccWrite = writes.find(w => w.key === 'p1');
assert.equal(ccWrite?.value, '101',
    `after a CC set p1=100 the knob wrote ${ccWrite?.value}; it must continue ` +
    'from 100, not from the pre-CC mirror');

/* 3. The Shift bank follows too — page_values carries both banks. */
externalCC(2, 20, 'alt');
settle(12);
writes.length = 0;
cc(MoveShift, 127);
cc(MoveKnob1 + 2, 1);
cc(MoveShift, 0);
ccWrite = writes.find(w => w.key === 'alt3');
assert.equal(ccWrite?.value, '21',
    `after a CC set alt3=20 the Shift knob wrote ${ccWrite?.value}, expected 21`);

/* 4. The editor must NOT chase its own writes. The engine's general `revision`
 *    counter also advances on mono_set_param, so watching that instead would
 *    make every knob detent trigger a page read — and a read racing a write
 *    reverts the mirror mid-turn. */
settle(12);
keyReads.set('page_values', 0);
for (let i = 0; i < 10; i++) { cc(MoveKnob1, 1); settle(4); }
assert.equal(keyReads.get('page_values'), 0,
    `ten knob detents caused ${keyReads.get('page_values')} page_values reads — ` +
    'the editor is following its own writes, not external CC');

/* 5. Following a CC sweep must stay inside what the param channel serves.
 *    A controller sweep bumps the counter on every message; the refresh is one
 *    joined read, not sixteen. */
settle(12);
roundTrips = 0;
for (let i = 0; i < 44; i++) { externalCC(0, 40 + i); ui.tick(); }
assert(roundTrips <= 30,
    `following a one-second CC sweep cost ${roundTrips} round-trips of the ~44 ` +
    'the channel serves — it must not claim the whole channel');

/* 6. An engine that does not serve ui_poll must not arm recording. Reads that
 *    fail come back null, but a version-skewed .so can answer a short or empty
 *    tuple — and `parseInt(undefined) !== 0` is true, which would silently turn
 *    on live lock recording and start overwriting steps. */
params.set('record', '0');
ui.init();
settle(8);
assert(!printed.some(t => t.includes('REC')),
    'recording is armed before the skew test even starts');
const realGet = context.host_module_get_param;
context.host_module_get_param = (key) => (key === 'ui_poll' ? '0' : realGet(key));
printed.length = 0;
settle(16);
context.host_module_get_param = realGet;
assert(!printed.some(t => t.includes('REC')),
    'an engine that does not serve ui_poll armed live lock recording by itself');

/* 7. A dead param channel must not let a stale poll corrupt the mirror. */
params.set('p1', '77');
ui.init();
settle(8);
readFailures = 400;
settle(40);
readFailures = 0;
writes.length = 0;
cc(MoveKnob1, 1);
ccWrite = writes.find(w => w.key === 'p1');
assert.equal(ccWrite?.value, '78',
    `after a dead channel the CC follower wrote p1=${ccWrite?.value}; it must ` +
    'continue from 77');

console.log('mono chain UI: param-channel, knob response, refresh, and ' +
    'external-CC follow tests passed');
