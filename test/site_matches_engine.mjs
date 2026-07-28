/*
 * The manual site carries its own copy of the machine table, because it is a
 * standalone HTML file with no build step. A second copy is a copy that
 * drifts — and it did: a careless edit silently swapped Warble's N.LEV and
 * N.HPF and the page would have shipped teaching the wrong knob.
 *
 * So the site is checked against the ENGINE's table, dumped by
 * test/dump_contract.c into build/contract.json (machines) and by the
 * generated hierarchy in build/hierarchy.json (per-machine knob labels).
 *
 * Run via `make test`.
 */
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

let checks = 0, failures = 0;
function check(cond, msg) {
    checks++;
    if (!cond) { failures++; console.log(`  FAIL  ${msg}`); }
}

/* engine truth */
const contract = JSON.parse(fs.readFileSync(path.join(root, 'build/contract.json'), 'utf8'));
const hierarchy = JSON.parse(fs.readFileSync(path.join(root, 'build/hierarchy.json'), 'utf8'));
const engineNames = contract.get.machines.split(',');

function slug(name) {
    return name.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '');
}

/* The C string table is ASCII, so "Comb +/- Filter"; the page is typeset and
 * says "Comb ± Filter". That one substitution is a deliberate display choice
 * — everything else must match character for character. */
function norm(s) { return s.replace(/±/g, '+/-'); }

/* site copy */
const html = fs.readFileSync(path.join(root, 'docs/index.html'), 'utf8');
const m = html.match(/var MACHINES = \[([\s\S]*?)\n {2}\];/);

console.log('manual site matches the engine\n');

if (!m) {
    check(false, 'could not find the MACHINES table in docs/index.html');
} else {
    const site = eval('[' + m[1] + ']');

    check(site.length === engineNames.length,
          `site lists ${site.length} machines, engine has ${engineNames.length}`);

    for (let i = 0; i < Math.min(site.length, engineNames.length); i++) {
        check(norm(site[i].n) === engineNames[i],
              `machine ${i}: site says "${site[i].n}", engine says "${engineNames[i]}"`);

        /* knob labels, from the generated hierarchy */
        const lvl = hierarchy.levels[`fx1_${slug(engineNames[i])}`];
        if (!lvl) { check(false, `no hierarchy level for ${engineNames[i]}`); continue; }
        const engineParams = lvl.params
            .map((p) => p.name)
            .filter((n) => n !== '(no parameters)');
        const siteParams = site[i].p || [];
        check(JSON.stringify(siteParams) === JSON.stringify(engineParams),
              `${engineNames[i]} knobs differ:\n      site   ${JSON.stringify(siteParams)}` +
              `\n      engine ${JSON.stringify(engineParams)}`);
    }
}

/* the version on the page must match what the modules actually ship */
const modVer = JSON.parse(
    fs.readFileSync(path.join(root, 'modules/audio_fx/work/module.json'), 'utf8')).version;
check(html.includes(`v${modVer}`),
      `site does not mention the shipped version v${modVer}`);

console.log(`\n${checks} checks, ${failures} failed`);
process.exit(failures ? 1 : 0);
