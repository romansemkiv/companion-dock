#!/usr/bin/env node
// Syntax check of the box's web pages (components/webui/www/*.html): every inline <script> and on* handler
// must compile. One stray quote stops the whole page script and the page shows only its overlay,
// with no error on the device. Run by the host tests and CI: node tools/check_pages.js [files...]
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const dir = path.join(__dirname, '..', 'components', 'webui', 'www');
const files = process.argv.length > 2 ? process.argv.slice(2)
  : fs.readdirSync(dir).filter((f) => f.endsWith('.html')).map((f) => path.join(dir, f));

const decode = (s) => s.replace(/&quot;/g, '"').replace(/&#39;/g, "'").replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&');
let bad = 0;
for (const file of files) {
  const html = fs.readFileSync(file, 'utf8');
  const lineAt = (i) => html.slice(0, i).split('\n').length;
  const check = (code, at, what) => {
    try {
      // padded with newlines so the reported line is the page's line
      new vm.Script('\n'.repeat(lineAt(at) - 1) + code, { filename: path.basename(file) });
    } catch (e) {
      bad++;
      const m = /:(\d+)/.exec(String(e.stack));
      console.error(`${path.relative(process.cwd(), file)}:${m ? m[1] : lineAt(at)}: ${what}: ${e.message}`);
    }
  };
  for (const m of html.matchAll(/<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/gi)) check(m[1], m.index + m[0].indexOf('>') + 1, 'script');
  // handlers in the markup itself (those built inside scripts are checked when they run)
  const markup = html.replace(/<script[\s\S]*?<\/script>/gi, (s) => s.replace(/[^\n]/g, ' '));
  for (const m of markup.matchAll(/\son[a-z]+\s*=\s*"([^"]*)"/gi)) check(`(function(event){${decode(m[1])}\n})`, m.index, 'handler');
}
// The icons: the symbols exist and are used where stated; the replaced ones are gone; every
// <use href="#i-..."> in the markup names a symbol that exists.
for (const file of files.filter((f) => path.basename(f) === 'index.html')) {
  const html = fs.readFileSync(file, 'utf8');
  const need = (ok, what) => { if (!ok) { bad++; console.error(`${path.relative(process.cwd(), file)}: icons: ${what}`); } };
  const symbol = (id) => new RegExp(`<symbol id="i-${id}" viewBox="[^"]*">([\\s\\S]*?)</symbol>`).exec(html);
  const has = (s) => html.includes(s);
  const circ = symbol('circuitry');
  need(circ && /viewBox="0 0 256 256"/.test(circ[0]), 'i-circuitry (Phosphor Circuitry, 256 box) missing');
  need(has("['device','circuitry','Device']"), 'the Device nav entry does not use i-circuitry');
  need(/panel\('Status',h\('dl',\{class:'kv'\},h\('dt',null,'Running for'\)[^;]*'circuitry'\)/.test(html), 'the uptime (on Device) does not use i-circuitry');
  // Companion is Phosphor's webhooks-logo, Satellite Lucide's satellite; one ink, no red dot
  const comp = symbol('companion'), sat = symbol('satellite');
  need(comp && /viewBox="0 0 256 256"/.test(comp[0]) && comp[1].includes('d="M178.16,176H111.32A48,48,0,1,1,25.6,139.19'),
    'i-companion is not Phosphor\'s webhooks-logo (256 box)');
  need(sat && /viewBox="0 0 24 24"/.test(sat[0]) && sat[1].includes('d="M9.352 10.648a1.205 1.205 0 0 0 0 1.704l2.296 2.296')
    && sat[1].includes('d="M9 21a6 6 0 0 0-6-6"'), 'i-satellite is not Lucide\'s satellite (24 box)');
  for (const [id, s] of [['companion', comp], ['satellite', sat]]) need(s && !/var\(--bad\)/.test(s[1]), `i-${id} still has the red dot`);
  need(has("['companion','companion','Companion']"), 'the Companion nav entry does not use i-companion');
  need(has("saveRow(saveComp)],null,'companion')"), 'the Find Companion panel heading does not use i-companion');
  need(has("C=(...a)=>L('companion','Companion',...a)"), "the Home chain's Companion link does not use i-companion");
  need(symbol('dock'), 'i-dock missing');
  need(/\['satellite','Satellite',"[^"]*",'satellite'\]/.test(html), "the mode chooser's Satellite choice has no i-satellite");
  need(/\['dock','Network Dock',`[^`]*`,'dock'\]/.test(html), "the mode chooser's Dock choice has no i-dock");
  need(/opts\.map\(\(\[v,t,d,n\]\)/.test(html) && has("n&&[ic(n),' ']"), 'choice() does not draw its icons');
  for (const gone of ['i-link', 'i-cpu', "'link'", "'cpu'"]) need(!has(gone), `${gone} is still on the page`);
  for (const m of html.matchAll(/href="#i-([\w-]+)"/g)) need(symbol(m[1]), `<use href="#i-${m[1]}"> has no symbol`);
}

// The colour tokens (WCAG AA): text 4.5:1, boundaries 3:1, in both themes. The light theme is written
// twice (prefers-color-scheme and data-theme=light): both copies must be the same.
for (const file of files.filter((f) => path.basename(f) === 'index.html')) {
  const html = fs.readFileSync(file, 'utf8');
  const need = (ok, what) => { if (!ok) { bad++; console.error(`${path.relative(process.cwd(), file)}: colours: ${what}`); } };
  const block = (re) => { const m = re.exec(html); return m ? Object.fromEntries([...m[1].matchAll(/--([\w-]+):([^;]+);/g)].map((x) => [x[1], x[2].trim()])) : null; };
  const dark = block(/:root\{([^}]*)\}/);
  const lightMq = /:root:not\(\[data-theme=dark\]\)\{([^}]*)\}/.exec(html), lightDt = /:root\[data-theme=light\]\{([^}]*)\}/.exec(html);
  need(lightMq && lightDt && lightMq[1].trim() === lightDt[1].trim(), 'the two light-theme copies differ');
  const light = Object.assign({}, dark, block(/:root\[data-theme=light\]\{([^}]*)\}/));
  const rgb = (v) => {
    let m = /^#([0-9a-f]{3}|[0-9a-f]{6})$/i.exec(v);
    if (m) { const h = m[1].length === 3 ? m[1].replace(/./g, '$&$&') : m[1]; return [0, 2, 4].map((i) => parseInt(h.slice(i, i + 2), 16)).concat(1); }
    m = /^rgba\(\s*(\d+),\s*(\d+),\s*(\d+),\s*([\d.]+)\)$/.exec(v);
    return m ? [+m[1], +m[2], +m[3], +m[4]] : null;
  };
  const over = (fg, bg) => fg.slice(0, 3).map((c, i) => c * fg[3] + bg[i] * (1 - fg[3])).concat(1);
  const lum = (c) => { const l = c.slice(0, 3).map((x) => { x /= 255; return x <= 0.03928 ? x / 12.92 : ((x + 0.055) / 1.055) ** 2.4; }); return 0.2126 * l[0] + 0.7152 * l[1] + 0.0722 * l[2]; };
  const ratio = (a, b) => { const x = lum(a), y = lum(b); return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05); };
  for (const [name, t] of [['dark', dark], ['light', light]]) {
    const c = (k) => { const v = t[k] && rgb(t[k]); need(v, `${name}: --${k} is not a colour`); return v || [0, 0, 0, 1]; };
    const bg = c('bg'), panel = c('panel'), panel2 = c('panel2');
    const pair = (fg, back, min, what) => { const r = ratio(fg, back); need(r >= min, `${name}: ${what} ${r.toFixed(2)}:1, under ${min}:1`); };
    for (const k of ['acc', 'ok', 'warn', 'bad']) { /* status and accent text: on its tint (over the page and a panel), the page, a panel */
      pair(c(k), over(c(k + '-soft'), bg), 4.5, `--${k} on --${k}-soft over --bg`);
      pair(c(k), over(c(k + '-soft'), panel), 4.5, `--${k} on --${k}-soft over --panel`);
      pair(c(k), bg, 4.5, `--${k} on --bg`);
      pair(c(k), panel, 4.5, `--${k} on --panel`);
    }
    for (const k of ['fg', 'mut', 'dim']) for (const [b, bn] of [[bg, 'bg'], [panel, 'panel'], [panel2, 'panel2']]) pair(c(k), b, 4.5, `--${k} on --${bn}`);
    for (const k of ['acc', 'ok', 'warn', 'bad']) for (const f of ['fg', 'mut']) /* text on a tint: a chosen card, a banner's words */
      pair(c(f), over(c(k + '-soft'), panel), 4.5, `--${f} on --${k}-soft over --panel`);
    pair(c('acc-ink'), c('acc'), 4.5, 'primary button text (--acc-ink on --acc)');
    pair(c('bad-ink'), c('bad'), 4.5, 'danger button text (--bad-ink on --bad)');
    pair(c('field-line'), panel, 3, '--field-line on --panel');
    pair(c('field-line'), panel2, 3, '--field-line on --panel2');
    pair(c('acc'), panel, 3, 'the focus ring (--acc) on --panel');
    pair(c('acc'), bg, 3, 'the focus ring (--acc) on --bg');
    if (name === 'light') pair(c('knob-ring'), panel, 3, "the switch knob's ring on --panel");
  }
  need(/button\.danger\{background:var\(--bad\);color:var\(--bad-ink\)\}/.test(html), 'button.danger does not use --bad-ink');
  for (const sel of ['input,select,textarea{', 'button.ghost{', '.sw input{'])
    need(new RegExp(sel.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + '[^}]*var\\(--field-line\\)').test(html), `${sel} has no --field-line boundary`);
}

// Keyboard focus (WCAG 2.4.7): no control loses its outline without a replacement. Only a rule that draws the
// accent border may drop the outline (the text fields); the switches and sliders get a 2 px accent outline on
// :focus-visible, like the buttons, links and radios.
for (const file of files.filter((f) => path.basename(f) === 'index.html')) {
  const html = fs.readFileSync(file, 'utf8');
  const need = (ok, what) => { if (!ok) { bad++; console.error(`${path.relative(process.cwd(), file)}: focus: ${what}`); } };
  const css = (/<style>([\s\S]*?)<\/style>/.exec(html) || [, ''])[1].replace(/\/\*[\s\S]*?\*\//g, '');
  const rules = [...css.matchAll(/([^{}]+)\{([^{}]*)\}/g)].map((m) => ({ sel: m[1].trim().split(',').map((x) => x.trim()), decl: m[2] }));
  for (const r of rules)
    if (/outline:\s*(none|0)\b/.test(r.decl))
      need(/border-color:var\(--acc\)/.test(r.decl) && r.sel.every((x) => /:focus\b/.test(x)), `"${r.sel.join(',')}" drops the outline with no focus border in its place`);
  const ring = (sel) => rules.some((r) => r.sel.includes(sel) && /outline:2px solid var\(--acc\)/.test(r.decl));
  for (const sel of ['.sw input:focus-visible', 'input[type=range]:focus-visible', 'button:focus-visible', 'a:focus-visible', '.choice input:focus-visible'])
    need(ring(sel), `${sel} has no 2 px accent outline`);
  need(rules.some((r) => r.sel.includes('.sw input:after') && /box-shadow:0 0 0 1px var\(--knob-ring/.test(r.decl)), 'the switch knob has no ring');
}

// The argument at s[i] of a call: up to the comma or closing parenthesis that ends it, brackets counted, strings and
// template literals skipped (the stripped page has no line break to stop at after ; { , ( [: pack_page.py).
function handlerAt(s, i) {
  let depth = 0;
  for (let j = i; j < s.length; j++) {
    const ch = s[j];
    if (ch === "'" || ch === '"' || ch === '`') {
      for (j++; j < s.length && s[j] !== ch; j++) if (s[j] === '\\') j++;
    } else if ('([{'.includes(ch)) depth++;
    else if (')]}'.includes(ch) || (ch === ',' && depth === 0)) {
      if (depth === 0) return s.slice(i, j);
      depth--;
    }
  }
  return s.slice(i);
}

// Saving: a switch or a radio never acts by itself, only a verb button does (the mode cards select, then
// "Switch to <mode> and restart…"; Wi-Fi on / off is a button); every Save stays off until its panel differs from what
// was loaded or saved, "Unsaved changes" beside it; leaving a page with changes asks first (the hash and beforeunload);
// a form error shows under its field (aria-invalid, aria-describedby, focus); the Wi-Fi and hotspot passwords can be shown.
for (const file of files.filter((f) => path.basename(f) === 'index.html')) {
  const html = fs.readFileSync(file, 'utf8');
  const need = (ok, what) => { if (!ok) { bad++; console.error(`${path.relative(process.cwd(), file)}: saving: ${what}`); } };
  for (const m of html.matchAll(/addEventListener\('change',/g)) {
    const fn = handlerAt(html, m.index + m[0].length); // the handler whole, however its lines are joined
    need(!/rpc\(|call\(|switchMode\(|setWifiOn\(|restartFlow\(/.test(fn), `a change handler acts by itself: ${fn.slice(0, 80)}`);
  }
  need(/id:'modego',hidden:true\},btn\('',act\(\(\)=>switchMode\(radio\('smode'\)\)\)\)\)/.test(html) && html.includes('`Switch to ${v===\'dock\'?\'Network Dock\':\'Satellite\'} and restart…`'),
    'the mode cards have no "Switch to <mode> and restart…" button');
  need(/class:'btns sv'\},btn\('Save',act\(async\(\)=>\{const n=OK;await fn\(\);if\(OK>n\)r\._s=null\}\)/.test(html) && html.includes("'Unsaved changes'"), 'saveRow has no dirty state');
  need(/r\.firstChild\.disabled=!r\._d\|\|!!r\.firstChild\._busy;r\.lastChild\.hidden=!r\._d/.test(html) && /OK\+\+/.test(html), 'Save is not off until something changes (and while it saves)');
  need(/'Leave without saving\?'/.test(html) && /BACK=true;location\.hash=page/.test(html) && /addEventListener\('beforeunload',e=>\{if\(dirty\(\)\)/.test(html), 'no guard on leaving unsaved changes');
  need(/setAttribute\('aria-invalid','true'\);i\.setAttribute\('aria-describedby',id\+'-e'\);i\.focus\(\)/.test(html) && /removeAttribute\('aria-invalid'\)/.test(html), 'bad() does not mark, describe and focus the field');
  for (const id of ['chost', 'cport', 'dns', 'ssid', 'wpass', 'bssid', 'bpass', 'appass', 'bname', 'pwnew', 'pwold', 'msgtext', 'msgsec'])
    need(new RegExp(`bad\\('${id}'`).test(html), `no error under the field ${id}`);
  need(html.includes("[['ethIp','ip'],['ethMask','mask'],['ethGw','gw']])if(!quad(p[k]))return bad(i,"), 'the Ethernet address errors are not under their fields');
  for (const gone of ["toast('Password: 8-63 characters',true)", "toast(l+': enter four numbers", "toast('Enter a box name',true)", "toast(bad,true)"])
    need(!html.includes(gone), `a form error is still only a toast: ${gone}`);
  for (const id of ['wpass', 'bpass', 'appass']) need(html.includes(`pfld('${id}'`), `the password ${id} has no Show button`);
  for (const m of html.matchAll(/\bon(?:change|input)\s*[:=]\s*["'`]?([^,}"'`]*)/gi))   // handlers set as attributes too
    need(!/rpc\(|call\(|switchMode\(|setWifiOn\(|restartFlow\(/.test(m[1]), `an onchange / oninput acts by itself: ${m[1].slice(0, 60)}`);
  need(/if\(!i\)\{toast\(m,true\);return false\}/.test(html), 'bad() fails silently on a missing field');
  // a11y: placeholders at 4.5:1 (--dim), named radio groups, the decks table's roles, a scan row says secured
  need(/::placeholder\{color:var\(--dim\)\}/.test(html), 'placeholders keep the browser grey');
  need(/role:'radiogroup','aria-label':label/.test(html) && (html.match(/\],'(Connection mode|How to find Companion)'\)/g) || []).length === 2 && html.includes(",'Companions found'))"),
    'a radio group has no name');
  need(/h\('table',\{class:'dtab',role:'table'/.test(html) && /role:'rowgroup'/.test(html) && /h\('tr',\{role:'row'\}/.test(html) && /h\('td',\{class:c,role:'cell'\}/.test(html),
    'the stacked deck rows lose their table semantics');
  need(/'aria-label':`\$\{n\.ssid\}, \$\{n\.auth==='open'\?'open':'secured'\}/.test(html), 'a scan row does not say secured');
  need(/\.panel>\.ph h2\{/.test(html) && /h\('div',\{class:'ph'\},h\('h2'/.test(html), 'panel titles are not h2 (a heading level skipped)');
  need(/const b=btn\('Show',\(\)=>\{const s=i\.type==='password';i\.type=s\?'text':'password';b\.textContent=s\?'Hide':'Show'\}/.test(html), 'pfld has no Show / Hide');
}

// Phones (WCAG 1.4.10 Reflow at 320 px): a panel's header actions wrap; at 600 px and under, the decks panel's
// actions wrap one by one under its title and each deck is a stacked row that carries its box menu control too; the
// drawer is the visible height (100dvh, the phone's toolbar never hides its foot).
for (const file of files.filter((f) => path.basename(f) === 'index.html')) {
  const html = fs.readFileSync(file, 'utf8');
  const need = (ok, what) => { if (!ok) { bad++; console.error(`${path.relative(process.cwd(), file)}: phones: ${what}`); } };
  const css = (/<style>([\s\S]*?)<\/style>/.exec(html) || [, ''])[1].replace(/\/\*[\s\S]*?\*\//g, '');
  const rule = (sel) => (new RegExp(sel.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + '\\{([^}]*)\\}').exec(css) || [, null])[1];
  need(/flex-wrap:wrap/.test(rule('.panel>.ph .act') || ''), 'the panel header actions do not wrap');
  const phone = (/@media \(max-width:600px\)\{([\s\S]*?\})\s*\}/.exec(css) || [, ''])[1];
  need(/\.dtab thead\{display:none\}/.test(phone) && /\.dtab td\.dsub\{display:block/.test(phone) && /\.dtab \.dm \.pill\{display:inline-flex\}/.test(phone),
    'at 600 px the decks are not stacked rows');
  need(/\.dtab td\.dmt\{display:block/.test(phone) && /td\('dmt',d\.screen&&h\('span',\{class:'mtc'\},mtCell\(d\)\)\)/.test(html),
    'at 600 px the stacked deck rows do not carry the box menu control');
  need(/height:100dvh/.test(rule('#side') || ''), 'the drawer is not 100dvh');
}

if (bad) { console.error(`${bad} page script error(s)`); process.exit(1); }
console.log(`pages: ${files.length} checked, scripts compile, icons in place, colours meet AA, focus visible, saving, fits phones`);

// Smoke test of index.html's renderers. The page script runs against a stand-in DOM (every element,
// method and property is one inert callable stand-in) with canned API answers, and every page and
// its live part is drawn for a few box states. A runtime typo in a renderer (an undefined name, a
// wrong field path) fails here instead of blanking that page on the box. Button handlers don't run.
const index = files.find((f) => path.basename(f) === 'index.html');
if (index) {
  let done = false;   // a renderer awaiting something that never settles would otherwise end the run silently, as a pass
  process.on('exit', () => { if (!done) { console.error(`${path.relative(process.cwd(), index)}: smoke test did not finish`); process.exitCode = 1; } });
  smoke(index).then(() => { done = true; }, (e) => { console.error(`${path.relative(process.cwd(), index)}: smoke test: ${e && e.stack || e}`); process.exit(1); });
}

async function smoke(file) {
  const html = fs.readFileSync(file, 'utf8');
  // the inline scripts, with the markup between them as empty lines so stack traces give page lines
  let code = '', at = 0;
  for (const m of html.matchAll(/<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/gi)) {
    const start = m.index + m[0].indexOf('>') + 1;
    code += html.slice(at, start).replace(/[^\n]/g, '') + m[1];
    at = start + m[1].length;
  }
  const stub = new Proxy(function () {}, {
    get: (t, k) => k === Symbol.toPrimitive ? () => '' : k === Symbol.iterator ? function* () {} : k === 'then' ? undefined : stub,
    set: () => true, apply: () => stub, construct: () => stub, has: () => true,
  });
  const store = { getItem: () => null, setItem() {}, removeItem() {} };
  const noop = () => 0;
  const ctx = vm.createContext({ document: stub, window: stub, Node: stub, WebSocket: stub, XMLHttpRequest: stub, AbortController: stub,
    fetch: stub, matchMedia: stub, localStorage: store, sessionStorage: store, TextEncoder, console,
    location: { hash: '', hostname: '192.168.8.225', host: '192.168.8.225' },
    setTimeout: noop, setInterval: noop, clearTimeout: noop, clearInterval: noop });
  vm.runInContext(code, ctx, { filename: path.basename(file) });

  // menu: the box menu is open on it; screen: it has one (the Pedal has none)
  // ignored: over the deck limit
  const deck = (model, serial, hub, menu = false, screen = true, ignored = false) => ({ device_id: 'streamdeck:' + serial, model, serial, fw: '1.01.000', high_speed: !hub, hub_depth: hub ? 1 : 0, port: hub ? 2 : 0, menu, screen, ignored });
  const base = () => ({ name: 'Box', uptime_s: 3600, mode: 'satellite',
    net: { route: 'wifi', ip: '192.168.8.225', eth_addr: '', sta_addr: '192.168.8.225', eth_dhcp: true, eth_link: false, wifi_connected: true, wifi_enabled: true, wifi_set: true,
      sta_net: 'main', sta_ssid: 'Studio', sta_rssi: -58, ap_on: false, ap_forced: false, ap_ssid: 'CompanionBox-A1B2', ap_ip: '', ap_clients: 0 },
    companion: { state: 'connected', version: '5.1.1', api_version: '1.14.0', host: '10.0.0.5', port: 16622, auto: true, pinned_id: 'x', selection: 'connect', instances: 1 },
    dock: [], decks: [deck('Stream Deck XL', 'CL1', false), deck('Stream Deck Mini', 'MINI1', true)], problems: ['a problem'],
    maintenance: false, maintenanceDeck: { serial: 'CL1', model: 'Stream Deck XL' }, // kept, the page no longer reads it
    deckLimit: 2, decksIgnored: 0,
    message: { active: true, text: 'Back\nsoon', remaining_s: 5 },
    firmware: { version: 'v1', partition: 'ota_0', pending_verify: true }, c6: { running: '3.0.9', carried: '3.0.10' }, heap: {} });
  const states = {
    satellite: base(),
    // on the backup Wi-Fi network
    dock: Object.assign(base(), { mode: 'dock', net: Object.assign(base().net, { sta_net: 'backup', sta_ssid: 'Phone', sta_rssi: -71 }), dock: [{ dock: 0, port: 5343, listening: true, model: 'Stream Deck XL', serial: 'CL1', companion: true, peer: '10.0.0.5' },
      { dock: 1, port: 5345, listening: true, model: '', serial: '', companion: false, peer: '' }] }),
    nonet: Object.assign(base(), { net: { route: 'none', ip: '', eth_addr: '', sta_addr: '', eth_link: false, wifi_connected: false, wifi_enabled: true, sta_net: 'none', sta_ssid: '', sta_rssi: 0,
      ap_on: true, ap_forced: true, ap_ip: '192.168.4.1', ap_clients: 1 }, decks: [], maintenanceDeck: null, message: { active: false } }),
    choose: Object.assign(base(), { companion: { state: 'connecting', host: '', port: 16622, auto: true, selection: 'ambiguous', instances: 2 } }),
    old: Object.assign(base(), { companion: { state: 'too_old', version: '4.2.1', api_version: '1.4.0', host: '10.0.0.5', port: 16622, auto: false } }),
    // dock mode on a network with no deck plugged in: no dock listens
    docknodeck: Object.assign(base(), { mode: 'dock', decks: [], maintenanceDeck: null, problems: [], message: { active: false },
      dock: [0, 1, 2, 3, 4, 5].map((k) => ({ dock: k, port: 5343 + 2 * k, listening: false, model: '', serial: '', companion: false, peer: '' })) }),
    // the box menu open on demand on the XL
    mtopen: Object.assign(base(), { maintenance: true, decks: [deck('Stream Deck XL', 'CL1', false, true), deck('Stream Deck Mini', 'MINI1', true)] }),
    // Ethernet routes (static) with Main joined behind it, the hotspot on with 2 devices
    ethboth: Object.assign(base(), { net: Object.assign(base().net, { route: 'eth', ip: '192.168.100.200', eth_link: true, eth_addr: '192.168.100.200', eth_dhcp: false,
      ap_on: true, ap_ip: '192.168.4.1', ap_clients: 2 }) }),
    // ... Wi-Fi off, the hotspot off
    wifioff: Object.assign(base(), { net: Object.assign(base().net, { route: 'eth', ip: '192.168.100.200', eth_link: true, eth_addr: '192.168.100.200', wifi_enabled: false,
      wifi_connected: false, sta_net: 'none', sta_ssid: '', sta_rssi: 0, sta_addr: '' }) }),
    // Wi-Fi on and not joined, the cable linked without an address yet: no route
    wifinojoin: Object.assign(base(), { net: Object.assign(base().net, { route: 'none', ip: '', eth_link: true, eth_addr: '', wifi_connected: false, sta_net: 'none',
      sta_ssid: '', sta_rssi: 0, sta_addr: '', ap_on: true, ap_ip: '192.168.4.1' }) }),
  };
  // the hotspot: the default, a password, without network (on) and in dock mode; the open option in
  // Satellite mode (off)
  const apSec = (st) => st.mode === 'dock' || st.net.route === 'none';
  const apCfg = (st) => ({ apEnabled: apSec(st), apSecured: apSec(st), apPass: apSec(st) ? 'secret123' : '', apSsid: 'CompanionBox-A1B2' });
  if (!apCfg(states.nonet).apSecured || !apCfg(states.dock).apSecured || apCfg(states.satellite).apSecured)
    throw new Error('apCfg: the default (a password) and the open option');
  let st = null;
  const asked = new Set();
  ctx.rpc = async (m) => (asked.add(m), {
    'Sys.GetStatus': st, 'Device.GetInfo': { id: 'companionbox-1', name: 'Box', version: 'v1', auth_en: st.mode === 'dock' },
    'Auth.GetStatus': { auth_en: st.mode === 'dock', authed: true, locked_s: 0 },
    'Companion.GetConfig': { companionAuto: st.companion.auto, companionHost: '10.0.0.5', companionPort: 16622, companionPinId: '', mode: st.mode },
    'Companion.List': [{ id: 'a', host: '10.0.0.5', port: 16622, version: '5.1.1', pinned: true }, { id: 'b', host: '10.0.0.9', port: 16623, version: '5.2.0', pinned: false }],
    'Eth.GetConfig': { ethDhcp: false, ethIp: '10.0.0.57', ethMask: '255.255.255.0', ethGw: '10.0.0.1', ethDns: '' },
    // Wi-Fi off (the hotspot stays) in dock mode, on elsewhere
    'Wifi.GetConfig': { wifiSsid: 'Studio', wifiHasPass: true, wifiBackupSsid: st.net.route === 'none' ? '' : 'Phone', wifiBackupHasPass: st.net.route !== 'none',
      wifiEnabled: st.mode !== 'dock' }, 'Ap.GetConfig': apCfg(st),
    'Ota.GetStatus': { state: 'idle', running: 'ota_0', next: 'ota_1', pending_verify: true },
    'Device.GetConfig': Object.assign({ boxName: 'Box', deckLimit: st.mode === 'dock' ? 4 : 2, companionAdopt: st.mode !== 'dock' }, // off in dock mode
      st.mode === 'dock' ? { gestureEnabled: false, gestureKeys: ['topLeft', 'bottomLeft'], gestureTaps: 2, gestureHoldSeconds: 5 } : {}), // absent: the default
    // the Speaker & LED page: defaults in Satellite mode, everything off in dock mode, a pin the list doesn't have without network
    'Feedback.GetConfig': Object.assign({ speakerEnabled: true, speakerVolume: 50, soundReady: true, soundConnected: true, soundProblem: true,
      soundDeckPlug: true, soundKeyClick: true, ledEnabled: true, ledFlash: true, ledPin: 23, ledBrightness: 30, ledPins: [0, 1, 2, 3, 4, 5, 20, 21, 22, 23, 26, 27, 33, 46, 47, 48] },
      st.mode === 'dock' ? { speakerEnabled: false, speakerVolume: 0, soundReady: false, soundConnected: false, soundProblem: false, soundDeckPlug: false,
        soundKeyClick: false, ledEnabled: false, ledFlash: false, ledBrightness: 1 } : {},
      st.net.route === 'none' ? { ledPin: 7, ledBrightness: 100, speakerVolume: 100, ledPins: [] } : {}),
    'Log.Get': { seq: 10, text: 'I (1) boot\n' },
  })[m];
  // the recovery slot: Ota.GetStatus.sync, one sync state per box state for the page renders (each state's view is checked below)
  const otaAns = (state, other = {}, top = {}) => Object.assign({ state: 'idle', running: 'ota_0', version: 'v1', next: 'ota_1', pending_verify: false,
    sync: { state, pct: state === 'syncing' ? 42 : state === 'in_sync' ? 100 : 0,
      other: Object.assign({ label: 'ota_1', project: 'companion_box', version: /empty|waiting/.test(state) ? '' : 'v0.9', date: 'Oct 30 2026',
        time: '18:25:00', valid: ['in_sync', 'differs', 'other_newer'].includes(state) }, other) } }, top);
  const syncFor = new Map([[states.satellite, 'in_sync'], [states.dock, 'syncing'], [states.nonet, 'other_newer'], [states.choose, 'empty'],
    [states.old, 'invalid'], [states.docknodeck, 'waiting']]);
  const rpcBase = ctx.rpc;
  ctx.rpc = async (m, p) => m === 'Ota.GetStatus' ? otaAns(syncFor.get(st) || 'differs') : rpcBase(m, p);
  const run = vm.runInContext(`(async (draw) => { const errs = [];
    for (const k of Object.keys(PAGES)) { page = k; PG = {}; const pg = PG;
      try { await draw(); await PAGES[k](pg); if (LIVE[k]) LIVE[k](); renderChips(); renderMsg(); } catch (e) { errs.push(k + ': ' + (e && e.stack || e)); } }
    return errs; })`, ctx);
  let n = 0;
  const errs = [];
  for (const [name, s] of Object.entries(states)) {
    st = s;
    asked.clear();
    for (const e of await run(async () => { vm.runInContext('S={sys:null,at:0,key:""};INFO={}', ctx); ctx.onSys(st); await ctx.loadInfo(); })) errs.push(`${name} / ${e}`);
    // pages that must be there: a renamed or dropped page would otherwise pass unseen
    for (const m of ['Feedback.GetConfig', 'Device.GetConfig', 'Ap.GetConfig']) if (!asked.has(m)) errs.push(`${name} / no page asked for ${m}`);
    n++;
  }
  // the light panel has the flash checkbox, fed by ledFlash and saved with it
  {
    const idx = fs.readFileSync(file, 'utf8') /* the copy being checked (stripped or source) */;
    if (!/act\(\(\)=>mtToggle\(true,d\)\)/.test(idx) || !/act\(\(\)=>mtToggle\(false,d\)\)/.test(idx) || !/act\(\(\)=>mtToggle\(false\)\)/.test(idx)
      || !/call\('Deck\.ShowMaintenance',d\?\{on,serial:d\.serial\}:\{on\}\)/.test(idx))
      errs.push('the box menu buttons do not send what they show to Deck.ShowMaintenance');
    if (!/chk\('ledfl',.*,c\.ledFlash\)/.test(idx)) errs.push('the light panel has no checkbox for ledFlash');
    if (!/ledFlash:\s*gv\('ledfl'\)/.test(idx)) errs.push('saveLight does not send ledFlash');
    // Wi-Fi on / off is a button of its own, fed by wifiEnabled, sent as wifiEnabled; the fields dimmed
    if (!/btn\(off\?'Turn Wi-Fi on':'Turn Wi-Fi off',act\(\(\)=>setWifiOn\(!!off\)\)/.test(idx)) errs.push('the Wi-Fi page has no Turn Wi-Fi on / off button');
    if (!/off=c\.wifiEnabled===false/.test(idx)) errs.push('the Wi-Fi button is not fed by wifiEnabled');
    if (!/call\('Wifi\.SetConfig',\{wifiEnabled:on\}/.test(idx)) errs.push('the Wi-Fi button does not send wifiEnabled');
    if (!/\.off\{opacity:/.test(idx)) errs.push('the fields of Wi-Fi off are not dimmed');
  }
  // the settings secret is a PIN of 4-8 digits; the sign-in and the Device page ask for it with the
  // phone's number pad, and a text password of before (Auth.GetStatus.pin false) keeps a plain field
  {
    const pinOk = vm.runInContext('typeof pinOk === "function" ? pinOk : null', ctx);
    if (!pinOk) errs.push('pinOk(pin) is missing');
    else for (const [p, ok] of [['123', false], ['1234', true], ['12345678', true], ['123456789', false], ['12a4', false], ['', false], [' 1234', false]])
      if (pinOk(p) !== ok) errs.push(`pinOk(${JSON.stringify(p)}) should be ${ok}`);
    const hOrig = ctx.h;
    const inputs = [];
    const labels = [];
    ctx.h = (tag, attrs, ...kids) => { if (tag === 'input') inputs.push(attrs || {}); for (const k of kids.flat(Infinity)) if (typeof k === 'string') labels.push(k); return hOrig(tag, attrs, ...kids); };
    const signIn = (pin) => { inputs.length = labels.length = 0; ctx.__st = { auth_en: true, pin, authed: false, locked_s: 0 }; vm.runInContext('loginDialog(__st)', ctx); return inputs.find((i) => i.id === 'lgp') || {}; };
    if (signIn(true).inputmode !== 'numeric' || !labels.includes('Settings PIN')) errs.push('the sign-in does not ask for the PIN with the number pad');
    if (signIn(false).inputmode || !labels.includes('Settings password')) errs.push('the sign-in of a text password (pin false) is not a plain password field');
    const rpcPrev = ctx.rpc;
    const device = async (pin) => {
      inputs.length = labels.length = 0;
      ctx.rpc = async (m, p) => m === 'Auth.GetStatus' ? { auth_en: true, pin, authed: true, locked_s: 0 } : rpcPrev(m, p);
      await vm.runInContext('(async()=>{page="device";PG={};await PAGES.device(PG)})()', ctx);
      return { nw: inputs.find((i) => i.id === 'pwnew') || {}, old: inputs.find((i) => i.id === 'pwold') || {}, text: labels.join(' | ') };
    };
    let d = await device(true);
    if (d.nw.inputmode !== 'numeric' || d.old.inputmode !== 'numeric' || !/New PIN/.test(d.text) || !/Remove PIN/.test(d.text))
      errs.push('the Device page does not set the settings PIN with the number pad');
    d = await device(false);
    if (d.old.inputmode || !/Current password/.test(d.text) || !/Set a PIN/.test(d.text) || d.nw.inputmode !== 'numeric')
      errs.push('the Device page with a text password of before does not ask to set a PIN');
    ctx.rpc = rpcPrev;
    ctx.h = hOrig;
  }
  // "Let Companion adopt this box without the PIN" in the Box name and PIN panel, fed by companionAdopt (absent:
  // on), with its note; part of the panel's Save (a saveRow: off until something differs), sent by saveDevice as
  // Device.SetConfig {companionAdopt} only when it changed; nothing acts on the checkbox by itself
  {
    const idx = fs.readFileSync(file, 'utf8');
    if (!idx.includes("Companion's Surfaces → Discover can point this box at itself. Turn off to require the PIN for that too."))
      errs.push('the adopt checkbox has no note');
    if (!/saveRow\(saveDevice,/.test(idx)) errs.push('the Box name and PIN panel saves without a saveRow (no dirty state)');
    if (/can't set up this box/.test(idx)) errs.push("the PIN note still says Companion's Discover can't set up the box");
    const chkOrig = ctx.chk, rpcPrev = ctx.rpc;
    let seen = null;
    ctx.chk = (id, label, on) => { if (id === 'cadopt') seen = { label, on }; return chkOrig(id, label, on); };
    for (const [cfg, want] of [[{ companionAdopt: true }, true], [{ companionAdopt: false }, false], [{}, true]]) {
      seen = null;
      ctx.rpc = async (m, p) => m === 'Device.GetConfig' ? Object.assign({ boxName: 'Box', deckLimit: 2 }, cfg) : rpcPrev(m, p);
      await vm.runInContext('(async()=>{page="device";PG={};await PAGES.device(PG)})()', ctx);
      if (!seen || seen.label !== 'Let Companion adopt this box without the PIN' || seen.on !== want)
        errs.push(`Device.GetConfig ${JSON.stringify(cfg)} gives the adopt checkbox ${JSON.stringify(seen)}, not on ${want}`);
    }
    ctx.chk = chkOrig;
    ctx.rpc = rpcPrev;
    const docOrig = ctx.document, callOrig = ctx.call, toastOrig = ctx.toast, infoOrig = ctx.loadInfo;
    const save = async (adopt, name, was = true) => {
      const calls = [];
      const el = { '#bname': { type: 'text', value: name }, '#pwnew': { type: 'password', value: '' }, '#pwrep': { type: 'password', value: '' },
        '#pwold': { type: 'password', value: '' }, '#cadopt': { type: 'checkbox', checked: adopt } };
      ctx.document = { querySelector: (q) => el[q] || stub }; // gv() reads the fields through $()
      ctx.call = async (m, p) => { calls.push([m, p]); return {}; };
      ctx.toast = () => {};
      ctx.loadInfo = async () => {};
      ctx.__pg = { auth: { auth_en: true, pin: true }, cfg: { boxName: 'Box', companionAdopt: was } };
      await vm.runInContext('(async()=>{PG=__pg;await saveDevice()})()', ctx);
      return JSON.stringify(calls);
    };
    const want = (p) => JSON.stringify(p ? [['Device.SetConfig', p]] : []);
    { // "Nothing to save" leaves the panel clean: the PIN fields emptied, the save row's baseline taken again
      const el = { '#bname': { type: 'text', value: 'Box' }, '#pwnew': { type: 'password', value: '' }, '#pwrep': { type: 'password', value: '' },
        '#pwold': { type: 'password', value: '1234' }, '#cadopt': { type: 'checkbox', checked: true } };
      ctx.document = { querySelector: (q) => el[q] || stub };
      ctx.call = async () => ({});
      ctx.toast = () => {};
      const ok0 = vm.runInContext('OK', ctx);
      ctx.__pg = { auth: { auth_en: true, pin: true }, cfg: { boxName: 'Box', companionAdopt: true } };
      await vm.runInContext('(async()=>{PG=__pg;await saveDevice()})()', ctx);
      if (el['#pwold'].value !== '' || vm.runInContext('OK', ctx) !== ok0 + 1) errs.push('"Nothing to save" leaves the PIN panel dirty');
      // without a PIN there is no #pwold (null): still clean, no TypeError
      delete el['#pwold'];
      ctx.document = { querySelector: (q) => (q === '#pwold' ? null : el[q] || stub) };
      const ok1 = vm.runInContext('OK', ctx);
      ctx.__pg = { auth: { auth_en: false, pin: false }, cfg: { boxName: 'Box', companionAdopt: true } };
      let threw = null;
      try { await vm.runInContext('(async()=>{PG=__pg;await saveDevice()})()', ctx); } catch (e) { threw = e; }
      if (threw || vm.runInContext('OK', ctx) !== ok1 + 1) errs.push(`T22-N2: "Nothing to save" without a PIN: ${threw || 'the panel stays dirty'}`);
    }
    for (const [got, exp, what] of [[await save(false, 'Box'), want({ companionAdopt: false }), 'turned off'],
      [await save(true, 'Box', false), want({ companionAdopt: true }), 'turned on'], [await save(true, 'Box'), want(null), 'unchanged'],
      [await save(true, 'Studio'), want({ boxName: 'Studio' }), 'a new name only'],
      [await save(false, 'Studio'), want({ boxName: 'Studio', companionAdopt: false }), 'both']])
      if (got !== exp) errs.push(`saveDevice, adopt ${what}: sends ${got}, not ${exp}`);
    Object.assign(ctx, { document: docOrig, call: callOrig, toast: toastOrig, loadInfo: infoOrig });
  }
  // the box menu gesture, a Device panel of its own with its own Save row: "Open the box menu from the keys"
  // (gestureEnabled, absent: on), the key to tap and the key held with it (gestureKeys, two of the four corners; absent:
  // top right, bottom right), the taps (1-3; absent 1) and the hold (2, 3 or 5 s; absent 2), fed by Device.GetConfig and
  // sent by saveGesture as Device.SetConfig {gestureEnabled, gestureKeys, gestureTaps, gestureHoldSeconds}; the same key
  // twice is an error under the second field, and nothing is sent
  {
    const idx = fs.readFileSync(file, 'utf8');
    if (!/panel\('Box menu gesture',/.test(idx)) errs.push('no Box menu gesture panel');
    if (!/saveRow\(saveGesture\)/.test(idx)) errs.push('the gesture panel saves without a saveRow (no dirty state)');
    const chkOrig = ctx.chk, pickOrig = ctx.pick, rpcPrev = ctx.rpc;
    if (typeof pickOrig !== 'function') errs.push('pick(id, label, opts, val) is missing');
    else {
      let seen = {};
      ctx.chk = (id, label, on) => { if (id === 'gon') seen.gon = { label, on }; return chkOrig(id, label, on); };
      ctx.pick = (id, label, opts, val) => { seen[id] = { label, opts: opts.map(([v, t]) => `${v}:${t}`).join(), val: String(val) }; return pickOrig(id, label, opts, val); };
      const corners = '0:Top left,1:Top right,2:Bottom left,3:Bottom right';
      for (const [cfg, want] of [
        [{}, { gon: true, gk1: '1', gk2: '3', gtaps: '1', ghold: '2' }],
        [{ gestureEnabled: true, gestureKeys: ['topRight', 'bottomRight'], gestureTaps: 1, gestureHoldSeconds: 2 }, { gon: true, gk1: '1', gk2: '3', gtaps: '1', ghold: '2' }],
        [{ gestureEnabled: false, gestureKeys: ['bottomLeft', 'topLeft'], gestureTaps: 3, gestureHoldSeconds: 5 }, { gon: false, gk1: '2', gk2: '0', gtaps: '3', ghold: '5' }]]) {
        seen = {};
        ctx.rpc = async (m, p) => m === 'Device.GetConfig' ? Object.assign({ boxName: 'Box', deckLimit: 2 }, cfg) : rpcPrev(m, p);
        await vm.runInContext('(async()=>{page="device";PG={};await PAGES.device(PG)})()', ctx);
        const got = { gon: seen.gon && seen.gon.on };
        for (const id of ['gk1', 'gk2', 'gtaps', 'ghold']) got[id] = seen[id] && seen[id].val;
        if (JSON.stringify(got) !== JSON.stringify(want)) errs.push(`Device.GetConfig ${JSON.stringify(cfg)} shows ${JSON.stringify(got)}, not ${JSON.stringify(want)}`);
        if (!seen.gon || seen.gon.label !== 'Open the box menu from the keys') errs.push(`the gesture switch reads ${JSON.stringify(seen.gon)}`);
        if (!seen.gk1 || seen.gk1.opts !== corners || !seen.gk2 || seen.gk2.opts !== corners) errs.push('the two key fields do not offer the four corners');
        if (!seen.gtaps || seen.gtaps.opts !== '1:Once,2:Twice,3:3 times') errs.push(`the taps field offers ${seen.gtaps && seen.gtaps.opts}`);
        if (!seen.ghold || seen.ghold.opts !== '2:2 s,3:3 s,5:5 s') errs.push(`the hold field offers ${seen.ghold && seen.ghold.opts}`);
      }
      Object.assign(ctx, { chk: chkOrig, pick: pickOrig, rpc: rpcPrev });
      const docOrig = ctx.document, callOrig = ctx.call, badOrig = ctx.bad;
      const save = async (on, k1, k2, taps, hold) => {
        const calls = [], bads = [];
        const el = { '#gon': { type: 'checkbox', checked: on }, '#gk1': { type: 'select-one', value: k1 }, '#gk2': { type: 'select-one', value: k2 },
          '#gtaps': { type: 'select-one', value: taps }, '#ghold': { type: 'select-one', value: hold } };
        ctx.document = { querySelector: (q) => el[q] || stub };
        ctx.call = async (m, p) => { calls.push([m, p]); return {}; };
        ctx.bad = (id) => { bads.push(id); return false; };
        await vm.runInContext('(async()=>{await saveGesture()})()', ctx);
        return JSON.stringify({ calls, bads });
      };
      const want = (p, bads = []) => JSON.stringify({ calls: p ? [['Device.SetConfig', p]] : [], bads });
      for (const [got, exp, what] of [
        [await save(true, '0', '2', '2', '3'), want({ gestureEnabled: true, gestureKeys: ['topLeft', 'bottomLeft'], gestureTaps: 2, gestureHoldSeconds: 3 }), 'top-left twice, bottom-left 3 s'],
        [await save(false, '1', '3', '1', '2'), want({ gestureEnabled: false, gestureKeys: ['topRight', 'bottomRight'], gestureTaps: 1, gestureHoldSeconds: 2 }), 'off'],
        [await save(true, '3', '0', '3', '5'), want({ gestureEnabled: true, gestureKeys: ['bottomRight', 'topLeft'], gestureTaps: 3, gestureHoldSeconds: 5 }), 'bottom-right 3 times, top-left 5 s'],
        [await save(true, '2', '2', '1', '2'), want(null, ['gk2']), 'the same key twice']])
        if (got !== exp) errs.push(`saveGesture, ${what}: ${got}, not ${exp}`);
      Object.assign(ctx, { document: docOrig, call: callOrig, bad: badOrig });
    }
  }
  // Live settings (R128): Sys.GetStatus.rev says what changed on the box since the page drew it: cfg counts the
  // settings saved (here, in another browser, on a deck), found and slots fingerprint the Companions found and the
  // firmware slots. The page reads again only what changed: a settings page its settings, drawn again panel by panel
  // (plan: a panel drawn the same stays as it is, one drawn differently is swapped in, one holding edits stays and is
  // marked "stale"; one changed and saved here is drawn fresh), the Companion page its list, the Firmware page its slots. Nothing is read again without a change,
  // and no page polls these on a timer any more. The settings pages draw through show(), never fill(V(), ...), and bind
  // no listener through $() after drawing (a panel kept from before would get it twice, a swapped one never).
  {
    const idx = fs.readFileSync(file, 'utf8');
    const plan = vm.runInContext('typeof plan === "function" ? plan : null', ctx);
    if (!plan) errs.push('R128: plan(old, now) is missing');
    else for (const [o, n, want] of [
      [[{ k: 'a' }, { k: 'b' }], [{ k: 'a' }, { k: 'b' }], ['keep', 'keep']],
      [[{ k: 'a' }, { k: 'b', held: true }], [{ k: 'x' }, { k: 'y' }], ['swap', 'stale']],
      [[{ k: 'a', held: true }], [{ k: 'a' }], ['keep']],
      [[{ k: 'a', edited: true }], [{ k: 'a' }], ['swap']],   // saved here: drawn fresh (a typed password goes)
      [[{ k: 'a', held: true, edited: true }], [{ k: 'a' }], ['keep']],
      [[{ k: 'a' }], [{ k: 'a' }, { k: 'b' }], null],
      [[], [], []]]) {
      const got = plan(o, n);
      if (JSON.stringify(got) !== JSON.stringify(want)) errs.push(`R128: plan(${JSON.stringify(o)}, ${JSON.stringify(n)}) gives ${JSON.stringify(got)}, not ${JSON.stringify(want)}`);
    }
    const revDiff = vm.runInContext('typeof revDiff === "function" ? revDiff : null', ctx);
    if (!revDiff) errs.push('R128: revDiff(was, now) is missing');
    else for (const [o, n, want] of [
      [{ cfg: 1, found: 2, slots: 3 }, { cfg: 1, found: 2, slots: 3 }, []],
      [{ cfg: 1, found: 2, slots: 3 }, { cfg: 2, found: 2, slots: 4 }, ['cfg', 'slots']],
      [{ cfg: 1 }, { cfg: 1, found: 5 }, ['found']],
      [null, { cfg: 1 }, []], [{ cfg: 1 }, undefined, []]]) {
      const got = revDiff(o, n);
      if (JSON.stringify(got) !== JSON.stringify(want)) errs.push(`R128: revDiff(${JSON.stringify(o)}, ${JSON.stringify(n)}) gives ${JSON.stringify(got)}, not ${JSON.stringify(want)}`);
    }
    if (!idx.includes('Unsaved changes. Also changed elsewhere: Save keeps yours.')) errs.push('R128: a panel kept with edits over a change elsewhere is not marked');
    const reads = { companion: 'Companion.GetConfig', ethernet: 'Eth.GetConfig', wifi: 'Wifi.GetConfig', ap: 'Ap.GetConfig', device: 'Device.GetConfig', feedback: 'Feedback.GetConfig' };
    for (const k of Object.keys(reads)) {
      const src = vm.runInContext(`PAGES[${JSON.stringify(k)}].toString()`, ctx);
      if (!/\bshow\(/.test(src) || /fill\(V\(\)/.test(src)) errs.push(`R128: the ${k} page does not draw through show()`);
      if (/\$\('#[\w-]+'\)\.addEventListener/.test(src)) errs.push(`R128: the ${k} page binds a listener through $() after drawing`);
    }
    if (/setInterval\(async\(\)=>\{[^}]*Companion\.List/.test(idx)) errs.push('R128: the Companion page still polls Companion.List on a timer');
    if (/setTimeout\(\(\)=>pollSlots/.test(idx)) errs.push('R128: the Firmware page still polls Ota.GetStatus on a timer');
    const settle = async () => { for (let i = 0; i < 20; i++) await new Promise((r) => setImmediate(r)); };
    const rpcR = ctx.rpc;
    ctx.rpc = async (m, p) => (asked.add(m), rpcR(m, p));   // every call seen (the Ota.GetStatus stand-in above records none)
    let up = 5000;
    const status = (rev, extra) => Object.assign(base(), { uptime_s: up++, rev: Object.assign({ cfg: 1, found: 7, slots: 9 }, rev) }, extra);
    const open = async (k, s) => { st = s; vm.runInContext('S={sys:null,at:0,key:""};INFO={};PG={}', ctx); ctx.onSys(st); ctx.location.hash = '#' + k; await vm.runInContext('go()', ctx); await settle(); };
    const after = async (s) => { asked.clear(); st = s; ctx.onSys(st); await settle(); return new Set(asked); };
    for (const [k, m] of Object.entries(reads)) {
      await open(k, status({}));
      let a = await after(status({}));
      if (a.has(m)) errs.push(`R128: the ${k} page reads ${m} again with nothing changed on the box`);
      a = await after(status({ cfg: 2 }));
      if (!a.has(m)) errs.push(`R128: a settings change on the box (rev.cfg) does not make the ${k} page read ${m} again`);
      if (!a.has('Device.GetInfo')) errs.push(`R128: a settings change on the box (rev.cfg) does not read Device.GetInfo again (the PIN, the sign-out button) on ${k}`);
      a = await after(status({ cfg: 2, slots: 10 }));
      if (a.has(m)) errs.push(`R128: the ${k} page reads ${m} again on a firmware slot change`);
    }
    await open('companion', status({}));
    let a = await after(status({ found: 8 }));
    if (!a.has('Companion.List') || a.has('Companion.GetConfig')) errs.push(`R128: a change in the Companions found (rev.found) reads ${[...a].join(', ') || 'nothing'}, not Companion.List alone`);
    await open('firmware', status({}));
    a = await after(status({ slots: 10 }));
    if (!a.has('Ota.GetStatus')) errs.push('R128: a firmware slot change (rev.slots) does not make the Firmware page read Ota.GetStatus');
    a = await after(status({ slots: 10 }));
    if (a.has('Ota.GetStatus')) errs.push('R128: the Firmware page reads Ota.GetStatus again with nothing changed');
    ctx.rpc = rpcR;
    vm.runInContext('S={sys:null,at:0,key:""};INFO={};PG={};page="home"', ctx);
  }
  // Logs (R129): "Export .log" saves the log shown as <box name>-<YYYY-MM-DD-HHMMSS>.log (a Blob download, the name
  // made safe: letters, digits, . _ -); the page first reads the box's whole ring, oldest first (Log.Get oldest: true,
  // again while a full chunk comes back), then follows it live as before.
  {
    const idx = fs.readFileSync(file, 'utf8');
    const logName = vm.runInContext('typeof logName === "function" ? logName : null', ctx);
    if (!logName) errs.push('R129: logName(name, date) is missing');
    else for (const [n, d, want] of [
      ['CompanionBox-8D1C', new Date(2026, 10, 4, 15, 30, 5), 'CompanionBox-8D1C-2026-11-04-153005.log'],
      ['Studio / FOH box', new Date(2026, 0, 2, 3, 4, 5), 'Studio-FOH-box-2026-01-02-030405.log'],
      ['', new Date(2026, 0, 2, 3, 4, 5), 'companion-box-2026-01-02-030405.log']]) {
      const got = logName(n, d);
      if (got !== want) errs.push(`R129: logName(${JSON.stringify(n)}) gives ${got}, not ${want}`);
    }
    if (!/btn\('Export \.log',exportLog,'ghost','download-simple'\)/.test(idx)) errs.push('R129: the Log panel has no "Export .log" button');
    if (!/<symbol id="i-download-simple" viewBox="0 0 256 256">/.test(idx)) errs.push('R129: i-download-simple (Phosphor) missing');
    if (!/new Blob\(\[/.test(idx) || !/\.download=/.test(idx)) errs.push('R129: exportLog does not save a file');
    const calls = [];
    const rpcL = ctx.rpc;
    let n = 0;
    ctx.rpc = async (m, p) => {
      if (m !== 'Log.Get') return rpcL(m, p);
      calls.push(JSON.stringify(p));
      n++;
      return n === 1 ? { seq: 8192, text: 'x'.repeat(8192) } : n === 2 ? { seq: 8300, text: 'tail\n' } : { seq: 8300, text: '' };
    };
    await vm.runInContext('(async()=>{page="logs";PG={};await PAGES.logs(PG)})()', ctx);
    for (let i = 0; i < 20; i++) await new Promise((r) => setImmediate(r));
    ctx.rpc = rpcL;
    if (calls[0] !== '{"since":0,"oldest":true}' || calls[1] !== '{"since":8192,"oldest":true}')
      errs.push(`R129: the Logs page's first reads are ${calls.slice(0, 3).join(' then ')}, not the whole ring oldest first`);
    else if (calls.length > 2 && /oldest/.test(calls[2])) errs.push('R129: the Logs page keeps reading oldest first after it caught up');
  }
  // the network signs after the Companion chip: Ethernet while linked, Wi-Fi and the hotspot always, in that
  // order, each a link to its page, its details in title and aria-label (the same), crossed grey (off) or red (not
  // joined), the route's marked (rt) and no other; phones: each sign (icon and address: the client count and bars hidden
  // at <= 600 px) fits 320 and 390 px less the 16 px gutters, the chips wrapping under the title
  {
    const idx = fs.readFileSync(file, 'utf8');
    if (!/c\.push\(signs\(n\)\)\}\s*fill\(\$\('#chips'\),c\)/.test(idx)) errs.push('renderChips does not put the network signs after the Companion chip');
    if (/title:'Box address'/.test(idx)) errs.push('the single IP chip is still there');
    const phone = (/@media \(max-width:600px\)\{([\s\S]*?\})\s*\}/.exec(idx) || [, ''])[1];
    if (!/\.sg small,\.sg \.bars\{display:none\}/.test(phone)) errs.push('at 600 px the signs keep the client count or the bars');
    if (!/\.sg\.x:before\{content:''[^}]*rotate\(-45deg\)\}/.test(idx) || !/\.sg\.bad:before\{background:var\(--bad\)\}/.test(idx))
      errs.push('no shared slash for the crossed signs (grey, red when not joined)');
    if (!/\.sg\.rt\{color:var\(--fg\);border-color:var\(--acc\)\}/.test(idx)) errs.push('the route sign is not marked (full ink, accent edge)');
    const hOrig = ctx.h;
    let made = [];
    const txt = (k) => (Array.isArray(k) ? k.flat(Infinity) : [k]).filter((x) => typeof x === 'string' || typeof x === 'number').join('');
    ctx.h = (tag, attrs, ...kids) => { const r = hOrig(tag, attrs, ...kids); if (tag === 'a') made.push({ a: attrs, text: txt(kids) });
      else if (tag === 'small' || tag === 'span') made.push({ tag, a: attrs, text: txt(kids) }); return r; };
    const signsOf = (net) => { made = []; ctx.__n = net; vm.runInContext('signs(__n)', ctx); return made; };
    const anchors = (m) => m.filter((x) => !x.tag);
    const cases = [
      ['satellite', states.satellite.net, [['#wifi', '192.168.8.225', ['rt'], /Wi-Fi Studio \(primary network\), -58 dBm: 192\.168\.8\.225/], ['#ap', 'Off', ['x', 'off'], /^Hotspot off$/]]],
      ['ethboth', states.ethboth.net, [['#ethernet', '192.168.100.200', ['rt'], /^Ethernet, static: 192\.168\.100\.200 · carries the route$/],
        ['#wifi', '192.168.8.225', [], /Wi-Fi Studio \(primary network\), -58 dBm/], ['#ap', '192.168.4.1', [], /Hotspot CompanionBox-A1B2: 192\.168\.4\.1, 2 devices joined/]]],
      ['wifioff', states.wifioff.net, [['#ethernet', '192.168.100.200', ['rt'], /Ethernet, DHCP/], ['#wifi', 'Off', ['x', 'off'], /^Wi-Fi off$/], ['#ap', 'Off', ['x', 'off'], /Hotspot off/]]],
      ['wifinojoin', states.wifinojoin.net, [['#ethernet', 'No address', [], /Ethernet, DHCP: no address$/], ['#wifi', 'Not connected', ['x', 'bad'], /^Wi-Fi not connected$/],
        ['#ap', '192.168.4.1', [], /0 devices joined/]]],
      ['nonet', states.nonet.net, [['#wifi', 'Not connected', ['x', 'bad'], /not connected/], ['#ap', '192.168.4.1', [], /1 device joined/]]],
      ['dock (backup)', states.dock.net, [['#wifi', '192.168.8.225', ['rt'], /\(backup network\), -71 dBm/], ['#ap', 'Off', ['x', 'off'], /off/]]],
      // no Wi-Fi network set up: grey "Not set up" while Ethernet routes, red only with no route at all; a
      // status without eth_dhcp reads as DHCP
      ['wifi not set up', Object.assign({}, states.wifinojoin.net, { route: 'eth', eth_addr: '10.0.0.7', wifi_set: false, eth_dhcp: undefined }),
        [['#ethernet', '10.0.0.7', ['rt'], /^Ethernet, DHCP: 10\.0\.0\.7/], ['#wifi', 'Not set up', ['x', 'off'], /^Wi-Fi not set up$/], ['#ap', '192.168.4.1', [], /joined/]]],
      ['wifi not set up, no route', Object.assign({}, states.wifinojoin.net, { wifi_set: false }),
        [['#ethernet', 'No address', [], /DHCP/], ['#wifi', 'Not connected', ['x', 'bad'], /^Wi-Fi not connected$/], ['#ap', '192.168.4.1', [], /joined/]]],
    ];
    const MONO = 6.9, PAD = 2 * 10 + 2 + 15 + 6; // 11.5 px mono; the chip's padding, border, icon and gap
    for (const [name, net, want] of cases) {
      const m = signsOf(net), a = anchors(m);
      if (a.length !== want.length) { errs.push(`${name}: ${a.length} signs, not ${want.length}`); continue; }
      want.forEach(([href, text, cls, title], i) => {
        const s = a[i], c = String(s.a.class).split(' ');
        if (s.a.href !== href) errs.push(`${name}: sign ${i} links to ${s.a.href}, not ${href}`);
        if (!s.text.startsWith(text)) errs.push(`${name}: sign ${i} says "${s.text}", not "${text}"`);
        for (const k of ['chip', 'sg', ...cls]) if (!c.includes(k)) errs.push(`${name}: sign ${i} lacks class ${k} (${s.a.class})`);
        for (const k of ['x', 'off', 'bad', 'rt']) if (c.includes(k) && !cls.includes(k)) errs.push(`${name}: sign ${i} has class ${k} (${s.a.class})`);
        if (!title.test(s.a.title) || s.a['aria-label'] !== s.a.title) errs.push(`${name}: sign ${i} title / label ${JSON.stringify([s.a.title, s.a['aria-label']])}`);
        for (const w of [320, 390]) if (PAD + MONO * text.length > w - 32) errs.push(`${name}: sign ${i} too wide for ${w} px`);
      });
      if (a.filter((s) => String(s.a.class).split(' ').includes('rt')).length !== (net.route === 'none' ? 0 : 1)) errs.push(`${name}: not exactly one route sign`);
      const bars = m.find((x) => x.tag === 'span' && /^bars b[1-4]$/.test(String((x.a || {}).class)));
      if (!!bars !== /main|backup/.test(net.sta_net)) errs.push(`${name}: Wi-Fi bars ${bars ? 'shown' : 'missing'}`);
      const cnt = m.find((x) => x.tag === 'small');
      if (net.ap_on && net.ap_clients > 0 ? !cnt || cnt.text !== ' · ' + net.ap_clients : cnt) errs.push(`${name}: the client count ${cnt ? cnt.text : 'missing'}`);
    }
    if (PAD + MONO * '255.255.255.255'.length > 320 - 32) errs.push('the longest address does not fit a 320 px phone');
    ctx.h = hOrig;
  }
  // the Wi-Fi panels offer "Use this network" for a network set up and not in use, and call Wifi.Use
  {
    const idx = fs.readFileSync(file, 'utf8') /* the copy being checked (stripped or source) */;
    if (!/call\('Wifi\.Use',\{net\}/.test(idx)) errs.push('the Use this network button does not call Wifi.Use');
    const useBtn = vm.runInContext('typeof useBtn === "function" ? useBtn : null', ctx);
    if (!useBtn) errs.push('useBtn(net, set) is missing');
    else {
      // built for a network set up, hidden while it is the one in use (LIVE.wifi keeps it so)
      vm.runInContext('S={sys:{net:{sta_net:"main"}},at:0,key:""}', ctx);
      const hOrig = ctx.h;
      let last = null;
      ctx.h = (tag, attrs, ...kids) => { if (tag === 'button') last = attrs || {}; return hOrig(tag, attrs, ...kids); };
      const shown = (net, set) => { last = null; return useBtn(net, set) && last ? !last.hidden : false; };
      if (shown('main', 'Studio')) errs.push('useBtn: the network in use offers Use');
      if (!shown('backup', 'Phone')) errs.push('useBtn: the backup not in use has no Use');
      if (useBtn('backup', '')) errs.push('useBtn: a network not set up offers Use');
      ctx.h = hOrig;
      if (!/b\.hidden=n\.sta_net===k/.test(idx)) errs.push('LIVE.wifi does not keep the Use buttons in step with the status');
    }
  }
  // the speaker's state in Feedback.Test / Device.Identify answers: "starting" is not a fault
  const note = vm.runInContext('typeof spkNote === "function" ? spkNote : null', ctx);
  if (!note) errs.push('spkNote(speaker) is missing');
  else {
    if (note('ok') !== null) errs.push('spkNote("ok") should be null');
    const st = note('starting');
    if (!st || /not work/i.test(st) || !/starting/i.test(st)) errs.push(`spkNote("starting") reads as a fault: ${st}`);
    const bad = note('the ES8311 codec does not answer at I2C 0x18');
    if (!bad || !/not work/i.test(bad) || !/ES8311/.test(bad)) errs.push(`spkNote(error) hides the fault: ${bad}`);
  }
  // the hotspot page: the 8-63 rule only while the password is on; an open hotspot says so
  const apProblem = vm.runInContext('typeof apPassProblem === "function" ? apPassProblem : null', ctx);
  const apInfo = vm.runInContext('typeof apPassInfo === "function" ? apPassInfo : null', ctx);
  if (!apProblem) errs.push('apPassProblem(settings) is missing');
  else {
    if (apProblem({ apEnabled: true, apSecured: false }) !== null) errs.push('apPassProblem: an open hotspot needs no password');
    if (apProblem({ apEnabled: true, apSecured: false, apPass: 'x' }) !== null) errs.push('apPassProblem: an open hotspot ignores the password');
    if (!apProblem({ apEnabled: true, apSecured: true, apPass: '1234567' })) errs.push('apPassProblem: 7 characters accepted');
    if (!apProblem({ apEnabled: true, apSecured: true, apPass: 'a'.repeat(64) })) errs.push('apPassProblem: 64 characters accepted');
    if (!apProblem({ apEnabled: true, apSecured: true, apPass: 'é'.repeat(32) })) errs.push('apPassProblem: 64 bytes accepted');
    if (apProblem({ apEnabled: true, apSecured: true, apPass: '12345678' }) !== null) errs.push('apPassProblem: 8 characters refused');
    if (apProblem({ apEnabled: true, apSecured: true, apPass: 'a'.repeat(63) }) !== null) errs.push('apPassProblem: 63 characters refused');
  }
  if (!apInfo) errs.push('apPassInfo(config) is missing');
  else {
    if (!/open, no password/i.test(apInfo({ apSecured: false }))) errs.push(`apPassInfo(open): ${apInfo({ apSecured: false })}`);
    if (/open|no password/i.test(apInfo({ apSecured: true }))) errs.push(`apPassInfo(secured): ${apInfo({ apSecured: true })}`);
  }
  // dock mode without a deck (no port is open): the home and Companion pages say to plug one in,
  // never to add the box in Companion and never "once the box is on a network". h(), ic()
  // and fill() are swapped for recorders, so the text the pages put up can be read.
  {
    const conn = vm.runInContext('conn', ctx);
    const docksEmpty = vm.runInContext('typeof docksEmpty === "function" ? docksEmpty : null', ctx);
    if (conn(states.docknodeck).k !== 'nodeck') errs.push(`dock mode, no deck: conn() says ${conn(states.docknodeck).k}, not nodeck`);
    if (conn(states.dock).k === 'nodeck') errs.push('dock mode with decks reads as no deck');
    if (conn(states.nonet).k !== 'nonet') errs.push('no network must win over everything');
    if (!docksEmpty) errs.push('docksEmpty(status) is missing');
    else {
      const t = docksEmpty(states.docknodeck);
      if (!/Plug in a Stream Deck/.test(t) || /once the box is on a network/i.test(t)) errs.push(`docksEmpty(no deck): ${t}`);
      if (!/network/i.test(docksEmpty(Object.assign({}, states.docknodeck, { net: { route: 'none' } }))))
        errs.push('docksEmpty(no network) should name the network');
    }
    const hOrig = ctx.h, icOrig = ctx.ic, fillOrig = ctx.fill;
    const walk = (n, f) => { f(n); if (n && n.kids) for (const k of n.kids) walk(k, f); else if (Array.isArray(n)) for (const k of n) walk(k, f); };
    const shownText = (st) => {
      const shown = [];
      ctx.fill = (e, ...kids) => { shown.push(kids); return e; };
      ctx.__st = st;
      vm.runInContext('S={sys:__st,at:0,key:""};PG={cfg:{mode:"dock"}};page="home";LIVE.home();page="companion";LIVE.companion();renderChips();', ctx);
      const t = [];
      walk(shown, (x) => { if (typeof x === 'string') t.push(x); });
      return t.join(' | ');
    };
    try {
      ctx.h = (tag, attrs, ...kids) => ({ tag, attrs: attrs || {}, kids: kids.flat(Infinity).filter((k) => k != null && k !== false) });
      ctx.ic = (name) => ({ icon: name });
      const none = shownText(states.docknodeck);
      if (!/Plug in a Stream Deck/.test(none)) errs.push('dock mode, no deck: nothing says to plug a deck in');
      for (const bad of [/add the box/i, /once the box is on a network/i, /Add Remote Surface/, /Waiting for Companion/])
        if (bad.test(none)) errs.push(`dock mode, no deck: the pages say ${bad}`);
      const some = shownText(states.dock); // the control: with decks, the docks and how to add them show
      if (!/Add Remote Surface/.test(some) || /Plug in a Stream Deck/.test(some)) errs.push('dock mode with decks: the docks panel is wrong');
      // each deck's box menu control is in its Home row (no longer on Device)
      if ((some.match(/\| Show box menu\b/g) || []).length !== 2) errs.push('the Home deck rows have no box menu control each');
      // on a phone each deck is a stacked row: the model and its In Companion pill, then serial · fw · USB
      if (!/Stream Deck XL \| Dock 1 \|/.test(some) || !/CL1 · 1\.01\.000 · High speed, direct/.test(some))
        errs.push(`the deck rows have no stacked form for phones: ${some}`);
      if (!/MINI1 · 1\.01\.000 · Full speed, hub port 2/.test(shownText(states.satellite)) || !/Stream Deck XL \| In Companion/.test(shownText(states.satellite)))
        errs.push('the deck rows (satellite) have no stacked form for phones');
      // while it is open, Home's one banner says so and closes it
      const on = shownText(Object.assign({}, states.dock, { maintenance: true, decks: states.mtopen.decks }));
      if (!/Box menu open on Stream Deck XL\. Companion gets the deck back when you close it\. \| Close menu/.test(on)) errs.push('Home: no banner for the open box menu');
    } catch (e) { errs.push(`dock mode, no deck: ${e && e.stack || e}`); }
    finally { ctx.h = hOrig; ctx.ic = icOrig; ctx.fill = fillOrig; }
  }
  // Home: the signal chain Network → Companion → Decks in every box state: each link's state, word, line and fix;
  // one banner at most (the first broken link's), or Ready when all three are good; first contact over the hotspot in
  // accent ("Step 1", Wi-Fi and Ethernet); the problem texts on their link, the banner, or folded under the chain
  {
    const hOrig = ctx.h, icOrig = ctx.ic, fillOrig = ctx.fill, docOrig = ctx.document, hostOrig = ctx.location.hostname;
    const walk = (n, f) => { f(n); if (n && n.kids) for (const k of n.kids) walk(k, f); else if (Array.isArray(n)) for (const k of n) walk(k, f); };
    const texts = (n) => { const t = []; walk(n, (x) => { if (typeof x === 'string') t.push(x); }); return t.join('|'); };
    const find = (n, pred) => { const r = []; walk(n, (x) => { if (x && typeof x === 'object' && !Array.isArray(x) && pred(x)) r.push(x); }); return r; };
    const cls = (x) => (x.attrs || {}).class || '';
    const view = (st, host) => {
      const shown = {};
      ctx.location.hostname = host || '192.168.8.225';
      ctx.document = { querySelector: (sel) => ({ sel }) };
      ctx.fill = (e, ...kids) => { shown[e.sel] = kids.flat(Infinity).filter((k) => k != null && k !== false); return e; };
      ctx.__st = st;
      vm.runInContext('S={sys:__st,at:0,key:""};page="home";LIVE.home()', ctx);
      return shown;
    };
    const want = {   // [the three links' dot classes, their words, the banner's class, a fix on the page]
      satellite: [['ok', 'ok', 'ok'], ['Wi-Fi', 'Connected', '2 decks'], 'ready', null],
      dock: [['ok', 'ok', 'warn'], ['Wi-Fi', 'Connected', '2 decks'], 'bn bw warn', 'Show how'],
      nonet: [['bad', '', ''], ['Offline', 'Waiting', 'No deck'], 'bn bw bad', null],   // the network's fixes are the banner's
      choose: [['ok', 'warn', ''], ['Wi-Fi', 'Choose', '2 decks'], 'bn bw warn', 'Choose one'],
      old: [['ok', 'bad', ''], ['Wi-Fi', 'Too old', '2 decks'], 'bn bw bad', null],
      docknodeck: [['ok', '', 'warn'], ['Wi-Fi', 'Waiting', 'No deck'], 'bn bw warn', null],
      mtopen: [['ok', 'ok', 'ok'], ['Wi-Fi', 'Connected', '2 decks'], 'bn acc bw', null],   // the open box menu's banner wins
      mttwo: [['ok', 'ok', 'ok'], ['Wi-Fi', 'Connected', '2 decks'], 'bn acc bw', null],    // on two decks at once
      limit: [['ok', 'ok', 'warn'], ['Wi-Fi', 'Connected', '2 decks'], 'bn bw warn', 'Open Device'], // a 3rd deck ignored at 2
      limit4: [['ok', 'ok', 'warn'], ['Wi-Fi', 'Connected', '4 decks'], 'bn bw warn', null],        // ... a 5th at 4: no Device link
      limitroom: [['ok', 'ok', 'warn'], ['Wi-Fi', 'Connected', '1 deck'], 'bn bw warn', null],      // room again, still ignored
      // the states the stand-in box has too
      looking: [['ok', 'warn', ''], ['Wi-Fi', 'Looking', '2 decks'], 'bn bw warn', 'Find Companion'],
      satnodeck: [['ok', 'ok', 'warn'], ['Wi-Fi', 'Connected', 'No deck'], 'bn bw warn', null],
      dockpart: [['ok', 'ok', 'warn'], ['Wi-Fi', 'Connected', '2 decks'], 'bn bw warn', 'Show how'],
      problems: [['ok', 'ok', 'ok'], ['Wi-Fi', 'Connected', '2 decks'], 'ready', null],
      // Ethernet carries the route (Main joined behind it, or Wi-Fi off); nothing does (the cable without an address)
      ethboth: [['ok', 'ok', 'ok'], ['Ethernet', 'Connected', '2 decks'], 'ready', null],
      wifioff: [['ok', 'ok', 'ok'], ['Ethernet', 'Connected', '2 decks'], 'ready', null],
      wifinojoin: [['bad', '', ''], ['Offline', 'Waiting', '2 decks'], 'bn bw bad', null],
    };
    const more = {
      looking: Object.assign({}, states.satellite, { companion: { state: 'connecting', host: '', port: 16622, auto: true, selection: 'none', instances: 0 } }),
      satnodeck: Object.assign({}, states.satellite, { decks: [], maintenanceDeck: null }),
      dockpart: Object.assign({}, states.dock),   // CL1 in dock 1, MINI1 in no dock
      problems: Object.assign({}, states.satellite, { problems: ['Stream Deck XL stopped responding', 'Unknown Elgato device 0x0099'] }),
      mttwo: Object.assign({}, states.mtopen, { decks: states.mtopen.decks.map((d) => Object.assign({}, d, { menu: true })) }),
      limit: Object.assign({}, states.satellite, { decksIgnored: 1, decks: states.satellite.decks.concat(deck('Stream Deck +', 'PL1', true, false, false, true)) }),
      limit4: Object.assign({}, states.satellite, { deckLimit: 4, decksIgnored: 1, decks: states.satellite.decks.concat(deck('Stream Deck MK.2', 'MK1', false),
        deck('Stream Deck Neo', 'NE1', true), deck('Stream Deck +', 'PL1', true, false, false, true)) }),
      limitroom: Object.assign({}, states.satellite, { decksIgnored: 1, decks: [states.satellite.decks[0], deck('Stream Deck +', 'PL1', true, false, false, true)] }),
    };
    try {
      ctx.h = (tag, attrs, ...kids) => ({ tag, attrs: attrs || {}, kids: kids.flat(Infinity).filter((k) => k != null && k !== false) });
      ctx.ic = (name) => ({ icon: name });
      for (const [name, st] of Object.entries(Object.assign({}, states, more))) {
        const w = want[name];
        if (!w) { errs.push(`Home: no expectation for the box state ${name}`); continue; }
        const v = view(st);
        const hops = (v['#chain'] || []).filter((x) => cls(x) === 'hop');
        if (hops.length !== 3) { errs.push(`Home ${name}: ${hops.length} links in the chain`); continue; }
        const dots = hops.map((x) => cls(find(x, (y) => /\bdot\b/.test(cls(y)))[0] || {}).replace(/^dot ?/, ''));
        const words = hops.map((x) => texts(find(x, (y) => cls(y) === 'val')[0]));
        if (dots.join() !== w[0].join()) errs.push(`Home ${name}: link states ${dots}, want ${w[0]}`);
        if (words.join() !== w[1].join()) errs.push(`Home ${name}: link words ${words}, want ${w[1]}`);
        const names = hops.map((x) => texts(find(x, (y) => cls(y) === 'lbl')[0]));
        if (names.join() !== 'Network,Companion,Decks') errs.push(`Home ${name}: the chain is ${names}`);
        const bn = v['#hbn'] || [];
        if (bn.length !== 1 || cls(bn[0]) !== w[2]) errs.push(`Home ${name}: the banner ${bn.map(cls)}, want one ${w[2]}`);
        const fixes = find(v['#chain'], (x) => x.tag === 'button').map(texts);
        if (fixes.length > 1 || (fixes[0] || null) !== w[3]) errs.push(`Home ${name}: fixes ${fixes}, want ${w[3]}`);
        for (const hop of hops) if (find(hop, (x) => x.tag === 'button').length > 1) errs.push(`Home ${name}: a link has more than one fix`);
        const np = (st.problems || []).length, rs = `Ready|2 decks in Companion over ${st.net.route === 'eth' ? 'Ethernet' : 'Wi-Fi'}${np ? ` · ${np} recent problem${np > 1 ? 's' : ''}` : ''}`;
        if (w[2] === 'ready' && texts(bn) !== rs) errs.push(`Home ${name}: Ready says ${texts(bn)}, want ${rs}`);
        // the banner doesn't repeat its link's line
        for (const hop of hops) { const sub = texts(find(hop, (y) => cls(y) === 'sub')[0]); if (sub && texts(bn).includes(sub)) errs.push(`Home ${name}: the banner repeats "${sub}"`); }
        if (w[2] !== 'ready' && /Ready/.test(texts(bn))) errs.push(`Home ${name}: Ready while a link is broken`);
        const all = texts(v['#hbn']) + '|' + texts(v['#chain']) + '|' + texts(v['#probs']);
        for (const p of st.problems || []) if (!all.includes(p)) errs.push(`Home ${name}: the problem "${p}" is not on the page`);
      }
      // the deck limit's banner, the Device link at 2 only; the ignored deck's row: "Over the limit", no box menu control
      const lb = (st) => texts(view(st)['#hbn']);
      if (lb(more.limit) !== 'Deck limit reached (2). Unplug one, or allow up to 4 on Device.') errs.push(`Home limit: the banner ${lb(more.limit)}`);
      if (lb(more.limit4) !== 'Deck limit reached (4). Unplug one.') errs.push(`Home limit 4: the banner ${lb(more.limit4)}`);
      /* a deck unplugged since, so there is room, but the extra one stays ignored until it is plugged in again */
      if (lb(more.limitroom) !== 'Stream Deck + is over the limit: unplug it and plug it back in.') errs.push(`Home limit, room again: the banner ${lb(more.limitroom)}`);
      const fixOf = (st) => find(view(st)['#chain'], (x) => x.tag === 'button').map((x) => `${texts(x)}`).join();
      if (fixOf(more.limit) !== 'Open Device' || fixOf(more.limit4) !== '') errs.push(`Home limit: the fixes ${fixOf(more.limit)} / ${fixOf(more.limit4)}`);
      if (!/1 deck over the limit/.test(texts(view(more.limit)['#chain']))) errs.push('Home limit: the Decks link does not say a deck is over the limit');
      const lr = find(view(more.limit)['#decks'], (x) => x.tag === 'tr')[2];
      if (!lr || !/Stream Deck \+\|Over the limit/.test(texts(lr)) || find(lr, (x) => x.tag === 'button').length || !find(lr, (x) => cls(x) === 'pill warn').length)
        errs.push(`Home limit: the ignored deck's row ${lr && texts(lr)}`);
      // the problem texts of before, on their link or in the banner
      const has = (st, re, host) => { const v = view(st, host); return re.test(texts(v['#hbn']) + '|' + texts(v['#chain'])); };
      if (!has(states.nonet, /Ethernet cable not connected/) || !has(states.nonet, /Wi-Fi is not connected/)) errs.push('Home nonet: the network problems are gone');
      if (!has(states.old, /4\.2\.1 at 10\.0\.0\.5:16622/) || !has(states.old, /older than 5\.0/)) errs.push('Home old: the too-old text is gone');
      if (!has(states.choose, /2 found/) || !has(states.choose, /won't pick one by itself/)) errs.push('Home choose: the count is gone');
      const nb = find(view(states.nonet)['#hbn'], (x) => x.tag === 'button').map(texts);   // no network, not over the hotspot
      if (nb.join() !== 'Set up Wi-Fi,Open Ethernet') errs.push(`Home nonet: the banner's buttons ${nb}`);
      // the header chip: accent in first contact, red otherwise
      const chip = (host) => { let got = null; ctx.location.hostname = host; ctx.fill = (e, ...k) => { if (e.sel === '#chips') got = k; return e; };
        ctx.__st = states.nonet; vm.runInContext('S={sys:__st,at:Date.now(),key:""};renderChips()', ctx);
        return cls(find(got, (y) => /\bdot\b/.test(cls(y)))[0] || {}); };
      if (chip('192.168.4.1') !== 'dot acc' || chip('192.168.8.225') !== 'dot bad') errs.push(`Home: the chip dot ${chip('192.168.4.1')} / ${chip('192.168.8.225')}`);
      if (!has(states.docknodeck, /Plug in a Stream Deck/)) errs.push('Home docknodeck: NODECK is gone');
      const lk = Object.assign({}, states.choose, { companion: { state: 'connecting', host: '', port: 16622, auto: true, selection: 'none', instances: 0 } });
      if (!has(lk, /hasn't found Companion yet\. Companion 5\.0\+ announces itself/)) errs.push('Home looking: the looking text is gone');
      if (!/1 recent problem/.test(texts(view(states.satellite)['#probs']))) errs.push('Home: the recent problems are not folded under the chain');
      // first contact: the page over the hotspot (192.168.4.1) with no uplink
      const fr = view(states.nonet, '192.168.4.1');
      const frb = fr['#hbn'] || [];
      if (frb.length !== 1 || cls(frb[0]) !== 'bn acc setup' || !/Step 1: connect the box to the venue network/.test(texts(frb)))
        errs.push(`Home first contact: the banner ${frb.map(cls)} ${texts(frb)}`);
      const frBtns = find(frb, (x) => x.tag === 'button').map(texts);
      if (frBtns.join() !== 'Set up Wi-Fi,Open Ethernet') errs.push(`Home first contact: buttons ${frBtns}`);
      if (find(fr['#chain'], (x) => /\b(bad|warn)\b/.test(cls(x))).length) errs.push('Home first contact: red or amber in the chain');
      if (find(fr['#chain'], (x) => x.tag === 'button').length) errs.push('Home first contact: the network fix is there twice');
      // Uptime: on Device, not Home
      if (/Uptime|since last restart/.test(texts(view(states.satellite)['#chain']))) errs.push('Home: the uptime is still there');
    } catch (e) { errs.push(`Home: ${e && e.stack || e}`); }
    finally { ctx.h = hOrig; ctx.ic = icOrig; ctx.fill = fillOrig; ctx.document = docOrig; ctx.location.hostname = hostOrig; }
  }
  // The box menu per deck, on Home: each deck with a screen has its own control in its row, "Show box menu"
  // (closed) or a "Box menu" accent pill and "Close menu" (open), none for a deck without a screen (the Pedal); the banner
  // names the one deck by its model or counts several, and closes them all; opening asks first when Companion drives that
  // deck (by its model); the control keeps one focus key (data-k) in both forms; Device has the message only
  {
    const hOrig = ctx.h, icOrig = ctx.ic, fillOrig = ctx.fill, docOrig = ctx.document, rpcOrig = ctx.rpc, confOrig = ctx.confirmAction;
    const walk = (n, f) => { f(n); if (n && n.kids) for (const k of n.kids) walk(k, f); else if (Array.isArray(n)) for (const k of n) walk(k, f); };
    const texts = (n) => { const t = []; walk(n, (x) => { if (typeof x === 'string') t.push(x); }); return t.join('|'); };
    const find = (n, pred) => { const r = []; walk(n, (x) => { if (x && typeof x === 'object' && !Array.isArray(x) && pred(x)) r.push(x); }); return r; };
    const cls = (x) => (x.attrs || {}).class || '';
    const view = (st) => {
      const shown = {};
      ctx.document = { querySelector: (sel) => ({ sel, contains: () => false }), activeElement: null };
      ctx.fill = (e, ...kids) => { shown[e.sel] = kids.flat(Infinity).filter((k) => k != null && k !== false); return e; };
      ctx.__st = st;
      vm.runInContext('S={sys:__st,at:0,key:""};page="home";LIVE.home()', ctx);
      return shown;
    };
    // each deck's row: its model, its box menu cell's pill and buttons (text, class, focus key)
    const rows = (st) => find(view(st)['#decks'], (x) => x.tag === 'tr').map((tr) => {
      const c = find(tr, (x) => x.tag === 'td' && cls(x) === 'dmt')[0];
      return { model: texts(find(tr, (x) => cls(x) === 'dm')[0]).split('|')[0], cell: c,
        pill: c ? find(c, (x) => /\bpill\b/.test(cls(x))).map((x) => cls(x) + ':' + texts(x)).join() : '',
        btns: c ? find(c, (x) => x.tag === 'button').map((x) => `${texts(x)}/${cls(x)}/${x.attrs['data-k']}`).join() : '' };
    });
    const pedal = Object.assign({}, states.satellite, { decks: states.satellite.decks.concat(deck('Stream Deck Pedal', 'PED1', true, false, false)) });
    try {
      ctx.h = (tag, attrs, ...kids) => ({ tag, attrs: attrs || {}, kids: kids.flat(Infinity).filter((k) => k != null && k !== false) });
      ctx.ic = (name) => ({ icon: name });
      let r = rows(states.satellite);   // closed on both
      if (r.length !== 2 || r[0].btns !== 'Show box menu/ghost sm/mtCL1' || r[1].btns !== 'Show box menu/ghost sm/mtMINI1' || r[0].pill || r[1].pill)
        errs.push(`Home box menu, closed: ${JSON.stringify(r.map((x) => [x.model, x.pill, x.btns]))}`);
      r = rows(states.mtopen);   // open on the XL: its row marked, the same focus key; the Mini untouched
      if (r[0].pill !== 'pill acc:Box menu' || r[0].btns !== 'Close menu/ghost sm/mtCL1' || r[1].btns !== 'Show box menu/ghost sm/mtMINI1' || r[1].pill)
        errs.push(`Home box menu, open on one: ${JSON.stringify(r.map((x) => [x.model, x.pill, x.btns]))}`);
      r = rows(pedal);   // the Pedal: no control
      if (r.length !== 3 || r[2].model !== 'Stream Deck Pedal' || !r[2].cell || r[2].cell.kids.length || r[0].btns !== 'Show box menu/ghost sm/mtCL1')
        errs.push(`Home box menu, a deck without a screen: ${JSON.stringify(r.map((x) => [x.model, x.pill, x.btns]))}`);
      const lbl = find(view(states.mtopen)['#decks'], (x) => x.tag === 'button').map((x) => x.attrs['aria-label']).join();
      if (lbl !== 'Close the box menu on Stream Deck XL,Show the box menu on Stream Deck Mini') errs.push(`Home box menu: the buttons' names ${lbl}`);
      const banner = (st) => { const b = view(st)['#hbn'] || []; return `${b.map(cls)}:${texts(b)}:${find(b, (x) => x.tag === 'button').map((x) => x.attrs['data-k'])}`; };
      let bn = banner(states.mtopen);
      if (bn !== 'bn acc bw:Box menu open on Stream Deck XL. Companion gets the deck back when you close it.|Close menu:mtall') errs.push(`Home box menu banner, one deck: ${bn}`);
      bn = banner(Object.assign({}, states.mtopen, { decks: states.mtopen.decks.map((d) => Object.assign({}, d, { menu: true })) }));
      if (bn !== 'bn acc bw:Box menu open on 2 decks. Companion gets each deck back when you close its menu.|Close all:mtall') errs.push(`Home box menu banner, two decks: ${bn}`);
      if (/Box menu open/.test(banner(states.satellite))) errs.push('Home box menu banner with none open');
      const idx = fs.readFileSync(file, 'utf8') /* the copy being checked (stripped or source) */;
      // Device: the message on the decks, no box menu control (on Home; the gesture setting is Device's)
      const dev = String(vm.runInContext('PAGES.device', ctx));
      if (!/panel\('Message on the decks',\[h\('p'/.test(dev) || /show box menu|close menu|box menu open|mtToggle|mtbox|renderMt/i.test(dev) || /mtbox|renderMt|LIVE\.device/.test(idx))
        errs.push('Device: the box menu is still there, or no "Message on the decks" panel');
      if (!/keepFocus\(\$\('#hbn'\)/.test(idx) || !/keepFocus\(\$\('#chain'\)/.test(idx) || !/keepFocus\(\$\('#decks'\)/.test(idx)) errs.push('a live redraw does not keep the focus');
      // opening asks first when Companion drives that deck; closing, or no Companion, doesn't
      ctx.document = docOrig;
      const run = async (st, i, on, answer) => {
        let asked = null; const sent = [];
        ctx.confirmAction = async (o) => { asked = o; return answer; };
        ctx.rpc = async (m, p) => { sent.push(m + JSON.stringify(p || {})); return {}; };
        ctx.__st = st;
        await vm.runInContext(`S={sys:__st,at:0,key:""};mtToggle(${on}${i < 0 ? '' : `,S.sys.decks[${i}]`})`, ctx);
        return { asked, sent };
      };
      let t = await run(states.satellite, 1, true, false);
      if (!t.asked || t.asked.title !== 'Show the box menu on Stream Deck Mini?' || t.asked.ok !== 'Show the box menu' || !/Companion loses \|Stream Deck Mini/.test(texts(t.asked.body)) || t.sent.length)
        errs.push(`box menu with Companion connected: asked ${t.asked && t.asked.title}, sent ${t.sent}`);
      t = await run(states.satellite, 0, true, true);
      if (!t.sent.includes('Deck.ShowMaintenance{"on":true,"serial":"CL1"}')) errs.push(`box menu confirmed: sent ${t.sent}`);
      t = await run(states.choose, 0, true, false);   // Companion not connected: no question
      if (t.asked || !t.sent.includes('Deck.ShowMaintenance{"on":true,"serial":"CL1"}')) errs.push(`box menu without Companion: asked ${!!t.asked}, sent ${t.sent}`);
      t = await run(states.dock, 0, true, false);   // dock mode: CL1's dock has Companion, the Mini's deck no dock
      if (!t.asked || t.asked.title !== 'Show the box menu on Stream Deck XL?' || t.sent.length) errs.push(`box menu, dock mode, CL1: asked ${!!t.asked}, sent ${t.sent}`);
      t = await run(states.dock, 1, true, false);
      if (t.asked || !t.sent.includes('Deck.ShowMaintenance{"on":true,"serial":"MINI1"}')) errs.push(`box menu, dock mode, MINI1: asked ${!!t.asked}, sent ${t.sent}`);
      t = await run(states.mtopen, 0, false, false);   // closing a deck: no question
      if (t.asked || !t.sent.includes('Deck.ShowMaintenance{"on":false,"serial":"CL1"}')) errs.push(`box menu closing: asked ${!!t.asked}, sent ${t.sent}`);
      t = await run(states.mtopen, -1, false, false);   // Close all (the banner): no deck named
      if (t.asked || !t.sent.includes('Deck.ShowMaintenance{"on":false}')) errs.push(`box menu, close all: asked ${!!t.asked}, sent ${t.sent}`);
    } catch (e) { errs.push(`box menu: ${e && e.stack || e}`); }
    finally { ctx.h = hOrig; ctx.ic = icOrig; ctx.fill = fillOrig; ctx.document = docOrig; ctx.rpc = rpcOrig; ctx.confirmAction = confOrig; }
  }
  // the deck limit on Device: at 2 the note and "Allow up to 4 decks…" (the warning dialog, its words and buttons, then
  // Device.SetConfig {deckLimit:4} and the restart flow; "Keep 2" sends nothing); at 4 the note and "Back to 2 decks…"
  // (a plain confirm that it restarts, then {deckLimit:2})
  {
    const hOrig = ctx.h, icOrig = ctx.ic, fillOrig = ctx.fill, rpcOrig = ctx.rpc, confOrig = ctx.confirmAction, rsOrig = ctx.restartFlow;
    const walk = (n, f) => { f(n); if (n && n.kids) for (const k of n.kids) walk(k, f); else if (Array.isArray(n)) for (const k of n) walk(k, f); };
    const texts = (n) => { const t = []; walk(n, (x) => { if (typeof x === 'string') t.push(x); }); return t.join('|'); };
    const find = (n, pred) => { const r = []; walk(n, (x) => { if (x && typeof x === 'object' && !Array.isArray(x) && pred(x)) r.push(x); }); return r; };
    try {
      ctx.h = (tag, attrs, ...kids) => ({ tag, attrs: attrs || {}, kids: kids.flat(Infinity).filter((k) => k != null && k !== false) });
      ctx.ic = (name) => ({ icon: name });
      const panelAt = async (lim) => {
        let shown = [];
        ctx.fill = (e, ...kids) => { shown.push(kids); return e; };
        ctx.rpc = async (m) => m === 'Device.GetConfig' ? { boxName: 'Box', deckLimit: lim } : m === 'Auth.GetStatus' ? { auth_en: false, authed: true } : {};
        await vm.runInContext('(async () => { page = "device"; PG = {}; await PAGES.device(PG); })()', ctx);
        return find(shown, (x) => x.tag === 'section' && /Stream Decks/.test(texts(find(x, (y) => y.tag === 'h2')[0] || '')))[0];
      };
      let pn = await panelAt(2);
      if (!pn || texts(find(pn, (x) => x.tag === 'p')[0]) !== "Up to 2 Stream Decks: the box's power over Ethernet is sized for 2."
          || find(pn, (x) => x.tag === 'button').map(texts).join() !== 'Allow up to 4 decks…')
        errs.push(`Device deck limit at 2: ${pn && texts(pn)}`);
      pn = await panelAt(4);
      if (!pn || texts(find(pn, (x) => x.tag === 'p')[0]) !== 'Up to 4 Stream Decks (at your own risk: power for 2).'
          || find(pn, (x) => x.tag === 'button').map(texts).join() !== 'Back to 2 decks…')
        errs.push(`Device deck limit at 4: ${pn && texts(pn)}`);
      const run = async (n, answer, restarting) => {
        let asked = null, flow = null; const sent = [];
        ctx.confirmAction = async (o) => { asked = o; return answer; };
        ctx.rpc = async (m, p) => { sent.push(m + JSON.stringify(p || {})); return { restart_required: true, restarting }; };
        ctx.restartFlow = async (a, msg) => { flow = msg; return true; };
        await vm.runInContext(`setDecks(${n})`, ctx);
        return { asked, sent, flow };
      };
      let t = await run(4, false, true);
      const paras = t.asked ? find(t.asked.body, (x) => x.tag === 'p').map((x) => texts(x).replace(/\|/g, '')) : [];
      if (!t.asked || t.asked.title !== 'Allow up to 4 Stream Decks?' || t.asked.ok !== 'Allow 4 and restart' || t.asked.no !== 'Keep 2'
          || !/danger|warn/.test(t.asked.kind || 'warn') || paras.join(' ') !== 'The box powers its USB port from Power over Ethernet, which is sized for 2 Stream Decks. '
            + 'With more, the decks can drop out or the box can restart. A powered USB hub gives the extra decks their own power. You enable this at your own risk. The box restarts.'
          || t.sent.length)
        errs.push(`deck limit 4: asked ${t.asked && JSON.stringify([t.asked.title, t.asked.ok, t.asked.no, t.asked.kind, paras])}, sent ${t.sent}`);
      t = await run(4, true, true);
      if (t.sent.join() !== 'Device.SetConfig{"deckLimit":4}' || !t.flow) errs.push(`deck limit 4 confirmed: sent ${t.sent}, restart ${t.flow}`);
      t = await run(2, true, true);
      if (!t.asked || t.asked.title !== 'Back to 2 Stream Decks?' || !/restart/.test(t.asked.ok) || t.asked.no || !/The box restarts/.test(texts(t.asked.body))
          || t.sent.join() !== 'Device.SetConfig{"deckLimit":2}' || !t.flow)
        errs.push(`deck limit back to 2: asked ${t.asked && t.asked.title}, sent ${t.sent}, restart ${t.flow}`);
      const idx = fs.readFileSync(file, 'utf8') /* the copy being checked (stripped or source) */;
      if (!/const no=btn\(nt,/.test(idx)) errs.push('the confirm dialog cannot name its cancel button');
    } catch (e) { errs.push(`deck limit: ${e && e.stack || e}`); }
    finally { ctx.h = hOrig; ctx.ic = icOrig; ctx.fill = fillOrig; ctx.rpc = rpcOrig; ctx.confirmAction = confOrig; ctx.restartFlow = rsOrig; }
  }
  // the Wi-Fi scan list: h() and ic() are swapped for recorders, so the tree the page builds can be read
  const scanView = vm.runInContext('typeof scanView === "function" ? scanView : null', ctx);
  const pickNet = vm.runInContext('typeof pickNet === "function" ? pickNet : null', ctx);
  const scanOutcome = vm.runInContext('typeof scanOutcome === "function" ? scanOutcome : null', ctx);
  if (!scanView || !pickNet || !scanOutcome) errs.push('scanView(state) / pickNet(...) / scanOutcome(answer) are missing');
  else {
    const hOrig = ctx.h, icOrig = ctx.ic;
    ctx.h = (tag, attrs, ...kids) => ({ tag, attrs: attrs || {}, kids: kids.flat(Infinity).filter((k) => k != null && k !== false) });
    ctx.ic = (name) => ({ icon: name });
    const walk = (n, f) => { f(n); if (n && n.kids) for (const k of n.kids) walk(k, f); };
    const texts = (n) => { const t = []; walk(n, (x) => { if (typeof x === 'string') t.push(x); }); return t.join('|'); };
    const find = (n, pred) => { const r = []; walk(n, (x) => { if (x && typeof x === 'object' && pred(x)) r.push(x); }); return r; };
    try {
      const evil = '<img src=x onerror=alert(1)>';
      const nets = [{ ssid: 'Studio', rssi: -48, auth: 'wpa2_psk', channel: 6 }, { ssid: 'Cafe', rssi: -71, auth: 'open', channel: 11 },
        { ssid: evil, rssi: -90, auth: 'wpa3_psk', channel: 1 }];
      const list = scanView({ results: nets });
      const rows = find(list, (x) => x.tag === 'button');
      if (rows.length !== 3) errs.push(`scan list: ${rows.length} rows for 3 networks`);
      const t = texts(list);
      for (const n of nets) if (!t.includes(n.ssid)) errs.push(`scan list: ${n.ssid} is not shown`);
      for (const d of ['-48 dBm', '-71 dBm', '-90 dBm']) if (!t.includes(d)) errs.push(`scan list: ${d} is not shown`);
      const locks = find(list, (x) => x.icon === 'lock').length;
      if (locks !== 2) errs.push(`scan list: ${locks} lock icons, the 2 protected networks need one each`);
      if (find(rows[1], (x) => x.icon === 'lock').length) errs.push('scan list: the open network shows a lock');
      const bars = rows.map((r) => find(r, (x) => /\bbars\b/.test((x.attrs || {}).class || ''))[0].attrs.class);
      if (!(bars[0].endsWith('b4') && bars[1].endsWith('b2') && bars[2].endsWith('b1'))) errs.push(`scan list: signal bars ${bars}`);
      if (find(list, (x) => /^(img|script)$/.test(x.tag)).length) errs.push('scan list: a network name became markup');
      if (!rows.every((r) => typeof r.attrs.onclick === 'function')) errs.push('scan list: a row does not react to a click');
      const none = scanView({ results: [] });
      if (!/No networks found/.test(texts(none))) errs.push(`scan list, empty: ${texts(none)}`);
      if (find(none, (x) => x.tag === 'button').length) errs.push('scan list, empty: has rows');
      const bad = scanView({ error: 'no Wi-Fi: the C6 does not answer' });
      if (!/Scan failed: no Wi-Fi: the C6 does not answer/.test(texts(bad))) errs.push(`scan list, error: ${texts(bad)}`);
      const busy = scanView({ busy: true });
      if (!find(busy, (x) => /\bspin\b/.test((x.attrs || {}).class || '')).length) errs.push('scan list, busy: no spinner');
      // what Wifi.Scan answers become: still scanning after the polls is a failure, never an empty list
      const o1 = scanOutcome({ scanning: true });
      if (!o1.error || o1.results) errs.push('scanOutcome(scanning) should be an error');
      const o2 = scanOutcome({ scanning: false, results: nets, ageMs: 10 });
      if (o2.error || o2.results.length !== 3) errs.push('scanOutcome(results) should be the list');
      const o3 = scanOutcome({ scanning: false, results: [], ageMs: 10 });
      if (o3.error || !Array.isArray(o3.results) || o3.results.length) errs.push('scanOutcome(empty) should be an empty list');
      const o4 = scanOutcome({ scanning: false, results: [], ageMs: -1, error: 'the scan did not finish' });
      if (o4.error !== 'the scan did not finish') errs.push('scanOutcome(error) should carry the box\'s reason');
      // scanWifi: start, poll the answer every 500 ms for up to 10 s (a fake timer, scripted answers)
      const scanWifi = vm.runInContext('scanWifi', ctx);
      const saved = { rpc: ctx.rpc, fill: ctx.fill, setTimeout: ctx.setTimeout };
      const drive = async (answers) => {
        const calls = [], shown = [];
        let i = 0, waited = 0;
        ctx.setTimeout = (f, ms) => { waited += ms; f(); return 0; };
        ctx.fill = (e, ...k) => { shown.push(k.flat(Infinity)[0]); return e; };
        ctx.rpc = async (m, p) => { calls.push(m + (p ? JSON.stringify(p) : '')); const a = answers[Math.min(i++, answers.length - 1)];
          if (a instanceof Error) throw a; if (a === 'leave') { vm.runInContext('PG={}', ctx); return { scanning: true }; } return a; };
        await scanWifi();
        return { calls, shown, waited };
      };
      let d = await drive([{ scanning: true }, { scanning: true, results: [] }, { scanning: true, results: [] }, { scanning: false, results: nets, ageMs: 5 }]);
      if (d.calls.join() !== 'Wifi.Scan,Wifi.Scan{"results":true},Wifi.Scan{"results":true},Wifi.Scan{"results":true}') errs.push(`scanWifi calls: ${d.calls}`);
      if (d.waited !== 1500) errs.push(`scanWifi waited ${d.waited} ms for 3 polls`);
      if (d.shown.length !== 2 || !find(d.shown[0], (x) => /\bspin\b/.test((x.attrs || {}).class || '')).length || find(d.shown[1], (x) => x.tag === 'button').length !== 3)
        errs.push('scanWifi should show the spinner, then the three networks');
      d = await drive([{ scanning: true, results: [] }]); // never finishes
      if (d.calls.length !== 21 || d.waited !== 10000) errs.push(`scanWifi gave up after ${d.calls.length} calls / ${d.waited} ms, not 1 + 20 polls / 10 s`);
      if (!/did not finish/.test(texts(d.shown[d.shown.length - 1]))) errs.push('scanWifi should say the box did not finish');
      d = await drive([new Error('no Wi-Fi: the C6 does not answer')]);
      if (d.shown.length !== 2 || !/Scan failed: no Wi-Fi/i.test(texts(d.shown[1]))) errs.push('scanWifi should show the error of a failed start');
      d = await drive([{ scanning: true }, new Error('no answer from the box')]);
      if (!/Scan failed: no answer/i.test(texts(d.shown[d.shown.length - 1]))) errs.push('scanWifi should show the error of a failed poll');
      d = await drive([{ scanning: true }, 'leave']); // the page changed meanwhile: nothing is drawn over the new one
      if (d.shown.length !== 1) errs.push(`scanWifi drew ${d.shown.length - 1} times after the page changed`);
      Object.assign(ctx, saved);
      // picking a network: the name goes into the field and the password gets the focus
      const el = (v) => ({ value: v, focused: false, focus() { this.focused = true; } });
      let ssid = el(''), pass = el('old secret');
      let hint = pickNet(nets[0], ssid, pass, true);
      if (ssid.value !== 'Studio' || !pass.focused || pass.value !== 'old secret' || hint) errs.push('pickNet(protected) should set the name and focus the password');
      ssid = el(''); pass = el('old secret');
      hint = pickNet(nets[1], ssid, pass, true);
      if (ssid.value !== 'Cafe' || !pass.focused || pass.value !== '') errs.push('pickNet(open) should set the name, empty the password and focus it');
      if (!/Forget password/.test(hint)) errs.push('pickNet(open) with a saved password should point at "Forget password"');
      if (pickNet(nets[1], el(''), el(''), false)) errs.push('pickNet(open) without a saved password needs no hint');
    } catch (e) { errs.push(`scan list: ${e && e.stack || e}`); }
    finally { ctx.h = hOrig; ctx.ic = icOrig; }
  }
  // the backup Wi-Fi network: its own name and password fields, its own Scan, a line for the network in use
  {
    const hOrig = ctx.h, icOrig = ctx.ic, fillOrig = ctx.fill, rpcOrig = ctx.rpc;
    ctx.h = (tag, attrs, ...kids) => ({ tag, attrs: attrs || {}, kids: kids.flat(Infinity).filter((k) => k != null && k !== false) });
    ctx.ic = (name) => ({ icon: name });
    const walk = (n, f) => { f(n); if (n && n.kids) for (const k of n.kids) walk(k, f); };
    const texts = (n) => { const t = []; walk(n, (x) => { if (typeof x === 'string') t.push(x); }); return t.join('|'); };
    const find = (n, pred) => { const r = []; walk(n, (x) => { if (x && typeof x === 'object' && pred(x)) r.push(x); }); return r; };
    try {
      const wifiPage = async (cfg) => {
        const shown = [];
        ctx.rpc = async () => cfg;
        ctx.fill = (e, ...k) => { shown.push(...k.flat(Infinity)); return e; };
        vm.runInContext('PG={}', ctx);
        await vm.runInContext('PAGES.wifi(PG)', ctx);
        return { kids: shown };
      };
      const withBackup = await wifiPage({ wifiSsid: 'Studio', wifiHasPass: true, wifiBackupSsid: 'Phone', wifiBackupHasPass: true });
      const ids = new Set(); walk(withBackup, (x) => { if (x && x.attrs && x.attrs.id) ids.add(x.attrs.id); });
      for (const id of ['ssid', 'wpass', 'wfscan', 'bssid', 'bpass', 'wfscan2', 'wbst', 'wbpill', 'wfst', 'wfpill', 'wmst', 'wmpill']) if (!ids.has(id)) errs.push(`Wi-Fi page: no #${id}`);
      // three cards in order, each with its own fields and Save; Wi-Fi off dims both settings cards
      const cards = (pg) => pg.kids.filter((x) => x && x.tag === 'section');
      const title = (card) => texts(find(card, (x) => x.tag === 'h2')[0]);
      const idsIn = (card) => { const r = new Set(); walk(card, (x) => { if (x && x.attrs && x.attrs.id) r.add(x.attrs.id); }); return r; };
      const saves = (card) => find(card, (x) => x.tag === 'div' && /\bsv\b/.test(x.attrs.class || '')).length;
      const cs = cards(withBackup);
      if (cs.map(title).join() !== 'Wi-Fi,Primary network,Backup network') errs.push(`the Wi-Fi cards are ${cs.map(title)}`);
      else {
        const want = [['wfst', 'wfpill'], ['ssid', 'wpass', 'wfscan', 'wmst', 'wmpill'], ['bssid', 'bpass', 'wfscan2', 'wbst', 'wbpill']];
        cs.forEach((card, i) => { const got = idsIn(card); for (const id of want[i]) if (!got.has(id)) errs.push(`#${id} is not in the ${title(card)} card`); });
        if (find(cs[0], (x) => x.tag === 'input').length || saves(cs[0])) errs.push('the Wi-Fi card has fields or a Save');
        if (!/Turn Wi-Fi off/.test(texts(cs[0])) || !/For places without a network cable/.test(texts(cs[0]))) errs.push('the Wi-Fi card lacks its note or the Turn Wi-Fi button');
        if (saves(cs[1]) !== 1 || saves(cs[2]) !== 1) errs.push('a settings card without its own Save row');
        if (find(cs[1], (x) => x.tag === 'button' && /Scan for networks/.test(texts(x))).length !== 1) errs.push('the Primary card has no Scan');
      }
      const offPage = cards(await wifiPage({ wifiSsid: 'Studio', wifiHasPass: true, wifiBackupSsid: 'Phone', wifiBackupHasPass: true, wifiEnabled: false }));
      const dim = (card) => find(card, (x) => x.tag === 'div' && x.attrs.class === 'off' && find(x, (y) => y.tag === 'input').length).length;
      if (offPage.length !== 3 || dim(offPage[0]) || !dim(offPage[1]) || !dim(offPage[2]) || !/Turn Wi-Fi on/.test(texts(offPage[0])))
        errs.push('Wi-Fi off does not dim exactly the two settings cards');
      const scans = find(withBackup, (x) => x.tag === 'button' && /Scan for networks/.test(texts(x)));
      if (scans.length !== 2) errs.push(`Wi-Fi page: ${scans.length} Scan buttons, one for each network`);
      const bpass = find(withBackup, (x) => x.attrs && x.attrs.id === 'bpass')[0];
      if (!bpass || bpass.attrs.type !== 'password' || bpass.attrs.maxlength !== 63 || !/keep the saved one/.test(bpass.attrs.placeholder || ''))
        errs.push('Wi-Fi page: the backup password field (password, 63 characters, saved one kept)');
      const without = await wifiPage({ wifiSsid: 'Studio', wifiHasPass: false, wifiBackupSsid: '', wifiBackupHasPass: false });
      const nb = find(without, (x) => x.attrs && x.attrs.id === 'bpass')[0];
      if (!nb || /keep the saved one/.test(nb.attrs.placeholder || '')) errs.push('Wi-Fi page: no saved backup password, yet the field says it is kept');
      // the live status: the Wi-Fi card's pill and list, each network card's pill and password line
      const live = (net, cfg, c6) => {
        const shown = [];
        ctx.fill = (e, ...k) => { shown.push(k.flat(Infinity)[0]); return e; };
        ctx.__t = { net, cfg, c6 };
        vm.runInContext('S={sys:{net:__t.net,c6:__t.c6},at:0,key:""};PG={cfg:__t.cfg};LIVE.wifi()', ctx);
        return { pill: texts(shown[0]), st: texts(shown[1]), mpill: texts(shown[2]), mst: texts(shown[3]), bpill: texts(shown[4]), bst: texts(shown[5]) };
      };
      const evil = '<img src=x onerror=alert(1)>';
      const cfg = { wifiSsid: 'Studio', wifiHasPass: true, wifiBackupSsid: 'Phone', wifiBackupHasPass: true };
      const up = { wifi_connected: true, route: 'wifi', ip: '10.0.0.5', sta_addr: '10.0.0.5', sta_net: 'main', sta_ssid: 'Studio', sta_rssi: -50 };
      const down = { wifi_connected: false, route: 'none', ip: '', sta_addr: '', sta_net: 'none', sta_ssid: '', sta_rssi: 0 };
      let r = live(up, cfg, { running: '3.0.9' });
      if (r.pill !== 'Connected' || r.st !== 'Network|Studio| |Primary|Address|10.0.0.5|Signal|-50 dBm|Wi-Fi chip|ESP-Hosted 3.0.9' || r.mpill !== 'In use'
        || r.mst !== 'Password|set' || r.bpill !== 'Standby') errs.push(`Wi-Fi status, primary: ${JSON.stringify(r)}`);
      r = live(Object.assign({}, up, { sta_net: 'backup', sta_ssid: evil, sta_rssi: -70 }), cfg, { running: 'unknown' });
      if (!/^Network\|<img src=x onerror=alert\(1\)>\| \|Backup\|/.test(r.st) || /Wi-Fi chip/.test(r.st) || r.bpill !== 'In use' || r.mpill !== 'Standby' || r.pill !== 'Connected')
        errs.push(`Wi-Fi status, backup: ${JSON.stringify(r)}`);
      for (const [what, net, c, want] of [['connecting', down, cfg, 'Connecting'], ['not found', Object.assign({}, down, { sta_not_found: true }), cfg, 'Not found'],
        ['radio down', Object.assign({}, down, { radio_down: true, sta_not_found: true }), cfg, 'Radio down'],
        ['off', down, Object.assign({}, cfg, { wifiEnabled: false }), 'Off'], ['not set up', down, Object.assign({}, cfg, { wifiSsid: '', wifiHasPass: false }), 'Not set up']]) {
        r = live(net, c);
        if (r.pill !== want) errs.push(`Wi-Fi ${what}: the pill says ${r.pill}, not ${want}`);
      }
      r = live(down, cfg);
      if (r.st !== 'Network|None' || r.mpill !== 'Standby' || r.bpill !== 'Standby') errs.push(`Wi-Fi status, none: ${JSON.stringify(r)}`);
      r = live(down, Object.assign({}, cfg, { wifiSsid: '', wifiHasPass: false }));
      if (r.mpill !== 'Not set' || r.mst !== '' || r.st !== '') errs.push(`Wi-Fi status, no primary: ${JSON.stringify(r)}`);
      r = live(up, Object.assign({}, cfg, { wifiBackupSsid: '', wifiBackupHasPass: false }));
      if (r.bpill !== 'Not set up' || r.bst !== '') errs.push(`Wi-Fi status, no backup: ${JSON.stringify(r)}`);
      // "Forget password" of the backup clears only the backup password
      const calls = [];
      const saved = { confirmAction: ctx.confirmAction, call: ctx.call };
      ctx.confirmAction = async () => true;
      ctx.call = async (m, p) => { calls.push(m + JSON.stringify(p)); };
      ctx.fill = fillOrig; ctx.rpc = async () => cfg;
      await vm.runInContext("forgetWifiPass('backup')", ctx);
      await vm.runInContext("forgetWifiPass('main')", ctx);
      Object.assign(ctx, saved);
      if (calls.join() !== 'Wifi.SetConfig{"wifiBackupPass":""},Wifi.SetConfig{"wifiPass":""}') errs.push(`forgetWifiPass: ${calls}`);
      // the scan list of the backup pair picks into the backup fields
      const wf = vm.runInContext('WF', ctx);
      if (wf.backup.ssid !== 'bssid' || wf.backup.pass !== 'bpass' || wf.backup.out !== 'wfscan2' || wf.backup.has !== 'wifiBackupHasPass') errs.push(`WF.backup: ${JSON.stringify(wf.backup)}`);
    } catch (e) { errs.push(`backup Wi-Fi: ${e && e.stack || e}`); }
    finally { ctx.h = hOrig; ctx.ic = icOrig; ctx.fill = fillOrig; ctx.rpc = rpcOrig; }
  }
  // the OTA self-check's countdown on the Firmware page: the next check, the one that goes back, why
  {
    const pvText = vm.runInContext('typeof pvText === "function" ? pvText : null', ctx);
    const clock = vm.runInContext('typeof clock === "function" ? clock : null', ctx);
    if (!pvText || !clock) errs.push('pvText(fw, up) or clock(s) is missing');
    else {
      const fw = (o) => Object.assign({ pending_verify: true, check_at_s: 34, rollback_at_s: 304, check_failed: '' }, o);
      const cases = [ // [what, fw, the box's uptime, texts it must have, texts it must not]
        ['no schedule yet', { pending_verify: true }, 10, ['about 30 seconds', 'Uploading works'], ['undefined', 'NaN']],
        ['before the first check', fw({}), 16, ['checks itself in 0:18', 'restart asked for now waits'], ['NaN', 'goes back']],
        ['the check running', fw({}), 34.4, ['checking itself now'], ['NaN', 'in 0:']],
        ['a check failed', fw({ check_at_s: 64, check_failed: 'no network route and no hotspot' }), 40,
          ['found: no network route and no hotspot', 'Next check in 0:24', 'still fail in 4:24', 'previous firmware'], ['NaN', 'is the last']],
        ['the recheck due', fw({ check_at_s: 64, check_failed: 'x' }), 70, ['Next check now', 'in 3:54'], ['NaN']],
        ['the last check next', fw({ check_at_s: 304, check_failed: 'the USB host is not running' }), 290,
          ['the USB host is not running', 'next check (in 0:14) is the last', 'previous firmware'], ['NaN', 'still fail']],
      ];
      for (const [what, f, up, want, not] of cases) {
        const t = pvText(f, up);
        for (const w of want) if (!t.includes(w)) errs.push(`self-check countdown (${what}): no "${w}" in: ${t}`);
        for (const w of not) if (t.includes(w)) errs.push(`self-check countdown (${what}): "${w}" in: ${t}`);
      }
      for (const [sec, want] of [[0, '0:00'], [-3, '0:00'], [9.2, '0:10'], [65, '1:05'], [300, '5:00']])
        if (clock(sec) !== want) errs.push(`clock(${sec}) is ${clock(sec)}, not ${want}`);
    }
  }
  // the Firmware page's slots: every sync state, Sync now and Boot other slot. h() and ic() are recorders.
  {
    const slotsView = vm.runInContext('typeof slotsView === "function" ? slotsView : null', ctx);
    if (!slotsView) errs.push('slotsView(ota) is missing');
    else {
      const saved = { h: ctx.h, ic: ctx.ic, rpc: ctx.rpc, confirmAction: ctx.confirmAction, toast: ctx.toast, restartFlow: ctx.restartFlow };
      ctx.h = (tag, attrs, ...kids) => ({ tag, attrs: attrs || {}, kids: kids.flat(Infinity).filter((k) => k != null && k !== false) });
      ctx.ic = (name) => ({ icon: name });
      const walk = (n, f) => { f(n); if (n && n.kids) for (const k of n.kids) walk(k, f); else if (Array.isArray(n)) for (const k of n) walk(k, f); };
      const texts = (n) => { const t = []; walk(n, (x) => { if (typeof x === 'string') t.push(x); }); return t.join('|'); };
      const find = (n, pred) => { const r = []; walk(n, (x) => { if (x && typeof x === 'object' && !Array.isArray(x) && pred(x)) r.push(x); }); return r; };
      const button = (n, label) => find(n, (x) => x.tag === 'button' && texts(x).includes(label))[0];
      const sSaved = vm.runInContext('S', ctx);
      vm.runInContext('S={sys:{firmware:{project:"companion_box",build:"Nov  1 2026 14:02:11"}},at:0,key:""}', ctx);
      try {
        // [what, Ota.GetStatus answer, pill, Boot other slot enabled]. Boot other only for a valid image of this project, not
        // while pending verify
        const lim = { project: 'other_reader', version: '0.3.0' };
        const cases = [['in_sync', otaAns('in_sync'), 'In sync', true], ['syncing', otaAns('syncing'), 'Syncing', false],
          ['differs', otaAns('differs'), 'Differs', true], ['other_newer', otaAns('other_newer'), 'Other slot newer', true],
          ['other_newer, pending verify', otaAns('other_newer', {}, { pending_verify: true }), 'Other slot newer', false],
          ['empty', otaAns('empty'), 'Empty', false], ['invalid', otaAns('invalid'), 'Invalid', false],
          ['invalid, no version', otaAns('invalid', { version: '' }), 'Invalid', false], ['waiting', otaAns('waiting'), 'Waiting', false],
          ['waiting, valid', otaAns('waiting', { version: 'v0.9', valid: true }), 'Waiting', true],
          ['waiting, readable but not valid', otaAns('waiting', { version: 'v0.9' }), 'Waiting', false],
          ['another project', otaAns('invalid', lim), 'Other project', false],
          ['another project, flagged by the box', otaAns('invalid', Object.assign({ foreign: true }, lim)), 'Other project', false],
          ['no app descriptor', otaAns('invalid', { project: '', version: '', foreign: true }), 'Unknown firmware', false],
          ['another project, even if called valid', otaAns('other_newer', Object.assign({ valid: true }, lim)), 'Other project', false]];
        for (const [name, o, label, boot] of cases) {
          const state = o.sync.state, v = slotsView(o), t = texts(v), what = `slots (${name})`;
          if (!t.includes(label)) errs.push(`${what}: no "${label}"`);
          if (!t.includes('ota_0 (running)') || !t.includes('Nov  1 2026 14:02:11')) errs.push(`${what}: the running slot is not marked`);
          const dl = find(v, (x) => x.tag === 'dl')[0], at = dl ? dl.kids.findIndex((k) => k.tag === 'dt' && texts(k) === 'ota_1') : -1;
          const row = at >= 0 ? texts(dl.kids[at + 1]) : '';
          if (label === 'Other project' && (!row.includes('other_reader') || !/another project/.test(t) || /Boot it|boot it/.test(t)))
            errs.push(`${what}: the foreign firmware is not named, or offered: ${t}`);
          if (label === 'Unknown firmware' && (!/app descriptor/.test(t) || /failed|went back/.test(t)))
            errs.push(`${what}: an image without an app descriptor reads as a failed update: ${t}`);
          if (!t.includes('ota_1')) errs.push(`${what}: the other slot is not named`);
          if (o.sync.other.version ? !t.includes(o.sync.other.version) || !t.includes('Oct 30 2026 18:25:00') : !t.includes('—'))
            errs.push(`${what}: the other slot's firmware is shown wrong: ${t}`);
          const sync = button(v, 'Sync now'), bootB = button(v, 'Boot other slot');
          if (!sync || !bootB) { errs.push(`${what}: Sync now or Boot other slot is missing`); continue; }
          if (!!sync.attrs.disabled !== (state === 'syncing')) errs.push(`${what}: Sync now ${sync.attrs.disabled ? 'disabled' : 'enabled'}`);
          if (!!bootB.attrs.disabled === boot) errs.push(`${what}: Boot other slot ${bootB.attrs.disabled ? 'disabled' : 'enabled'}`);
          if (typeof sync.attrs.onclick !== 'function' || typeof bootB.attrs.onclick !== 'function') errs.push(`${what}: a button does not react`);
          const bars = find(v, (x) => x.attrs && x.attrs.class === 'bar');
          if (state === 'syncing' ? bars.length !== 1 || !t.includes('42 %') || find(bars[0], (x) => x.tag === 'i')[0].attrs.style !== 'width:42%' : bars.length)
            errs.push(`${what}: the progress bar is wrong`);
          if (!find(v, (x) => x.tag === 'p' && texts(x).length > 20).length) errs.push(`${what}: no note says what it means`);
        }
        const evil = '<img src=x onerror=alert(1)>', v = slotsView(otaAns('invalid', { version: evil, project: evil }));
        if (!texts(v).includes(evil) || find(v, (x) => /^(img|script)$/.test(x.tag)).length) errs.push('slots: a version or project became markup');
        // the live view: when pending verify ends (sync unchanged), the next poll's render enables Boot other
        const drawn = [], fillOrig = ctx.fill;
        ctx.fill = (e, ...kids) => (drawn.push(kids), e);
        ctx.__a = otaAns('waiting', { version: 'v0.9', valid: true }, { pending_verify: true });
        ctx.__b = otaAns('waiting', { version: 'v0.9', valid: true }, { pending_verify: false });
        vm.runInContext('PG={ota:__a};renderSlots();PG.ota=__b;renderSlots();renderSlots()', ctx);
        ctx.fill = fillOrig;
        const bootIn = (k) => button(k, 'Boot other slot');
        if (drawn.length !== 2) errs.push(`renderSlots: ${drawn.length} renders for pending -> confirmed -> unchanged, not 2`);
        else if (!bootIn(drawn[0]).attrs.disabled || bootIn(drawn[1]).attrs.disabled)
          errs.push('renderSlots: Boot other is not enabled once pending verify ends');
        // Sync now: over a newer image only after a confirmation, then with force; Boot other slot: a confirmation that says it restarts
        const calls = [], asked = [];
        ctx.confirmAction = async (d) => (asked.push(d), true);
        ctx.toast = () => {};
        ctx.restartFlow = async (act) => act();
        ctx.rpc = async (m, p) => (calls.push(m + JSON.stringify(p || {})), m === 'Ota.GetStatus' ? otaAns('syncing') : {});
        const press = async (fn, state) => { ctx.__ota = otaAns(state); vm.runInContext('PG={ota:__ota}', ctx); await vm.runInContext(fn, ctx)(); };
        await press('syncNow', 'other_newer');
        if (asked.length !== 1 || calls[0] !== 'Ota.Sync{"force":true}') errs.push(`Sync now over a newer image: ${asked.length} confirmations, ${calls}`);
        await press('syncNow', 'differs');
        if (asked.length !== 1 || !calls.includes('Ota.Sync{}')) errs.push(`Sync now: ${asked.length} confirmations, ${calls}`);
        calls.length = 0;
        await press('bootOther', 'other_newer');
        if (asked.length !== 2 || !/restarts/.test(texts(asked[1].body)) || calls[0] !== 'Ota.BootOther{}')
          errs.push(`Boot other slot: ${asked.length - 1} confirmations (${asked[1] ? texts(asked[1].body) : ''}), ${calls}`);
      } catch (e) { errs.push(`slots: ${e && e.stack || e}`); }
      finally { Object.assign(ctx, saved); ctx.__s = sSaved; vm.runInContext('S=__s', ctx); }
    }
  }
  if (errs.length) { for (const e of errs) console.error(`${path.relative(process.cwd(), file)}: smoke test, ${e}`); process.exit(1); }
  console.log(`pages: index.html renders every page in ${n} box states`);
}
