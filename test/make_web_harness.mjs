/* Builds a standalone harness page: the REAL src/web_ui.html, with the
 * schwungRemote bridge replaced by a mock that replays a REAL state blob
 * (build/state.json, emitted from the engine by test/dump_state.c).
 *
 * The mock reproduces the manager's semantics rather than a convenient
 * version of them: fetchAllParams parses "state" as a flat object and keeps
 * ONLY scalar fields, dropping every array — so anything this page can render
 * here is something it can render on the device, and anything the engine puts
 * in an array is correctly invisible.
 */
import { readFileSync, writeFileSync } from 'node:fs';

const page = readFileSync('src/web_ui.html', 'utf8');
const state = JSON.parse(readFileSync('build/state.json', 'utf8'));

const params = {};
for (const [k, v] of Object.entries(state)) {
    if (v === null || typeof v === 'object') continue;   /* the manager drops these */
    params['overtake_dsp:' + k] = typeof v === 'boolean' ? (v ? '1' : '0') : String(v);
}

const mock = `<script>
window.__sent = [];
window.schwungRemote = {
  _cbs: [],
  onParamChange(cb) { this._cbs.push(cb); },
  setParam(k, v) { window.__sent.push(k + " = " + v); },
  getParam(k) { return Promise.resolve(undefined); },
  getHierarchy() { return Promise.resolve(null); },
  getChainParams() { return Promise.resolve(null); }
};
window.__params = ${JSON.stringify(params)};
window.addEventListener("load", function () {
  window.schwungRemote._cbs.forEach(function (cb) { cb(window.__params); });
});
</script>`;

/* Drop only the external bridge; everything else is the shipped file. */
const body = page.replace(
    '<script src="/static/schwung-remote-api.js"></script>', mock);

writeFileSync('build/web_ui_harness.html',
    '<!doctype html><meta charset="utf-8"><title>Work — editor harness</title>\n' + body);
console.log(`harness written, ${Object.keys(params).length} flat params replayed`);
