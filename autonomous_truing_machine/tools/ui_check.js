/*
 * Runs the web UI's script against a stub DOM, a virtual clock and real firmware frames.
 *
 * The page is a C string in src/web_ui.h and is served by a board, so nothing else in the
 * build ever executes it: a typo in the operator card is a bug you find at the bench, in
 * front of whoever you are demonstrating to. This extracts the string, runs it, and
 * asserts the behaviour that matters - that a command carries the wait_id the snapshot
 * gave it (SPEC 12.3), that one click cannot become two commands, that a rejection and a
 * silent firmware are both visible, that internal enums reach the operator translated
 * rather than raw, and that state queries have not crept back into every event.
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
const page = arg
  ? (arg.endsWith('.h') ? fromHeader(arg) : fs.readFileSync(arg, 'utf8'))
  : fromHeader(path.join(__dirname, '..', 'src', 'web_ui.h'));

const m = page.match(/<script>([\s\S]*?)<\/script>/);
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
    disabled: false, onclick: null, onload: null, style: {}, _text: '', _html: '', children: [],
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
  createTextNode(t) { const e = makeEl('#text'); e.textContent = t; return e; },
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

// ---- stub socket + /id ----------------------------------------------------------------
let sent = [];
function WebSocket(url) { this.url = url; this.readyState = 1; WebSocket.last = this; this.send = s => sent.push(JSON.parse(s)); }
const IDENT = {
  board: 'Arduino Nano ESP32', firmware: '0.1.0-phase1f', build: 'abc1234',
  ui: 'e5797f70', ssid: 'truing-000000', uptime_s: 42, clients: 1, mode: 'interactive',
  acquisition: 'manual', acquisition_selectable: false,
  real_front_end: false, acoustic_demo_spokes: 0, composite_navigation: false,
};
function XMLHttpRequest() {
  this.open = (mth, url) => { this._url = url; };
  this.send = () => { this.responseText = JSON.stringify(IDENT); if (this.onload) this.onload(); };
}
const location = { protocol: 'http:', host: '192.168.4.1' };
// Minimal Web Storage stub: the diagnostics history lives in the tab's sessionStorage.
const memStore = new Map();
const sessionStorage = {
  getItem: (k) => (memStore.has(k) ? memStore.get(k) : null),
  setItem: (k, v) => { memStore.set(k, String(v)); },
  removeItem: (k) => { memStore.delete(k); },
};

const sandbox = {
  document, WebSocket, XMLHttpRequest, sessionStorage, location, JSON, Math, console, parseFloat, String, Object, Array,
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
  { wait_id: 1, kind: 'CONFIRM_SPOKE0_AT_STATION', expected_intent: 'CONFIRM_POSITIONED', station: 'acoustic_left', target_index: 0, timeout_ms: 0 },
  { wait_id: 2, kind: 'POSITION_TO_SPOKE', expected_intent: 'CONFIRM_POSITIONED', station: 'acoustic_right', target_index: 17, timeout_ms: 0 },
  { wait_id: 3, kind: 'POSITION_TO_RIM_INDEX', expected_intent: 'CONFIRM_POSITIONED', station: 'runout', target_index: 9, timeout_ms: 0 },
  { wait_id: 4, kind: 'POSITION_TO_RIM_ANGLE', expected_intent: 'CONFIRM_POSITIONED', station: 'runout', target_angle_rad: 1.9635, timeout_ms: 0 },
  { wait_id: 5, kind: 'ENTER_RUNOUT', expected_intent: 'SUBMIT_RUNOUT', station: 'runout', target_index: 12, timeout_ms: 0 },
  { wait_id: 6, kind: 'APPLY_ADJUSTMENT', expected_intent: 'CONFIRM_ADJUSTMENT_DONE', station: 'adjust', target_index: 3, display_turns_rev: -0.3, timeout_ms: 0 },
];
const PROV = {
  t: 'provenance', session_id: 1, influence_artifact_id: 1,
  generating_fingerprint: '25b68917ad02f8b4', tension_model_profile_id: 1,
  chain_profile_id: 1, machine_profile_id: 1, active_layout: 'NONE',
  wheel_state_summary: { n_spokes: 32, n_rim_angles: 32, spokes_solver_admissible: 32, runout_solver_admissible: 32, spokes_verification_grade: 0 },
  plan: { valid: true, n_valid_rows: 32, n_suspect_rows: 0, policy_reason: 'MEAN_TENSION_MODEL_UNAVAILABLE', mean_tension_targeting_applied: false, solver_version: 1 },
  verification: { max_lateral_mm: 0.08, max_radial_mm: 0.12, geometric_converged: true, tension_evaluated: false, tension_compliant: false, reason: 'TENSION_NOT_VERIFICATION_GRADE' },
  contains_non_real_implementations: true,
};
function answerBtn() {
  const found = [];
  (function walk(e) { if (e.tag === 'button' && e.onclick) found.push(e); (e.children || []).forEach(walk); })(byId['answer']);
  return found[found.length - 1];
}
const txt = (id) => byId[id].textContent;
const feedText = (id) => byId[id].children.map(d => d.textContent).join('\n');

// ---- two acoustic stations: the prompt names which one, in words ------------------------
sandbox.handle(PROV);
sandbox.handle(S({ active_wait: WAITS[1] }));
ok(txt('statusbody').indexOf('right acoustic station') >= 0 || JSON.stringify(byId).indexOf('right acoustic station') >= 0,
   'a spoke prompt names its acoustic station (right acoustic station), not the raw id', txt('statusbody'));
ok(JSON.stringify(byId).indexOf('acoustic_right') < 0, 'the raw station id acoustic_right never reaches the page');

// ---- every wait kind renders and answers with its own wait_id -------------------------
sandbox.handle(PROV);
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
  sandbox.handle({ t: 'ack', seq: c.seq, cmd_kind: 'INTENT', intent: c.cmd, accepted: true, wire: 'OK', verdict: 'ACCEPT', reason: 'NONE' });
  advance(1000);
}

// ---- one click cannot become two commands ---------------------------------------------
sandbox.handle(S({ active_wait: WAITS[1] }));
sent = [];
const b1 = answerBtn();
b1.onclick(); b1.onclick(); b1.onclick();
ok(sent.length === 1, 'triple click sends exactly one command', sent.length + ' sent');
ok(txt('statusbody').indexOf('Sending') >= 0, 'shows Sending immediately');
ok(byId['b-start'].disabled === true && byId['b-abort'].disabled === true, 'other controls locked while in flight');

const seqUsed = sent[0].seq;
sandbox.handle({ t: 'ack', seq: seqUsed, cmd_kind: 'INTENT', intent: 'CONFIRM_POSITIONED', accepted: true, wire: 'OK', verdict: 'ACCEPT', reason: 'NONE' });
ok(txt('statusbody').indexOf('Confirmed') >= 0, 'accepted ack shows Confirmed');
ok(feedText('act').indexOf('accepted') >= 0, 'activity feed records the accepted answer');
advance(1000);

// ---- an ack for somebody else's command must not clear ours ---------------------------
sandbox.handle(S({ active_wait: WAITS[1] }));
sent = [];
answerBtn().onclick();
const mySeq = sent[0].seq;
sandbox.handle({ t: 'ack', seq: mySeq + 500, cmd_kind: 'GET_CURRENT_STATE', accepted: true, wire: 'OK', verdict: 'ACCEPT', reason: 'NONE' });
ok(txt('statusbody').indexOf('Sending') >= 0, 'foreign ack does not resolve our command');
sandbox.handle({ t: 'ack', seq: mySeq, cmd_kind: 'INTENT', intent: 'CONFIRM_POSITIONED', accepted: true, wire: 'OK', verdict: 'ACCEPT', reason: 'NONE' });
advance(1000);

// ---- rejection and silence are both explained in words ---------------------------------
sandbox.handle(S({ active_wait: WAITS[1] }));
sent = [];
answerBtn().onclick();
sandbox.handle({ t: 'ack', seq: sent[0].seq, cmd_kind: 'INTENT', intent: 'CONFIRM_POSITIONED', accepted: false, wire: 'OK', verdict: 'REJECT_STALE_INTENT', reason: 'NONE' });
ok(txt('hint').indexOf('already answered') >= 0, 'stale rejection is explained in words', JSON.stringify(txt('hint')));
ok(sent.some(c => c.cmd === 'GET_CURRENT_STATE'), 'rejection triggers a resync');

sandbox.handle(S({ active_wait: WAITS[1] }));
sent = [];
answerBtn().onclick();
advance(7000);
ok(txt('hint').indexOf('No reply') >= 0, 'no ack within 6 s raises a warning');
ok(feedText('act').indexOf('NO REPLY') >= 0, 'the unanswered command is marked in the feed');

// ---- state queries did not creep back into every event --------------------------------
sandbox.handle(S({ active_wait: null }));
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
ok(sent.filter(c => c.cmd === 'GET_CURRENT_STATE').length === 1, 'one state query per spoke, not one per transition');

// ---- the three audiences stay separate -------------------------------------------------
ok(feedText('act').indexOf('ack seq') < 0, 'activity feed carries no ack spam');
ok(feedText('dbg').indexOf('ack seq') >= 0, 'protocol feed keeps the acks');
ok(feedText('act').indexOf('MEASURE_SPOKE_TENSION -> MEASURE_WHEEL_STATE') < 0, 'activity feed carries no raw transitions');
ok(feedText('dbg').indexOf('MEASURE_SPOKE_TENSION -> MEASURE_WHEEL_STATE') >= 0, 'protocol feed keeps the raw transitions');

// ---- latest measurement is translated, not an enum dump --------------------------------
ok(txt('last').indexOf('460 Hz') >= 0 && txt('last').indexOf('781.55 N') >= 0, 'latest measurement shows frequency and tension');
ok(txt('last').indexOf('Provisional measurement') >= 0, 'suspect reason is translated for the operator', JSON.stringify(txt('last').slice(-90)));
ok(txt('last').indexOf('PROVISIONAL_MODE_ID') >= 0, 'the original reason code is still shown alongside');

// ---- busy state -------------------------------------------------------------------------
sandbox.handle(S({ active_wait: null, current_state: 'MEASURE_SPOKE_TENSION' }));
sandbox.handle({ t: 'event', kind: 'STATE_TRANSITION', ts_ms: 9000, from: 'WAIT_FOR_OPERATOR', to: 'MEASURE_SPOKE_TENSION' });
ok(txt('statusbody').indexOf('Measuring') >= 0, 'measuring is announced, not left blank');
advance(2500);
ok(/2\.\d s elapsed/.test(byId['elapsed'].textContent), 'elapsed time is real and ticking', JSON.stringify(byId['elapsed'].textContent));

// ---- session progress maps to the real workflow ----------------------------------------
ok(txt('stepper').indexOf('Measure spokes') >= 0 && txt('stepper').indexOf('Verify') >= 0, 'stepper lists the real stages');
ok(txt('stepper').indexOf('17 / 32') >= 0, 'stepper shows spoke progress against the total', JSON.stringify(txt('stepper')));
sandbox.handle(S({ active_wait: null, current_state: 'COMPUTE_ADJUSTMENTS' }));
sandbox.handle({ t: 'event', kind: 'STATE_TRANSITION', ts_ms: 9500, from: 'READ_RUNOUT', to: 'COMPUTE_ADJUSTMENTS' });
ok(txt('statusbody').indexOf('Computing the plan') >= 0, 'compute stage is named in plain language');

// ---- Run Details pulls the authoritative record, bounded and without state spam -----------
sent = [];
sandbox.handle({ t: 'event', kind: 'STATE_TRANSITION', ts_ms: 9550, from: 'COMPUTE_ADJUSTMENTS', to: 'EVALUATE_CONVERGENCE' });
ok(sent.filter(c => c.cmd === 'GET_CURRENT_CYCLE_PROVENANCE').length === 1,
  'the verification milestone pulls provenance once', JSON.stringify(sent.map(c => c.cmd)));
ok(sent.filter(c => c.cmd === 'GET_CURRENT_STATE').length === 0,
  'milestones never trigger a state query');
sent = [];
byId['tab-run'].onclick();
ok(sent.filter(c => c.cmd === 'GET_CURRENT_CYCLE_PROVENANCE').length === 1,
  'entering Run Details pulls the current provenance', JSON.stringify(sent.map(c => c.cmd)));
ok(sent.filter(c => c.cmd === 'GET_CURRENT_STATE').length === 0,
  'the run-tab pull is not a state query');
byId['tab-op'].onclick();


// ---- terminal results are translated ----------------------------------------------------
sandbox.handle(S({ current_state: 'TERMINAL', session_active: false, active_wait: null, last_known_result: 'CONVERGED_GEOMETRIC_ONLY', last_reason: 'MEAN_TENSION_MODEL_UNAVAILABLE' }));
ok(txt('statusbody').indexOf('Geometry target reached') >= 0, 'CONVERGED_GEOMETRIC_ONLY is translated');
ok(txt('statusbody').indexOf('Mean-tension correction unavailable') >= 0, 'its reason is translated too');
ok(txt('statusbody').indexOf('CONVERGED_GEOMETRIC_ONLY') >= 0, 'the enum is still shown for reference');
ok(txt('statusbody').indexOf('0.08 mm') >= 0, 'finished summary shows the real final runout', JSON.stringify(txt('statusbody').slice(-120)));
sandbox.handle(S({ current_state: 'TERMINAL', session_active: false, active_wait: null, last_known_result: 'ABORT_OPERATOR', last_reason: 'NONE' }));
ok(txt('statusbody').indexOf('Run stopped') >= 0 && txt('statusbody').indexOf('cancelled') >= 0, 'ABORT_OPERATOR is translated');

// ---- run details are sentences, raw JSON is behind a toggle -----------------------------
ok(txt('rundl').indexOf('32 spokes') >= 0, 'run details name the wheel');
ok(txt('rundl').indexOf('Geometry only') >= 0, 'run details name the solver mode');
ok(txt('rundl').indexOf('Not a physical wheel result') >= 0, 'synthetic components are called out prominently');
ok(txt('outcome').indexOf('Within tolerance') >= 0, 'outcome reports the geometry verdict');
ok(txt('outcome').indexOf('Not verification-grade') >= 0, 'outcome is honest about tension');
ok(txt('techdl').indexOf('25b68917ad02f8b4') >= 0, 'fingerprint lives under technical detail');
ok(byId['prov'].className.indexOf('hide') >= 0, 'raw JSON is hidden until asked for');
byId['b-raw'].onclick.call(byId['b-raw']);
ok(byId['prov'].className.indexOf('hide') < 0, 'raw JSON toggles open');

// ---- build identity ---------------------------------------------------------------------
ok(txt('i-board') === IDENT.board && txt('i-build') === IDENT.build && txt('i-ui') === IDENT.ui, 'build identity is rendered from /id');
ok(txt('ident').indexOf(IDENT.build) >= 0 && txt('ident').indexOf(IDENT.ui) >= 0, 'identity is visible in the header');

// ---- session lifecycle ------------------------------------------------------------------
sandbox.handle(S({ current_state: 'READY', session_active: false, active_wait: null, last_known_result: null }));
ok(byId['b-start'].disabled === false && byId['b-abort'].disabled === true, 'READY enables Start only');
sent = [];
byId['b-start'].onclick();
ok(sent.length === 1 && sent[0].cmd === 'START_TRUING', 'Start sends START_TRUING', JSON.stringify(sent[0]));
sandbox.handle(S({ current_state: 'MEASURE_SPOKE_TENSION', session_active: true, active_wait: null }));
ok(byId['b-start'].disabled === true && byId['b-abort'].disabled === false, 'running enables Abort only');
sent = [];
byId['b-abort'].onclick();
ok(sent.length === 1 && sent[0].cmd === 'ABORT', 'Abort sends ABORT');

// ---- tabs -------------------------------------------------------------------------------
byId['tab-dev'].onclick();
ok(byId['v-op'].className === 'hide' && byId['v-dev'].className === '', 'Diagnostics tab hides the operator view');
byId['tab-op'].onclick();
ok(byId['v-op'].className === '' && byId['v-dev'].className === 'hide', 'Operator tab comes back');

// ---- reconnect resyncs -------------------------------------------------------------------
sent = [];
WebSocket.last.onclose();
advance(2000);
WebSocket.last.onopen();
ok(sent.some(c => c.cmd === 'GET_CURRENT_STATE'), 'reconnect re-asks for the authoritative state');
ok(byId['v-op'].className === '', 'reconnect leaves the operator view in place');

// ---- a second viewer is visible in the header, awareness only -----------------------------
ok(txt('viewers') === '', 'one client shows no viewer notice');
IDENT.clients = 2;
byId['b-refresh'].onclick();
ok(txt('viewers').indexOf('2 viewers') >= 0, 'a second client is counted in the header');
ok(txt('viewers').indexOf('another client is also connected') >= 0, 'the notice warns in words');
ok(byId['viewers'].className.indexOf('multi') >= 0, 'more than one viewer reads as a warning tone');
IDENT.clients = 1;
byId['b-refresh'].onclick();
ok(txt('viewers') === '', 'the notice clears when it is one client again');

// ---- diagnostics history: persisted per tab, marked at session edges, cleared deliberately --
sandbox.handle(S({ current_state: 'READY', session_active: false, active_wait: null, last_known_result: null }));
sandbox.handle(S({ current_state: 'MEASURE_WHEEL_STATE', session_active: true, active_wait: null }));
ok(feedText('dbg').indexOf('new machine session') >= 0, 'a machine session start marks the log');
advance(300);   // let the debounced sessionStorage mirror flush
const stored = () => JSON.parse(sessionStorage.getItem('truing_diag') || '[]');
ok(stored().length > 0, 'feed entries are mirrored to the tab sessionStorage');
ok(stored().some(e => e.t === 'connected'), 'mirrored entries keep their text');
byId['b-clear'].onclick();
ok(byId['dbg'].children.length === 1 && byId['act'].children.length === 1,
  'Clear empties both feeds down to one marker row');
ok(stored().length === 2 && stored().every(e => e.c === 'gap'),
  'Clear also wipes the persisted history');
// A reload of the page runs the script again in a fresh context over the same tab storage.
const sandbox2 = {
  document, WebSocket, XMLHttpRequest, sessionStorage, location, JSON, Math, console, parseFloat, String, Object, Array,
  setTimeout: setTimeout_, setInterval: setInterval_, clearTimeout: clearAny, clearInterval: clearAny,
  Date: { now: () => now },
};
sandbox2.window = sandbox2;
vm.createContext(sandbox2);
vm.runInContext(script, sandbox2, { filename: 'web_ui.js' });
ok(feedText('dbg').indexOf('history cleared') >= 0, 'a page reload restores the persisted history');
ok(feedText('dbg').indexOf('restored - frames while the page was closed are lost') >= 0,
  'the reload says honestly that closed-page frames are gone');

// ---- the completed run's provenance is kept in this browser, labeled as browser-held -------
sandbox.handle(S({ current_state: 'TERMINAL', session_active: false, active_wait: null, last_known_result: 'CONVERGED_GEOMETRIC_ONLY', last_reason: 'MEAN_TENSION_MODEL_UNAVAILABLE' }));
sent = [];
sandbox.handle({ t: 'event', kind: 'TERMINAL_RESULT', ts_ms: 20000, result: 'CONVERGED_GEOMETRIC_ONLY' });
ok(sent.some(c => c.cmd === 'GET_CURRENT_CYCLE_PROVENANCE'), 'a finished run pulls the final provenance');
sandbox.handle(PROV);   // the answer to that pull is the run's final authoritative record
ok(feedText('dbg').indexOf('completed-run record kept in this browser') >= 0,
  'the freeze is visible in the log, not silent');
ok(txt('rundl').indexOf('Last completed run - session 1') >= 0,
  'Run Details names the completed-run record once the machine is idle');
ok(txt('rundl').indexOf('the machine itself does not retain it once the next run starts') >= 0,
  'the record states honestly that retention is browser-held, not machine history');
ok(txt('rundl').indexOf('32 spokes') >= 0 && txt('techdl').indexOf('25b68917ad02f8b4') >= 0,
  'the completed record still carries the full provenance detail');
// A new live session takes the tab back to the current cycle -- and must do so from the
// snapshot alone. Run Details used to redraw only when a provenance frame arrived, so a
// viewer already sitting on the tab kept reading the finished run all through the next one.
// Nothing is handed to the page here except the state frame, which is the whole point.
sent = [];
sandbox.handle(S({ current_state: 'MEASURE_WHEEL_STATE', session_active: true, active_wait: null }));
ok(txt('rundl').indexOf('Last completed run') < 0,
  'a new live session takes Run Details back without waiting for a provenance frame',
  txt('rundl').slice(0, 70));
ok(sent.some(c => c.cmd === 'GET_CURRENT_CYCLE_PROVENANCE'),
  'a new session pulls a fresh record: starting a run is a milestone, not just a tab switch');
ok(txt('techdl').indexOf('25b68917ad02f8b4') < 0 && txt('rundl').indexOf('32 spokes') < 0,
  'the finished run\'s numbers are not shown as the live cycle while that pull is in flight',
  txt('rundl').slice(0, 70));
sandbox.handle(Object.assign({}, PROV, { session_id: 2, plan: null, verification: null }));
ok(txt('rundl').indexOf('Last completed run') < 0,
  'a new live session takes Run Details back from the completed run');
// Ending the run puts the frozen record back, again from the snapshot alone.
sandbox.handle(S({ current_state: 'READY', session_active: false, active_wait: null, last_known_result: 'CONVERGED_GEOMETRIC_ONLY' }));
ok(txt('rundl').indexOf('Last completed run - session 1') >= 0,
  'returning to idle restores the completed-run record without a tab switch');



// ---- fast demo is a property of the firmware, and the page only ever mirrors it ---------
// The capability is not in the page: there is no command that makes a session synthetic, so
// a page talking to an interactive board cannot produce one however it is driven. What the
// page does is refuse to let a synthetic run look like a physical one.
IDENT.mode = 'interactive'; IDENT.acquisition = 'manual'; IDENT.acquisition_selectable = false;
sandbox.loadIdent();
ok(byId['demobanner'].className.indexOf('hide') >= 0, 'interactive build shows no Fast Demo banner');
ok(byId['acqcard'].className.indexOf('hide') >= 0,
  'interactive build offers no acquisition selector at all');
ok(byId['modechip'].className.indexOf('hide') >= 0, 'interactive build shows no Fast Demo chip');
ok(txt('b-start') === 'Start truing', 'interactive build offers Start truing', txt('b-start'));
ok(txt('i-mode') === 'INTERACTIVE / acquisition MANUAL (operator)',
  'Diagnostics names the build and the path', txt('i-mode'));
// No control anywhere on the page can ask for synthetic measurements.
sandbox.handle(S({ current_state: 'READY', session_active: false, active_wait: null, last_known_result: null }));
sent = [];
byId['b-start'].onclick();
ok(sent.length === 1 && sent[0].cmd === 'START_TRUING' && Object.keys(sent[0]).length === 2,
  'Start sends START_TRUING and nothing else, in either build', JSON.stringify(sent[0]));

IDENT.mode = 'fastdemo'; IDENT.acquisition = 'auto'; IDENT.acquisition_selectable = true;
sandbox.loadIdent();
ok(byId['demobanner'].className.indexOf('hide') < 0, 'Fast Demo build shows the banner');
ok(byId['acqcard'].className.indexOf('hide') < 0, 'the Fast Demo build offers the acquisition selector');
ok(byId['b-acq-auto'].className === 'sel' && byId['b-acq-manual'].className === '',
  'the selector shows which path is active');
ok(byId['modechip'].className.indexOf('hide') < 0, 'Fast Demo build shows the header chip');
ok(page.indexOf('It is not a physical wheel-truing result.') >= 0,
  'the banner says plainly that this is not a physical result');
ok(page.indexOf('This run uses simulated measurements.') >= 0,
  'the banner says the measurements are simulated');
ok(page.indexOf('Synthetic measurement mode') >= 0, 'the banner names the mode');
ok(txt('b-start') === 'Start Fast Demo', 'Fast Demo build labels the action honestly', txt('b-start'));
ok(txt('i-mode') === 'FAST DEMO / acquisition AUTOMATIC (synthetic)',
  'Diagnostics names the Fast Demo build and its path', txt('i-mode'));

// The automated-acquisition line counts what the machine reported, never a timer.
sandbox.handle(PROV);
sandbox.handle(S({ current_state: 'MEASURE_SPOKE_TENSION', session_active: true, active_wait: null }));
ok(txt('statusbody').indexOf('0 / 32') >= 0, 'progress starts from nothing measured', txt('statusbody').slice(-60));
for (let i = 0; i < 17; ++i) {
  sandbox.handle({ t: 'event', kind: 'MEASUREMENT_RESULT', channel: 'TENSION', index: i, ts_ms: 1000 + i,
    status: 'valid', selected_frequency_hz: 460, tension_n: 1000 });
}
ok(txt('statusbody').indexOf('17 / 32') >= 0, 'the spoke count follows the measurement events',
  txt('statusbody').slice(-70));
ok(txt('statusbody').indexOf('spokes measured') >= 0, 'the count says what is being counted');
const bar = (function find(e) {
  if (e.className === 'prog') return e.children[0];
  for (const c of (e.children || [])) { const r = find(c); if (r) return r; }
  return null;
})(byId['statusbody']);
ok(bar !== null, 'a progress bar is rendered during the automated pass');
ok(bar && bar.style.width === (100 * 17 / 32) + '%', 'the bar width is the real fraction measured',
  bar && bar.style.width);
ok(txt('statusbody').indexOf('synthetic acoustic source') >= 0,
  'the automated phase names the synthetic source');
for (let i = 17; i < 32; ++i) {
  sandbox.handle({ t: 'event', kind: 'MEASUREMENT_RESULT', channel: 'TENSION', index: i, ts_ms: 2000 + i,
    status: 'valid', selected_frequency_hz: 460, tension_n: 1000 });
}
ok(txt('statusbody').indexOf('Measurement pass complete: 32 / 32') >= 0,
  'a complete pass is reported as complete', txt('statusbody').slice(-70));
sandbox.handle(S({ current_state: 'READ_RUNOUT', session_active: true, active_wait: null }));
ok(txt('statusbody').indexOf('simulated runout') >= 0, 'the runout phase says it is simulated',
  txt('statusbody').slice(-80));
// Run Details must not describe a Fast Demo acquisition as manual work someone did. These
// two rows used to state "Manual operator positioning" and "Manual dial entry" as facts.
sandbox.showTab('run');
sandbox.handle(PROV);
ok(txt('rundl').indexOf('synthetic navigation') >= 0,
  'Run Details says the positioning was automatic and synthetic', txt('rundl').slice(0, 60));
ok(txt('rundl').indexOf('No dial gauge was read') >= 0,
  'Run Details does not claim a dial gauge was read in Fast Demo');
ok(txt('rundl').indexOf('Manual dial entry') < 0, 'the manual-entry claim is gone in Fast Demo');
ok(txt('rundl').indexOf('Not a physical wheel result') >= 0,
  'the non-physical warning is still on the Run Details tab');
IDENT.mode = 'interactive'; IDENT.acquisition = 'manual';
sandbox.loadIdent();
sandbox.handle(PROV);
ok(txt('rundl').indexOf('Manual dial entry') >= 0,
  'the manual path reports manual dial entry, which is what happens there');
ok(txt('rundl').indexOf('synthetic navigation') < 0,
  'the manual path does not claim synthetic acquisition');
IDENT.mode = 'fastdemo'; IDENT.acquisition = 'auto';
sandbox.loadIdent();
sandbox.showTab('op');

// The Fast Demo IMAGE running the MANUAL path is an ordinary physical-path session, and must
// present as one: no synthetic banner, and the provenance sentences back to manual.
IDENT.acquisition = 'manual';
sandbox.loadIdent();
ok(byId['demobanner'].className.indexOf('hide') >= 0,
  'selecting the manual path takes the synthetic banner down');
ok(byId['acqcard'].className.indexOf('hide') < 0, 'the selector stays available on the manual path');
ok(byId['b-acq-manual'].className === 'sel', 'the selector follows the machine, not the click');
ok(txt('b-start') === 'Start truing', 'the manual path is not labelled Fast Demo');
sandbox.handle(PROV);
ok(txt('rundl').indexOf('Manual dial entry') >= 0,
  'the manual path reports manual dial entry even in the Fast Demo image');
ok(txt('rundl').indexOf('synthetic navigation') < 0,
  'the manual path claims no synthetic acquisition');
IDENT.acquisition = 'auto';
sandbox.loadIdent();

// ---- the acoustic demonstration image -------------------------------------------------------
// mode is 'fastdemo+inmp441' here, so anything testing the image by string equality against
// 'fastdemo' silently stops recognising it - the banner and the Start label included.
IDENT.mode = 'fastdemo+inmp441'; IDENT.real_front_end = true; IDENT.acoustic_demo_spokes = 3;
sandbox.loadIdent();
ok(byId['demobanner'].className.indexOf('hide') < 0,
  'the acoustic demonstration image still raises the synthetic banner');
ok(txt('b-start') === 'Start Fast Demo', 'the acoustic demonstration image is still a Fast Demo');
ok(txt('i-mode').indexOf('FAST DEMO + REAL MIC') >= 0,
  'the identity line distinguishes the real microphone from a fully synthetic demo');
ok(txt('acqnote').indexOf('solenoids strike 3 spokes') >= 0,
  'the selector explains that the station solenoids strike a few spokes');
ok(txt('acqnote').indexOf('by hand') < 0, 'and never that anyone plucks by hand');
ok(txt('acqnote').indexOf('not in the active solver layout') >= 0,
  'the selector gives the reason the rest are left uncollected');
ok(txt('acqnote').indexOf('measures all 32 spokes') < 0,
  'the bounded image does not claim it measures all 32 spokes');

// ---- bounded acoustic sampling -------------------------------------------------------------
// Three tension measurements on a 32-spoke wheel must read as a bounded demonstration, never as
// a wheel that was measured and mostly failed. The page has to say the number AND the reason.
const PROV_ACOUSTIC = Object.assign({}, PROV, {
  active_layout: 'TENSION_ABSENT',
  tension_sample_limit: 3, tension_sampled: 3, tension_omitted_by_layout: true,
});
sandbox.handle(PROV_ACOUSTIC);
ok(txt('rundl').indexOf('3 of 32 spokes struck') >= 0,
  'the page says how many spokes were actually struck');
ok(txt('rundl').indexOf('Remaining tension measurements omitted') >= 0,
  'the page says the rest were omitted, not attempted and lost');
ok(txt('rundl').indexOf('not part of the active solver layout') >= 0,
  'the page gives the reason: those rows are not in the solve');
ok(txt('rundl').indexOf('TENSION_ABSENT') >= 0,
  'the page names the layout that makes the omission legal');
ok(txt('rundl').indexOf('Runout is synthetic in this Fast Demo session') >= 0,
  'the page still says the runout was synthetic');

// An ordinary run carries no bound, and must not grow a sampling notice out of nowhere.
sandbox.handle(PROV);
ok(txt('rundl').indexOf('spokes struck') < 0,
  'a run with no bound says nothing about bounded sampling');
ok(txt('rundl').indexOf('Remaining tension measurements omitted') < 0,
  'a run with no bound claims no omission');

// A bound that is NOT taking effect (tension is in the solve) must not claim an omission.
sandbox.handle(Object.assign({}, PROV, {
  active_layout: 'FULL', tension_sample_limit: 3, tension_sampled: 32, tension_omitted_by_layout: false,
}));
ok(txt('rundl').indexOf('32 of 32 spokes struck') >= 0,
  'a refused bound reports the full sweep it actually did');
ok(txt('rundl').indexOf('Remaining tension measurements omitted') < 0,
  'a refused bound claims no omission');
sandbox.handle(PROV);
// ---- who moved the wheel: composite navigation vs the images that keep synthetic navigation --------
// Three images all report acquisition 'auto', and two of them have a real microphone, so what the page
// says about positioning must come from the composite_navigation FACT, never from real_front_end.
// (1) the acoustic demonstration: the operator places each struck spoke, runout is simulated.
IDENT.composite_navigation = true;
sandbox.loadIdent();
ok(txt('acqnote').indexOf('You place each struck spoke') >= 0,
  'with composite navigation the selector says the operator places each struck spoke');
ok(txt('acqnote').indexOf('done by the synthetic implementations') < 0,
  'and no longer claims positioning is synthetic');
ok(txt('i-mode').indexOf('spokes operator-placed, runout synthetic') >= 0,
  'the identity line says who positions and what is still synthetic');
sandbox.handle(PROV_ACOUSTIC);
ok(txt('rundl').indexOf('Operator at the acoustic stations, simulated elsewhere') >= 0,
  'the run details name the operator at the acoustic stations');
ok(txt('rundl').indexOf('nothing senses placement') >= 0,
  'and say the placement is the operator\'s assertion, not a sensed fact');
ok(txt('rundl').indexOf('No wheel was moved') < 0,
  'a composite session never claims that no wheel was moved');
// (2) the campaign bench image: real microphone, automatic path, but synthetic navigation.
IDENT.composite_navigation = false;
sandbox.loadIdent();
ok(txt('acqnote').indexOf('done by the synthetic implementations') >= 0,
  'with synthetic navigation the selector still says positioning is synthetic, whatever the microphone');
ok(txt('acqnote').indexOf('You place each struck spoke') < 0,
  'and does not claim an operator places spokes');
ok(txt('i-mode').indexOf('AUTOMATIC (synthetic)') >= 0,
  'the identity line says AUTOMATIC (synthetic) for it');
sandbox.handle(PROV_ACOUSTIC);
ok(txt('rundl').indexOf('No wheel was moved') >= 0,
  'its run details keep saying that no wheel was moved');
ok(txt('rundl').indexOf('Operator at the acoustic stations') < 0,
  'and never name an operator it does not have');
sandbox.handle(PROV);
// Back to the plain Fast Demo image, so nothing below inherits the microphone.
IDENT.mode = 'fastdemo'; IDENT.real_front_end = false; IDENT.acoustic_demo_spokes = 0;
sandbox.loadIdent();

// An operator wait still takes over the card: automation never hides something asked of you.
sandbox.handle(S({ current_state: 'WAIT_FOR_OPERATOR', session_active: true, active_wait: WAITS[5] }));
ok(txt('statusbody').indexOf('Waiting for you') >= 0,
  'Fast Demo still hands the adjustment back to the operator');
ok(byId['demobanner'].className.indexOf('hide') < 0, 'the synthetic warning stays up during the run');
// The automatic path issues NO waits for the whole acquisition, so the page used to sit on
// the snapshot it had from before Start: session_active false, which disables Abort and
// suppresses the busy card entirely. A session beginning must refresh the snapshot by itself.
sandbox.handle(S({ current_state: 'READY', session_active: false, active_wait: null, last_known_result: null }));
sent = [];
sandbox.handle({ t: 'event', kind: 'STATE_TRANSITION', ts_ms: 500, from: 'READY', to: 'MEASURE_WHEEL_STATE' });
ok(sent.some(c => c.cmd === 'GET_CURRENT_STATE'),
  'a session beginning refreshes the snapshot without waiting for a wait');
sandbox.handle(S({ current_state: 'MEASURE_WHEEL_STATE', session_active: true, active_wait: null }));
ok(byId['b-abort'].disabled === false,
  'Abort is enabled during the automated acquisition, with no wait outstanding');
sent = [];
byId['b-abort'].onclick();
ok(sent.length === 1 && sent[0].cmd === 'ABORT', 'Abort mid-acquisition sends ABORT', JSON.stringify(sent[0]));

// ---- the acoustic measurement lifecycle -----------------------------------------------------
// One acoustic call is a strike and a capture window, then seconds of arithmetic. Nobody plucks:
// each spoke is struck by the solenoid at its own acoustic station, and a station without one is
// refused by the firmware before any window opens (plan A10). These pin that the card names the
// actuator that fired, that the phases look different, and that losing frames costs the cue only.
IDENT.mode = 'interactive+inmp441'; IDENT.acquisition = 'manual';
IDENT.acquisition_selectable = false; IDENT.real_front_end = true; IDENT.acoustic_demo_spokes = 0;
sandbox.loadIdent();
const PH = (x) => Object.assign({
  t: 'event', kind: 'ACOUSTIC_PHASE', ts_ms: 5000, cycle_index: 1, phase: 'LISTENING',
  spoke_index: 7, actuator: 'RIGHT', fired: true, window_ms: 1000, attempt: 1,
}, x);
const noPluckAsk = () => !/pluck/i.test(txt('statusbody'));

sandbox.handle(S({ current_state: 'MEASURE_SPOKE_TENSION', session_active: true, active_wait: null }));
ok(txt('statusbody').indexOf('Capturing and analysing') >= 0,
  'with no phase frame the ordinary measuring card is what shows');

sandbox.handle(PH({}));
ok(txt('statusbody').indexOf('Striking spoke 7') >= 0 && txt('statusbody').indexOf('right actuator') >= 0,
  'LISTENING names the spoke and the actuator that struck it');
ok(txt('statusbody').indexOf('Hands off') >= 0, 'and asks for hands off the wheel');
ok(noPluckAsk(), 'nothing on the listening card asks anyone to pluck', txt('statusbody'));
ok(byId['status'].className.indexOf('wait') < 0,
  'the listening card is styled as the machine working, not as something asked of you');
ok(txt('statusbody').indexOf('ACOUSTIC LISTENING') >= 0, 'the card names the phase it is in');
ok(txt('statusbody').indexOf('Attempt') < 0, 'a first attempt is not labelled as a retry');
ok(feedText('act').indexOf('striking spoke 7 (right actuator)') >= 0,
  'the log says which actuator struck which spoke');

// The retry the orchestrator does silently: same state, no transition, so the next attempt's
// LISTENING frame is the only thing that reports it.
sandbox.handle(PH({ ts_ms: 6600, attempt: 2 }));
ok(txt('statusbody').indexOf('Attempt 2') >= 0,
  'the retry says which attempt it is - nothing else reports this');
ok(txt('statusbody').indexOf('not heard') >= 0, 'and says why there is another attempt');

// ONSET_DETECTED means only that the detector picked a sample in the finished capture; the
// peak/SNR gates run after it, so a rejection can still follow. The card must not read as success.
sandbox.handle(PH({ ts_ms: 7700, phase: 'ONSET_DETECTED', window_ms: 0 }));
ok(txt('statusbody').indexOf('Window closed') >= 0, 'ONSET_DETECTED says the window has closed');
ok(txt('statusbody').indexOf('may still be a rejection') >= 0,
  'and does not promise the strike was usable');
ok(byId['status'].className.indexOf('good') < 0, 'and is not styled as success');
ok(noPluckAsk(), 'the closed-window card asks no one to pluck', txt('statusbody'));
ok(txt('statusbody').indexOf('ACOUSTIC ONSET_DETECTED') >= 0
   && txt('statusbody').indexOf('ACOUSTIC LISTENING') < 0,
  'the listening card is replaced once the window has closed, not stacked under it');

sandbox.handle(PH({ ts_ms: 7750, phase: 'ANALYZING', window_ms: 0 }));
ok(txt('statusbody').indexOf('Analysing spoke 7') >= 0, 'ANALYZING names the work being done');
ok(txt('statusbody').indexOf('hands off') >= 0, 'and keeps hands off the wheel');
ok(txt('statusbody').indexOf('elapsed') >= 0, 'the analysis shows honest elapsed time');

// A result ends the lifecycle: the cue must not linger over the next thing.
sandbox.handle({ t: 'event', kind: 'MEASUREMENT_RESULT', channel: 'TENSION', index: 7, ts_ms: 9900,
  status: 'suspect', reason: 'PROVISIONAL_MODE_ID', tension_n: 1000, selected_frequency_hz: 460 });
ok(txt('statusbody').indexOf('Striking spoke') < 0, 'a result clears the strike card');

// So does leaving the state, however many frames went missing on the way.
sandbox.handle(PH({ ts_ms: 10000, spoke_index: 6, actuator: 'LEFT' }));
ok(txt('statusbody').indexOf('Striking spoke 6') >= 0 && txt('statusbody').indexOf('left actuator') >= 0,
  'a LEFT station spoke names the left actuator');
sandbox.handle({ t: 'event', kind: 'STATE_TRANSITION', ts_ms: 10100,
  from: 'MEASURE_SPOKE_TENSION', to: 'POSITION' });
sandbox.handle(S({ current_state: 'POSITION', session_active: true, active_wait: null }));
ok(txt('statusbody').indexOf('Striking spoke') < 0,
  'leaving the acoustic state ends the window even if its closing frames were dropped');

// "Striking" is a claim that a command went out. A capture where nothing was fired (a campaign
// no-fire control) must never make it.
sandbox.handle(S({ current_state: 'MEASURE_SPOKE_TENSION', session_active: true, active_wait: null }));
sandbox.handle(PH({ ts_ms: 11000, fired: false, actuator: 'NONE' }));
ok(txt('statusbody').indexOf('Striking') < 0, 'a capture with nothing fired never claims a strike');
ok(txt('statusbody').indexOf('nothing was fired') >= 0, 'and says plainly that nothing was fired');
ok(noPluckAsk(), 'and still asks no one to pluck instead', txt('statusbody'));
sandbox.handle({ t: 'event', kind: 'STATE_TRANSITION', ts_ms: 11500,
  from: 'MEASURE_SPOKE_TENSION', to: 'POSITION' });

// ---- a rejected measurement says WHY, in the log, not only in a card the next spoke overwrites --
sandbox.handle(S({ current_state: 'MEASURE_SPOKE_TENSION', session_active: true, active_wait: null }));
sandbox.handle({ t: 'event', kind: 'MEASUREMENT_RESULT', channel: 'TENSION', index: 4, ts_ms: 12000,
  status: 'rejected', reason: 'AMBIGUOUS_PEAK', tension_n: null, selected_frequency_hz: null });
sandbox.handle({ t: 'event', kind: 'MEASUREMENT_RESULT', channel: 'TENSION', index: 5, ts_ms: 12100,
  status: 'unavailable', reason: 'VALUE_OUT_OF_RANGE', tension_n: null, selected_frequency_hz: null });
let feed = feedText('act');
ok(feed.indexOf('spoke 4 measured') >= 0 && feed.indexOf('rejected') >= 0
   && feed.indexOf('Ambiguous peak') >= 0,
  'a rejected tension line carries the reason, translated, in the activity log');
ok(feed.indexOf('spoke 5 measured') >= 0 && feed.indexOf('Not enough signal') >= 0,
  'VALUE_OUT_OF_RANGE now has a translation and is shown (it had neither before)');
// A rejected runout must not read green either.
sandbox.handle({ t: 'event', kind: 'MEASUREMENT_RESULT', channel: 'RUNOUT', index: 2, ts_ms: 12200,
  status: 'unavailable', reason: 'SENSOR_TIMEOUT', lateral_mm: null, radial_mm: null });
feed = feedText('act');
ok(feed.indexOf('rim 2 read') >= 0 && feed.indexOf('Sensor timeout') >= 0,
  'a rejected runout line carries its reason and is not hard-coded to success styling');

// ---- an aborted terminal is not styled or logged as success -------------------------------
// Each feed row is <div><span.ts><span.CLS text></div>; the second span carries the class.
const rowCls = (id) => { const r = byId[id].children; return r.length ? (r[r.length - 1].children[1] || {}).className : ''; };
sandbox.handle({ t: 'event', kind: 'TERMINAL_RESULT', ts_ms: 13000, result: 'ABORT_OPERATOR' });
ok(feedText('act').indexOf('run finished: ABORT_OPERATOR') >= 0, 'the abort is logged');
ok(rowCls('act') === 't-bad', 'an aborted run is not logged in the success colour');
sandbox.handle({ t: 'event', kind: 'TERMINAL_RESULT', ts_ms: 13100, result: 'CONVERGED_GEOMETRIC_ONLY' });
ok(rowCls('act') === 't-good', 'a converged run still reads as success');

// ---- every reason the firmware can emit has an operator translation -----------------------
// Mirrors k_reason_str in lib/truing_core/src/status.c (NONE needs no entry). If the firmware
// grows a reason, this fails until the page learns to say it.
const REASONS = [
  'NO_ONSET_DETECTED', 'ONSET_COUNT_MISMATCH', 'LOW_SNR', 'AMBIGUOUS_PEAK', 'FREQ_OUT_OF_RANGE',
  'NO_F2_PARTNER', 'PROVISIONAL_MODE_ID', 'MODEL_REJECTED', 'SENSOR_TIMEOUT', 'VALUE_OUT_OF_RANGE',
  'NOT_IMPLEMENTED', 'CALIBRATION_MISSING', 'STALE_MEASUREMENT', 'PARTIAL_WHEEL_STATE',
  'ARTIFACT_INVALID', 'STALE_INTENT', 'REQUIRES_ARTIFACT_REGENERATION',
  'MEAN_TENSION_MODEL_UNAVAILABLE', 'TENSION_NOT_VERIFICATION_GRADE', 'CANCELLED',
  'WHEEL_REFERENCE_LOST', 'CAPTURE_OVERRUN', 'EXCITATION_UNAVAILABLE',
];
const missing = REASONS.filter(r => !(sandbox.REASON && sandbox.REASON[r]));
ok(missing.length === 0, 'every firmware reason code has a page translation', missing.join(', '));

IDENT.mode = 'interactive'; IDENT.acquisition = 'manual'; IDENT.acquisition_selectable = false;
IDENT.real_front_end = false;
sandbox.loadIdent();

// ---- replay a real recorded session (4c) ---------------------------------------------
// Hand-built frames test the cases someone thought to write. This pushes a whole session
// captured off a board (tools/probe/session_capture.py) through the same handle(), so the
// page also has to survive the exact sequence and shapes the firmware really emits.
(function replayRecordedSession() {
  const file = path.join(__dirname, '..', 'test', 'fixtures', 'ui', 'session_fastdemo_auto.json');
  let doc;
  try { doc = JSON.parse(fs.readFileSync(file, 'utf8')); }
  catch (e) { ok(false, 'recorded session fixture loads', e.message); return; }
  const frames = doc.frames || [];
  ok(frames.length > 100, 'recorded session has a full cycle of frames', frames.length + ' frames');

  // start from a clean READY, the way a browser joining an idle board would
  sandbox.handle(S({ current_state: 'READY', session_active: false, active_wait: null, last_known_result: null }));
  sandbox.handle(PROV);
  const feedStart = feedText('act').length;

  let threw = null, prevTs = null;
  for (let i = 0; i < frames.length && !threw; i++) {
    const f = frames[i];
    if (typeof f.ts_ms === 'number') {
      if (prevTs !== null) advance(Math.max(0, Math.min(4000, f.ts_ms - prevTs)));
      prevTs = f.ts_ms;
    }
    try { sandbox.handle(f); }
    catch (e) { threw = 'frame ' + i + ' (' + (f.kind || f.t) + '): ' + e.message; }
  }
  ok(!threw, 'every frame of a real recorded session is handled without throwing', threw || '');

  const fresh = feedText('act').slice(feedStart);
  // every demo spoke's capture window reached the log, naming the spoke it was for
  ok(/(striking|recording) spoke 0/.test(fresh) && /(striking|recording) spoke 1/.test(fresh) && /(striking|recording) spoke 2/.test(fresh),
    'replay: LISTENING reaches the log for every demo spoke');
  // every reason the session actually carried reached the operator translated
  const reasonPhrase = { AMBIGUOUS_PEAK: 'Ambiguous peak', VALUE_OUT_OF_RANGE: 'Not enough signal' };
  const carried = new Set(frames.filter(f => f.kind === 'MEASUREMENT_RESULT' && f.reason && f.reason !== 'NONE').map(f => f.reason));
  for (const r of carried) {
    ok(reasonPhrase[r] && fresh.indexOf(reasonPhrase[r]) >= 0,
      'replay: ' + r + ' reached the operator translated', reasonPhrase[r] || '(no phrase mapped in this test)');
  }
  // the run ended, said so as success, and left nothing mid-flight on the status card
  ok(fresh.indexOf('run finished: CONVERGED_GEOMETRIC_ONLY') >= 0, 'replay: the terminal result is logged');
  ok(rowCls('act') === 't-good', 'replay: a geometry-only convergence still reads as success');
  ok(txt('statusbody').indexOf('Sending') < 0 && txt('statusbody').indexOf('Confirming') < 0,
    'replay: no in-flight command is left stuck on the status card after the run ends');
})();

console.log(failures ? ('\n' + failures + ' FAILED') : '\nall checks passed');
process.exit(failures ? 1 : 0);
