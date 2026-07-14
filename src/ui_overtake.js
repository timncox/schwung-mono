/* Mono — six-track machine synth and lock sequencer, full-surface UI. */
import {
    MoveKnob1, MoveShift, MoveMainKnob, MoveLeft, MoveRight,
    Black, White, LightGrey, BrightRed, Blue, Green, BrightGreen,
    Cyan, Purple, YellowGreen, OrangeRed
} from '/data/UserData/schwung/shared/constants.mjs';
import { decodeDelta, setLED } from '/data/UserData/schwung/shared/input_filter.mjs';
import { drawMenuHeader as drawHeader, drawMenuFooter as drawFooter }
    from '/data/UserData/schwung/shared/menu_layout.mjs';
import { announce, announceParameter, announceView }
    from '/data/UserData/schwung/shared/screen_reader.mjs';

const MACHINES = ['SW SAW', 'SW PULS', 'SW ENS', 'SID6581', 'DIGIPRO', 'FM+STAT'];
const MACHINE_COLORS = [OrangeRed, BrightRed, YellowGreen, Purple, Cyan, Blue];
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

const TRACK_PADS = [92, 93, 94, 95, 96, 97];
const PAD_MACHINE = 98, PAD_TRANSPORT = 99;
const STEP_FIRST = 16, STEP_COUNT = 16;

let track = 0, page = 0, stepPage = 0, machine = 0, transport = 0;
let patternLen = 16, playStep = -1, shift = false, tickCount = 0;
let values = new Array(8).fill(0), steps = new Array(16).fill(0);
let heldStep = null, ready = false, needsRedraw = true, resumePaints = 0;
let focusBank = 0;

function gp(key) {
    const v = host_module_get_param(key);
    return v === null || v === undefined ? null : String(v);
}
function names() { return page === 0 ? SYNTH[machine] : COMMON[page]; }

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
    let v = Math.max(0, Math.min(127, values[i] + delta));
    if (v === values[i]) return;
    values[i] = v;
    if (heldStep) {
        heldStep.used = true;
        const absoluteParam = page * 8 + i;
        if (shift)
            host_module_set_param('unlock', `${track}:${heldStep.step}:${absoluteParam}:0`);
        else
            host_module_set_param('lock', `${track}:${heldStep.step}:${absoluteParam}:${v}`);
        parseSteps(); paintSteps(false);
    } else {
        host_module_set_param(`p${i + 1}`, `${v}`);
    }
    announceParameter(names()[i], shift && heldStep ? 'unlocked' : `${v}`);
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
    const first = focusBank * 4;
    for (let column = 0; column < 4; column++) {
        const i = first + column;
        const x = column * 32 + 2;
        print(x, 18, n[i], 1);
        print(x, 34, `${values[i]}`.padStart(3, '0'), 1);
    }
    drawFooter({left: `${PAGES[page]} K${first + 1}-${first + 4}`,
                right: heldStep ? (shift ? 'turn=unlock' : 'turn=lock')
                    : `S${stepPage * 16 + 1}-${stepPage * 16 + 16}`});
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
    if (!ready) ready = fetchAll();
    if (ready && !heldStep && tickCount % 6 === 0) {
        const oldPlay = playStep, oldTransport = transport;
        fetchAll();
        if (oldPlay !== playStep) paintSteps(false);
        if (oldTransport !== transport) paintGlobals(false);
        needsRedraw = true;
    }
    if (resumePaints > 0 && tickCount % 8 === 0) { paintAll(true); resumePaints--; }
    if (needsRedraw) draw();
};

globalThis.onMidiMessageInternal = function(data) {
    const status = data[0] & 0xF0, d1 = data[1], d2 = data[2];
    if (status === 0xB0) {
        if (d1 === MoveShift) { shift = d2 > 0; needsRedraw = true; return; }
        if (d1 === MoveMainKnob) { const d = decodeDelta(d2); if (d) setPage(page + d); return; }
        if (d1 === MoveLeft && d2 >= 64) { setStepPage(stepPage - 1); return; }
        if (d1 === MoveRight && d2 >= 64) { setStepPage(stepPage + 1); return; }
        if (d1 >= MoveKnob1 && d1 < MoveKnob1 + 8) {
            const d = decodeDelta(d2); if (d) adjust(d1 - MoveKnob1, d); return;
        }
        return;
    }

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
        if (d1 === PAD_MACHINE) { cycleMachine(shift ? -1 : 1); return; }
        if (d1 === PAD_TRANSPORT) { toggleTransport(); return; }
        if (d1 >= 68 && d1 < 92) return;
    }
};

globalThis.onUnload = function() {};
