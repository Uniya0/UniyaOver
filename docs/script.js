(() => {
  "use strict";

  document.documentElement.classList.add("js");
  const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  /* ---------------- i18n ---------------- */
  const I18N = {
    ru: {
      "meta.title": "UniyaOver — Instant Replay для Windows",
      "nav.how": "Как работает", "nav.features": "Возможности", "nav.hotkeys": "Хоткеи", "nav.faq": "FAQ", "nav.download": "Скачать",
      "hero.badge": "v1.1 · Windows · NVIDIA NVENC",
      "hero.title": "Лучший момент<br><span>уже записан.</span>",
      "hero.sub": "UniyaOver тихо держит в памяти последние секунды экрана. Нажал хоткей — и клип уже лежит в MP4. Без аккаунта, без облака, без лишнего.",
      "hero.cta": "Скачать для Windows",
      "hero.meta": "Бесплатно · ~125 MB · Windows 10/11 x64",
      "app.home": "Главная", "app.library": "Библиотека", "app.settings": "Настройки",
      "app.desc": "Последние {n} секунд экрана — по горячей клавише.",
      "app.recent": "Последние записи",
      "hk.arm.short": "старт буфера", "hk.save.short": "сохранить клип", "hk.region.short": "область", "hk.stop.short": "стоп буфера",
      "stat.fps": "FPS максимум", "stat.buffer": "длина буфера", "stat.codec": "на NVENC", "stat.accounts": "аккаунтов и облаков",
      "how.eyebrow": "Как работает",
      "how.title": "Кольцевой буфер.<br>Ты решаешь, что сохранить.",
      "how.sub": "Экран пишется непрерывно, но на диск попадает только то, что ты попросил. Старые секунды просто перезаписываются.",
      "demo.buffer": "Длина буфера", "demo.past": "− 2 мин · перезаписано", "demo.now": "сейчас",
      "demo.save": "Сохранить клип", "demo.hint": "или нажми это сочетание прямо на странице",
      "step1.t": "Запусти буфер", "step1.d": "Ctrl+Shift+R — UniyaOver начинает держать последние секунды экрана и звука. Статус меняется на BUFFERING.",
      "step2.t": "Играй как обычно", "step2.d": "Кодирование идёт на NVENC — отдельном блоке видеокарты. Приложение сидит в трее и не мешает.",
      "step3.t": "Случилось что-то крутое?", "step3.d": "Ctrl+Shift+X — готовый MP4 со звуком появляется в библиотеке. Без пост-обработки и ожидания.",
      "feat.eyebrow": "Возможности", "feat.title": "Всё нужное.<br>Ничего лишнего.",
      "f.region.t": "Replay области", "f.region.d": "Ctrl+Shift+A — кадр замирает, выделяешь нужный кусок экрана, и в клип попадает только он. Звук и тайминг сохраняются точно.",
      "f.instant.t": "Мгновенное сохранение", "f.instant.d": "Буфер уже закодирован — клип пишется на диск за секунды, без перекодирования.",
      "f.nvenc.t": "NVIDIA NVENC", "f.nvenc.d": "H.264 или HEVC — выбирается автоматически под твою видеокарту. До 240 FPS.",
      "f.audio.t": "Звук системы", "f.audio.d": "WASAPI-захват выбранного устройства → AAC. Видео и звук идут синхронно.",
      "f.library.t": "Библиотека с превью", "f.library.d": "Все клипы в одном месте, с миниатюрами. Клик — и видео открывается.",
      "f.offline.t": "Полностью offline", "f.offline.d": "Ни аккаунта, ни телеметрии, ни загрузок в облако. Клипы остаются только на твоём диске.",
      "f.tune.t": "Настраивается под тебя", "f.tune.d": "Монитор, устройство звука, FPS, битрейт, кодек, длина буфера и любые хоткеи. UniyaOver сам предлагает лимиты под твоё железо.",
      "tv.fps": "FPS", "tv.codec": "Кодек", "tv.auto": "Авто", "tv.buffer": "Буфер", "tv.bitrate": "Битрейт",
      "hk.eyebrow": "Хоткеи", "hk.title": "Четыре сочетания.<br>Запоминаются за минуту.",
      "hk.sub": "Попробуй нажать их прямо сейчас. Любое сочетание можно поменять в настройках.",
      "hk.arm.t": "Старт буфера", "hk.arm.d": "Начинает держать последние секунды",
      "hk.save.t": "Сохранить клип", "hk.save.d": "Весь экран → MP4 в библиотеку",
      "hk.region.t": "Сохранить область", "hk.region.d": "Заморозить кадр и выделить кусок",
      "hk.stop.t": "Стоп буфера", "hk.stop.d": "Освобождает видеокарту и память",
      "req.eyebrow": "Требования", "req.title": "Что нужно для запуска",
      "req.os": "Windows 10 или 11, 64-бит",
      "req.gpu": "Видеокарта NVIDIA с NVENC (GeForce GTX / RTX)",
      "req.vc": "<a href=\"https://aka.ms/vs/17/release/vc_redist.x64.exe\" target=\"_blank\" rel=\"noopener\">Visual C++ Redistributable x64</a>",
      "req.disk": "~130 MB на диске + место под клипы",
      "honest.eyebrow": "Честно", "honest.title": "Что стоит знать заранее",
      "honest.amd.t": "AMD и Intel пока нет.", "honest.amd.d": "Без NVENC запись не стартует — приложение честно скажет об этом при запуске.",
      "honest.ac.t": "Соревновательный античит.", "honest.ac.d": "Совместимость с Valorant и подобными не гарантируется. Используй на свой риск.",
      "honest.ss.t": "SmartScreen.", "honest.ss.d": "Сборка пока без цифровой подписи, Windows может предупредить. «Подробнее» → «Выполнить в любом случае».",
      "faq.title": "Частые вопросы",
      "faq.q1": "Это бесплатно?", "faq.a1": "Да. Скачать и пользоваться можно бесплатно. Если хочешь поддержать разработку — можно задонатить на itch.io.",
      "faq.q2": "Чем это отличается от NVIDIA App / ShadowPlay?", "faq.a2": "UniyaOver — маленький и сфокусированный: только Instant Replay, без оверлея, аккаунта и фоновых сервисов. Плюс есть сохранение выбранной области экрана.",
      "faq.q3": "Сильно ли падает FPS в играх?", "faq.a3": "Кодирует NVENC — отдельный аппаратный блок видеокарты, поэтому нагрузка на игру минимальна. На слабых картах лучше выбрать 60 FPS и умеренный битрейт.",
      "faq.q4": "Где лежат клипы?", "faq.a4": "По умолчанию в «Видео\\instant-replay». Папку можно поменять в настройках.",
      "faq.q5": "Как удалить?", "faq.a5": "Через «Приложения» в Windows или Uninstall.exe в папке установки. Клипы при этом не удаляются.",
      "final.title": "Не упусти следующий момент.", "final.sub": "Поставь один раз — дальше UniyaOver просто тихо работает в трее.",
      "final.all": "Все версии и заметки к релизам →",
      "footer.license": "Лицензия",
      "footer.note": "FFmpeg используется по лицензии LGPL. NVIDIA и NVENC — товарные знаки NVIDIA Corporation; проект с ней не связан.",
      "clip.just": "только что", "clip.ago": "{n} мин назад", "clip.region": "область",
      "toast.saved": "Клип сохранён · {name}",
      "toast.region": "Кадр заморожен — выдели область",
      "toast.arm": "Буфер запущен · BUFFERING",
      "toast.stop": "Буфер остановлен · IDLE",
      "toast.idle": "Буфер не запущен — сначала Ctrl+Shift+R",
    },
    en: {
      "meta.title": "UniyaOver — Instant Replay for Windows",
      "nav.how": "How it works", "nav.features": "Features", "nav.hotkeys": "Hotkeys", "nav.faq": "FAQ", "nav.download": "Download",
      "hero.badge": "v1.1 · Windows · NVIDIA NVENC",
      "hero.title": "Your best moment<br><span>is already saved.</span>",
      "hero.sub": "UniyaOver quietly keeps the last seconds of your screen in memory. Hit a hotkey and the clip is already an MP4. No account, no cloud, no bloat.",
      "hero.cta": "Download for Windows",
      "hero.meta": "Free · ~125 MB · Windows 10/11 x64",
      "app.home": "Home", "app.library": "Library", "app.settings": "Settings",
      "app.desc": "The last {n} seconds of your screen — on a hotkey.",
      "app.recent": "Recent clips",
      "hk.arm.short": "start buffer", "hk.save.short": "save clip", "hk.region.short": "region", "hk.stop.short": "stop buffer",
      "stat.fps": "FPS max", "stat.buffer": "buffer length", "stat.codec": "on NVENC", "stat.accounts": "accounts or clouds",
      "how.eyebrow": "How it works",
      "how.title": "A ring buffer.<br>You decide what to keep.",
      "how.sub": "Your screen is recorded continuously, but only what you ask for hits the disk. Older seconds are simply overwritten.",
      "demo.buffer": "Buffer length", "demo.past": "− 2 min · overwritten", "demo.now": "now",
      "demo.save": "Save clip", "demo.hint": "or press this combo right on the page",
      "step1.t": "Start the buffer", "step1.d": "Ctrl+Shift+R — UniyaOver starts holding the last seconds of screen and audio. Status switches to BUFFERING.",
      "step2.t": "Play as usual", "step2.d": "Encoding runs on NVENC, a dedicated block on your GPU. The app lives in the tray and stays out of the way.",
      "step3.t": "Something epic happened?", "step3.d": "Ctrl+Shift+X — a finished MP4 with audio lands in your library. No post-processing, no waiting.",
      "feat.eyebrow": "Features", "feat.title": "Everything you need.<br>Nothing you don’t.",
      "f.region.t": "Region replay", "f.region.d": "Ctrl+Shift+A — the frame freezes, you select a part of the screen, and only that goes into the clip. Audio and timing stay exact.",
      "f.instant.t": "Instant save", "f.instant.d": "The buffer is already encoded, so the clip is written to disk in seconds with no re-encoding.",
      "f.nvenc.t": "NVIDIA NVENC", "f.nvenc.d": "H.264 or HEVC, picked automatically for your GPU. Up to 240 FPS.",
      "f.audio.t": "System audio", "f.audio.d": "WASAPI capture of the device you choose → AAC. Video and audio stay in sync.",
      "f.library.t": "Library with previews", "f.library.d": "All clips in one place, with thumbnails. One click opens the video.",
      "f.offline.t": "Fully offline", "f.offline.d": "No account, no telemetry, no cloud uploads. Your clips stay on your drive only.",
      "f.tune.t": "Tuned to you", "f.tune.d": "Monitor, audio device, FPS, bitrate, codec, buffer length and any hotkeys. UniyaOver suggests limits that fit your hardware.",
      "tv.fps": "FPS", "tv.codec": "Codec", "tv.auto": "Auto", "tv.buffer": "Buffer", "tv.bitrate": "Bitrate",
      "hk.eyebrow": "Hotkeys", "hk.title": "Four combos.<br>Learned in a minute.",
      "hk.sub": "Try pressing them right now. Every combo can be changed in Settings.",
      "hk.arm.t": "Start buffer", "hk.arm.d": "Begins holding the last seconds",
      "hk.save.t": "Save clip", "hk.save.d": "Full screen → MP4 in your library",
      "hk.region.t": "Save region", "hk.region.d": "Freeze the frame and select an area",
      "hk.stop.t": "Stop buffer", "hk.stop.d": "Frees up GPU and memory",
      "req.eyebrow": "Requirements", "req.title": "What you need",
      "req.os": "Windows 10 or 11, 64-bit",
      "req.gpu": "NVIDIA GPU with NVENC (GeForce GTX / RTX)",
      "req.vc": "<a href=\"https://aka.ms/vs/17/release/vc_redist.x64.exe\" target=\"_blank\" rel=\"noopener\">Visual C++ Redistributable x64</a>",
      "req.disk": "~130 MB of disk space + room for clips",
      "honest.eyebrow": "Honestly", "honest.title": "Good to know upfront",
      "honest.amd.t": "No AMD or Intel yet.", "honest.amd.d": "Recording won’t start without NVENC, and the app tells you so on launch.",
      "honest.ac.t": "Competitive anti-cheat.", "honest.ac.d": "Compatibility with Valorant and similar games isn’t guaranteed. Use at your own risk.",
      "honest.ss.t": "SmartScreen.", "honest.ss.d": "The build isn’t code-signed yet, so Windows may warn you. Click “More info” → “Run anyway”.",
      "faq.title": "FAQ",
      "faq.q1": "Is it free?", "faq.a1": "Yes, it’s free to download and use. If you’d like to support development, you can donate on itch.io.",
      "faq.q2": "How is it different from NVIDIA App / ShadowPlay?", "faq.a2": "UniyaOver is small and focused: just Instant Replay, with no overlay, account or background services. It can also save a selected region of the screen.",
      "faq.q3": "Does it hurt in-game FPS?", "faq.a3": "Encoding runs on NVENC, a separate hardware block on the GPU, so the impact on games is minimal. On weaker cards, pick 60 FPS and a moderate bitrate.",
      "faq.q4": "Where are my clips?", "faq.a4": "By default in “Videos\\instant-replay”. You can change the folder in Settings.",
      "faq.q5": "How do I uninstall?", "faq.a5": "From Windows “Apps” or with Uninstall.exe in the install folder. Your clips are not deleted.",
      "final.title": "Don’t miss the next moment.", "final.sub": "Install it once and UniyaOver quietly does its thing from the tray.",
      "final.all": "All versions and release notes →",
      "footer.license": "License",
      "footer.note": "FFmpeg is used under the LGPL. NVIDIA and NVENC are trademarks of NVIDIA Corporation; this project is not affiliated with it.",
      "clip.just": "just now", "clip.ago": "{n} min ago", "clip.region": "region",
      "toast.saved": "Clip saved · {name}",
      "toast.region": "Frame frozen — select a region",
      "toast.arm": "Buffer started · BUFFERING",
      "toast.stop": "Buffer stopped · IDLE",
      "toast.idle": "Buffer isn’t running — press Ctrl+Shift+R first",
    },
  };

  const HTML_KEYS = new Set(["hero.title", "how.title", "feat.title", "hk.title", "req.vc"]);
  let lang = "ru";
  let bufferSec = 30;

  const t = (key, vars) => {
    let s = (I18N[lang] && I18N[lang][key]) ?? I18N.ru[key] ?? key;
    if (vars) for (const k in vars) s = s.replace(`{${k}}`, vars[k]);
    return s;
  };

  function applyLang(next) {
    lang = I18N[next] ? next : "en";
    document.documentElement.lang = lang;
    document.title = t("meta.title");
    document.querySelectorAll("[data-i18n]").forEach((el) => {
      const key = el.getAttribute("data-i18n");
      if (key === "app.desc") { el.textContent = t(key, { n: bufferSec }); return; }
      if (HTML_KEYS.has(key)) el.innerHTML = t(key);
      else el.textContent = t(key);
    });
    document.querySelectorAll(".lang button").forEach((b) => b.classList.toggle("is-active", b.dataset.lang === lang));
    refreshClipTimes();
    try { localStorage.setItem("uo-lang", lang); } catch (_) {}
  }

  document.querySelectorAll(".lang button").forEach((b) => b.addEventListener("click", () => applyLang(b.dataset.lang)));

  let initial = "en";
  try { initial = localStorage.getItem("uo-lang") || ""; } catch (_) {}
  if (!initial) initial = /^(ru|uk|be|kk)\b/i.test(navigator.language || "") ? "ru" : "en";

  /* ---------------- Nav / glow ---------------- */
  const nav = document.querySelector(".nav");
  const onScroll = () => nav.classList.toggle("is-scrolled", window.scrollY > 8);
  window.addEventListener("scroll", onScroll, { passive: true });
  onScroll();

  const glow = document.querySelector(".bg-glow");
  window.addEventListener("pointermove", (e) => {
    glow.style.setProperty("--gx", `${(e.clientX / window.innerWidth) * 100}%`);
    glow.style.setProperty("--gy", `${(e.clientY / window.innerHeight) * 100}%`);
  }, { passive: true });

  document.querySelectorAll(".card").forEach((card) => {
    card.addEventListener("pointermove", (e) => {
      const r = card.getBoundingClientRect();
      card.style.setProperty("--mx", `${e.clientX - r.left}px`);
      card.style.setProperty("--my", `${e.clientY - r.top}px`);
    });
  });

  /* ---------------- Reveal ---------------- */
  const io = new IntersectionObserver((entries) => {
    entries.forEach((en) => {
      if (!en.isIntersecting) return;
      const el = en.target;
      const siblings = [...el.parentElement.children].filter((c) => c.classList.contains("reveal"));
      const idx = Math.max(0, siblings.indexOf(el));
      el.style.transitionDelay = `${Math.min(idx, 6) * 70}ms`;
      el.classList.add("is-in");
      io.unobserve(el);
    });
  }, { threshold: 0.12, rootMargin: "0px 0px -40px 0px" });
  document.querySelectorAll(".reveal").forEach((el) => io.observe(el));

  /* ---------------- Toast ---------------- */
  const toastEl = document.getElementById("toast");
  let toastTimer = 0;
  function toast(msg) {
    toastEl.textContent = msg;
    toastEl.classList.add("is-on");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => toastEl.classList.remove("is-on"), 2400);
  }

  /* ---------------- Scene noise ---------------- */
  const hash = (n) => {
    const x = Math.sin(n * 127.1 + 311.7) * 43758.5453;
    return x - Math.floor(x);
  };
  const noise = (x) => {
    const i = Math.floor(x), f = x - i;
    const u = f * f * (3 - 2 * f);
    return hash(i) * (1 - u) + hash(i + 1) * u;
  };
  const lum = (sec) => 0.25 + 0.5 * noise(sec / 6) + 0.25 * noise(sec / 1.3 + 50);
  const amp = (sec) => {
    const base = 0.15 + 0.55 * noise(sec / 3 + 200);
    const burst = noise(sec / 9 + 400) > 0.72 ? 0.35 : 0;
    return Math.min(1, base + burst) * (0.55 + 0.45 * hash(Math.floor(sec * 8)));
  };

  /* ---------------- Thumbnails ---------------- */
  function drawThumb(canvas, seed) {
    const w = (canvas.width = 176), h = (canvas.height = 99);
    const g = canvas.getContext("2d");
    const l0 = lum(seed), l1 = lum(seed + 7);
    const top = Math.round(20 + l0 * 60), bot = Math.round(10 + l1 * 30);
    const sky = g.createLinearGradient(0, 0, 0, h);
    sky.addColorStop(0, `rgb(${top},${top},${top})`);
    sky.addColorStop(1, `rgb(${bot},${bot},${bot})`);
    g.fillStyle = sky; g.fillRect(0, 0, w, h);

    const sx = w * (0.2 + 0.6 * hash(seed + 1)), sy = h * (0.2 + 0.3 * hash(seed + 2));
    const sun = g.createRadialGradient(sx, sy, 0, sx, sy, 40);
    sun.addColorStop(0, "rgba(255,255,255,0.75)"); sun.addColorStop(1, "rgba(255,255,255,0)");
    g.fillStyle = sun; g.fillRect(0, 0, w, h);

    g.fillStyle = "rgba(0,0,0,0.55)";
    g.beginPath(); g.moveTo(0, h);
    for (let x = 0; x <= w; x += 8) g.lineTo(x, h * 0.62 + (noise(seed * 3 + x / 22) - 0.5) * 30);
    g.lineTo(w, h); g.closePath(); g.fill();

    g.fillStyle = "rgba(0,0,0,0.8)";
    g.beginPath(); g.moveTo(0, h);
    for (let x = 0; x <= w; x += 6) g.lineTo(x, h * 0.8 + (noise(seed * 5 + x / 12 + 90) - 0.5) * 16);
    g.lineTo(w, h); g.closePath(); g.fill();

    g.strokeStyle = "rgba(255,255,255,0.7)"; g.lineWidth = 1.2;
    const cx = w / 2, cy = h / 2;
    g.beginPath(); g.moveTo(cx - 5, cy); g.lineTo(cx + 5, cy); g.moveTo(cx, cy - 5); g.lineTo(cx, cy + 5); g.stroke();
  }

  const pad = (n) => String(n).padStart(2, "0");
  const clipName = (d) => `clip_${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}_${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}.mp4`;
  const fmtDur = (s) => `${Math.floor(s / 60)}:${pad(s % 60)}`;

  const clips = [];
  function clipEl(c, wide) {
    const el = document.createElement("div");
    el.className = "clip";
    const th = document.createElement("div");
    th.className = "clip-thumb";
    const cv = document.createElement("canvas");
    drawThumb(cv, c.seed);
    const em = document.createElement("em");
    em.textContent = fmtDur(c.dur);
    th.append(cv, em);
    const info = document.createElement("div");
    info.style.minWidth = "0";
    const name = document.createElement("div");
    name.className = "clip-name"; name.textContent = c.name;
    const meta = document.createElement("div");
    meta.className = "clip-meta"; meta.dataset.at = String(c.at); meta.dataset.dur = String(c.dur);
    meta.dataset.region = c.region ? "1" : "";
    meta.textContent = clipMeta(c.at, c.dur, c.region);
    info.append(name, meta);
    el.append(th, info);
    if (!wide) el.style.animationDelay = "0.05s";
    return el;
  }
  function clipMeta(at, dur, region) {
    const mins = Math.floor((Date.now() - at) / 60000);
    const when = mins < 1 ? t("clip.just") : t("clip.ago", { n: mins });
    return `${dur} s${region ? ` · ${t("clip.region")}` : ""} · ${when}`;
  }
  function refreshClipTimes() {
    document.querySelectorAll(".clip-meta").forEach((m) => {
      m.textContent = clipMeta(Number(m.dataset.at), Number(m.dataset.dur), !!m.dataset.region);
    });
  }
  setInterval(refreshClipTimes, 30000);

  const mockList = document.getElementById("mockClips");
  const demoList = document.getElementById("demoClips");

  function addClip({ toDemo = true, silent = false, region = false, at = Date.now(), seed } = {}) {
    const c = { at, dur: bufferSec, seed: seed ?? (performance.now() / 1000 + Math.random() * 1000), region, name: clipName(new Date(at)) };
    clips.unshift(c);
    mockList.prepend(clipEl(c, false));
    while (mockList.children.length > 4) mockList.lastElementChild.remove();
    if (toDemo) {
      demoList.prepend(clipEl(c, true));
      while (demoList.children.length > 4) demoList.lastElementChild.remove();
    }
    if (!silent) {
      flashMock();
      toast(t("toast.saved", { name: c.name }));
    }
    return c;
  }

  const mockFlash = document.getElementById("mockFlash");
  const mockSaveRow = document.getElementById("mockSaveRow");
  function flashMock() {
    mockFlash.classList.remove("is-on"); void mockFlash.offsetWidth; mockFlash.classList.add("is-on");
    mockSaveRow.classList.add("is-flash");
    setTimeout(() => mockSaveRow.classList.remove("is-flash"), 650);
  }

  /* ---------------- Buffer state + timer ---------------- */
  let armed = true;
  let armedAt = Date.now();
  const timerEl = document.getElementById("mockTimer");
  const statusLabel = document.querySelector(".app-status .status-label");
  const statusDot = document.querySelector(".app-status .status-dot");
  function setArmed(on) {
    armed = on;
    armedAt = Date.now();
    statusLabel.textContent = on ? "BUFFERING" : "IDLE";
    statusDot.style.animationPlayState = on ? "running" : "paused";
    statusDot.style.opacity = on ? "" : "0.3";
    document.querySelectorAll(".ring-run").forEach((r) => (r.style.animationPlayState = on ? "running" : "paused"));
  }
  setInterval(() => {
    if (!armed) { timerEl.textContent = "--:--"; return; }
    const s = Math.floor((Date.now() - armedAt) / 1000);
    timerEl.textContent = `${pad(Math.floor(s / 60) % 100)}:${pad(s % 60)}`;
  }, 500);

  /* ---------------- Timeline canvas ---------------- */
  const cv = document.getElementById("timeline");
  const ctx = cv.getContext("2d");
  const WINDOW_SEC = 120;
  let W = 0, H = 0, dpr = 1;
  let saveFx = 0;
  const t0 = performance.now();

  function resize() {
    dpr = Math.min(window.devicePixelRatio || 1, 2);
    const r = cv.getBoundingClientRect();
    W = r.width; H = r.height;
    cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }
  window.addEventListener("resize", resize);
  resize();

  function roundRect(x, y, w, h, r) {
    ctx.beginPath();
    ctx.moveTo(x + r, y); ctx.arcTo(x + w, y, x + w, y + h, r); ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r); ctx.arcTo(x, y, x + w, y, r); ctx.closePath();
  }

  function draw(now) {
    const sec = (now - t0) / 1000;
    const pxPerSec = W / WINDOW_SEC;
    const step = 4;
    const vTop = 26, vH = H * 0.46, aMid = vTop + vH + (H - vTop - vH) / 2 + 2, aH = (H - vTop - vH) * 0.38;
    const bufW = (bufferSec / WINDOW_SEC) * W;
    const bufX = W - bufW;

    ctx.clearRect(0, 0, W, H);

    const offset = (sec * pxPerSec) % step;
    for (let x = W - offset; x > -step; x -= step) {
      const tsec = sec - (W - x) / pxPerSec;
      const inBuf = armed && x >= bufX;
      const L = lum(tsec);
      const c = Math.round(20 + Math.pow(L, 1.6) * 220 + hash(Math.floor(tsec * 4)) * 18);
      ctx.globalAlpha = inBuf ? 1 : 0.22;
      ctx.fillStyle = `rgb(${c},${c},${c})`;
      ctx.fillRect(x, vTop, step - 1, vH);

      const a = amp(tsec) * aH;
      ctx.fillStyle = inBuf ? "#e8e8e8" : "#9a9a9a";
      ctx.fillRect(x, aMid - a, step - 1.5, a * 2);
    }
    ctx.globalAlpha = 1;

    const fade = ctx.createLinearGradient(0, 0, 80, 0);
    fade.addColorStop(0, "rgba(10,10,10,1)"); fade.addColorStop(1, "rgba(10,10,10,0)");
    ctx.fillStyle = fade; ctx.fillRect(0, 0, 80, H);

    if (armed) {
      ctx.strokeStyle = "rgba(255,255,255,0.9)";
      ctx.lineWidth = 1.5;
      roundRect(bufX + 0.75, 14, bufW - 1.5, H - 20, 8);
      ctx.stroke();

      ctx.font = `600 11px ${getComputedStyle(document.body).getPropertyValue("--mono") || "monospace"}`;
      const label = `${bufferSec} s`;
      const lw = ctx.measureText(label).width + 12;
      const lx = Math.max(6, bufX + 8);
      ctx.fillStyle = "#e8e8e8";
      roundRect(lx, 5, lw, 17, 5); ctx.fill();
      ctx.fillStyle = "#0b0b0b";
      ctx.fillText(label, lx + 6, 17.5);

      if (saveFx > 0) {
        ctx.fillStyle = `rgba(255,255,255,${saveFx * 0.55})`;
        roundRect(bufX, 14, bufW, H - 20, 8); ctx.fill();
        saveFx = Math.max(0, saveFx - 0.035);
      }
    }

    ctx.fillStyle = "#fff";
    ctx.shadowColor = "rgba(255,255,255,0.9)"; ctx.shadowBlur = 12;
    ctx.fillRect(W - 2, 0, 2, H);
    ctx.shadowBlur = 0;

    if (!reduceMotion) requestAnimationFrame(draw);
  }
  requestAnimationFrame(draw);

  /* ---------------- Buffer slider ---------------- */
  const range = document.getElementById("bufRange");
  const out = document.getElementById("bufOut");
  const appDesc = document.querySelector('[data-i18n="app.desc"]');
  function onRange() {
    bufferSec = Number(range.value);
    out.textContent = `${bufferSec} s`;
    range.style.setProperty("--p", `${((bufferSec - 5) / (120 - 5)) * 100}%`);
    appDesc.textContent = t("app.desc", { n: bufferSec });
    if (reduceMotion) requestAnimationFrame(draw);
  }
  range.addEventListener("input", onRange);

  /* ---------------- Actions ---------------- */
  let userInteracted = false;
  function doSave(region = false) {
    userInteracted = true;
    if (!armed) { toast(t("toast.idle")); return; }
    saveFx = 1;
    if (reduceMotion) requestAnimationFrame(draw);
    addClip({ region });
  }
  document.getElementById("saveBtn").addEventListener("click", () => doSave(false));

  const keyEls = {
    Control: document.querySelector('.key[data-key="Control"]'),
    Shift: document.querySelector('.key[data-key="Shift"]'),
  };
  const comboCards = {};
  document.querySelectorAll(".keycard").forEach((kc) => {
    comboCards[kc.dataset.combo] = kc;
    kc.style.cursor = "pointer";
    kc.addEventListener("click", () => runCombo(kc.dataset.combo));
  });

  function hitCard(k) {
    const kc = comboCards[k];
    if (!kc) return;
    const key = kc.querySelector(".key");
    kc.classList.add("is-hit"); key.classList.add("is-down");
    keyEls.Control.classList.add("is-down"); keyEls.Shift.classList.add("is-down");
    setTimeout(() => {
      kc.classList.remove("is-hit"); key.classList.remove("is-down");
      keyEls.Control.classList.remove("is-down"); keyEls.Shift.classList.remove("is-down");
    }, 260);
  }

  function runCombo(k) {
    hitCard(k);
    if (k === "x") doSave(false);
    else if (k === "a") {
      userInteracted = true;
      if (!armed) { toast(t("toast.idle")); return; }
      toast(t("toast.region"));
      setTimeout(() => doSave(true), 900);
    } else if (k === "r") { userInteracted = true; if (!armed) setArmed(true); toast(t("toast.arm")); }
    else if (k === "s") { userInteracted = true; setArmed(false); toast(t("toast.stop")); }
  }

  window.addEventListener("keydown", (e) => {
    if (e.key === "Control") keyEls.Control.classList.add("is-down");
    if (e.key === "Shift") keyEls.Shift.classList.add("is-down");
    if (!(e.ctrlKey && e.shiftKey) || e.altKey || e.metaKey || e.repeat) return;
    const k = (e.code || "").replace("Key", "").toLowerCase();
    if (!["r", "x", "a", "s"].includes(k)) return;
    e.preventDefault();
    runCombo(k);
  });
  window.addEventListener("keyup", (e) => {
    if (e.key === "Control") keyEls.Control.classList.remove("is-down");
    if (e.key === "Shift") keyEls.Shift.classList.remove("is-down");
  });
  window.addEventListener("blur", () => {
    keyEls.Control.classList.remove("is-down"); keyEls.Shift.classList.remove("is-down");
  });

  /* ---------------- Region card animation ---------------- */
  const sel = document.querySelector(".rv-sel");
  const selScreen = document.querySelector(".rv-screen");
  const selSize = document.querySelector(".rv-size");
  const presets = [
    { x: 22, y: 22, w: 52, h: 54 },
    { x: 48, y: 14, w: 40, h: 44 },
    { x: 10, y: 40, w: 58, h: 46 },
    { x: 30, y: 30, w: 36, h: 36 },
  ];
  let pi = 0;
  function applySel(p) {
    sel.style.setProperty("--sx", `${p.x}%`); sel.style.setProperty("--sy", `${p.y}%`);
    sel.style.setProperty("--sw", `${p.w}%`); sel.style.setProperty("--sh", `${p.h}%`);
    const x2 = p.x + p.w, y2 = p.y + p.h;
    selScreen.style.setProperty("--hole",
      `polygon(evenodd, 0 0, 100% 0, 100% 100%, 0 100%, 0 0, ${p.x}% ${p.y}%, ${p.x}% ${y2}%, ${x2}% ${y2}%, ${x2}% ${p.y}%, ${p.x}% ${p.y}%)`);
    selSize.textContent = `${Math.round((p.w / 100) * 2560 / 2) * 2} × ${Math.round((p.h / 100) * 1440 / 2) * 2}`;
  }
  selScreen.style.setProperty("--hole", "inset(0 0 0 0)");
  applySel(presets[0]);
  if (!reduceMotion) setInterval(() => { pi = (pi + 1) % presets.length; applySel(presets[pi]); }, 2800);

  /* ---------------- Boot ---------------- */
  applyLang(initial);
  onRange();
  const now = Date.now();
  addClip({ toDemo: false, silent: true, at: now - 7 * 60000, seed: 12.3 });
  addClip({ toDemo: false, silent: true, at: now - 2 * 60000, seed: 47.9 });

  const hero = document.querySelector(".hero");
  let heroVisible = true;
  new IntersectionObserver(([en]) => { heroVisible = en.isIntersecting; }, { threshold: 0.3 }).observe(hero);
  if (!reduceMotion) {
    setTimeout(() => { if (!userInteracted && armed) { addClip({ toDemo: false, silent: true }); flashMock(); } }, 2600);
    setInterval(() => {
      if (!userInteracted && armed && heroVisible && !document.hidden) { addClip({ toDemo: false, silent: true }); flashMock(); }
    }, 11000);
  }
})();
