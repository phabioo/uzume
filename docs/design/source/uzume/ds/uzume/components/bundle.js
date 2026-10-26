/* @ds-bundle: {"format":4,"namespace":"Uzume","components":[{"name":"AppNav"},{"name":"Toggle"},{"name":"Select"},{"name":"Menu"},{"name":"SettingsRow"},{"name":"GamepadHint"},{"name":"Checkbox"},{"name":"Tag"},{"name":"Kbd"},{"name":"HelpColumn"},{"name":"Slider"},{"name":"Popover"},{"name":"Button"},{"name":"TextField"},{"name":"SearchField"},{"name":"SystemChip"},{"name":"FilterChip"},{"name":"Tabs"},{"name":"RunTabs"},{"name":"Segmented"},{"name":"Progress"},{"name":"TableRow"},{"name":"Toast"},{"name":"Dialog"},{"name":"StatusIcon"},{"name":"StatusLine"},{"name":"SectionLabel"},{"name":"PageHeader"},{"name":"OpenColumn"},{"name":"RegisterMarks"},{"name":"GameCover"},{"name":"MultiviewTile"},{"name":"Enso"},{"name":"StateBlock"},{"name":"Avatar"},{"name":"RunHeader"},{"name":"GameStage"},{"name":"RunnerRow"},{"name":"ChatMessage"},{"name":"Mark"},{"name":"Lockup"},{"name":"AppIcon"},{"name":"ServerGlyph"},{"name":"HintRow"},{"name":"ColumnNav"}]} */
(function () {
  var React = window.React, h = React.createElement;
  function cx() { var o = []; for (var i = 0; i < arguments.length; i++) if (arguments[i]) o.push(arguments[i]); return o.join(' '); }
  function rest(p, keys) { var o = {}; for (var k in p) if (keys.indexOf(k) < 0) o[k] = p[k]; return o; }
  var PEOPLE = ['wasurenagusa', 'momo', 'moegi', 'fuji'];
  function personColor(p) { if (typeof p === 'number') p = PEOPLE[p % 4]; return 'var(--person-' + (PEOPLE.indexOf(p) >= 0 ? p : 'wasurenagusa') + ')'; }
  function stateClass(s) { return s && s !== 'default' ? 'is-' + s : null; }

  /* ---------- Brand ---------- */
  var INK = 'M14 42 L30 38 L30 56 A18 18 0 0 0 66 56 L66 16 L82 10 L82 56 A34 34 0 0 1 14 56 Z';
  function Mark(p) {
    var s = p.size || 20, mono = !!p.mono;
    return h('svg', { viewBox: '0 0 96 96', width: s, height: s, role: 'img', 'aria-label': p.title || 'Uzume', style: { display: 'block', flex: 'none' } },
      h('path', { d: INK, className: mono ? null : 'uz-mark-ink', fill: mono ? 'currentColor' : undefined }),
      h('circle', { cx: 48, cy: 30, r: 9, className: mono ? null : 'uz-mark-light', fill: mono ? 'currentColor' : undefined }));
  }
  function Lockup(p) {
    var display = p.variant === 'display';
    return h('span', { className: cx('uz uz-lockup', display && 'display') },
      h(Mark, { size: display ? 52 : 20 }),
      h('span', { className: 'uz-lockup-word' }, 'Uzume'),
      display && p.jp ? h('span', { className: 'uz-lockup-jp', lang: 'ja' }, 'ウズメ') : null);
  }
  var TONE = { sumi: { bg: '#1c1b19', fg: '#efece4' }, kinari: { bg: '#fbfaf5', fg: '#1c1b19' } };
  function glyphParts(x, y, bS, boxFill, lineFill, ring, ringFill) {
    var bR = bS * .24, uH = bS * .22, uB = bS * .06, uR = bS * .05, gap = bS * .09, pad = bS * .2, dS = bS * .09, dP = bS * .05;
    var top = y + (bS - (2 * uH + gap)) / 2, uw = bS - 2 * pad, out = [];
    if (ring) out.push(h('rect', { key: 'r', x: x - ring, y: y - ring, width: bS + 2 * ring, height: bS + 2 * ring, rx: bR + ring, fill: ringFill }));
    out.push(h('rect', { key: 'b', x: x, y: y, width: bS, height: bS, rx: bR, fill: boxFill }));
    for (var i = 0; i < 2; i++) {
      var uy = top + i * (uH + gap);
      out.push(h('rect', { key: 'u' + i, x: x + pad + uB / 2, y: uy + uB / 2, width: uw - uB, height: uH - uB, rx: Math.max(uR - uB / 2, 0), fill: 'none', stroke: lineFill, strokeWidth: uB }));
    }
    out.push(h('rect', { key: 'd', x: x + pad + uw - uB - dP - dS, y: top + (uH - dS) / 2, width: dS, height: dS, fill: '#f8b500' }));
    return out;
  }
  function AppIcon(p) {
    var s = p.size || 112, t = TONE[p.tone] || TONE.sumi, light = p.tone === 'kinari', ms = s * .66, server = !!p.server;
    var ext = server ? s * 1.03 : s, bw = s / 220;
    return h('svg', { viewBox: '0 0 ' + ext + ' ' + ext, width: ext, height: ext, role: 'img', 'aria-label': server ? 'Uzume server' : 'Uzume', style: { display: 'block', flex: 'none' } },
      h('rect', { width: s, height: s, rx: s * .225, fill: t.bg }),
      light ? h('rect', { x: bw / 2, y: bw / 2, width: s - bw, height: s - bw, rx: s * .225 - bw / 2, fill: 'none', stroke: '#e2dfd5', strokeWidth: bw }) : null,
      h('g', { transform: 'translate(' + (s - ms) / 2 + ' ' + (s - ms) / 2 + ') scale(' + ms / 96 + ')' }, h('path', { d: INK, fill: t.fg }), h('circle', { cx: 48, cy: 30, r: 9, fill: '#f8b500' })),
      server ? glyphParts(s * .6, s * .6, s * .4, light ? '#1c1b19' : '#efece4', light ? '#efece4' : '#1c1b19', s * .03, t.bg) : null);
  }
  function ServerGlyph(p) {
    var s = p.size || 24;
    return h('svg', { viewBox: '0 0 96 96', width: s, height: s, role: 'img', 'aria-label': p.title || 'Uzume server', style: { display: 'block', flex: 'none' } },
      glyphParts(0, 0, 96, 'var(--ink)', 'var(--ground)'));
  }

  /* ---------- Structure ---------- */
  function SectionLabel(p) {
    var num = p.number != null ? String(p.number).padStart(2, '0') + ' — ' : '';
    return h('div', { className: cx('uz uz-section', p.className) }, h('span', null, num, p.children), p.meta ? h('span', { className: 'uz-section-meta' }, p.meta) : null);
  }
  function PageHeader(p) {
    /* Readable revision: the eyebrow is optional (label/number), an optional meta line sits under the title,
       and the actions wrap below the title instead of truncating it. */
    var eyebrow = (p.label || p.number != null) ? h(SectionLabel, { number: p.number }, p.label || p.title) : null;
    return h('div', { className: 'uz uz-pagehead' },
      h('div', { className: 'uz-pagehead-text' }, eyebrow, h('h1', { className: 'uz-title' }, p.title),
        p.meta ? h('span', { className: 'uz-pagehead-meta' }, p.meta) : null),
      p.actions ? h('div', { className: 'uz-pagehead-actions' }, p.actions) : null);
  }
  function OpenColumn(p) {
    return h('aside', { className: 'uz uz-opencol', style: p.style },
      h('span', { className: 'uz-opencol-label' }, p.label),
      h('span', { className: 'uz-opencol-rule' }),
      (p.lines || []).map(function (l, i) { return h('span', { key: i }, l); }));
  }
  function RegisterMarks(p) {
    var i = p.inset == null ? 24 : p.inset, o = typeof i === 'number' ? i + 'px' : i;
    var spots = [
      { left: o, top: o, borderWidth: '1px 0 0 1px' }, { right: o, top: o, borderWidth: '1px 1px 0 0' },
      { left: o, bottom: o, borderWidth: '0 0 1px 1px' }, { right: o, bottom: o, borderWidth: '0 1px 1px 0' }];
    return h('div', { className: cx('uz-regmarks', p.tone === 'dim' && 'dim'), 'aria-hidden': true }, spots.map(function (s, k) { return h('span', { key: k, className: 'uz-regmark', style: s }); }));
  }

  /* ---------- Actions ---------- */
  function Button(p) {
    var v = p.variant || 'secondary';
    return h('button', Object.assign({ type: 'button' }, rest(p, ['variant', 'size', 'state', 'className', 'children']), {
      className: cx('uz uz-btn uz-focusable', 'uz-btn-' + v, p.size === 'sm' && 'sm', stateClass(p.state), p.disabled && 'is-disabled', p.className)
    }), p.children);
  }
  function FilterChip(p) {
    /* Readable revision: optional `count` (always visible) and `attention` (the warning square before the label). */
    var label = typeof p.children === 'string' ? p.children : '';
    var name = label + (p.count != null ? ', ' + p.count : '') + (p.attention ? ', needs attention' : '');
    return h('button', Object.assign({ type: 'button', 'aria-pressed': !!p.selected, 'aria-label': name || undefined }, rest(p, ['selected', 'count', 'attention', 'className', 'children']), { className: cx('uz uz-chip uz-focusable', p.selected && 'is-selected', p.attention && 'is-attention', p.className) }),
      p.attention ? h('span', { className: 'uz-chip-mark', 'aria-hidden': true }, '!') : null,
      p.children,
      p.count != null ? h('span', { className: 'uz-chip-count', 'aria-hidden': true }, p.count) : null);
  }
  function SystemChip(p) { return h('span', { className: 'uz uz-sys', title: p.title }, p.system || p.children); }
  function Tabs(p) {
    /* Items are strings or { label, icon, count }: a plugin tab always shows its icon AND a short label, never a single letter. */
    return h('div', { className: cx('uz uz-tabs', p.size === 'lg' && 'lg'), role: 'tablist' }, (p.items || []).map(function (it) {
      var o = typeof it === 'string' ? { label: it } : it, on = o.label === p.active;
      return h('button', { key: o.label, role: 'tab', 'aria-selected': on, title: o.label, className: cx('uz-tab uz-focusable', on && 'is-active'), onClick: p.onChange ? function () { p.onChange(o.label); } : undefined },
        o.icon ? h('span', { className: 'uz-tab-icon', 'aria-hidden': true }, o.icon) : null, o.label,
        o.count ? h('span', { className: 'uz-tab-count', 'aria-label': o.count + ' new' }, o.count) : null);
    }));
  }
  function RunTabs(p) {
    var items = p.items || ['Run', 'Chat', 'Saves'];
    return h('div', { className: 'uz uz-runtabs', role: 'tablist' }, items.map(function (it) {
      var on = it === (p.active || items[0]);
      return h('button', { key: it, role: 'tab', 'aria-selected': on, className: cx('uz-tab uz-focusable', on && 'is-active'), onClick: p.onChange ? function () { p.onChange(it); } : undefined }, it);
    }));
  }
  function Segmented(p) {
    return h('div', { className: cx('uz uz-seg', p.size === 'sm' && 'sm'), role: 'radiogroup', 'aria-label': p.label }, (p.items || []).map(function (it) {
      var on = it === p.active;
      return h('button', { key: it, role: 'radio', 'aria-checked': on, className: cx('uz-seg-item uz-focusable', on && 'is-active'), onClick: p.onChange ? function () { p.onChange(it); } : undefined }, it);
    }));
  }
  function Progress(p) {
    var v = Math.max(0, Math.min(1, p.value || 0));
    return h('span', { className: cx('uz-progress', p.size === 'thin' && 'thin'), role: 'progressbar', 'aria-valuemin': 0, 'aria-valuemax': 100, 'aria-valuenow': Math.round(v * 100), 'aria-label': p.label, style: p.style }, h('span', { style: { width: v * 100 + '%' } }));
  }

  /* ---------- Fields ---------- */
  function TextField(p) {
    var id = p.id || ('uz-f-' + String(p.label || 'field').replace(/\W+/g, '-').toLowerCase());
    var err = !!p.error;
    return h('div', { className: cx('uz uz-field', p.disabled && 'is-disabled'), style: p.style },
      p.label ? h('label', { className: 'uz-field-label', htmlFor: id }, p.label) : null,
      h('div', { className: cx('uz-field-box', err && 'is-error', p.state === 'focus' && 'is-focus') },
        h('input', Object.assign({ id: id, 'aria-invalid': err || undefined }, rest(p, ['label', 'hint', 'error', 'state', 'shortcut', 'style', 'id']))),
        p.shortcut ? h('span', { className: 'uz-field-key' }, p.shortcut) : null),
      err ? h('span', { className: 'uz-field-hint is-error', role: 'alert' }, h('span', { className: 'uz-field-hint-icon' }), p.error)
        : p.hint ? h('span', { className: 'uz-field-hint' }, p.hint) : null);
  }
  function SearchField(p) {
    return h('div', { className: 'uz uz-search' }, h('input', Object.assign({ type: 'search', placeholder: 'Search games', 'aria-label': p.placeholder || 'Search games' }, rest(p, ['shortcut']))), h('span', { className: 'uz-field-key' }, p.shortcut || 'CTRL K'));
  }

  /* ---------- People ---------- */
  /* Voice: speaking bars and mic marks (batch 4) */
  function VoiceBars(p) {
    return h('span', { className: cx('uz-bars', p.still && 'still'), 'aria-hidden': true }, h('i'), h('i'), h('i'));
  }
  var VOICE_LABEL = { speaking: 'speaking', micOff: 'mic off', mutedByHost: 'muted by host' };
  function VoiceMark(p) {
    var v = p.voice;
    if (!v) return null;
    if (v === 'speaking') return h('span', { className: 'uz-voice is-speaking' }, h(VoiceBars, { still: p.still }), p.word !== false ? h('span', { className: 'uz-voice-word' }, 'SPEAKING') : null);
    return h('span', { className: cx('uz-voice', v === 'mutedByHost' ? 'is-host' : 'is-off') },
      h('span', { className: 'uz-voice-ico' }, h(Glyph, { name: 'micOff', size: 12 })),
      p.word !== false ? h('span', { className: 'uz-voice-word' }, v === 'mutedByHost' ? 'MUTED BY HOST' : 'MIC OFF') : null);
  }
  function Avatar(p) {
    var s = p.size || 28, st = p.status;
    var face = h('span', { className: 'uz-avatar', 'aria-hidden': true, style: { width: s, height: s, background: personColor(p.person), fontSize: s >= 40 ? 16 : s >= 28 ? 12 : 10 } }, (p.name || '?').charAt(0));
    var v = p.voice, vl = v ? VOICE_LABEL[v] : null, lab = [p.name || ''].concat(st ? [st] : []).concat(vl ? [vl] : []);
    return h('span', { className: cx('uz-avatar-wrap', st && 'has-status', v && 'voice-' + v, s >= 40 && 'lg'), role: 'img', 'aria-label': lab.join(', '), title: lab.join(' · ') },
      face, st ? h('span', { className: cx('uz-avatar-status', st === 'online' ? 'on' : 'off', s >= 40 && 'lg') }) : null,
      v ? h('span', { className: 'uz-avatar-voice' }, v === 'speaking' ? h(VoiceBars, { still: p.still }) : h(Glyph, { name: 'micOff', size: s >= 40 ? 12 : 10 })) : null);
  }
  function AvatarStack(p) {
    return h('div', { className: 'uz uz-avstack' }, h('div', { className: 'uz-avstack-faces' }, (p.people || []).map(function (x, i) { return h(Avatar, { key: i, name: x.name, person: x.person, size: p.size || 20 }); })), p.label ? h('span', { className: 'uz-avstack-label' }, p.label) : null);
  }

  /* ---------- Table ---------- */
  var COLS = ['No.', 'Game', 'Sys', 'Played', 'Last', ''];
  function TableHead(p) { return h('div', { className: 'uz uz-thead', role: 'row' }, (p.columns || COLS).map(function (c, i) { return h('span', { key: i, role: 'columnheader' }, c); })); }
  function TableRow(p) {
    var st = p.state || 'default';
    return h('div', { className: cx('uz uz-trow', stateClass(st)), role: 'row', 'aria-selected': st === 'selected' || undefined, 'aria-disabled': st === 'disabled' || undefined, tabIndex: p.tabIndex },
      h('span', { className: 'uz-trow-no' }, p.no),
      h('span', { className: 'uz-trow-title' }, p.title),
      h('span', { className: 'uz-trow-m' }, p.system),
      h('span', { className: 'uz-trow-m' }, p.played),
      h('span', { className: 'uz-trow-last' }, p.last),
      h('span', { className: 'uz-trow-status' }, p.status || null, p.progress != null ? h(Progress, { value: p.progress, size: 'thin', label: p.status }) : null));
  }

  /* ---------- Status ---------- */
  var GLYPH = { done: '✓', warning: '!', error: '✕' };
  function StatusIcon(p) {
    var k = p.kind || 'done';
    var icon = h('span', { className: cx('uz-sicon', k, p.size === 'sm' && 'sm'), 'aria-hidden': !!p.children }, GLYPH[k]);
    return p.children ? h('span', { className: 'uz uz-status' }, icon, p.children) : icon;
  }
  function StatusLine(p) {
    var on = (p.status || 'online') === 'online';
    return h('div', { className: 'uz uz-statusline' },
      h('div', { className: 'uz-statusline-top' }, h('span', { className: cx('uz-ok-sq', !on && 'off') }), p.name || 'Home server', h('span', { className: 'uz-statusline-state' }, on ? 'ONLINE' : 'OFFLINE')),
      p.address ? h('span', { className: 'uz-statusline-addr' }, p.address) : null);
  }
  function Toast(p) {
    var k = p.kind || 'done';
    return h('div', { className: 'uz uz-toast', role: k === 'error' ? 'alert' : 'status' },
      h(StatusIcon, { kind: k }), h('span', { className: 'uz-toast-msg' }, p.children),
      p.action ? h('button', { className: 'uz-toast-action uz-focusable', onClick: p.onAction }, p.action) : null,
      p.shortcut ? h('span', { className: 'uz-toast-key' }, p.shortcut) : null,
      p.onClose !== false ? h('button', { className: 'uz-toast-close uz-focusable', 'aria-label': 'Dismiss', onClick: p.onClose || undefined }, '✕') : null);
  }
  /* ---------- Games ---------- */
  function GameCover(p) {
    var st = p.state;
    return h('div', { className: 'uz uz-cover' },
      h('div', { className: cx('uz-cover-art uz-focusable', st === 'hover' && 'is-hover', st === 'focus' && 'is-focus'), tabIndex: 0, role: 'button', 'aria-label': p.title },
        h(SystemChip, { system: p.system }), p.src ? h('img', { src: p.src, alt: '' }) : p.resolution),
      h('div', null, h('div', { className: 'uz-cover-title' }, p.title), h('div', { className: 'uz-cover-meta' }, p.meta)));
  }
  var RAW = { GB: [[160, 144]], GBC: [[160, 144]], GBA: [[240, 160]], NDS: [[256, 192], [256, 192]], '3DS': [[400, 240], [320, 240]] };
  function fit(sys, W, H) {
    var r = RAW[sys] || RAW.GBA, sh = r.reduce(function (a, x) { return a + x[1]; }, 0) + (r.length - 1) * 4, mw = Math.max.apply(null, r.map(function (x) { return x[0]; })), k = Math.min(H / sh, W / mw);
    return r.map(function (x) { return { w: Math.round(x[0] * k), h: Math.round(x[1] * k), l: x[0] + ' × ' + x[1] }; });
  }
  function Screens(p) {
    return h('div', { className: p.className || 'uz-tile-screens' }, fit(p.system, p.width || 360, p.height || 200).map(function (g, i) {
      return h('div', { key: i, className: 'uz-screen', style: { width: g.w, height: g.h } }, p.labels ? p.labels[i] : g.l);
    }));
  }
  function Enso(p) {
    var s = p.size || 48;
    return h('svg', { width: s, height: s, viewBox: '0 0 56 56', role: p.label ? 'img' : undefined, 'aria-label': p.label, 'aria-hidden': p.label ? undefined : true, style: { display: 'block', flex: 'none' } },
      h('path', { className: 'uz-enso-path', d: 'M 41.6 11.6 A 22 22 0 1 0 49.9 31.6' }), h('circle', { className: 'uz-enso-point', cx: 49.9, cy: 31.6, r: 2.2 }));
  }
  function MultiviewTile(p) {
    var wait = !!p.waiting;
    return h('div', { className: cx('uz uz-tile', p.audio && 'is-audio', p.voice === 'speaking' && 'is-speaking'), style: p.style },
      h('span', { className: 'uz-tile-marker', style: { background: personColor(p.person) } }),
      h('div', { className: 'uz-tile-stage' }, wait
        ? h('div', { className: 'uz-tile-wait', role: 'status' }, h(Enso, { size: 56 }), h('span', { className: 'uz-tile-wait-title' }, 'Waiting for ' + p.name), h('span', { className: 'uz-tile-wait-meta' }, p.system + ' · ' + (p.game || '') + ' · STARTING'), h(Button, { variant: 'ghost', size: 'sm', onClick: p.onStopWaiting }, 'Stop waiting'))
        : p.children || h(Screens, { system: p.system, width: p.screenWidth || 300, height: p.screenHeight || 150 })),
      h('div', { className: 'uz-tile-bar' },
        h('span', { className: 'uz-tile-sq', style: { background: personColor(p.person) } }),
        h('span', { className: 'uz-tile-name' }, p.name),
        p.voice ? h(VoiceMark, { voice: p.voice, still: p.still }) : null,
        h('span', { className: 'uz-tile-meta' }, p.system + (p.game ? ' · ' + p.game : '')),
        p.audio ? h('span', { className: 'uz-audio' }, 'AUDIO') : h('span', { className: 'uz-tile-right' }, wait ? 'NOT STARTED' : (p.right || ''))));
  }
  function StateBlock(p) {
    var k = p.kind || 'empty';
    return h('div', { className: 'uz uz-state', role: k === 'error' ? 'alert' : 'status', style: p.style },
      k === 'error' ? h('span', { className: 'uz-state-err', 'aria-hidden': true }, '!') : h(Enso, { size: 48 }),
      h('span', { className: 'uz-state-title' }, p.title),
      p.text ? h('span', { className: 'uz-state-text' }, p.text) : null,
      p.action ? h(Button, { variant: k === 'empty' ? 'primary' : 'secondary', size: 'sm', onClick: p.onAction }, p.action) : null);
  }

  /* ---------- Running game ---------- */
  function RunHeader(p) {
    return h('header', { className: 'uz uz-runhead' },
      h('button', { className: 'uz-runhead-back uz-focusable', onClick: p.onBack }, '← ' + (p.back || 'Library')),
      h('span', { className: 'uz-runhead-sep' }),
      p.number != null ? h('span', { className: 'uz-runhead-no' }, String(p.number).padStart(2, '0')) : null,
      h('span', { className: 'uz-runhead-title' }, p.title),
      p.system ? h(SystemChip, { system: p.system }) : null,
      p.sub ? h('span', { className: 'uz-runhead-sub' }, p.sub) : null,
      p.layout || null,
      h('div', { className: 'uz-runhead-end' }, p.watchers ? h(AvatarStack, { people: p.watchers, label: p.watchLabel }) : null, p.actions));
  }
  var HOTKEYS = [['A', 'Save state'], ['B', 'Load state'], ['L', 'Previous slot'], ['R', 'Next slot'], ['X', 'Screenshot'], ['Y', 'Fast forward'], ['UpDown', 'Volume'], ['Start', 'Pause menu']];
  function StageDiagnostics(p) {
    var c = React.useState(!!p.copied), copied = p.copied !== undefined ? p.copied : c[0];
    return h('div', { className: 'uz uz-diag', role: 'region', 'aria-label': 'Diagnostics' },
      h('div', { className: 'uz-diag-head' }, h('span', { className: 'uz-diag-label' }, p.title || 'Diagnostics'),
        h('button', { type: 'button', className: cx('uz-diag-copy uz-focusable', stateClass(p.copyState), copied && 'is-done'), onClick: function () { c[1](true); if (p.onCopy) p.onCopy(); } },
          copied ? [h(MiniGlyph, { key: 'g', name: 'check' }), 'Copied'] : 'Copy')),
      h('dl', { className: 'uz-diag-rows' }, (p.rows || []).map(function (r, i) {
        return h(React.Fragment, { key: i }, h('dt', null, r[0]), h('dd', { className: r[2] ? 'is-' + r[2] : null }, r[2] === 'warn' ? h('span', { className: 'uz-diag-bang', 'aria-label': 'warning' }, '!') : null, r[1]));
      })));
  }
  function HotkeySheet(p) {
    var keys = p.keys || HOTKEYS;
    return h('div', { className: 'uz uz-hotkeys', role: 'dialog', 'aria-label': 'Select shortcuts' },
      h('div', { className: 'uz-hotkeys-head' }, h(GamepadHint, { k: 'Select', state: 'pressed' }), h('span', { className: 'uz-diag-label' }, 'Hold Select +')),
      h('div', { className: 'uz-hotkeys-grid' }, keys.map(function (x, i) {
        return h('div', { key: i, className: cx('uz-hotkeys-row', x[2] && 'is-disabled') }, h(GamepadHint, { k: x[0], disabled: !!x[2] }), h('span', null, x[1]), x[2] ? h('span', { className: 'uz-hotkeys-why' }, x[2]) : null);
      })),
      h('div', { className: 'uz-hotkeys-foot' }, 'Release Select to close'));
  }
  function GameStage(p) {
    return h('div', { className: 'uz uz-stage', style: p.style },
      h('div', { className: 'uz-stage-screens' }, h(RegisterMarks, { inset: -16, tone: 'dim' }), p.children || h(Screens, { system: p.system || 'NDS', width: p.width || 256, height: p.height || 392, labels: p.labels, className: 'uz-stage-inner' })),
      p.caption ? h('span', { className: 'uz-stage-caption' }, p.caption) : null,
      p.diagnostics ? h('div', { className: 'uz-stage-diag' }, h(StageDiagnostics, p.diagnostics)) : null,
      p.hotkeys ? h('div', { className: 'uz-stage-hotkeys' }, h(HotkeySheet, p.hotkeys === true ? {} : p.hotkeys)) : null,
      p.toast ? h('div', { className: 'uz-stage-toast' }, p.toast) : null);
  }
  function RunnerRow(p) {
    return h('div', { className: 'uz uz-runner' }, h(Avatar, { name: p.name, person: p.person, size: 28, status: p.presence }),
      h('div', { className: 'uz-runner-text' }, h('span', { className: 'uz-runner-name' }, p.name), h('span', { className: 'uz-runner-status' }, p.status)),
      p.meta ? h('span', { className: 'uz-runner-meta' }, p.meta) : null);
  }
  function ChatMessage(p) {
    return h('div', { className: 'uz uz-chat' },
      h('div', { className: 'uz-chat-head' }, h('span', { className: 'uz-persontag-sq', style: { background: personColor(p.person) } }), h('span', { className: 'uz-chat-name' }, p.name), h('span', { className: 'uz-chat-time' }, p.time)),
      h('span', { className: 'uz-chat-text' }, p.children));
  }

  /* ---------- Small glyphs (1.5 px, square caps, no curves) ---------- */
  function Glyph(p) {
    var d = { chevronDown: 'M3 6 L8 11 L13 6', chevronUp: 'M3 10 L8 5 L13 10', check: 'M3 8.5 L6.5 12 L13 4.5', more: '', bell: 'M5 3 H11 L12.5 11 H3.5 Z M2 11 H14 M7 13.5 H9', volume: 'M2 6 H5 L9 3 V13 L5 10 H2 Z M11.5 6 V10 M14 4.5 V11.5', volumeOff: 'M2 6 H5 L9 3 V13 L5 10 H2 Z M11 6 L15 10 M15 6 L11 10', mic: 'M6 2 H10 V9 H6 Z M3.5 7.5 V11 H12.5 V7.5 M8 11 V14 M5.5 14 H10.5', micOff: 'M6 2 H10 V9 H6 Z M3.5 7.5 V11 H12.5 V7.5 M8 11 V14 M5.5 14 H10.5 M2 2 L14 14', up: 'M8 13 V3 M4 7 L8 3 L12 7', down: 'M8 3 V13 M4 9 L8 13 L12 9', left: 'M13 8 H3 M7 4 L3 8 L7 12', right: 'M3 8 H13 M9 4 L13 8 L9 12' }[p.name];
    return h('svg', { width: p.size || 16, height: p.size || 16, viewBox: '0 0 16 16', 'aria-hidden': true, style: { display: 'block', flex: 'none' } },
      p.name === 'more'
        ? [2, 7, 12].map(function (x) { return h('rect', { key: x, x: x, y: 7, width: 2, height: 2, fill: 'currentColor' }); })
        : h('path', { d: d, fill: 'none', stroke: 'currentColor', strokeWidth: 1.5, strokeLinecap: 'square', strokeLinejoin: 'miter' }));
  }

  /* ---------- Toggle ---------- */
  function Toggle(p) {
    var ctl = p.checked !== undefined, st = React.useState(!!p.defaultChecked), on = ctl ? !!p.checked : st[0];
    var id = p.id || ('uz-t-' + String(p.label || 'toggle').replace(/\W+/g, '-').toLowerCase());
    function flip() { if (p.disabled) return; if (!ctl) st[1](!on); if (p.onChange) p.onChange(!on); }
    return h('div', { className: cx('uz uz-toggle', p.disabled && 'is-disabled', p.error && 'is-error'), style: p.style },
      p.label ? h('label', { className: 'uz-toggle-label', htmlFor: id }, p.label) : null,
      h('span', { className: 'uz-toggle-state', 'aria-hidden': true }, on ? 'ON' : 'OFF'),
      h('button', { id: id, type: 'button', role: 'switch', 'aria-checked': on, 'aria-invalid': p.error ? true : undefined, disabled: p.disabled,
        className: cx('uz-switch uz-focusable', on && 'is-on', stateClass(p.state)), onClick: flip }, h('span', { className: 'uz-switch-knob' })),
      p.error ? h('span', { className: 'uz-field-hint is-error uz-toggle-err', role: 'alert' }, h('span', { className: 'uz-field-hint-icon' }), p.error) : null);
  }

  /* ---------- Menu ---------- */
  function MenuList(p) {
    var items = p.items || [];
    return h('div', { className: cx('uz uz-menu', p.className), role: p.role || 'menu', 'aria-label': p.label, style: p.style },
      items.map(function (it, i) {
        if (it.separator) return h('div', { key: i, className: 'uz-menu-sep', role: 'separator' });
        if (it.heading) return h('div', { key: i, className: 'uz-menu-head' }, it.heading);
        var sel = p.selectable && it.value === p.value;
        return h('button', { key: i, type: 'button', role: p.selectable ? 'option' : 'menuitem', 'aria-selected': p.selectable ? sel : undefined, disabled: it.disabled, tabIndex: -1,
          className: cx('uz-menu-item', it.danger && 'is-danger', sel && 'is-selected', i === p.activeIndex && 'is-active', it.disabled && 'is-disabled'),
          onMouseDown: function (e) { e.preventDefault(); }, onClick: function () { if (!it.disabled && p.onSelect) p.onSelect(it, i); } },
          p.selectable ? h('span', { className: 'uz-menu-check' }, sel ? h(Glyph, { name: 'check', size: 14 }) : null) : null,
          h('span', { className: 'uz-menu-text' }, h('span', null, it.label), it.hint ? h('span', { className: 'uz-menu-hint' }, it.hint) : null),
          it.shortcut ? h('span', { className: 'uz-menu-key' }, it.shortcut) : null);
      }),
      p.footer ? h('div', { className: 'uz-menu-foot' }, p.footer) : null);
  }
  function nextEnabled(items, from, dir) {
    for (var k = 1; k <= items.length; k++) { var j = (from + dir * k + items.length * 4) % items.length; var it = items[j]; if (!it.separator && !it.heading && !it.disabled) return j; }
    return from;
  }
  function Menu(p) {
    var items = p.items || [], o = React.useState(!!p.defaultOpen), open = p.open !== undefined ? p.open : o[0];
    var a = React.useState(p.activeIndex != null ? p.activeIndex : -1), active = p.activeIndex != null ? p.activeIndex : a[0];
    function setOpen(v) { o[1](v); if (!v) a[1](-1); if (p.onOpenChange) p.onOpenChange(v); }
    function key(e) {
      if (!open && (e.key === 'Enter' || e.key === ' ' || e.key === 'ArrowDown')) { e.preventDefault(); setOpen(true); return; }
      if (!open) return;
      if (e.key === 'ArrowDown') { e.preventDefault(); a[1](nextEnabled(items, active, 1)); }
      else if (e.key === 'ArrowUp') { e.preventDefault(); a[1](nextEnabled(items, active, -1)); }
      else if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); var it = items[active]; if (it && !it.disabled && it.onSelect) it.onSelect(); setOpen(false); }
      else if (e.key === 'Escape' || e.key === 'Tab') setOpen(false);
    }
    return h('div', { className: cx('uz uz-menuwrap', p.placement === 'top' && 'top', p.align === 'start' && 'start'), onKeyDown: key, onBlur: function (e) { if (!e.currentTarget.contains(e.relatedTarget)) setOpen(false); } },
      p.trigger ? p.trigger({ open: open, toggle: function () { setOpen(!open); } })
        : h('button', { type: 'button', className: cx('uz-btn uz-btn-ghost sm uz-iconbtn uz-focusable', open && 'is-on', stateClass(p.state)), 'aria-haspopup': 'menu', 'aria-expanded': open, 'aria-label': p.label || 'More', onClick: function () { setOpen(!open); } }, h(Glyph, { name: 'more' })),
      open ? h(MenuList, { items: items, activeIndex: active, label: p.label, style: { minWidth: p.width || 200 }, className: 'uz-menu-pop', onSelect: function (it) { if (it.onSelect) it.onSelect(); setOpen(false); } }) : null);
  }

  /* ---------- Select ---------- */
  function Select(p) {
    var opts = (p.options || []).map(function (x) { return typeof x === 'string' ? { value: x, label: x } : x; });
    var vs = React.useState(p.defaultValue), value = p.value !== undefined ? p.value : vs[0];
    var o = React.useState(!!p.defaultOpen), open = p.open !== undefined ? p.open : o[0];
    var cur = Math.max(0, opts.findIndex(function (x) { return x.value === value; }));
    var a = React.useState(cur), active = p.activeIndex != null ? p.activeIndex : a[0];
    var id = p.id || ('uz-s-' + String(p.label || 'select').replace(/\W+/g, '-').toLowerCase());
    function setOpen(v) { if (p.disabled) return; o[1](v); if (v) a[1](cur); }
    function choose(i) { var x = opts[i]; if (!x || x.disabled) return; vs[1](x.value); if (p.onChange) p.onChange(x.value); setOpen(false); }
    function key(e) {
      var k = e.key;
      if (!open) { if (k === 'Enter' || k === ' ' || k === 'ArrowDown' || k === 'ArrowUp') { e.preventDefault(); setOpen(true); } return; }
      if (k === 'ArrowDown') { e.preventDefault(); a[1](nextEnabled(opts, active, 1)); }
      else if (k === 'ArrowUp') { e.preventDefault(); a[1](nextEnabled(opts, active, -1)); }
      else if (k === 'Home') { e.preventDefault(); a[1](nextEnabled(opts, -1, 1)); }
      else if (k === 'End') { e.preventDefault(); a[1](nextEnabled(opts, opts.length, -1)); }
      else if (k === 'Enter' || k === ' ') { e.preventDefault(); choose(active); }
      else if (k === 'Escape' || k === 'Tab') setOpen(false);
    }
    var label = (opts.find(function (x) { return x.value === value; }) || {}).label;
    return h('div', { className: cx('uz uz-field uz-select', p.disabled && 'is-disabled'), style: p.style, onBlur: function (e) { if (!e.currentTarget.contains(e.relatedTarget)) setOpen(false); } },
      p.label ? h('label', { className: 'uz-field-label', htmlFor: id }, p.label) : null,
      h('div', { className: 'uz-select-anchor' },
        h('button', { id: id, type: 'button', disabled: p.disabled, 'aria-haspopup': 'listbox', 'aria-expanded': open, 'aria-invalid': p.error ? true : undefined,
          className: cx('uz-field-box uz-select-box uz-focusable', open && 'is-open', p.error && 'is-error', stateClass(p.state)), onClick: function () { setOpen(!open); }, onKeyDown: key },
          h('span', { className: cx('uz-select-value', !label && 'is-empty') }, label || p.placeholder || 'Choose…'),
          h('span', { className: 'uz-select-chev' }, h(Glyph, { name: open ? 'chevronUp' : 'chevronDown', size: 14 }))),
        open ? h(MenuList, { role: 'listbox', label: p.label, selectable: true, value: value, items: opts, activeIndex: active, className: 'uz-menu-pop uz-select-list',
          onSelect: function (it, i) { choose(i); }, footer: p.gamepad ? h('div', { className: 'uz-hints' }, h(GamepadHint, { k: 'A', label: 'Choose' }), h(GamepadHint, { k: 'B', label: 'Close' })) : null }) : null),
      p.error ? h('span', { className: 'uz-field-hint is-error', role: 'alert' }, h('span', { className: 'uz-field-hint-icon' }), p.error)
        : p.hint ? h('span', { className: 'uz-field-hint' }, p.hint) : null);
  }

  /* ---------- Settings ---------- */
  function SettingsRow(p) {
    return h('div', { className: cx('uz uz-setrow', p.disabled && 'is-disabled', p.error && 'is-error', p.advanced && 'is-advanced'), style: p.style },
      h('div', { className: 'uz-setrow-text' },
        h('div', { className: 'uz-setrow-label' }, p.changed ? h('span', { className: 'uz-changed', title: 'Changed from default' }) : null, h('span', null, p.label), p.changed ? h('span', { className: 'uz-sr' }, ' (changed)') : null),
        p.error ? h('span', { className: 'uz-field-hint is-error', role: 'alert' }, h('span', { className: 'uz-field-hint-icon' }), p.error)
          : p.help ? h('span', { className: 'uz-setrow-help' }, p.help) : null,
        p.changed && !p.disabled ? h('button', { type: 'button', className: 'uz-setrow-reset uz-focusable', onClick: p.onReset }, 'Reset to default' + (p.defaultLabel ? ' · ' + p.defaultLabel : '')) : null),
      h('div', { className: 'uz-setrow-ctl' }, p.children));
  }
  function SettingsDisclosure(p) {
    return h('button', { type: 'button', className: cx('uz uz-disclosure uz-focusable', stateClass(p.state)), 'aria-expanded': !!p.open, onClick: p.onToggle },
      h(Glyph, { name: p.open ? 'chevronUp' : 'chevronDown', size: 14 }),
      h('span', null, p.open ? 'Less' : 'More'),
      p.count != null ? h('span', { className: 'uz-disclosure-meta' }, p.count + ' ADVANCED') : null);
  }

  /* ---------- Gamepad hint ---------- */
  var DPAD = { Up: 'up', Down: 'down', Left: 'left', Right: 'right', '↑': 'up', '↓': 'down', '←': 'left', '→': 'right' };
  var DPAD_NAME = { up: 'D-pad up', down: 'D-pad down', left: 'D-pad left', right: 'D-pad right' };
  function GamepadHint(p) {
    var k = String(p.k || p.button || 'A');
    var dirs = k === 'LeftRight' ? ['left', 'right'] : k === 'UpDown' ? ['up', 'down'] : DPAD[k] ? [DPAD[k]] : null;
    var face = dirs
      ? h('span', { className: cx('uz-key uz-key-dpad', dirs.length > 1 && 'wide'), role: 'img', 'aria-label': dirs.map(function (d) { return DPAD_NAME[d]; }).join(' / ') }, dirs.map(function (d) { return h(Glyph, { key: d, name: d, size: 12 }); }))
      : h('span', { className: cx('uz-key', k.length > 1 && 'wide'), 'aria-label': k + ' button' }, k.toUpperCase());
    return h('span', { className: cx('uz uz-hint', stateClass(p.state), p.disabled && 'is-disabled') }, face,
      p.label ? h('span', { className: 'uz-hint-label' }, p.label) : null);
  }

  /* ---------- Navigation ---------- */
  /* Round 5: PLAY for everyone · SERVER for admins · THIS PC only without a server · Settings pinned to the bottom. */
  var NAV_PLAY = { head: 'Play', items: ['Library', 'Runs', 'Emulation', 'Controllers'] };
  var NAV_SERVER = { head: 'Server', server: true, items: ['Status', 'Users', 'Systems & cores', 'Plugins', 'Reports'] };
  var NAV_PC = { head: 'This PC', items: ['Systems & cores', 'Plugins'] };
  function navBadge(b) {
    if (!b) return null;
    if (typeof b === 'object' && b.warning) return h('span', { className: 'uz-nav-warn', role: 'img', 'aria-label': 'Warning: ' + b.warning, title: b.warning }, h('span', { className: 'uz-nav-warn-sq', 'aria-hidden': true }, '!'), h('span', { className: 'uz-nav-warn-word', 'aria-hidden': true }, b.warning));
    return h('span', { className: 'uz-nav-badge', 'aria-label': b + ' new' }, b);
  }
  function NavBell(p) {
    var n = p.count || 0;
    return h('button', { type: 'button', className: cx('uz-btn uz-btn-ghost sm uz-iconbtn uz-bell uz-focusable', stateClass(p.state)), 'aria-label': n ? 'Notifications, ' + n + ' new' : 'Notifications', onClick: p.onClick },
      h(Glyph, { name: 'bell' }), n ? h('span', { className: 'uz-bell-count', 'aria-hidden': true }, n > 9 ? '9+' : n) : null);
  }
  function AppNav(p) {
    var member = p.role === 'member', hasServer = p.server !== false, compact = p.density === 'compact';
    var active = p.active || 'Library', badges = p.badges || {};
    var groups = p.groups || [NAV_PLAY].concat(!hasServer ? [NAV_PC] : member ? [] : [NAV_SERVER]);
    var online = hasServer ? (p.online || []) : [];
    var ss = p.serverStatus || { status: 'online', name: 'Home server', address: 'server.example.com:8443' };
    var serverItems = p.serverActions || [{ label: 'Switch server' }, { label: 'Details' }, { label: 'Share …', hint: 'Invite code or link' }];
    var userItems = p.userActions || [{ label: 'My account' }, { label: 'Switch user' }, { separator: true }, { label: 'Sign out' }];
    function item(it) {
      return h('button', { key: it, className: cx('uz-nav-item uz-focusable', it === active && 'is-active', it === p.hover && 'is-hover'), 'aria-current': it === active ? 'page' : undefined, onClick: p.onNavigate ? function () { p.onNavigate(it); } : undefined },
        h('span', null, it), navBadge(badges[it]));
    }
    function head(label, meta) { return h('div', { className: 'uz-nav-head' }, h('span', { className: 'uz-nav-head-rule' }), label, meta != null ? h('span', { className: 'uz-nav-head-meta' }, meta) : null); }
    return h('nav', { className: cx('uz uz-nav', compact && 'compact'), 'aria-label': 'Main', style: p.style },
      h('div', { className: 'uz-nav-brand' }, h(Lockup), p.notifications != null ? h(NavBell, { count: p.notifications, state: p.bellState, onClick: p.onNotifications }) : null),
      groups.map(function (g, gi) {
        return h(React.Fragment, { key: gi }, g.head != null ? (g.head ? head(g.head) : h('div', { className: 'uz-nav-gap' })) : null, g.items.map(item));
      }),
      online.length ? h('div', { className: 'uz-nav-online', role: 'list', 'aria-label': 'Online' },
        head('Online', online.length),
        online.map(function (o, i) {
          return h('button', { key: i, role: 'listitem', className: 'uz-nav-person uz-focusable', onClick: p.onPerson ? function () { p.onPerson(o); } : undefined, title: o.playing ? 'Watch ' + o.name : o.name },
            h('span', { className: 'uz-persontag-sq', style: { background: personColor(o.person), width: 8, height: 8 } }),
            h('span', { className: 'uz-nav-person-name' }, o.name),
            h('span', { className: 'uz-nav-person-doing' }, o.playing ? o.playing + (o.system && !compact ? ' · ' + o.system : '') : 'Idle'));
        })) : null,
      h('div', { className: 'uz-nav-spacer' }),
      p.settings !== false ? h('div', { className: 'uz-nav-settings' }, item('Settings')) : null,
      hasServer ? h('div', { className: 'uz-nav-foot' },
        h('div', { className: 'uz-nav-server' }, h(StatusLine, ss),
          h(Menu, { label: 'Server actions', items: serverItems, placement: 'top', open: p.serverMenuOpen, width: 208 }))) : null,
      p.user ? h(Menu, { label: 'Account', items: userItems, placement: 'top', align: 'start', open: p.userMenuOpen, width: 200,
        trigger: function (m) {
          return h('button', { type: 'button', className: cx('uz-nav-user uz-focusable', m.open && 'is-open'), 'aria-haspopup': 'menu', 'aria-expanded': m.open, onClick: m.toggle },
            h(Avatar, { name: p.user.name, person: p.user.person, size: 28 }),
            h('span', { className: 'uz-nav-user-text' }, h('span', { className: 'uz-nav-user-name' }, p.user.name), h('span', { className: 'uz-nav-user-role' }, p.user.role || (member ? 'Member' : 'Admin'))),
            h('span', { className: 'uz-nav-user-chev' }, h(Glyph, { name: m.open ? 'chevronDown' : 'chevronUp', size: 14 })));
        } }) : null);
  }

  /* ---------- Batch 3 ---------- */
  function MiniGlyph(p) {
    var d = { check: 'M2 5.5 L4.2 7.8 L8.5 2.8', cross: 'M2.5 2.5 L7.5 7.5 M7.5 2.5 L2.5 7.5', dash: 'M2 5 H8' }[p.name];
    return h('svg', { width: p.size || 10, height: p.size || 10, viewBox: '0 0 10 10', 'aria-hidden': true, style: { display: 'block', flex: 'none' } },
      h('path', { d: d, fill: 'none', stroke: 'currentColor', strokeWidth: p.weight || 1.5, strokeLinecap: 'square', strokeLinejoin: 'miter' }));
  }

  function Checkbox(p) {
    var ctl = p.checked !== undefined, st = React.useState(!!p.defaultChecked), on = ctl ? !!p.checked : st[0];
    var mixed = !!p.indeterminate;
    var id = p.id || ('uz-c-' + String(p.label || 'check').replace(/\W+/g, '-').toLowerCase());
    function flip() { if (p.disabled) return; var v = mixed ? true : !on; if (!ctl) st[1](v); if (p.onChange) p.onChange(v); }
    return h('div', { className: cx('uz uz-check', p.disabled && 'is-disabled', p.error && 'is-error'), style: p.style },
      h('button', { id: id, type: 'button', role: 'checkbox', 'aria-checked': mixed ? 'mixed' : on, 'aria-invalid': p.error ? true : undefined, 'aria-describedby': p.hint || p.error ? id + '-d' : undefined,
        disabled: p.disabled, className: cx('uz-check-box uz-focusable', (on || mixed) && 'is-on', stateClass(p.state)), onClick: flip },
        mixed ? h(MiniGlyph, { name: 'dash', size: 12, weight: 2 }) : on ? h(MiniGlyph, { name: 'check', size: 12, weight: 2 }) : null),
      p.label ? h('label', { className: 'uz-check-label', htmlFor: id }, p.label) : null,
      p.error ? h('span', { id: id + '-d', className: 'uz-field-hint is-error uz-check-sub', role: 'alert' }, h('span', { className: 'uz-field-hint-icon' }), p.error)
        : p.hint ? h('span', { id: id + '-d', className: 'uz-check-sub uz-check-hint' }, p.hint) : null);
  }

  var TAG_ICON = { ok: 'check', danger: 'cross' };
  function Tag(p) {
    var tone = p.tone || 'neutral', icon = p.icon === false ? null : (p.icon || TAG_ICON[tone]);
    return h('span', { className: cx('uz uz-tag', 'tone-' + tone, p.variant === 'plane' ? 'is-plane' : 'is-outline', p.disabled && 'is-disabled'), title: p.title },
      icon === 'warn' ? h('span', { className: 'uz-tag-bang', 'aria-hidden': true }, '!') : icon ? h(MiniGlyph, { name: icon }) : null,
      h('span', null, p.children));
  }

  function Kbd(p) {
    var keys = Array.isArray(p.keys) ? p.keys : String(p.keys || p.children || '').split('+');
    return h('span', { className: cx('uz uz-hint uz-kbd', stateClass(p.state), p.disabled && 'is-disabled') },
      h('kbd', { className: 'uz-kbd-keys', 'aria-label': keys.join(' plus ') },
        keys.map(function (k, i) {
          k = String(k).trim();
          return h(React.Fragment, { key: i }, i ? h('span', { className: 'uz-kbd-plus', 'aria-hidden': true }, '+') : null,
            h('span', { className: cx('uz-key', k.length > 1 && 'wide') }, k.length > 1 ? k.toUpperCase() : k.toUpperCase()));
        })),
      p.label ? h('span', { className: 'uz-hint-label' }, p.label) : null);
  }

  function HelpColumn(p) {
    var items = p.items || [];
    return h('aside', { className: 'uz uz-opencol uz-helpcol', style: p.style, 'aria-label': 'Help' },
      p.label ? h('span', { className: 'uz-opencol-label' }, p.label) : null,
      h('div', { className: 'uz-help' },
        h('div', { className: 'uz-help-head' }, 'Help'),
        items.map(function (it, i) {
          return h('div', { key: i, className: 'uz-help-item' }, it.title ? h('div', { className: 'uz-help-title' }, it.title) : null, h('div', { className: 'uz-help-text' }, it.text));
        }),
        p.children || null),
      p.lines && p.lines.length ? h('div', { className: 'uz-help-foot' }, h('span', { className: 'uz-opencol-rule' }), p.lines.map(function (l, i) { return h('span', { key: i }, l); })) : null);
  }

  function DialogFrame(p) {
    return h('div', { className: cx('uz uz-dialog', p.size === 'lg' && 'lg', p.busy && 'is-busy', p.stacked && 'is-stacked'), role: 'dialog', 'aria-modal': true, 'aria-label': p.title, style: p.style },
      h('div', { className: 'uz-dialog-head' },
        h('div', { className: 'uz-dialog-headtext' }, p.label ? h('span', { className: 'uz-dialog-label' }, p.label) : null, h('h2', { className: 'uz-dialog-title' }, p.title)),
        p.onClose ? h('button', { type: 'button', className: 'uz-btn uz-btn-ghost sm uz-iconbtn uz-focusable', 'aria-label': 'Close', onClick: p.onClose }, h(MiniGlyph, { name: 'cross', size: 12 })) : null),
      p.text ? h('div', { className: 'uz-dialog-body' }, p.text) : null,
      p.error ? h('span', { className: 'uz-field-hint is-error', role: 'alert' }, h('span', { className: 'uz-field-hint-icon' }), p.error) : null,
      p.children ? h('div', { className: 'uz-dialog-content' }, p.children) : null,
      p.actions || p.dangerAction ? h('div', { className: 'uz-dialog-actions' },
        h('div', { className: 'uz-dialog-actions-main' }, p.actions),
        p.dangerAction ? h('div', { className: 'uz-dialog-actions-danger' }, p.dangerAction) : null) : null,
      p.hints ? h('div', { className: 'uz-dialog-hints' }, p.hints) : null);
  }
  function Dialog(p) {
    var frame = h(DialogFrame, p);
    return p.modal ? h('div', { className: 'uz uz-scrim', style: p.scrimStyle }, frame) : frame;
  }

  /* ---------- Readable revision ---------- */
  function HintRow(p) {
    /* The footer of every main page while the last input was a gamepad (always at the TV scale); keys otherwise. */
    var pad = p.input !== 'keys';
    function one(x, i) {
      return pad ? h(GamepadHint, { key: i, k: x.k, label: x.label })
        : h(Kbd, { key: i, keys: x.keys || x.k, label: x.label });
    }
    return h('div', { className: 'uz uz-hintrow', role: 'note', 'aria-label': 'Controls' }, (p.hints || []).map(one),
      p.end ? h('div', { className: 'uz-hintrow-end' }, p.end.map(one)) : null);
  }
  function ColumnNav(p) {
    /* The second column of Settings, Emulation, Controllers and Server settings: every row is focusable,
       Up/Down move, Enter/A opens, Esc/B goes back to AppNav. Items: { label, meta, warning } or a { head } line. */
    var items = p.items || [], refs = React.useRef([]);
    function key(e, i) {
      var d = e.key === 'ArrowDown' ? 1 : e.key === 'ArrowUp' ? -1 : 0; if (!d) return;
      for (var j = i + d; j >= 0 && j < items.length; j += d) if (!items[j].head && refs.current[j]) { refs.current[j].focus(); e.preventDefault(); return; }
    }
    return h('nav', { className: 'uz uz-colnav', 'aria-label': p.label || 'Sections', style: p.style }, items.map(function (it, i) {
      if (it.head) return h('div', { key: 'h' + i, className: 'uz-colnav-head' }, it.head);
      var on = it.label === p.active;
      return h('button', { key: it.label, ref: function (el) { refs.current[i] = el; }, type: 'button', 'aria-current': on ? 'page' : undefined,
        className: cx('uz-colnav-item', on && 'is-active', it.label === p.focus && 'is-focus', it.label === p.hover && 'is-hover'),
        onKeyDown: function (e) { key(e, i); }, onClick: p.onChange ? function () { p.onChange(it.label); } : undefined },
        h('span', null, it.label),
        it.warning ? h('span', { className: 'uz-colnav-warn' }, h('i', { 'aria-hidden': true }), it.warning)
          : it.meta ? h('span', { className: 'uz-colnav-meta' }, it.meta) : null);
    }));
  }

  /* ---------- Batch 4 ---------- */
  function Slider(p) {
    var min = p.min != null ? p.min : 0, max = p.max != null ? p.max : 100, step = p.step || 1;
    var vs = React.useState(p.defaultValue != null ? p.defaultValue : min), value = p.value != null ? p.value : vs[0];
    var track = React.useRef(null);
    var id = p.id || ('uz-sl-' + String(p.label || 'slider').replace(/\W+/g, '-').toLowerCase());
    function clamp(v) { v = Math.round((v - min) / step) * step + min; return Math.max(min, Math.min(max, +v.toFixed(4))); }
    function set(v) { if (p.disabled) return; v = clamp(v); vs[1](v); if (p.onChange) p.onChange(v); }
    function key(e) {
      var big = Math.max(step, (max - min) / 10), k = e.key;
      if (k === 'ArrowRight' || k === 'ArrowUp') set(value + (e.shiftKey ? big : step));
      else if (k === 'ArrowLeft' || k === 'ArrowDown') set(value - (e.shiftKey ? big : step));
      else if (k === 'PageUp') set(value + big); else if (k === 'PageDown') set(value - big);
      else if (k === 'Home') set(min); else if (k === 'End') set(max); else return;
      e.preventDefault();
    }
    function fromX(x) { var r = track.current.getBoundingClientRect(); set(min + (max - min) * Math.max(0, Math.min(1, (x - r.left) / r.width))); }
    function down(e) {
      if (p.disabled) return; fromX(e.clientX);
      function mv(ev) { fromX(ev.clientX); } function up() { window.removeEventListener('pointermove', mv); window.removeEventListener('pointerup', up); }
      window.addEventListener('pointermove', mv); window.addEventListener('pointerup', up);
    }
    var pct = (value - min) / (max - min) * 100, off = !!p.muted;
    var shown = off ? (p.offLabel || 'OFF') : p.format ? p.format(value) : value + (p.unit != null ? p.unit : ' %');
    return h('div', { className: cx('uz uz-slider', !p.label && !p.lead && 'bare', p.disabled && 'is-disabled', off && 'is-muted', stateClass(p.state)), style: p.style },
      p.label ? h('label', { className: 'uz-slider-label', htmlFor: id }, p.label) : null,
      p.lead || null,
      h('div', { className: 'uz-slider-track', ref: track, onPointerDown: down },
        h('span', { className: 'uz-slider-line' }),
        h('span', { className: 'uz-slider-fill', style: { width: (off ? 0 : pct) + '%' } }),
        h('span', { id: id, className: 'uz-slider-thumb uz-focusable', role: 'slider', tabIndex: p.disabled ? -1 : 0, 'aria-valuemin': min, 'aria-valuemax': max, 'aria-valuenow': value,
          'aria-valuetext': String(shown), 'aria-disabled': p.disabled || undefined, 'aria-label': p.label ? undefined : p.ariaLabel, onKeyDown: key, style: { left: pct + '%' } })),
      h('span', { className: 'uz-slider-value' }, shown));
  }

  function Popover(p) {
    var o = React.useState(!!p.defaultOpen), open = p.open !== undefined ? p.open : o[0];
    function setOpen(v) { o[1](v); if (p.onOpenChange) p.onOpenChange(v); if (!v && p.onClose) p.onClose(); }
    var panel = open ? h('div', { className: cx('uz uz-popover', 'at-' + (p.placement || 'bottom'), p.align === 'end' && 'end', p.align === 'center' && 'center'), role: 'dialog', 'aria-label': p.title || p.label, style: { width: p.width || 280 } },
      p.title ? h('div', { className: 'uz-popover-head' }, p.label ? h('span', { className: 'uz-popover-label' }, p.label) : null, h('span', { className: 'uz-popover-title' }, p.title),
        h('button', { type: 'button', className: 'uz-btn uz-btn-ghost sm uz-iconbtn uz-focusable', 'aria-label': 'Close', onClick: function () { setOpen(false); } }, h(MiniGlyph, { name: 'cross', size: 12 }))) : null,
      h('div', { className: 'uz-popover-body' }, p.children),
      p.actions ? h('div', { className: 'uz-popover-actions' }, p.actions) : null,
      p.hints ? h('div', { className: 'uz-popover-hints' }, p.hints) : null) : null;
    return h('div', { className: 'uz uz-popwrap', onKeyDown: function (e) { if (e.key === 'Escape' && open) { e.stopPropagation(); setOpen(false); } } },
      p.trigger ? p.trigger({ open: open, toggle: function () { setOpen(!open); } }) : null, panel);
  }
  function IconButton(p) {
    return h('button', { type: 'button', className: cx('uz uz-btn uz-btn-ghost sm uz-iconbtn uz-focusable', p.on && 'is-on', stateClass(p.state)), 'aria-label': p.label, 'aria-expanded': p.expanded, 'aria-pressed': p.pressed, onClick: p.onClick },
      h(Glyph, { name: p.icon }), p.count ? h('span', { className: 'uz-bell-count', 'aria-hidden': true }, p.count > 9 ? '9+' : p.count) : null);
  }
  function MapPoint(p) {
    return h('button', { type: 'button', className: cx('uz-mappoint uz-focusable', p.active && 'is-active', stateClass(p.state)), 'aria-label': p.label, 'aria-expanded': p.expanded, onClick: p.onClick, style: p.style });
  }

  window.Uzume = Object.assign(window.Uzume || {}, {
    AppNav: AppNav, Button: Button, TextField: TextField, SearchField: SearchField, SystemChip: SystemChip, FilterChip: FilterChip,
    Tabs: Tabs, RunTabs: RunTabs, Segmented: Segmented, Progress: Progress, TableRow: TableRow, TableHead: TableHead,
    Toast: Toast, Dialog: Dialog, StatusIcon: StatusIcon, StatusLine: StatusLine, SectionLabel: SectionLabel, PageHeader: PageHeader,
    OpenColumn: OpenColumn, RegisterMarks: RegisterMarks, GameCover: GameCover, MultiviewTile: MultiviewTile, Enso: Enso,
    StateBlock: StateBlock, Avatar: Avatar, AvatarStack: AvatarStack, RunHeader: RunHeader, GameStage: GameStage,
    RunnerRow: RunnerRow, ChatMessage: ChatMessage, Toggle: Toggle, Select: Select, Menu: Menu, MenuList: MenuList, SettingsRow: SettingsRow, SettingsDisclosure: SettingsDisclosure, GamepadHint: GamepadHint, NavBell: NavBell, Checkbox: Checkbox, Tag: Tag, Kbd: Kbd, HelpColumn: HelpColumn, Slider: Slider, Popover: Popover, IconButton: IconButton, MapPoint: MapPoint, VoiceMark: VoiceMark, StageDiagnostics: StageDiagnostics, HotkeySheet: HotkeySheet, Mark: Mark, Lockup: Lockup, AppIcon: AppIcon, ServerGlyph: ServerGlyph, HintRow: HintRow, ColumnNav: ColumnNav
  });
})();
