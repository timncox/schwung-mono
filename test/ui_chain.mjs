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

const context = vm.createContext({
    console, Math, Number, JSON, String, Array, parseInt, parseFloat, isFinite,
    clear_screen() {}, print() {}, fill_rect() {}, draw_rect() {},
    text_width(t) { return String(t).length * 6; },
    move_midi_internal_send() {},
    host_module_get_param(key) {
        roundTrips++;
        if (readFailures > 0) { readFailures--; return null; }
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
        drawMenuHeader() {}, drawMenuFooter() {}
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

console.log('mono chain UI: param-channel, knob response, and refresh tests passed');
