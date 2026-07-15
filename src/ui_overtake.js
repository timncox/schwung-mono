/* Mono — six-track machine synth and lock sequencer, full-surface UI. */
import * as os from 'os';
import {
    MoveKnob1, MoveShift, MoveMainKnob, MoveMainButton, MoveBack, MoveLeft, MoveRight,
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

const TRACK_PADS = [92, 93, 94, 95, 96, 97];
const PAD_MACHINE = 98, PAD_TRANSPORT = 99;
const STEP_FIRST = 16, STEP_COUNT = 16;
const PRESET_DIR = '/data/UserData/schwung/presets/mono';
const SAVE_ROW = '[Save current...]';

let track = 0, page = 0, stepPage = 0, machine = 0, transport = 0;
let patternLen = 16, playStep = -1, shift = false, tickCount = 0;
let shiftVisual = false;
let values = new Array(8).fill(0), steps = new Array(16).fill(0);
let altValues = new Array(8).fill(0);
let heldStep = null, ready = false, needsRedraw = true, resumePaints = 0;
let focusBank = 0;
let presetMode = false, presetIndex = 0, presets = [];

function gp(key) {
    const v = host_module_get_param(key);
    return v === null || v === undefined ? null : String(v);
}

function safePresetStem(name) {
    return name.replace(/[\/\\\x00-\x1f]/g, '').trim() || 'Mono Pattern';
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

function uniquePresetName(rawName) {
    const base = rawName.trim() || 'Mono Pattern';
    const used = new Set(presets.map(p => p.name.toLowerCase()));
    if (!used.has(base.toLowerCase())) return base;
    let suffix = 2;
    while (used.has(`${base} ${suffix}`.toLowerCase())) suffix++;
    return `${base} ${suffix}`;
}

function openPresetBrowser() {
    loadPresetList();
    presetIndex = 0;
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
    const stem = safePresetStem(name);
    let state;
    try { state = JSON.parse(stateJson); } catch (e) { state = stateJson; }
    const payload = JSON.stringify({name, module: 'mono', version: 1, state});
    const ok = typeof host_write_file === 'function'
        && host_write_file(`${PRESET_DIR}/${stem}.json`, payload);
    if (!ok) { announce('Preset save failed'); return; }
    loadPresetList();
    const found = presets.findIndex(p => p.name === name);
    presetIndex = found >= 0 ? found + 1 : 0;
    needsRedraw = true;
    announce(`Saved ${name}`);
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
function shiftLayer() { return page === 0 && shiftActive() && !heldStep; }
function names() { return page === 0 ? (shiftLayer() ? SYNTH_SHIFT[machine] : SYNTH[machine]) : COMMON[page]; }
function activeValues() { return shiftLayer() ? altValues : values; }
function isLfoDestination(i) { return page >= 4 && i === 0; }
function destinationIndex(value) { return Math.max(0, Math.min(LFO_DESTS.length - 1, Math.round(value))); }
function displayValue(i, value) {
    return isLfoDestination(i) ? LFO_DESTS[destinationIndex(value)]
        : `${value}`.padStart(3, '0');
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

function cycleMachine(delta) {
    machine = (machine + delta + MACHINES.length) % MACHINES.length;
    host_module_set_param('machine', `${machine}`);
    fetchAll(); paintTracks(false); needsRedraw = true;
    announceParameter('Machine', MACHINES[machine]);
}

function adjust(i, delta) {
    focusBank = i >= 4 ? 1 : 0;
    const target = activeValues();
    let v = isLfoDestination(i)
        ? Math.max(0, Math.min(LFO_DESTS.length - 1, destinationIndex(target[i]) + delta))
        : Math.max(0, Math.min(127, target[i] + delta));
    if (v === target[i]) return;
    target[i] = v;
    if (heldStep) {
        heldStep.used = true;
        const absoluteParam = page * 8 + i;
        if (shiftActive())
            host_module_set_param('unlock', `${track}:${heldStep.step}:${absoluteParam}:0`);
        else
            host_module_set_param('lock', `${track}:${heldStep.step}:${absoluteParam}:${v}`);
        parseSteps(); paintSteps(false);
    } else {
        host_module_set_param(shiftLayer() ? `alt${i + 1}` : `p${i + 1}`, `${v}`);
    }
    announceParameter(names()[i], shiftActive() && heldStep ? 'unlocked' : displayValue(i, v));
    needsRedraw = true;
}

function toggleTransport() {
    transport = transport ? 0 : 1;
    host_module_set_param('transport', `${transport}`);
    announce(transport ? 'Sequencer playing' : 'Sequencer stopped');
    paintGlobals(false); needsRedraw = true;
}

function paintTracks(force) {
    for (let i = 0; i < 6; i++)
        setLED(TRACK_PADS[i], i === track ? MACHINE_COLORS[machine] : 0x10, force);
}

function paintGlobals(force) {
    setLED(PAD_MACHINE, MACHINE_COLORS[machine], force);
    setLED(PAD_TRANSPORT, transport ? Green : LightGrey, force);
}

function paintSteps(force) {
    const localPlay = playStep >= stepPage * 16 && playStep < stepPage * 16 + 16
        ? playStep - stepPage * 16 : -1;
    for (let i = 0; i < 16; i++) {
        let c = steps[i] === 2 ? Purple : (steps[i] === 1 ? BrightRed : Black);
        if (i === localPlay) c = White;
        if (heldStep && heldStep.step === stepPage * 16 + i) c = BrightGreen;
        setLED(STEP_FIRST + i, c, force);
    }
}

function paintAll(force) { paintTracks(force); paintGlobals(force); paintSteps(force); }

function draw() {
    clear_screen();
    drawHeader(`MONO · T${track + 1} ${MACHINES[machine]}`);
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
                right: heldStep ? (shiftActive() ? 'turn=unlock' : 'turn=lock')
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
    drawFooter({left: 'Back', right: 'Click: save/load'});
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

globalThis.tick = function() {
    tickCount++;
    const active = shiftActive();
    if (active !== shiftVisual) { shiftVisual = active; needsRedraw = true; }
    if (!ready) ready = fetchAll();
    if (ready && !heldStep && tickCount % 6 === 0) {
        const oldPlay = playStep, oldTransport = transport;
        fetchAll();
        if (oldPlay !== playStep) paintSteps(false);
        if (oldTransport !== transport) paintGlobals(false);
        needsRedraw = true;
    }
    if (resumePaints > 0 && tickCount % 8 === 0) { paintAll(true); resumePaints--; }
    if (needsRedraw) presetMode ? drawPresetBrowser() : draw();
};

globalThis.onMidiMessageInternal = function(data) {
    const status = data[0] & 0xF0, d1 = data[1], d2 = data[2];
    if (status === 0xB0) {
        if (d1 === MoveShift) { shift = d2 > 0; needsRedraw = true; return; }
        if (presetMode) {
            if (d1 === MoveMainKnob) {
                const d = decodeDelta(d2);
                if (d) {
                    presetIndex = Math.max(0, Math.min(presets.length, presetIndex + d));
                    const label = presetIndex === 0 ? SAVE_ROW : presets[presetIndex - 1].name;
                    announce(`Preset, ${label}`);
                    needsRedraw = true;
                }
                return;
            }
            if (d1 === MoveMainButton && d2 > 0) {
                presetIndex === 0 ? startPresetSave() : loadSelectedPreset();
                return;
            }
            if (d1 === MoveBack && d2 > 0) { closePresetBrowser(); return; }
            return;
        }
        if (d1 === MoveMainButton && d2 > 0 && shiftActive()) {
            openPresetBrowser();
            return;
        }
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
        if (d1 >= STEP_FIRST && d1 < STEP_FIRST + STEP_COUNT) {
            heldStep = {step: stepPage * 16 + d1 - STEP_FIRST, used: false};
            paintSteps(false); needsRedraw = true; return;
        }
        for (let i = 0; i < 6; i++) if (d1 === TRACK_PADS[i]) { selectTrack(i); return; }
        if (d1 === PAD_MACHINE) { cycleMachine(shiftActive() ? -1 : 1); return; }
        if (d1 === PAD_TRANSPORT) { toggleTransport(); return; }
        if (d1 >= 68 && d1 < 92) return;
    }
};

globalThis.onUnload = function() {};
