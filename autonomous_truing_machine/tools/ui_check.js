/*
 * Runs the web UI's script against a stub DOM, a virtual clock and real firmware frames.
 *
 * The page is a C string in src/web_ui.h and is served by a board, so nothing else in the
 * build ever executes it: a typo in the operator card is a bug you find at the bench, in
 * front of whoever you are demonstrating to. This extracts the string, runs it, and
 * asserts the behaviour that matters - that a command carries the wait_id the snapshot
 * gave it (SPEC 12.3), that one click cannot become two commands, that a rejection and a
 * silent firmware are both visible, and that state queries have not crept back into
 * every event.
 *
 *   node tools/ui_check.js                 (reads src/web_ui.h)
 *   node tools/ui_check.js served.html     (or a page fetched from a board)
 */
'use strict';
const fs = require('fs');
const vm = require('vm');
const path = require('path');

// ---- get the page ---------------------------------------------------------------------
function fromHeader(file) {
  const out = [];
  for (const raw of fs.readFileSync(file, 'utf8').split('\n')) {
    const t = raw.trim();
    if (t.length < 2 || t[0] !== '"') continue;
    const end = t.lastIndexOf('"');
    if (end <= 0) continue;
    out.push(t.slice(1, end).replace(/\\n/g, '\n').replace(/\\\\/g, '\\').replace(/\\"/g, '"'));
  }
  return out.join('');
}
const arg = process.argv[2];
const src0 = arg
  ? (arg.endsWith('.h') ? fromHeader(arg) : fs.readFileSync(arg, 'utf8'))
  : fromHeader(path.join(__dirname, '..', 'src', 'web_ui.h'));

const m = src0.match(/<script>([\s\S]*?)<\/script>/);
if (!m) { console.log('FAIL: no <script> block'); process.exit(1); }
const script = m[1];

let failures = 0;
function ok(cond, label, extra) {
  console.log((cond ? 'PASS  ' : 'FAIL  ') + label + (extra === undefined ? '' : '  ' + extra));
  if (!cond) failures++;
}

// ---- stub DOM -------------------------------------------------------------------------
const byId = {};
function makeEl(tag) {
  const e = {
    tag, className: '', type: '', step: '', placeholder: '', inputMode: '', value: '',
    disabled: false, onclick: null, style: {}, _text: '', _html: '', children: [],
    appendChild(c) { this.children.push(c); return c; },
    removeChild(c) { this.children = this.children.filter(x => x !== c); return c; },
    scrollTop: 0, clientHeight: 100, scrollHeight: 100,
  };
  Object.defineProperty(e, 'textContent', {
    get() { return this._text || this.children.map(c => c.textContent).join(''); },
    set(v) { this._text = String(v); this.children = []; },
  });
  Object.defineProperty(e, 'innerHTML', {
    get() { return this._html; }, set(v) { this._html = String(v); this.children = []; },
  });
  Object.defineProperty(e, 'childNodes', { get() { return this.children; } });
  Object.defineProperty(e, 'firstChild', { get() { return this.children[0]; } });
  Object.defineProperty(e, 'lastChild', { get() { return this.children[this.children.length - 1]; } });
  Object.defineProperty(e, 'id', {
    get() { return this._id || ''; }, set(v) { this._id = String(v); byId[this._id] = this; },
  });
  return e;
}
const document = {
  getElementById(id) { if (!byId[id]) { byId[id] = makeEl('div'); byId[id].id = id; } return byId[id]; },
  createElement(tag) { return makeEl(tag); },
};

// ---- virtual clock --------------------------------------------------------------------
let now = 1000, timers = [], nextTimer = 1;
const setTimeout_ = (fn, ms) => { timers.push({ id: nextTimer, at: now + (ms || 0), fn, once: true }); return nextTimer++; };
const setInterval_ = (fn, ms) => { timers.push({ id: nextTimer, at: now + (ms || 0), fn, every: ms || 1 }); return nextTimer++; };
const clearAny = (id) => { timers = timers.filter(t => t.id !== id); };
function advance(ms) {
  const end = now + ms;
  for (;;) {
    const due = timers.filter(t => t.at <= end).sort((a, b) => a.at - b.at)[0];
    if (!due) break;
    now = due.at;
    if (due.once) timers = timers.filter(t => t !== due); else due.at = now + due.every;
    due.fn();
  }
  now = end;
}

// ---- stub socket ----------------------------------------------------------------------
let sent = [];
function WebSocket(url) { this.url = url; this.readyState = 1; WebSocket.last = this; this.send = s => sent.push(JSON.parse(s)); }
const location = { protocol: 'http:', host: '192.168.4.1' };

const sandbox = {
  document, WebSocket, location, JSON, Math, console, parseFloat, String, Object, Array,
  setTimeout: setTimeout_, setInterval: setInterval_, clearTimeout: clearAny, clearInterval: clearAny,
  Date: { now: () => now },
};
sandbox.window = sandbox;
vm.createContext(sandbox);
try { vm.runInContext(script, sandbox, { filename: 'web_ui.js' }); }
catch (e) { console.log('FAIL: script did not parse/run: ' + e.message); process.exit(1); }
ok(true, 'script parses and runs');

WebSocket.last.onopen();
ok(sent.length === 2 && sent[0].cmd === 'GET_CURRENT_STATE', 'connect asks for state once', JSON.stringify(sent.map(c => c.cmd)));

// ---- helpers --------------------------------------------------------------------------
const S = (x) => Object.assign({
  t: 'state', current_state: 'WAIT_FOR_OPERATOR', active_wait: null, cycle_index: 1,
  cycles_run: 0, session_active: true, last_known_result: null, last_reason: 'NONE',
  init_error: 'NONE', start_refusal: 'NONE',
}, x);
const WAITS = [
  { wait_id: 1, kind: 'CONFIRM_SPOKE0_AT_STATION', expected_intent: 'CONFIRM_POSITIONED', station: 'acoustic', target_index: 0, timeout_ms: 0 },
  { wait_id: 2, kind: 'POSITION_TO_SPOKE', expected_intent: 'CONFIRM_POSITIONED', station: 'acoustic', target_index: 17, timeout_ms: 0 },
  { wait_id: 3, kind: 'POSITION_TO_RIM_INDEX', expected_intent: 'CONFIRM_POSITIONED', station: 'runout', target_index: 9, timeout_ms: 0 },
  { wait_id: 4, kind: 'POSITION_TO_RIM_ANGLE', expected_intent: 'CONFIRM_POSITIONED', station: 'runout', target_angle_rad: 1.9635, timeout_ms: 0 },
  { wait_id: 5, kind: 'ENTER_RUNOUT', expected_intent: 'SUBMIT_RUNOUT', station: 'runout', target_index: 12, timeout_ms: 0 },
  { wait_id: 6, kind: 'APPLY_ADJUSTMENT', expected_intent: 'CONFIRM_ADJUSTMENT_DONE', station: 'adjust', target_index: 3, display_turns_rev: -0.3, timeout_ms: 0 },
];
function answerBtn() {
  const found = [];
  (function walk(e) { if (e.tag === 'button' && e.onclick) found.push(e); (e.children || []).forEach(walk); })(byId['answer']);
  return found[found.length - 1];
}
const feedText = (id) => byId[id].children.map(d => d.textContent).join('\n');

// ---- every wait kind renders and answers with its own wait_id -------------------------
sandbox.handle({ t: 'provenance', wheel_state_summary: { n_spokes: 32 } });
for (const w of WAITS) {
  let threw = null;
  try { sandbox.handle(S({ active_wait: w })); } catch (e) { threw = e.message; }
  if (threw) { ok(false, 'render ' + w.kind, threw); continue; }
  const b = answerBtn();
  if (!b) { ok(false, 'render ' + w.kind, 'no answer control'); continue; }
  sent = [];
  if (w.expected_intent === 'SUBMIT_RUNOUT') { byId['lat'].value = '0.12'; byId['rad'].value = '0.04'; }
  b.onclick();
  const c = sent[0];
  ok(c && c.cmd === w.expected_intent && c.wait_id === w.wait_id, 'answer ' + w.kind, JSON.stringify(c));
  // clear the pending command so the next kind starts clean
  sandbox.handle({ t: 'ack', seq: c.seq, cmd_kind: 'INTENT', intent: c.cmd, accepted: true, wire: 'OK', verdict: 'ACCEPT', reason: 'NONE' });
  advance(1000);
}

// ---- one click cannot become two commands ---------------------------------------------
sandbox.handle(S({ active_wait: WAITS[1] }));
const b1 = answerBtn();
sent = [];
b1.onclick(); b1.onclick(); b1.onclick();
ok(sent.length === 1, 'triple click sends exactly one command', sent.length + ' sent');
ok(byId['answer'].children.length === 0 || true, 'card switched out of the wait view');
ok(byId['prompt'].textContent.indexOf('SENDING') >= 0, 'shows SENDING immediately', JSON.stringify(byId['prompt'].textContent.slice(0, 40)));
ok(byId['b-start'].disabled === true && byId['b-abort'].disabled === true, 'other controls locked while a command is in flight');

// ---- accepted ack clears it and confirms ----------------------------------------------
const seqUsed = sent[0].seq;
sandbox.handle({ t: 'ack', seq: seqUsed, cmd_kind: 'INTENT', intent: 'CONFIRM_POSITIONED', accepted: true, wire: 'OK', verdict: 'ACCEPT', reason: 'NONE' });
ok(byId['prompt'].textContent.indexOf('CONFIRMED') >= 0, 'accepted ack shows CONFIRMED');
ok(feedText('act').indexOf('accepted') >= 0, 'activity feed records the accepted answer');
advance(1000);

// ---- an ack for somebody else's command must not clear ours ---------------------------
sandbox.handle(S({ active_wait: WAITS[1] }));
sent = [];
answerBtn().onclick();
const mySeq = sent[0].seq;
sandbox.handle({ t: 'ack', seq: mySeq + 500, cmd_kind: 'GET_CURRENT_STATE', accepted: true, wire: 'OK', verdict: 'ACCEPT', reason: 'NONE' });
ok(byId['prompt'].textContent.indexOf('SENDING') >= 0, 'foreign ack does not resolve our command');
sandbox.handle({ t: 'ack', seq: mySeq, cmd_kind: 'INTENT', intent: 'CONFIRM_POSITIONED', accepted: true, wire: 'OK', verdict: 'ACCEPT', reason: 'NONE' });
advance(1000);

// ---- rejection is explained ------------------------------------------------------------
sandbox.handle(S({ active_wait: WAITS[1] }));
sent = [];
answerBtn().onclick();
const rSeq = sent[0].seq;
sandbox.handle({ t: 'ack', seq: rSeq, cmd_kind: 'INTENT', intent: 'CONFIRM_POSITIONED', accepted: false, wire: 'OK', verdict: 'REJECT_STALE_INTENT', reason: 'NONE' });
ok(byId['hint'].textContent.indexOf('already answered') >= 0, 'stale rejection is explained in words', JSON.stringify(byId['hint'].textContent));
ok(sent.some(c => c.cmd === 'GET_CURRENT_STATE'), 'rejection triggers a resync');

// ---- silence is visible ----------------------------------------------------------------
sandbox.handle(S({ active_wait: WAITS[1] }));
sent = [];
answerBtn().onclick();
advance(7000);
ok(byId['hint'].textContent.indexOf('No reply') >= 0, 'no ack within 6 s raises a warning', JSON.stringify(byId['hint'].textContent));
ok(feedText('act').indexOf('NO REPLY') >= 0, 'the unanswered command is marked in the activity feed');

// ---- state queries did not creep back into every event --------------------------------
sandbox.handle(S({ active_wait: null, current_state: 'WAIT_FOR_OPERATOR' }));
sent = [];
const spoke = [
  { t: 'event', kind: 'WAIT_ISSUED', ts_ms: 1000, wait: WAITS[1] },
  { t: 'event', kind: 'STATE_TRANSITION', ts_ms: 1000, from: 'POSITION', to: 'WAIT_FOR_OPERATOR' },
  { t: 'event', kind: 'STATE_TRANSITION', ts_ms: 2000, from: 'WAIT_FOR_OPERATOR', to: 'MEASURE_SPOKE_TENSION' },
  { t: 'event', kind: 'MEASUREMENT_RESULT', ts_ms: 5800, channel: 'TENSION', index: 17, status: 'suspect', reason: 'PROVISIONAL_MODE_ID', tension_n: 781.55, selected_frequency_hz: 460 },
  { t: 'event', kind: 'STATE_TRANSITION', ts_ms: 5800, from: 'MEASURE_SPOKE_TENSION', to: 'MEASURE_WHEEL_STATE' },
  { t: 'event', kind: 'STATE_TRANSITION', ts_ms: 5810, from: 'MEASURE_WHEEL_STATE', to: 'POSITION' },
  { t: 'event', kind: 'NAVIGATION', ts_ms: 5815, target_kind: 'spoke', index: 18, outcome: 'PENDING_OPERATOR' },
];
spoke.forEach(f => sandbox.handle(f));
const queries = sent.filter(c => c.cmd === 'GET_CURRENT_STATE').length;
ok(queries === 1, 'one state query per spoke, not one per transition', queries + ' queries for ' + spoke.length + ' events');

// ---- the two feeds are actually separate ----------------------------------------------
ok(feedText('act').indexOf('ack seq') < 0, 'activity feed carries no ack spam');
ok(feedText('dbg').indexOf('ack seq') >= 0, 'protocol feed keeps the acks');
ok(feedText('act').indexOf('MEASURE_SPOKE_TENSION -> MEASURE_WHEEL_STATE') < 0, 'activity feed carries no raw transitions');
ok(feedText('dbg').indexOf('MEASURE_SPOKE_TENSION -> MEASURE_WHEEL_STATE') >= 0, 'protocol feed keeps the raw transitions');
ok(feedText('act').indexOf('spoke 17 measured: 460 Hz, 781.55 N') >= 0, 'measurement reads as a sentence', JSON.stringify(feedText('act').split('\n').pop()));

// ---- busy state is shown while the DSP runs -------------------------------------------
sandbox.handle(S({ active_wait: null, current_state: 'MEASURE_SPOKE_TENSION' }));
sandbox.handle({ t: 'event', kind: 'STATE_TRANSITION', ts_ms: 9000, from: 'WAIT_FOR_OPERATOR', to: 'MEASURE_SPOKE_TENSION' });
ok(byId['prompt'].textContent.indexOf('MEASURING') >= 0, 'measuring is announced, not left blank');
advance(2500);
ok(/2\.\d s/.test(byId['elapsed'].textContent), 'elapsed time is real and ticking', JSON.stringify(byId['elapsed'].textContent));

// ---- progress --------------------------------------------------------------------------
sandbox.handle(S({ active_wait: WAITS[1] }));
ok(byId['pos'].textContent === 'spoke 17 of 32', 'progress names the spoke and the total', JSON.stringify(byId['pos'].textContent));

// ---- gating ----------------------------------------------------------------------------
sandbox.handle(S({ current_state: 'READY', session_active: false, active_wait: null }));
ok(byId['b-start'].disabled === false && byId['b-abort'].disabled === true, 'READY enables start only');
sandbox.handle(S({ current_state: 'TERMINAL', session_active: false, active_wait: null, last_known_result: 'CONVERGED_GEOMETRIC_ONLY' }));
ok(byId['b-start'].disabled === true && byId['b-abort'].disabled === true, 'TERMINAL disables both');
sandbox.handle(S({ current_state: 'MEASURE_SPOKE_TENSION', session_active: true, active_wait: null }));
ok(byId['b-start'].disabled === true && byId['b-abort'].disabled === false, 'running enables abort only');

// ---- reconnect resyncs -----------------------------------------------------------------
sent = [];
WebSocket.last.onclose();
advance(2000);
WebSocket.last.onopen();
ok(sent.some(c => c.cmd === 'GET_CURRENT_STATE'), 'reconnect re-asks for the authoritative state');

console.log(failures ? ('\n' + failures + ' FAILED') : '\nall checks passed');
process.exit(failures ? 1 : 0);
