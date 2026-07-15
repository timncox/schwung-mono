/* Mono Voice — compact seven-page editor for a normal Move synth slot. */
import {
    MoveKnob1, MoveShift, MoveMainKnob, MoveLeft, MoveRight
} from '/data/UserData/schwung/shared/constants.mjs';
import { decodeDelta } from '/data/UserData/schwung/shared/input_filter.mjs';
import { drawMenuHeader as drawHeader, drawMenuFooter as drawFooter }
    from '/data/UserData/schwung/shared/menu_layout.mjs';
import { announce, announceParameter, announceView }
    from '/data/UserData/schwung/shared/screen_reader.mjs';

const MACHINES = ['SW SAW', 'SW PULS', 'SW ENS', 'SID6581', 'DIGIPRO', 'FM+STAT'];
const PAGES = ['SYNTH', 'AMP', 'FILTER', 'EFFECT', 'LFO 1', 'LFO 2', 'LFO 3'];
const LFO_DESTS = [
    'OFF','PITCH',
    'SYN 1','SYN 2','SYN 3','SYN 4','SYN 5','SYN 6','SYN 7','TUNE',
    'AMP ATK','AMP HLD','AMP DEC','AMP REL','DIST','VOLUME','PAN','PORT',
    'FLT BASE','FLT WID','HP Q','LP Q','F ATK','F DEC','F BASE','F WID',
    'EQ FREQ','EQ GAIN','SR RED','D SEND','D TIME','D FB','D BASE','D WIDTH',
    'L1 DEST','L1 TRIG','L1 WAVE','L1 MULT','L1 SPEED','L1 INTL','L1 DEPTH','L1 PHASE',
    'L2 DEST','L2 TRIG','L2 WAVE','L2 MULT','L2 SPEED','L2 INTL','L2 DEPTH','L2 PHASE',
    'L3 DEST','L3 TRIG','L3 WAVE','L3 MULT','L3 SPEED','L3 INTL','L3 DEPTH','L3 PHASE',
    'ALT 1','ALT 2','ALT 3','ALT 4','DRIFT','FOLD','BITS','NOISE'
];
const COMMON = [
    null,
    ['ATK','HOLD','DEC','REL','DIST','VOL','PAN','PORT'],
    ['BASE','WDTH','HPQ','LPQ','FATK','FDEC','FBA','FWD'],
    ['EQF','EQG','SRR','DSND','DTIM','DFB','DBAS','DWID'],
    ['DEST','TRIG','WAVE','MULT','SPD','INTL','DPTH','PHAS'],
    ['DEST','TRIG','WAVE','MULT','SPD','INTL','DPTH','PHAS'],
    ['DEST','TRIG','WAVE','MULT','SPD','INTL','DPTH','PHAS']
];
const SYNTH = [
    ['UNIL','UNIW','UNIX','SUBX','SUB1','SUB2','-','TUNE'],
    ['UNIL','UNIW','UNIX','PW','PWAD','PWRS','-','TUNE'],
    ['PCH2','PCH3','PCH4','WAVE','PW','CHRL','CHRW','TUNE'],
    ['PW','PWAD','PWRS','WAVE','MOD','MSRC','MFRQ','TUNE'],
    ['WAVE','WP','WPM','WPRS','SYNC','SFRQ','-','TUNE'],
    ['1FRQ','1FIN','1FB','1ENV','2FRQ','2VOL','TONE','TUNE']
];
const SYNTH_SHIFT = [
    ['BRITE','STACK','SUBPW','SUBO','DRIFT','FOLD','BITS','NOISE'],
    ['ASYM','STACK','SUB','SYNC','DRIFT','FOLD','BITS','NOISE'],
    ['LEV1','LEV2','LEV3','LEV4','DRIFT','FOLD','BITS','NOISE'],
    ['RING','SUB','MODMX','CHAOS','DRIFT','FOLD','BITS','NOISE'],
    ['WAVE2','BLEND','DETUN','OCT','DRIFT','FOLD','BITS','NOISE'],
    ['2FINE','2FB','3FRQ','3LVL','DRIFT','FOLD','BITS','NOISE']
];

let page = 0, machine = 0, shift = false, shiftVisual = false;
let values = new Array(8).fill(0);
let altValues = new Array(8).fill(0);
let needsRedraw = true, ready = false, focusBank = 0;

function gp(key) {
    const v = host_module_get_param(key);
    return v === null || v === undefined ? null : String(v);
}

function shiftActive() {
    if (typeof shadow_get_shift_held === 'function' && shadow_get_shift_held() !== 0)
        return true;
    return shift;
}

function shiftLayer() { return page === 0 && shiftActive(); }
function names() { return page === 0 ? (shiftLayer() ? SYNTH_SHIFT[machine] : SYNTH[machine]) : COMMON[page]; }
function activeValues() { return shiftLayer() ? altValues : values; }
function isLfoDestination(i) { return page >= 4 && i === 0; }
function destinationIndex(value) { return Math.max(0, Math.min(LFO_DESTS.length - 1, Math.round(value))); }
function displayValue(i, value) {
    return isLfoDestination(i) ? LFO_DESTS[destinationIndex(value)]
        : `${value}`.padStart(3, '0');
}

function fetchAll() {
    const mv = gp('machine');
    if (mv === null) return false;
    machine = Math.max(0, Math.min(MACHINES.length - 1, parseInt(mv, 10) || 0));
    host_module_set_param('page', `${page}`);
    for (let i = 0; i < 8; i++) values[i] = parseInt(gp(`p${i + 1}`) || '0', 10);
    for (let i = 0; i < 8; i++) altValues[i] = parseInt(gp(`alt${i + 1}`) || '0', 10);
    return true;
}

function setPage(next) {
    page = (next + PAGES.length) % PAGES.length;
    focusBank = 0;
    host_module_set_param('page', `${page}`);
    fetchAll();
    announce(`${PAGES[page]} page`);
    needsRedraw = true;
}

function setMachine(next) {
    machine = (next + MACHINES.length) % MACHINES.length;
    host_module_set_param('machine', `${machine}`);
    fetchAll();
    announceParameter('Machine', MACHINES[machine]);
    needsRedraw = true;
}

function adjust(i, delta) {
    focusBank = i >= 4 ? 1 : 0;
    const target = activeValues();
    const v = isLfoDestination(i)
        ? Math.max(0, Math.min(LFO_DESTS.length - 1, destinationIndex(target[i]) + delta))
        : Math.max(0, Math.min(127, target[i] + delta));
    if (v === target[i]) return;
    target[i] = v;
    host_module_set_param(shiftLayer() ? `alt${i + 1}` : `p${i + 1}`, `${v}`);
    announceParameter(names()[i], displayValue(i, v));
    needsRedraw = true;
}

function draw() {
    clear_screen();
    drawHeader(`MONO V · ${MACHINES[machine]}`);
    const n = names();
    const shown = activeValues();
    const first = focusBank * 4;
    for (let column = 0; column < 4; column++) {
        const i = first + column;
        const x = column * 32 + 2;
        print(x, 18, n[i], 1);
        print(x, 34, displayValue(i, shown[i]), 1);
    }
    drawFooter({left: `${shiftLayer() ? 'SHIFT SYN' : PAGES[page]} K${first + 1}-${first + 4}`,
                right: shiftActive() ? 'jog=machine' : 'jog=page'});
    needsRedraw = false;
}

globalThis.init = function() {
    ready = fetchAll();
    needsRedraw = true;
    announceView('Mono Voice');
};

globalThis.onResume = function() { ready = fetchAll(); needsRedraw = true; };

globalThis.tick = function() {
    if (!ready) ready = fetchAll();
    const active = shiftActive();
    if (active !== shiftVisual) { shiftVisual = active; needsRedraw = true; }
    if (needsRedraw) draw();
};

globalThis.onMidiMessageInternal = function(data) {
    if ((data[0] & 0xF0) !== 0xB0) return;
    const cc = data[1], val = data[2];
    if (cc === MoveShift) { shift = val > 0; needsRedraw = true; return; }
    if (cc === MoveMainKnob) {
        const d = decodeDelta(val);
        if (d) shiftActive() ? setMachine(machine + d) : setPage(page + d);
        return;
    }
    if (cc === MoveLeft && val >= 64) { setPage(page - 1); return; }
    if (cc === MoveRight && val >= 64) { setPage(page + 1); return; }
    if (cc >= MoveKnob1 && cc < MoveKnob1 + 8) {
        const d = decodeDelta(val);
        if (d) adjust(cc - MoveKnob1, d);
    }
};
