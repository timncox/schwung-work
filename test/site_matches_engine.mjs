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

/* ---------------------------------------------------------------- manifests
 *
 * THE 8192-BYTE CLIFF. schwung's module manager rejects any module.json over
 * 8192 bytes outright — parse_module_json() in src/host/module_manager.c:
 *
 *     if (len > 8192) { printf("mm: module.json too large: %s\n", ...); return -1; }
 *
 * A rejected manifest means the module never appears at all. Work shipped over
 * that limit from v0.1.0 (11 kB) and reached 78 kB once a full static
 * ui_hierarchy was baked in, so the FX build could not be opened on hardware.
 *
 * The hierarchy belongs in the DSP: the host tries get_param("ui_hierarchy")
 * FIRST (shadow_chain_mgmt.c) and reads it into a 64 KB buffer, falling back to
 * module.json only if the DSP returns nothing. So the manifest stays small and
 * the labels stay correct.
 */
const MANIFEST_LIMIT = 8192;
const manifests = [
    'modules/audio_fx/work/module.json',
    'modules/sound_generators/work-in/module.json',
    'modules/overtake/overwork/module.json'
];
for (const rel of manifests) {
    const bytes = fs.statSync(path.join(root, rel)).size;
    check(bytes <= MANIFEST_LIMIT,
          `${rel} is ${bytes} B — over the host's ${MANIFEST_LIMIT} B limit, so the ` +
          `module will not load at all`);
    const m = JSON.parse(fs.readFileSync(path.join(root, rel), 'utf8'));
    check(!m.capabilities || !m.capabilities.ui_hierarchy,
          `${rel} embeds a ui_hierarchy — it belongs in the DSP, and it is what ` +
          `pushed this file over the size limit before`);
}


/* Every machine names a FAMILY, and the page looks that name up in FAM to
 * colour the card. A machine whose family is not in FAM throws on `fam.css`
 * and the ENTIRE machine list renders empty — not just that one card. The
 * five Phase 3 machines shipped with `f:"src"` before FAM had a `src` entry,
 * so the manual site was blank and no test noticed, because this file only
 * ever parsed the table without rendering it. */
const famBlock = html.slice(html.indexOf('var FAM = {'),
                            html.indexOf('};', html.indexOf('var FAM = {')) + 1);
const families = new Set([...famBlock.matchAll(/^\s*([a-z]+):/gm)].map((m) => m[1]));
if (m) {
    const site = eval('[' + m[1] + ']');
    const orphans = [...new Set(site.filter((x) => !families.has(x.f)).map((x) => x.f))];
    check(orphans.length === 0,
          `machine families ${orphans.join(', ')} are not in FAM — every machine ` +
          `card would fail to render, leaving the page blank`);

    /* and each family needs the CSS variable it points at */
    const missingCss = [];
    for (const fam of families) {
        const cssm = famBlock.match(new RegExp(fam + ':\\s*\\{[^}]*css:"(--[a-z-]+)"'));
        if (cssm && !html.includes(cssm[1] + ':')) missingCss.push(cssm[1]);
    }
    check(missingCss.length === 0,
          `family colours ${missingCss.join(', ')} are used but never defined in CSS`);
}

console.log(`\n${checks} checks, ${failures} failed`);
process.exit(failures ? 1 : 0);
