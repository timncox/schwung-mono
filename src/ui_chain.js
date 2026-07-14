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

let page = 0, machine = 0, shift = false, values = new Array(8).fill(0);
let needsRedraw = true, ready = false, focusBank = 0;

function gp(key) {
    const v = host_module_get_param(key);
    return v === null || v === undefined ? null : String(v);
}

function names() { return page === 0 ? SYNTH[machine] : COMMON[page]; }

function fetchAll() {
    const mv = gp('machine');
    if (mv === null) return false;
    machine = Math.max(0, Math.min(MACHINES.length - 1, parseInt(mv, 10) || 0));
    host_module_set_param('page', `${page}`);
    for (let i = 0; i < 8; i++) values[i] = parseInt(gp(`p${i + 1}`) || '0', 10);
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
    const v = Math.max(0, Math.min(127, values[i] + delta));
    if (v === values[i]) return;
    values[i] = v;
    host_module_set_param(`p${i + 1}`, `${v}`);
    announceParameter(names()[i], `${v}`);
    needsRedraw = true;
}

function draw() {
    clear_screen();
    drawHeader(`MONO V · ${MACHINES[machine]}`);
    const n = names();
    const first = focusBank * 4;
    for (let column = 0; column < 4; column++) {
        const i = first + column;
        const x = column * 32 + 2;
        print(x, 18, n[i], 1);
        print(x, 34, `${values[i]}`.padStart(3, '0'), 1);
    }
    drawFooter({left: `${PAGES[page]} K${first + 1}-${first + 4}`,
                right: shift ? 'jog=machine' : 'jog=page'});
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
    if (needsRedraw) draw();
};

globalThis.onMidiMessageInternal = function(data) {
    if ((data[0] & 0xF0) !== 0xB0) return;
    const cc = data[1], val = data[2];
    if (cc === MoveShift) { shift = val > 0; needsRedraw = true; return; }
    if (cc === MoveMainKnob) {
        const d = decodeDelta(val);
        if (d) shift ? setMachine(machine + d) : setPage(page + d);
        return;
    }
    if (cc === MoveLeft && val >= 64) { setPage(page - 1); return; }
    if (cc === MoveRight && val >= 64) { setPage(page + 1); return; }
    if (cc >= MoveKnob1 && cc < MoveKnob1 + 8) {
        const d = decodeDelta(val);
        if (d) adjust(cc - MoveKnob1, d);
    }
};
