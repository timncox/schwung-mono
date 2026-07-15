/* Mono — six-track machine synth and lock sequencer, full-surface UI. */
import * as os from 'os';
import {
    MoveKnob1, MoveShift, MoveMainKnob, MoveMainButton, MoveBack, MoveLeft, MoveRight, MoveRec,
    MoveDelete, MoveCopy, MoveUndo,
    Black, White, LightGrey, BrightRed, Blue, Green, BrightGreen,
    Cyan, Purple, YellowGreen, OrangeRed
} from '/data/UserData/schwung/shared/constants.mjs';
import { decodeDelta, setLED } from '/data/UserData/schwung/shared/input_filter.mjs';
import { drawMenuHeader as drawHeader, drawMenuFooter as drawFooter }
    from '/data/UserData/schwung/shared/menu_layout.mjs';
import { announce, announceParameter, announceView }
    from '/data/UserData/schwung/shared/screen_reader.mjs';
import { openTextEntry } from '/data/UserData/schwung/shared/text_entry.mjs';

const MACHINES = ['SW SAW', 'SW PULS', 'SW ENS', 'SID6581', 'DIGIPRO', 'FM+STAT'];
const MACHINE_COLORS = [OrangeRed, BrightRed, YellowGreen, Purple, Cyan, Blue];
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
/* The Move screen gives each knob a 32 px column (five glyphs). Keep a
 * separate compact destination table so long names never overwrite the next
 * column. Full names remain available to the screen reader and Remote UI. */
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
const PLAY_ORDERS = ['FORWARD', 'REVERSE', 'PENDULUM', 'RANDOM'];
const PLAY_ORDER_SCREEN = ['FWD', 'REV', 'PEND', 'RAND'];
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
    ['UNIL','UNIW','SUB1','SUB2','PW','PWAD','PWRS','TUNE'],
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

const TRACK_PADS = [92, 93, 94, 95, 96, 97];
const PAD_MACHINE = 98, PAD_TRANSPORT = 99;
const STEP_FIRST = 16, STEP_COUNT = 16;
const SHIFT_PARAM_BASE = 56;
const PRESET_DIR = '/data/UserData/schwung/presets/mono';
const SAVE_ROW = '[Save current...]';

let track = 0, page = 0, stepPage = 0, machine = 0, transport = 0;
let recordArmed = false;
let patternStart = 0, patternLen = 16, playOrder = 0, playStep = -1;
let swing = 0, trackFollow = true, trackStart = 0, trackLen = 16;
let trackRotate = 0, trackDiv = 1;
let trackStates = new Array(6).fill(0);
let shift = false, deleteHeld = false, deleteUsed = false, tickCount = 0;
let shiftVisual = false;
let values = new Array(8).fill(0), steps = new Array(16).fill(0);
let altValues = new Array(8).fill(0);
let heldStep = null, ready = false, needsRedraw = true, resumePaints = 0;
let focusBank = 0;
let presetMode = false, presetIndex = 0, presets = [];
let deletePresetFile = null;
let seqSetup = false;

function gp(key) {
    const v = host_module_get_param(key);
    return v === null || v === undefined ? null : String(v);
}

function safePresetStem(name) {
    const clean = name.replace(/[\/\\:*?"<>|\x00-\x1f]/g, '-')
        .replace(/\s+/g, ' ').replace(/[. ]+$/g, '').trim();
    return (clean || 'Mono Pattern').slice(0, 64);
}

function loadPresetList() {
    let files = [];
    try { files = os.readdir(PRESET_DIR) || []; } catch (e) { files = []; }
    presets = files.filter(file => /\.json$/i.test(file)).map(file => {
        let name = file.replace(/\.json$/i, '');
        try {
            const parsed = JSON.parse(host_read_file(`${PRESET_DIR}/${file}`) || '{}');
            if (parsed.name) name = String(parsed.name);
        } catch (e) {}
        return {name, file};
    }).sort((a, b) => a.name.toLowerCase().localeCompare(b.name.toLowerCase()));
}

function uniquePresetName(rawName, excludeFile = null) {
    const base = rawName.trim() || 'Mono Pattern';
    const used = new Set(presets.filter(p => p.file !== excludeFile)
        .map(p => p.name.toLowerCase()));
    if (!used.has(base.toLowerCase())) return base;
    let suffix = 2;
    while (used.has(`${base} ${suffix}`.toLowerCase())) suffix++;
    return `${base} ${suffix}`;
}

function uniquePresetStem(rawName, excludeFile = null) {
    const base = safePresetStem(rawName);
    const used = new Set(presets.filter(p => p.file !== excludeFile)
        .map(p => p.file.replace(/\.json$/i, '').toLowerCase()));
    if (!used.has(base.toLowerCase())) return base;
    let suffix = 2, candidate = '';
    do { candidate = `${base.slice(0, 58)} ${suffix++}`; }
    while (used.has(candidate.toLowerCase()));
    return candidate;
}

function writePresetAtomically(file, payload) {
    if (typeof host_write_file !== 'function') return false;
    const path = `${PRESET_DIR}/${file}`;
    const temp = `${path}.tmp`;
    if (!host_write_file(temp, payload)) return false;
    try {
        os.rename(temp, path);
        return true;
    } catch (e) {
        try { os.remove(temp); } catch (ignored) {}
        return false;
    }
}

function openPresetBrowser() {
    loadPresetList();
    presetIndex = 0;
    deletePresetFile = null;
    presetMode = true;
    needsRedraw = true;
    announce(`Mono presets, ${presets.length} saved`);
}

function closePresetBrowser() {
    presetMode = false;
    needsRedraw = true;
    announceView('Mono');
}

function saveCurrentPreset(rawName) {
    const stateJson = gp('state');
    if (!stateJson) { announce('Preset save failed'); return; }
    if (typeof host_ensure_dir === 'function') host_ensure_dir(PRESET_DIR);
    else { try { os.mkdir(PRESET_DIR); } catch (e) {} }
    const name = uniquePresetName(rawName);
    const stem = uniquePresetStem(name);
    let state;
    try { state = JSON.parse(stateJson); } catch (e) { state = stateJson; }
    const payload = JSON.stringify({name, module: 'mono', version: 1, state});
    const ok = writePresetAtomically(`${stem}.json`, payload);
    if (!ok) { announce('Preset save failed'); return; }
    loadPresetList();
    const found = presets.findIndex(p => p.name === name);
    presetIndex = found >= 0 ? found + 1 : 0;
    needsRedraw = true;
    announce(`Saved ${name}`);
}

function startPresetRename() {
    const entry = presets[presetIndex - 1];
    if (!entry) return;
    openTextEntry({
        title: '',
        initialText: entry.name,
        onAnnounce: announce,
        onConfirm: rawName => {
            let payload;
            try { payload = JSON.parse(host_read_file(`${PRESET_DIR}/${entry.file}`) || '{}'); }
            catch (e) { announce('Preset rename failed'); return; }
            const name = uniquePresetName(rawName || entry.name, entry.file);
            const file = `${uniquePresetStem(name, entry.file)}.json`;
            payload.name = name;
            if (!writePresetAtomically(file, JSON.stringify(payload))) {
                announce('Preset rename failed'); return;
            }
            if (file !== entry.file) {
                try { os.remove(`${PRESET_DIR}/${entry.file}`); } catch (e) {}
            }
            loadPresetList();
            presetIndex = Math.max(1, presets.findIndex(p => p.file === file) + 1);
            deletePresetFile = null;
            needsRedraw = true;
            announce(`Renamed ${name}`);
        },
        onCancel: () => { needsRedraw = true; announce('Rename cancelled'); }
    });
}

function deleteSelectedPreset() {
    const entry = presets[presetIndex - 1];
    if (!entry) return;
    if (deletePresetFile !== entry.file) {
        deletePresetFile = entry.file;
        needsRedraw = true;
        announce(`Press Delete again to remove ${entry.name}`);
        return;
    }
    try {
        if (os.remove(`${PRESET_DIR}/${entry.file}`) < 0) throw new Error('remove failed');
    } catch (e) { announce('Preset delete failed'); return; }
    deletePresetFile = null;
    loadPresetList();
    presetIndex = Math.min(presetIndex, presets.length);
    needsRedraw = true;
    announce(`Deleted ${entry.name}`);
}

function startPresetSave() {
    openTextEntry({
        title: '',
        initialText: 'Mono Pattern',
        onAnnounce: announce,
        onConfirm: name => saveCurrentPreset(name || 'Mono Pattern'),
        onCancel: () => { needsRedraw = true; announce('Save cancelled'); }
    });
}

function loadSelectedPreset() {
    const entry = presets[presetIndex - 1];
    if (!entry || typeof host_read_file !== 'function') return;
    let stateJson = null;
    try {
        const payload = JSON.parse(host_read_file(`${PRESET_DIR}/${entry.file}`) || '{}');
        stateJson = typeof payload.state === 'string'
            ? payload.state : JSON.stringify(payload.state);
    } catch (e) {}
    if (!stateJson) { announce('Preset load failed'); return; }
    if (typeof host_module_set_param_blocking === 'function')
        host_module_set_param_blocking('state', stateJson, 1000);
    else
        host_module_set_param('state', stateJson);
    track = Math.max(0, Math.min(5, parseInt(gp('track') || '0', 10)));
    page = Math.max(0, Math.min(6, parseInt(gp('page') || '0', 10)));
    stepPage = Math.max(0, Math.min(3, parseInt(gp('step_page') || '0', 10)));
    fetchAll();
    presetMode = false;
    paintAll(true);
    needsRedraw = true;
    announce(`Loaded ${entry.name}`);
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
function currentParamId(i) { return (shiftLayer() ? SHIFT_PARAM_BASE : 0) + page * 8 + i; }
const FM_RATIO_NAMES = ['1/64','1/32','1/16','3/32','1/8','3/16','1/4','5/16',
    '3/8','7/16','1/2','5/8','3/4','7/8','1','1.25','1.5','1.75','2','2.5',
    '3','3.5','4','5','6','7','8','9','10','12','16','24'];
function bandIndex(value, count) { return Math.max(0, Math.min(count - 1, Math.floor(value * count / 128))); }
function noteName(value) {
    const names = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
    return `${names[value % 12]}${Math.floor(value / 12) - 1}`;
}
function machineParameterValue(i, value) {
    if (shiftLayer() || page !== 0) return null;
    if (machine === 1 && i === 6) return value < 64 ? 'OFF' : 'ON';
    if (machine === 2 && i < 3) {
        if (value < 4) return 'OFF';
        if (value >= 112) return ['6/5','5/4','4/3','3/2'][Math.min(3, Math.floor((value - 112) / 4))];
        return `${Math.round((value - 4) * 24 / 107)}ST`;
    }
    if (machine === 3) {
        if (i === 2) return value < 64 ? 'OFF' : 'ON';
        if (i === 3) return ['TRI','SAW','PULS','MIX','NOIS'][bandIndex(value, 5)];
        if (i === 4) return ['OFF','RING','SYNC','R+S'][bandIndex(value, 4)];
        if (i === 5) return value < 64 ? 'MFRQ' : 'PRCH';
        if (i === 6) return noteName(value);
    }
    if (machine === 4) {
        if (i === 0) return `W${String(bandIndex(value, 32)).padStart(2, '0')}`;
        if (i === 3) return value < 64 ? 'OFF' : 'ON';
        if (i === 4) return ['OFF','SFRQ','PRCH'][bandIndex(value, 3)];
        if (i === 5) return noteName(value);
    }
    if (machine === 5 && (i === 0 || i === 4)) return FM_RATIO_NAMES[bandIndex(value, 32)];
    return null;
}
function secondsFromParam(value, maxSeconds) {
    return value <= 0 ? 0 : 0.002 * Math.pow(maxSeconds / 0.002, value / 127);
}
function shortTime(seconds) {
    if (seconds <= 0) return '0MS';
    if (seconds < 1) return `${Math.round(seconds * 1000)}MS`.slice(0, 5);
    return `${seconds < 10 ? seconds.toFixed(2) : seconds.toFixed(1)}S`.slice(0, 5);
}
function shortHz(value) {
    const hz = 18 * Math.pow(1000, value / 127);
    return hz >= 1000 ? `${(hz / 1000).toFixed(hz < 10000 ? 1 : 0)}K` : `${Math.round(hz)}HZ`;
}
function parameterValue(i, value) {
    const machineValue = machineParameterValue(i, value);
    if (machineValue !== null) return machineValue;
    const pid = currentParamId(i);
    const times = {8:4, 9:4, 10:12, 11:8, 15:3, 20:4, 21:8,
        88:8, 89:4, 90:2, 96:8, 97:4, 98:2, 104:8, 105:4, 106:2};
    if (times[pid]) return shortTime(secondsFromParam(value, times[pid]));
    if (pid === 28) return shortTime(0.015 + 1.82 * value / 127);
    if (pid === 24) return shortHz(value);
    if (pid === 7) { const cents = Math.round((value - 64) * 100 / 64); return `${cents >= 0 ? '+' : ''}${cents}C`; }
    if (pid === 14) { const pan = Math.round((value - 64) * 100 / 64); return pan === 0 ? 'CENTR' : `${pan < 0 ? 'L' : 'R'}${Math.abs(pan)}`; }
    if (pid === 25) { const gain = value - 64; return gain === 0 ? '0DB' : `${gain > 0 ? '+' : ''}${gain}DB`; }
    if (pid === 76 || pid === 77) return value < 64 ? '12DB' : '24DB';
    if (pid === 93 || pid === 101 || pid === 109) return value < 64 ? 'UNI' : 'BI';
    if (pid === 62 || pid === 82) return `${16 - Math.round(value * 12 / 127)}BIT`;
    return `${value}`.padStart(3, '0');
}
function displayValue(i, value) {
    if (isLfoDestination(i)) return LFO_DEST_SCREEN[destinationIndex(value)];
    if (isLfoTrigger(i)) return LFO_TRIGGER_SCREEN[lfoModeIndex(value)];
    if (isLfoWave(i)) return LFO_WAVE_SCREEN[lfoModeIndex(value)];
    return parameterValue(i, value);
}
function announcedValue(i, value) {
    if (isLfoDestination(i)) return LFO_DESTS[destinationIndex(value)];
    if (isLfoTrigger(i)) return LFO_TRIGGER_NAMES[lfoModeIndex(value)];
    if (isLfoWave(i)) return LFO_WAVE_NAMES[lfoModeIndex(value)];
    return displayValue(i, value);
}

function parseSteps() {
    const s = (gp('steps') || '').split(',');
    for (let i = 0; i < 16; i++) steps[i] = parseInt(s[i], 10) || 0;
}

function fetchAll() {
    const status = gp('status');
    if (status === null) return false;
    const p = status.split(':');
    transport = parseInt(p[0], 10) || 0;
    playStep = parseInt(p[1], 10);
    patternLen = parseInt(p[6], 10) || 16;
    recordArmed = parseInt(p[7] || gp('record') || '0', 10) !== 0;
    patternStart = Math.max(0, Math.min(63, parseInt(p[8] || gp('pattern_start') || '0', 10) || 0));
    playOrder = Math.max(0, Math.min(PLAY_ORDERS.length - 1,
        parseInt(p[9] || gp('play_order') || '0', 10) || 0));
    swing = Math.max(0, Math.min(127, parseInt(gp('swing') || '0', 10) || 0));
    trackFollow = parseInt(gp('track_follow') || '1', 10) !== 0;
    trackStart = Math.max(0, Math.min(63, parseInt(gp('track_start') || '0', 10) || 0));
    trackLen = Math.max(1, Math.min(64 - trackStart, parseInt(gp('track_len') || '16', 10) || 16));
    trackRotate = Math.max(0, Math.min(63, parseInt(gp('track_rotate') || '0', 10) || 0));
    trackDiv = Math.max(1, Math.min(8, parseInt(gp('track_div') || '1', 10) || 1));
    const states = (gp('track_states') || '').split(',');
    for (let i = 0; i < 6; i++) trackStates[i] = parseInt(states[i], 10) || 0;
    machine = Math.max(0, Math.min(5, parseInt(gp('machine') || '0', 10)));
    for (let i = 0; i < 8; i++) values[i] = parseInt(gp(`p${i + 1}`) || '0', 10);
    for (let i = 0; i < 8; i++) altValues[i] = parseInt(gp(`alt${i + 1}`) || '0', 10);
    parseSteps();
    return true;
}

function selectTrack(next) {
    track = Math.max(0, Math.min(5, next));
    host_module_set_param('track', `${track}`);
    fetchAll(); paintAll(false); needsRedraw = true;
    announce(`Track ${track + 1}, ${MACHINES[machine]}`);
}

function setPage(next) {
    page = (next + PAGES.length) % PAGES.length;
    focusBank = 0;
    host_module_set_param('page', `${page}`);
    fetchAll(); needsRedraw = true;
    announce(`${PAGES[page]} page`);
}

function setStepPage(next) {
    stepPage = (next + 4) % 4;
    host_module_set_param('step_page', `${stepPage}`);
    parseSteps(); paintSteps(false); needsRedraw = true;
    announce(`Steps ${stepPage * 16 + 1} to ${stepPage * 16 + 16}`);
}

function openSeqSetup() {
    seqSetup = true;
    heldStep = null;
    fetchAll();
    paintSteps(false);
    needsRedraw = true;
    announce('Sequence setup');
}

function closeSeqSetup() {
    seqSetup = false;
    paintAll(true);
    needsRedraw = true;
    announceView('Mono');
}

function setPatternStart(next) {
    host_module_set_param('pattern_start', `${Math.max(0, Math.min(63, next))}`);
    fetchAll(); paintSteps(false); needsRedraw = true;
    announceParameter('Sequence start', `${patternStart + 1}`);
}

function setPatternLength(next) {
    host_module_set_param('pattern_len', `${Math.max(1, Math.min(64 - patternStart, next))}`);
    fetchAll(); paintSteps(false); needsRedraw = true;
    announceParameter('Sequence length', `${patternLen}`);
}

function setPlayOrder(next) {
    playOrder = Math.max(0, Math.min(PLAY_ORDERS.length - 1, next));
    host_module_set_param('play_order', `${playOrder}`);
    fetchAll(); paintSteps(false); needsRedraw = true;
    announceParameter('Play order', PLAY_ORDERS[playOrder]);
}

function setSwing(next) {
    swing = Math.max(0, Math.min(127, next));
    host_module_set_param('swing', `${swing}`);
    needsRedraw = true;
    announceParameter('Swing', `${50 + Math.round(swing * 25 / 127)} percent`);
}

function setTrackTiming(key, value, label) {
    host_module_set_param(key, `${value}`);
    fetchAll(); paintSteps(false); needsRedraw = true;
    announceParameter(label, `${value}`);
}

function adjustSeqSetup(i, delta) {
    focusBank = i >= 4 ? 1 : 0;
    if (i === 0) setPatternStart(patternStart + delta);
    else if (i === 1) setPatternLength(patternLen + delta);
    else if (i === 2) setPlayOrder(playOrder + delta);
    else if (i === 3) setSwing(swing + delta);
    else if (shiftActive()) {
        host_module_set_param('track_follow', '1');
        fetchAll(); paintSteps(false); needsRedraw = true;
        announce(`Track ${track + 1} follows global window`);
    } else if (i === 4) setTrackTiming('track_start', Math.max(0, Math.min(63, trackStart + delta)), 'Track start');
    else if (i === 5) setTrackTiming('track_len', Math.max(1, Math.min(64 - trackStart, trackLen + delta)), 'Track length');
    else if (i === 6) setTrackTiming('track_rotate', (trackRotate + delta + 64) % 64, 'Track rotation');
    else if (i === 7) setTrackTiming('track_div', Math.max(1, Math.min(8, trackDiv + delta)), 'Track division');
}

function cycleMachine(delta) {
    machine = (machine + delta + MACHINES.length) % MACHINES.length;
    host_module_set_param('machine', `${machine}`);
    fetchAll(); paintTracks(false); needsRedraw = true;
    announceParameter('Machine', MACHINES[machine]);
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
    if (heldStep) {
        heldStep.used = true;
        const absoluteParam = (shiftLayer() ? SHIFT_PARAM_BASE : 0) + page * 8 + i;
        if (deleteHeld) {
            deleteUsed = true;
            host_module_set_param('unlock', `${track}:${heldStep.step}:${absoluteParam}:0`);
        } else {
            host_module_set_param('lock', `${track}:${heldStep.step}:${absoluteParam}:${v}`);
        }
        parseSteps(); paintSteps(false);
    } else {
        host_module_set_param(shiftLayer() ? `alt${i + 1}` : `p${i + 1}`, `${v}`);
    }
    announceParameter(names()[i], deleteHeld && heldStep ? 'unlocked' : announcedValue(i, v));
    needsRedraw = true;
}

function toggleTransport() {
    transport = transport ? 0 : 1;
    host_module_set_param('transport', `${transport}`);
    announce(transport ? 'Sequencer playing' : 'Sequencer stopped');
    paintGlobals(false); needsRedraw = true;
}

function toggleRecord() {
    recordArmed = !recordArmed;
    host_module_set_param('record', recordArmed ? '1' : '0');
    announce(recordArmed ? 'Automation recording armed' : 'Automation recording off');
    paintGlobals(false); needsRedraw = true;
}

function paintTracks(force) {
    for (let i = 0; i < 6; i++) {
        const state = trackStates[i];
        const color = state & 2 ? BrightGreen : (state & 1 ? 0x10
            : (i === track ? MACHINE_COLORS[machine] : LightGrey));
        setLED(TRACK_PADS[i], color, force);
    }
}

function paintGlobals(force) {
    setLED(PAD_MACHINE, MACHINE_COLORS[machine], force);
    setLED(PAD_TRANSPORT, transport ? Green : LightGrey, force);
    setLED(MoveRec, recordArmed ? BrightRed : Black, force);
}

function paintSteps(force) {
    const localPlay = playStep >= stepPage * 16 && playStep < stepPage * 16 + 16
        ? playStep - stepPage * 16 : -1;
    for (let i = 0; i < 16; i++) {
        const absolute = stepPage * 16 + i;
        if (seqSetup) {
            const shownStart = focusBank && !trackFollow ? trackStart : patternStart;
            const shownLen = focusBank && !trackFollow ? trackLen : patternLen;
            const inWindow = absolute >= shownStart && absolute < shownStart + shownLen;
            let c = inWindow ? Green : 0x10;
            if (absolute === shownStart) c = White;
            if (absolute === playStep) c = BrightRed;
            setLED(STEP_FIRST + i, c, force);
            continue;
        }
        let c = steps[i] === 2 ? Purple : (steps[i] === 1 ? BrightRed : Black);
        if (i === localPlay) c = White;
        if (heldStep && heldStep.step === stepPage * 16 + i) c = BrightGreen;
        setLED(STEP_FIRST + i, c, force);
    }
}

function paintAll(force) { paintTracks(force); paintGlobals(force); paintSteps(force); }

function draw() {
    clear_screen();
    drawHeader(recordArmed ? `REC · T${track + 1} ${MACHINES[machine]}`
                           : `MONO · T${track + 1} ${MACHINES[machine]}`);
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
                right: heldStep ? (deleteHeld ? 'Delete+turn=clear'
                    : (shiftLayer() ? 'Shift lock' : 'turn=lock'))
                    : `S${stepPage * 16 + 1}-${stepPage * 16 + 16}`});
    needsRedraw = false;
}

function drawPresetBrowser() {
    clear_screen();
    drawHeader('MONO · PATTERN PRESETS');
    const rows = [SAVE_ROW, ...presets.map(p => p.name)];
    const first = Math.max(0, Math.min(Math.max(0, rows.length - 3), presetIndex - 1));
    for (let row = 0; row < 3 && first + row < rows.length; row++) {
        const index = first + row, y = 19 + row * 11;
        const selected = index === presetIndex;
        if (selected) fill_rect(0, y - 2, 128, 10, 1);
        const label = `${selected ? '> ' : '  '}${rows[index]}`.slice(0, 20);
        print(3, y, label, selected ? 0 : 1);
    }
    drawFooter({left: deletePresetFile ? 'Delete again' : 'Back · Delete',
                right: 'Click load · Shift rename'});
    needsRedraw = false;
}

function drawSeqSetup() {
    clear_screen();
    drawHeader(`MONO · ${focusBank ? `T${track + 1} TIMING` : 'GLOBAL SEQ'}`);
    const labels = ['START', 'LENGTH', 'ORDER', 'SWING', 'TSTART', 'TLEN', 'ROTATE', 'DIV'];
    const shown = [String(patternStart + 1).padStart(2, '0'),
        String(patternLen).padStart(2, '0'), PLAY_ORDER_SCREEN[playOrder],
        `${50 + Math.round(swing * 25 / 127)}%`,
        trackFollow ? 'GLOBAL' : String(trackStart + 1).padStart(2, '0'),
        trackFollow ? 'GLOBAL' : String(trackLen).padStart(2, '0'),
        String(trackRotate).padStart(2, '0'), `1/${trackDiv}`];
    const first = focusBank * 4;
    for (let column = 0; column < 4; column++) {
        const i = first + column, x = column * 32 + 2;
        print(x, 18, labels[i], 1);
        print(x, 34, shown[i], 1);
    }
    drawFooter({left: focusBank ? 'Shift+turn: global' : `END ${String(patternStart + patternLen).padStart(2, '0')}`,
                right: `K${first + 1}-${first + 4} · Step=start`});
    needsRedraw = false;
}

globalThis.init = function() {
    host_module_set_param('track', '0');
    host_module_set_param('page', '0');
    host_module_set_param('step_page', '0');
    ready = fetchAll(); paintAll(true); needsRedraw = true;
    announceView('Mono, six track machine synth');
};

globalThis.onResume = function() {
    ready = fetchAll(); paintAll(true); resumePaints = 3; needsRedraw = true;
};

/* `suspend_keeps_js` normally reserves Back for Schwung's host-level suspend.
 * Claim it only while Mono has an internal modal open; the host then forwards
 * the original button event to onMidiMessageInternal(), which closes the modal.
 * At the main instrument screen Back keeps its normal suspend behavior. */
globalThis.wantsBack = function() { return presetMode || seqSetup; };

globalThis.tick = function() {
    tickCount++;
    const active = shiftActive();
    if (active !== shiftVisual) { shiftVisual = active; needsRedraw = true; }
    if (!ready) ready = fetchAll();
    if (ready && !heldStep && tickCount % 6 === 0) {
        const oldPlay = playStep, oldTransport = transport, oldRecord = recordArmed;
        fetchAll();
        if (oldPlay !== playStep) paintSteps(false);
        if (oldTransport !== transport || oldRecord !== recordArmed) paintGlobals(false);
        needsRedraw = true;
    }
    if (resumePaints > 0 && tickCount % 8 === 0) { paintAll(true); resumePaints--; }
    if (needsRedraw) presetMode ? drawPresetBrowser() : (seqSetup ? drawSeqSetup() : draw());
};

globalThis.onMidiMessageInternal = function(data) {
    const status = data[0] & 0xF0, d1 = data[1], d2 = data[2];
    if (status === 0xB0) {
        if (d1 === MoveShift) { shift = d2 > 0; needsRedraw = true; return; }
        if (seqSetup) {
            if (d1 === MoveBack && d2 > 0) { closeSeqSetup(); return; }
            if (d1 === MoveLeft && d2 >= 64) { setStepPage(stepPage - 1); return; }
            if (d1 === MoveRight && d2 >= 64) { setStepPage(stepPage + 1); return; }
            if (d1 >= MoveKnob1 && d1 < MoveKnob1 + 8) {
                const d = decodeDelta(d2); if (d) adjustSeqSetup(d1 - MoveKnob1, d); return;
            }
            return;
        }
        if (presetMode) {
            if (d1 === MoveDelete) {
                deleteHeld = d2 > 0;
                if (d2 > 0) deleteSelectedPreset();
                return;
            }
            if (d1 === MoveMainKnob) {
                const d = decodeDelta(d2);
                if (d) {
                    presetIndex = Math.max(0, Math.min(presets.length, presetIndex + d));
                    deletePresetFile = null;
                    const label = presetIndex === 0 ? SAVE_ROW : presets[presetIndex - 1].name;
                    announce(`Preset, ${label}`);
                    needsRedraw = true;
                }
                return;
            }
            if (d1 === MoveMainButton && d2 > 0) {
                if (presetIndex === 0) startPresetSave();
                else if (shiftActive()) startPresetRename();
                else loadSelectedPreset();
                return;
            }
            if (d1 === MoveBack && d2 > 0) { closePresetBrowser(); return; }
            return;
        }
        if (d1 === MoveCopy && d2 > 0) {
            if (heldStep) {
                heldStep.used = true;
                if (shiftActive()) {
                    host_module_set_param('paste_step', `${track}:${heldStep.step}`);
                    announce(`Pasted step ${heldStep.step + 1}`);
                } else {
                    host_module_set_param('copy_step', `${track}:${heldStep.step}`);
                    announce(`Copied step ${heldStep.step + 1}`);
                }
            } else if (shiftActive()) {
                host_module_set_param('paste_track', `${track}`);
                fetchAll(); paintAll(false); announce(`Pasted track ${track + 1}`);
            } else {
                host_module_set_param('copy_track', `${track}`);
                announce(`Copied track ${track + 1}`);
            }
            needsRedraw = true; return;
        }
        if (d1 === MoveUndo && d2 > 0) {
            host_module_set_param('undo', '1');
            fetchAll(); paintAll(false); needsRedraw = true; announce('Undo'); return;
        }
        if (d1 === MoveDelete) {
            if (d2 > 0) { deleteHeld = true; deleteUsed = false; }
            else {
                if (heldStep && !deleteUsed) {
                    host_module_set_param('clear_step', `${track}:${heldStep.step}`);
                    heldStep.used = true; parseSteps(); paintSteps(false);
                    announce(`Cleared step ${heldStep.step + 1}`);
                }
                deleteHeld = false; deleteUsed = false;
            }
            needsRedraw = true; return;
        }
        if (d1 === MoveMainButton && d2 > 0 && shiftActive()) {
            openPresetBrowser();
            return;
        }
        if (d1 === MoveRec && d2 > 0) { toggleRecord(); return; }
        if (d1 === MoveMainKnob) { const d = decodeDelta(d2); if (d) setPage(page + d); return; }
        if (d1 === MoveLeft && d2 >= 64) { setStepPage(stepPage - 1); return; }
        if (d1 === MoveRight && d2 >= 64) { setStepPage(stepPage + 1); return; }
        if (d1 >= MoveKnob1 && d1 < MoveKnob1 + 8) {
            const d = decodeDelta(d2); if (d) adjust(d1 - MoveKnob1, d); return;
        }
        return;
    }

    if (presetMode) return;

    const release = status === 0x80 || (status === 0x90 && d2 === 0);
    if (release) {
        if (heldStep && d1 === STEP_FIRST + (heldStep.step % 16)) {
            if (!heldStep.used) {
                host_module_set_param('toggle_step', `${heldStep.step}`);
                parseSteps();
                announce(`Step ${heldStep.step + 1} ${steps[heldStep.step % 16] ? 'on' : 'off'}`);
            }
            const editedLock = heldStep.used;
            heldStep = null;
            if (editedLock) fetchAll();
            paintSteps(false); needsRedraw = true; return;
        }
        if (d1 >= 68 && d1 < 92) return;
        return;
    }

    if (status === 0x90 && d2 > 0) {
        if (seqSetup) {
            for (let i = 0; i < 6; i++) if (d1 === TRACK_PADS[i]) {
                selectTrack(i); focusBank = 1; needsRedraw = true; return;
            }
            if (d1 >= STEP_FIRST && d1 < STEP_FIRST + STEP_COUNT) {
                const start = stepPage * 16 + d1 - STEP_FIRST;
                if (focusBank) setTrackTiming('track_start', start, 'Track start');
                else setPatternStart(start);
                return;
            }
            if (d1 === PAD_TRANSPORT) {
                shiftActive() ? closeSeqSetup() : toggleTransport();
                return;
            }
            return;
        }
        if (d1 >= STEP_FIRST && d1 < STEP_FIRST + STEP_COUNT) {
            heldStep = {step: stepPage * 16 + d1 - STEP_FIRST, used: false};
            paintSteps(false); needsRedraw = true; return;
        }
        for (let i = 0; i < 6; i++) if (d1 === TRACK_PADS[i]) {
            if (deleteHeld) {
                deleteUsed = true;
                host_module_set_param(shiftActive() ? 'track_solo_toggle' : 'track_mute_toggle', `${i}`);
                fetchAll(); paintTracks(false); needsRedraw = true;
                announce(`Track ${i + 1} ${shiftActive()
                    ? (trackStates[i] & 2 ? 'solo' : 'solo off')
                    : (trackStates[i] & 1 ? 'muted' : 'unmuted')}`);
            } else selectTrack(i);
            return;
        }
        if (d1 === PAD_MACHINE) { cycleMachine(shiftActive() ? -1 : 1); return; }
        if (d1 === PAD_TRANSPORT) {
            shiftActive() ? openSeqSetup() : toggleTransport();
            return;
        }
        if (d1 >= 68 && d1 < 92) return;
    }
};

globalThis.onUnload = function() {};
