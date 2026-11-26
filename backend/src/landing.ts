// Invite landing page, served as the bot's Mini App (PLAN.md §2.3). People on official Telegram who tap a
// Listen Together invite land here. Tunegram users never see it: their app intercepts the t.me link natively.
// Self-contained HTML. The only external script is Telegram's Mini App SDK (theme colours + start_param).

export const DOWNLOAD_URL = "https://github.com/adilabdu/tunegram/releases";

export function landingPage(): Response {
  return new Response(LANDING_HTML, {
    headers: {
      "content-type": "text/html; charset=utf-8",
      "cache-control": "public, max-age=300",
      "x-content-type-options": "nosniff",
      "referrer-policy": "no-referrer",
    },
  });
}

const LANDING_HTML = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>Listen Together · Tunegram</title>
<script src="https://telegram.org/js/telegram-web-app.js"></script>
<style>
  :root {
    --bg: var(--tg-theme-bg-color, #ffffff);
    --text: var(--tg-theme-text-color, #000000);
    --hint: var(--tg-theme-hint-color, #707579);
    --button: var(--tg-theme-button-color, #3390ec);
    --button-text: var(--tg-theme-button-text-color, #ffffff);
    --card: var(--tg-theme-secondary-bg-color, #f4f4f5);
  }
  @media (prefers-color-scheme: dark) {
    :root:not(.tg) {
      --bg: #17212b; --text: #f5f5f5; --hint: #8a96a3; --button: #5288c1; --card: #232e3c;
    }
  }
  * { box-sizing: border-box; }
  html, body { margin: 0; background: var(--bg); color: var(--text);
    font: 16px/1.45 -apple-system, BlinkMacSystemFont, "SF Pro Text", Roboto, "Segoe UI", sans-serif; }
  main { max-width: 420px; margin: 0 auto; padding: 40px 20px 28px; text-align: center; }
  .icon { width: 88px; height: 88px; margin: 0 auto 18px; }
  h1 { font-size: 22px; margin: 0 0 8px; font-weight: 600; }
  p { margin: 0 0 22px; color: var(--hint); }
  .btn { display: block; width: 100%; padding: 13px 16px; margin: 0 0 10px; border: 0; border-radius: 12px;
    font-family: inherit; font-size: 16px; font-weight: 600; line-height: 1.2; text-decoration: none; cursor: pointer; }
  .primary { background: var(--button); color: var(--button-text); }
  .secondary { background: var(--card); color: var(--text); }
  .row { display: flex; gap: 10px; }
  .row .btn { flex: 1; font-weight: 500; font-size: 15px; }
  small { display: block; margin-top: 18px; color: var(--hint); font-size: 13px; }
</style>
</head>
<body>
<main>
  <svg class="icon" viewBox="0 0 88 88" aria-hidden="true">
    <defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#7b5cff"/><stop offset="1" stop-color="#3390ec"/></linearGradient></defs>
    <rect width="88" height="88" rx="22" fill="url(#g)"/>
    <path d="M22 26h44a6 6 0 0 1 6 6v22a6 6 0 0 1-6 6H40l-11 9v-9h-7a6 6 0 0 1-6-6V32a6 6 0 0 1 6-6z" fill="#fff"/>
    <path d="M50 33v15.5a5 5 0 1 1-3-4.6V37l-9 2v11.5a5 5 0 1 1-3-4.6V36.5z" fill="#5a6cf5"/>
  </svg>
  <h1>Listen together on Tunegram</h1>
  <p id="lead">You've been invited to a Listen Together session. Open this invite in the Tunegram app to join and hear the same music, in sync.</p>
  <a class="btn primary" id="open" href="#">Open in Tunegram</a>
  <div class="row">
    <a class="btn secondary" href="${DOWNLOAD_URL}" target="_blank" rel="noopener">iPhone</a>
    <a class="btn secondary" href="${DOWNLOAD_URL}" target="_blank" rel="noopener">Android</a>
    <a class="btn secondary" href="${DOWNLOAD_URL}" target="_blank" rel="noopener">Mac</a>
  </div>
  <small>Don't have Tunegram yet? Pick your device above to get it. Your Telegram account works as-is.</small>
</main>
<script>
  (function () {
    var tg = window.Telegram && window.Telegram.WebApp;
    var start = "";
    if (tg) {
      document.documentElement.classList.add("tg");
      try { tg.ready(); tg.expand(); } catch (e) {}
      start = (tg.initDataUnsafe && tg.initDataUnsafe.start_param) || "";
    }
    if (!start) start = new URLSearchParams(location.search).get("tgWebAppStartParam") || "";
    var safe = /^[A-Za-z0-9_-]{1,64}$/.test(start) ? start : "";
    var deep = "tunegram://join" + (safe ? "?t=" + safe : "");
    var open = document.getElementById("open");
    open.href = deep;
    open.addEventListener("click", function (e) {
      e.preventDefault();
      if (tg && tg.openLink) {
        try { tg.openLink(deep); return; } catch (err) {}
      }
      location.href = deep;
    });
  })();
</script>
</body>
</html>`;
