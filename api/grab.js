// /api/grab.js
import chromium from '@sparticuz/chromium';
import puppeteer from 'puppeteer-core';

const GOAT_RE = /goatembed\.com|\/goatembed\//i;

const extractVariants = (m3u) =>
  (m3u || '').split(/\r?\n/).map(s => s.trim()).filter(Boolean)
    .filter(l => !l.startsWith('#') && /^https?:\/\//i.test(l));

const norm = (s) => String(s || '').toLowerCase();
const isVttURL = (u) => /\.vtt(\?|$)/i.test(u);

function pushSubCandidate(arr, item) {
  if (!item?.url) return;
  if (!arr.find(x => x.url === item.url)) arr.push(item);
}
function pickVietnameseSubtitle(candidates) {
  if (!candidates.length) return null;
  const score = (c) => {
    const L = norm(c.label), U = norm(c.url);
    let s = 0;
    if (
      L.includes('tiếng việt') || L.includes('tieng viet') ||
      L.includes('vietsub')    || L.includes('vietnamese') ||
      L === 'vi' || L === 'vn' || L.includes(' viet ')
    ) s += 5;
    if (/-vi\.vtt|_vi\.vtt|\/vi\/|\/vn\/|\.vi\.vtt/i.test(U)) s += 4;
    if (/english|eng|spanish|spa|french|fr|german|de|thai|th|korean|ko|japanese|jp|ja|chinese|zh|cn/i.test(L+U)) s -= 6;
    return s;
  };
  let best = null;
  for (const c of candidates) {
    const sc = score(c);
    if (!best || sc > best.s) best = { ...c, s: sc };
  }
  return best && best.s > 0 ? best.url : null;
}
function extractTracksFromPlaylist(playlist) {
  const list = Array.isArray(playlist) ? playlist : [playlist].filter(Boolean);
  const out = [];
  for (const it of list) {
    if (Array.isArray(it?.tracks)) {
      for (const t of it.tracks) {
        const file = t?.file || '';
        if (isVttURL(file)) out.push({ url: String(file), label: String(t?.label || ''), kind: String(t?.kind || '') });
      }
    }
  }
  return out;
}

const stealthInit = `
(() => {
  try {
    Object.defineProperty(navigator, 'webdriver', { get: () => false });
    Object.defineProperty(navigator, 'languages', { get: () => ['vi-VN','vi','en-US','en'] });
    Object.defineProperty(navigator, 'platform',  { get: () => 'Win32' });
    Object.defineProperty(navigator, 'hardwareConcurrency', { get: () => 8 });
    Object.defineProperty(navigator, 'deviceMemory', { get: () => 8 });
    Object.defineProperty(navigator, 'plugins', { get: () => [{name:'Chrome PDF Plugin'}] });
    window.chrome = window.chrome || { runtime: {} };
    const oq = navigator.permissions?.query?.bind(navigator.permissions);
    if (oq) navigator.permissions.query = (p)=> p?.name==='notifications'? Promise.resolve({state:'default'}) : oq(p);
    const gp = WebGLRenderingContext.prototype.getParameter;
    WebGLRenderingContext.prototype.getParameter = function(p){
      if (p===37445) return 'Intel Inc.'; if (p===37446) return 'Intel(R) UHD'; return gp.call(this,p);
    };
  } catch {}
})();
`;

// Hook jwplayer trong mọi frame: đẩy playlist & tracks ra __captured
const jwHookInit = `
(() => {
  window.__captured = [];
  const push = (p) => { try { window.__captured.push(p); } catch {} };
  const isData = (s) => typeof s === 'string' && /^data:application\\/vnd\\.apple\\.mpegurl;base64,/i.test(s);
  const decode = (u) => { try { return atob(String(u).split(',')[1]); } catch { return null; } };
  const norm = (s) => String(s || '').toLowerCase();
  const isCaption = (t) => norm(t?.kind) === 'captions';
  const isVN = (label) => {
    const L = norm(label);
    return L.includes('tiếng việt') || L.includes('tieng viet') ||
           L.includes('vietsub')    || L.includes('vietnamese') ||
           L === 'vi' || L === 'vn' || L.includes(' viet ');
  };
  const packTracks = (arr) => {
    const out = [];
    if (!Array.isArray(arr)) return out;
    for (const t of arr) {
      const file = t?.file || '';
      const label = t?.label || '';
      const kind = t?.kind || '';
      if (file && /\.vtt(\\?|$)/i.test(file)) out.push({ file: String(file), label: String(label), kind: String(kind) });
    }
    return out;
  };
  function collectTracksFromCfg(cfg) {
    const found = [];
    if (cfg && Array.isArray(cfg.tracks)) found.push(...packTracks(cfg.tracks));
    const list = Array.isArray(cfg?.playlist) ? cfg.playlist : [cfg?.playlist].filter(Boolean);
    for (const it of list) if (Array.isArray(it?.tracks)) found.push(...packTracks(it.tracks));
    if (found.length) {
      const vi = found.find(t => isCaption(t) && isVN(t.label))?.file || null;
      push({ type: 'jw.tracks', tracks: found.slice(0, 8), vi });
    }
  }
  function handleM3UFrom(cfgOrItem) {
    try {
      const list = Array.isArray(cfgOrItem?.playlist) ? cfgOrItem.playlist : [cfgOrItem?.playlist].filter(Boolean);
      for (const it of list) for (const src of (it?.sources || [])) {
        const f = String(src.file || '');
        if (isData(f)) {
          const m3u = decode(f) || '';
          const variants = m3u.split(/\\r?\\n/).filter(l => !l.startsWith('#') && /^https?:\\/\\//i.test(l));
          push({ type:'jw.m3u', variants });
        }
      }
    } catch {}
  }
  function hookJW(){
    try{
      if (typeof window.jwplayer !== 'function') return false;
      const _jw = window.jwplayer;
      window.jwplayer = function(){
        const p = _jw.apply(this, arguments);
        try{
          p.on && p.on('captionsList', (e) => {
            try {
              const tracks = Array.isArray(e?.tracks) ? e.tracks : [];
              const packed = packTracks(tracks);
              const vi = packed.find(t => isCaption(t) && isVN(t.label))?.file || null;
              if (packed.length || vi) push({ type: 'jw.captionsList', tracks: packed.slice(0,8), vi });
            } catch {}
          });
          p.on && p.on('playlistItem', (e) => {
            try {
              const packed = packTracks(e?.item?.tracks || []);
              const vi = packed.find(t => isCaption(t) && isVN(t.label))?.file || null;
              if (packed.length || vi) push({ type: 'jw.playlistItem', tracks: packed.slice(0,8), vi });
            } catch {}
          });
          p.on && p.on('ready', () => {
            try {
              const list = (typeof p.getCaptionsList === 'function') ? p.getCaptionsList() : [];
              if (Array.isArray(list) && list.length) push({ type:'jw.getCaptionsList', list: list.slice(0,8) });
            } catch {}
          });
          const _setup = p.setup.bind(p);
          p.setup = function(cfg){
            try {
              if (cfg && cfg.playlist) push({ type: 'jw-setup-playlist', playlist: cfg.playlist });
              collectTracksFromCfg(cfg);
              handleM3UFrom(cfg);
            } catch {}
            return _setup(cfg);
          };
          const _load = p.load?.bind(p);
          if (_load){
            p.load = function(pl){
              try {
                push({ type: 'jw-load-playlist', playlist: pl });
                const fakeCfg = Array.isArray(pl) ? { playlist: pl } : { playlist: [pl] };
                collectTracksFromCfg(fakeCfg);
                handleM3UFrom(fakeCfg);
              } catch {}
              return _load(pl);
            };
          }
        }catch{}
        return p;
      };
      return true;
    }catch{ return false; }
  }
  const iv = setInterval(() => { if (hookJW()) clearInterval(iv); }, 50);
})();
`;

async function findGoatFrame(page, timeoutMs = 12000) {
  const t0 = Date.now();
  let last = null;
  while (Date.now() - t0 < timeoutMs) {
    for (const f of page.frames()) {
      const u = f.url();
      if (GOAT_RE.test(u)) {
        if (u !== last) last = u;
        return f;
      }
    }
    await page.waitForTimeout(250);
  }
  return null;
}
async function nudge(page) {
  try { await page.mouse.click(480, 360, { delay: 50 }); } catch {}
  const sels = ['button:has-text("Play")','button:has-text("Xem")','.btn-play','.btn-watch','.plyr__control','.btn-sv','.server-item','[data-server]','[data-embed]','[data-episode]'];
  for (const sel of sels) {
    try { const el = await page.$(sel); if (el) { await el.click({ delay: 40 }); await page.waitForTimeout(300); } } catch {}
  }
}

// Thu hoạch từ mọi frame
async function harvestFromAllFrames(page, masters, seenMasters, subtitleCandidates) {
  // 1) __captured trong từng frame
  for (const f of page.frames()) {
    try {
      const cap = await f.evaluate(() => window.__captured || []);
      for (const c of cap) {
        if (c?.type === 'jw-setup-playlist' || c?.type === 'jw-load-playlist') {
          const found = extractTracksFromPlaylist(c.playlist);
          for (const t of found) pushSubCandidate(subtitleCandidates, { ...t, source: c.type });
        }
        if (c?.type === 'jw.tracks' || c?.type === 'jw.playlistItem' || c?.type === 'jw.captionsList') {
          if (Array.isArray(c.tracks)) {
            for (const t of c.tracks) {
              if (t?.file && isVttURL(t.file)) pushSubCandidate(subtitleCandidates, { url: t.file, label: t.label || '', kind: t.kind || '', source: c.type });
            }
          }
          if (c.vi) pushSubCandidate(subtitleCandidates, { url: c.vi, label: 'Tiếng Việt(?)', kind: 'captions', source: c.type });
        }
        if (c?.type === 'jw.m3u' && Array.isArray(c.variants) && c.variants.length && !seenMasters.has('jw')) {
          masters.push({ url: '(from data URL)', source: 'jw.dataurl', variantCount: c.variants.length, host: null });
          seenMasters.add('jw');
        }
      }
    } catch {}
  }
  // 2) fallback DOM <track>
  for (const f of page.frames()) {
    try {
      const tracks = await f.evaluate(() => {
        const out = [];
        for (const el of Array.from(document.querySelectorAll('track'))) {
          const src = el.getAttribute('src') || '';
          const label = el.getAttribute('label') || '';
          const kind  = el.getAttribute('kind') || '';
          if (src && /\.vtt(\?|$)/i.test(src)) out.push({ url: new URL(src, location.href).toString(), label, kind });
        }
        return out;
      });
      for (const t of tracks) pushSubCandidate(subtitleCandidates, { ...t, source: 'dom.track' });
    } catch {}
  }
}

export default async function handler(req, res) {
  // CORS nhẹ để gọi từ browser nếu cần
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
  if (req.method === 'OPTIONS') return res.status(204).end();

  const url = String(req.query.url || '');
  const collectAll = req.query.all === '1';
  if (!/^https?:\/\//i.test(url)) {
    return res.status(400).json({ ok:false, error:'Missing ?url=' });
  }

  let browser = null;
  try {
    // cấu hình chromium cho Lambda
    chromium.setHeadlessMode = true;
    chromium.setGraphicsMode = false;

    browser = await puppeteer.launch({
      args: chromium.args,
      executablePath: await chromium.executablePath(),
      headless: chromium.headless
    });

    const page = await browser.newPage();
    await page.setUserAgent(
      'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 ' +
      '(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36'
    );
    await page.setExtraHTTPHeaders({ 'Accept-Language': 'vi-VN,vi;q=0.9,en-US;q=0.8' });
    await page.evaluateOnNewDocument(stealthInit);
    await page.evaluateOnNewDocument(jwHookInit);

    const masters = [];
    const seenMasters = new Set();
    const subCandidates = [];

    // Network listener (toàn trang)
    page.on('response', async (resp) => {
      try {
        const rurl = resp.url();
        const ct = resp.headers()['content-type'] || '';

        // master m3u8
        const isM3U = /application\/vnd\.apple\.mpegurl/i.test(ct) || /\.m3u8(\?|$)/i.test(rurl);
        if (isM3U) {
          const text = (await resp.buffer()).toString('utf8');
          if (/#EXT-X-STREAM-INF/i.test(text)) {
            const variants = extractVariants(text);
            if (variants.length && !seenMasters.has(rurl)) {
              masters.push({ url: rurl, source: 'network.m3u8', variantCount: variants.length, host: new URL(rurl).host });
              seenMasters.add(rurl);
            }
          }
        }

        // JSON goat: có thể chứa playlist base64 + tracks[]
        const looksJson = /application\/json/i.test(ct) || /\/v1\/player\/vvv\//i.test(rurl);
        if (looksJson && GOAT_RE.test(rurl)) {
          const raw = (await resp.buffer()).toString('utf8');
          try {
            const j = JSON.parse(raw);
            if (j && typeof j.playlist === 'string') {
              const m3u = Buffer.from(j.playlist, 'base64').toString('utf8');
              const variants = extractVariants(m3u);
              if (variants.length && !seenMasters.has('json:'+rurl)) {
                masters.push({ url: '(from JSON)', source: 'network.json.playlist', variantCount: variants.length, host: new URL(rurl).host });
                seenMasters.add('json:'+rurl);
              }
            }
            if (Array.isArray(j?.tracks)) {
              for (const t of j.tracks) {
                const f = t?.file || '';
                if (isVttURL(f)) pushSubCandidate(subCandidates, { url: f, label: t?.label || '', kind: t?.kind || '', source: 'json.tracks' });
              }
            }
          } catch {}
        }

        // bắt trực tiếp .vtt (nếu site load khi bật CC)
        if (isVttURL(rurl) || /text\/vtt/i.test(ct)) {
          pushSubCandidate(subCandidates, { url: rurl, label: '', kind: '', source: 'network.vtt' });
        }
      } catch {}
    });

    // Điều hướng
    await page.goto(url, { waitUntil: 'networkidle2', timeout: 60000 });
    await nudge(page);

    const goatFrame = await findGoatFrame(page, 15000);
    if (goatFrame) {
      // không có waitForLoadState như Playwright; rely on global networkidle + harvest polling
    }

    const MAX_WAIT_MS = collectAll ? 30000 : 18000;
    const t0 = Date.now();
    let lastMasters = 0;
    while (Date.now() - t0 < MAX_WAIT_MS) {
      // Thu hoạch từ MỌI frame
      await harvestFromAllFrames(page, masters, seenMasters, subCandidates);

      if ((masters.length && !collectAll) || subCandidates.length) break;
      if (masters.length && masters.length === lastMasters && collectAll) break;

      lastMasters = masters.length;
      await page.waitForTimeout(250);
    }

    const primary = masters.find(m => m.url && m.url.startsWith('http'))?.url || null;
    const subtitleVI = pickVietnameseSubtitle(subCandidates);

    return res.json({
      ok: !!(primary || subtitleVI),
      embed: page.frames().find(f => GOAT_RE.test(f.url()))?.url() || null,
      primary,
      subtitleVI,
      tracksSample: subCandidates.slice(0, 6),
      masters
    });

  } catch (e) {
    return res.status(500).json({ ok:false, error: String(e) });
  } finally {
    try { await browser?.close(); } catch {}
  }
}
