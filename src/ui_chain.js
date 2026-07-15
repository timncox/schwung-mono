/* Mono Voice — compact seven-page editor for a normal Move synth slot. */
import {
    MoveKnob1, MoveShift, MoveMainKnob, MoveLeft, MoveRight, MoveRec
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
    'ALT 1','ALT 2','ALT 3','ALT 4','DRIFT','FOLD','BITS','NOISE',
    'AMP A CURVE','AMP D CURVE','AMP R CURVE','VELOCITY','KEY LEVEL','ENV AMOUNT','PAN KEY','GAIN',
    'FILTER KEY','VELOCITY TO FILTER','FILTER ENV AMOUNT','FILTER DRIVE','HP SLOPE','LP SLOPE','FILTER MIX','FILTER SAT',
    'EQ Q','EQ MIX','BIT DEPTH','DELAY PING PONG','DELAY DUCK','DELAY DRIVE','DELAY MOD RATE','DELAY MOD DEPTH',
    'LFO 1 FADE','LFO 1 DELAY','LFO 1 SLEW','LFO 1 SYMMETRY','LFO 1 STEPS','LFO 1 POLARITY','LFO 1 VELOCITY','LFO 1 KEY TRACK',
    'LFO 2 FADE','LFO 2 DELAY','LFO 2 SLEW','LFO 2 SYMMETRY','LFO 2 STEPS','LFO 2 POLARITY','LFO 2 VELOCITY','LFO 2 KEY TRACK',
    'LFO 3 FADE','LFO 3 DELAY','LFO 3 SLEW','LFO 3 SYMMETRY','LFO 3 STEPS','LFO 3 POLARITY','LFO 3 VELOCITY','LFO 3 KEY TRACK'
];
/* Five-glyph values fit one Move knob column without overwriting the next. */
const LFO_DEST_SCREEN = [
    'OFF','PITCH',
    'SYN1','SYN2','SYN3','SYN4','SYN5','SYN6','SYN7','TUNE',
    'AATK','AHOLD','ADEC','AREL','DIST','VOL','PAN','PORT',
    'FBASE','FWID','HPQ','LPQ','FATK','FDEC','EBASE','EWID',
    'EQFRQ','EQGN','SRATE','DSEND','DTIME','DFB','DBASE','DWID',
    'L1DST','L1TRG','L1WAV','L1MUL','L1SPD','L1INT','L1DEP','L1PHS',
    'L2DST','L2TRG','L2WAV','L2MUL','L2SPD','L2INT','L2DEP','L2PHS',
    'L3DST','L3TRG','L3WAV','L3MUL','L3SPD','L3INT','L3DEP','L3PHS',
    'ALT1','ALT2','ALT3','ALT4','DRIFT','FOLD','BITS','NOISE',
    'ACRV','DCRV','RCRV','VEL','KLVL','EAMT','PKEY','GAIN',
    'KTRK','V2F','FEAMT','FDRV','HPSLP','LPSLP','FMIX','FSAT',
    'EQQ','EQMIX','BITS','PING','DUCK','DDRV','DMODR','DMODD',
    'L1FAD','L1DLY','L1SLW','L1SYM','L1STP','L1POL','L1VEL','L1KEY',
    'L2FAD','L2DLY','L2SLW','L2SYM','L2STP','L2POL','L2VEL','L2KEY',
    'L3FAD','L3DLY','L3SLW','L3SYM','L3STP','L3POL','L3VEL','L3KEY'
];
const LFO_MODE_VALUES = [0, 32, 64, 96, 127];
const LFO_TRIGGER_NAMES = ['Free', 'Retrigger', 'Hold', 'One Shot', 'Half Shot'];
const LFO_TRIGGER_SCREEN = ['FREE', 'TRIG', 'HOLD', 'ONE', 'HALF'];
const LFO_WAVE_NAMES = ['Sine', 'Saw', 'Triangle', 'Square', 'Random'];
const LFO_WAVE_SCREEN = ['SINE', 'SAW', 'TRI', 'SQR', 'RAND'];
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
const COMMON_SHIFT = [null,
    ['ACRV','DCRV','RCRV','VEL','KLVL','EAMT','PKEY','GAIN'],
    ['KTRK','V2F','EAMT','FDRV','HPSLP','LPSLP','MIX','SAT'],
    ['EQQ','EQMIX','BITS','PING','DUCK','DDRV','DMODR','DMODD'],
    ['FADE','DELAY','SLEW','SYMM','STEPS','POL','VEL','KEY'],
    ['FADE','DELAY','SLEW','SYMM','STEPS','POL','VEL','KEY'],
    ['FADE','DELAY','SLEW','SYMM','STEPS','POL','VEL','KEY']
];

let page = 0, machine = 0, shift = false, shiftVisual = false;
let recordArmed = false, tickCount = 0;
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

function shiftLayer() { return shiftActive(); }
function names() { return shiftLayer() ? (page === 0 ? SYNTH_SHIFT[machine] : COMMON_SHIFT[page])
                                      : (page === 0 ? SYNTH[machine] : COMMON[page]); }
function activeValues() { return shiftLayer() ? altValues : values; }
function isLfoDestination(i) { return !shiftLayer() && page >= 4 && i === 0; }
function isLfoTrigger(i) { return !shiftLayer() && page >= 4 && i === 1; }
function isLfoWave(i) { return !shiftLayer() && page >= 4 && i === 2; }
function isLfoMode(i) { return isLfoTrigger(i) || isLfoWave(i); }
function destinationIndex(value) { return Math.max(0, Math.min(LFO_DESTS.length - 1, Math.round(value))); }
function lfoModeIndex(value) {
    const raw = Math.max(0, Math.min(127, Math.round(value)));
    return Math.max(0, Math.min(4, Math.floor(raw * 5 / 128)));
}
function displayValue(i, value) {
    if (isLfoDestination(i)) return LFO_DEST_SCREEN[destinationIndex(value)];
    if (isLfoTrigger(i)) return LFO_TRIGGER_SCREEN[lfoModeIndex(value)];
    if (isLfoWave(i)) return LFO_WAVE_SCREEN[lfoModeIndex(value)];
    return `${value}`.padStart(3, '0');
}
function announcedValue(i, value) {
    if (isLfoDestination(i)) return LFO_DESTS[destinationIndex(value)];
    if (isLfoTrigger(i)) return LFO_TRIGGER_NAMES[lfoModeIndex(value)];
    if (isLfoWave(i)) return LFO_WAVE_NAMES[lfoModeIndex(value)];
    return displayValue(i, value);
}

function fetchAll() {
    const mv = gp('machine');
    if (mv === null) return false;
    machine = Math.max(0, Math.min(MACHINES.length - 1, parseInt(mv, 10) || 0));
    recordArmed = parseInt(gp('record') || '0', 10) !== 0;
    host_module_set_param('page', `${page}`);
    for (let i = 0; i < 8; i++) values[i] = parseInt(gp(`p${i + 1}`) || '0', 10);
    for (let i = 0; i < 8; i++) altValues[i] = parseInt(gp(`alt${i + 1}`) || '0', 10);
    return true;
}

function toggleRecord() {
    recordArmed = !recordArmed;
    host_module_set_param('record', recordArmed ? '1' : '0');
    announce(recordArmed ? 'Automation recording armed' : 'Automation recording off');
    needsRedraw = true;
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
    let v;
    if (isLfoDestination(i)) {
        v = Math.max(0, Math.min(LFO_DESTS.length - 1, destinationIndex(target[i]) + delta));
    } else if (isLfoMode(i)) {
        const next = Math.max(0, Math.min(4, lfoModeIndex(target[i]) + delta));
        v = LFO_MODE_VALUES[next];
    } else {
        v = Math.max(0, Math.min(127, target[i] + delta));
    }
    if (v === target[i]) return;
    target[i] = v;
    host_module_set_param(shiftLayer() ? `alt${i + 1}` : `p${i + 1}`, `${v}`);
    announceParameter(names()[i], announcedValue(i, v));
    needsRedraw = true;
}

function draw() {
    clear_screen();
    drawHeader(recordArmed ? `REC · ${MACHINES[machine]}`
                           : `MONO V · ${MACHINES[machine]}`);
    const n = names();
    const shown = activeValues();
    const first = focusBank * 4;
    for (let column = 0; column < 4; column++) {
        const i = first + column;
        const x = column * 32 + 2;
        print(x, 18, n[i], 1);
        print(x, 34, displayValue(i, shown[i]), 1);
    }
    drawFooter({left: `${shiftLayer() ? `SHIFT ${PAGES[page]}` : PAGES[page]} K${first + 1}-${first + 4}`,
                right: recordArmed ? 'REC: turn knobs'
                    : (shiftActive() ? 'jog=machine' : 'jog=page')});
    needsRedraw = false;
}

globalThis.init = function() {
    ready = fetchAll();
    needsRedraw = true;
    announceView('Mono Voice');
};

globalThis.onResume = function() { ready = fetchAll(); needsRedraw = true; };

globalThis.tick = function() {
    tickCount++;
    if (!ready) ready = fetchAll();
    if (ready && tickCount % 6 === 0) {
        const nextRecord = parseInt(gp('record') || '0', 10) !== 0;
        if (nextRecord !== recordArmed) {
            recordArmed = nextRecord;
            needsRedraw = true;
        }
    }
    const active = shiftActive();
    if (active !== shiftVisual) { shiftVisual = active; needsRedraw = true; }
    if (needsRedraw) draw();
};

globalThis.onMidiMessageInternal = function(data) {
    if ((data[0] & 0xF0) !== 0xB0) return;
    const cc = data[1], val = data[2];
    if (cc === MoveShift) { shift = val > 0; needsRedraw = true; return; }
    if (cc === MoveRec && val > 0) { toggleRecord(); return; }
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
