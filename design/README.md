# Tunegram design assets

**Status: stand-ins (2026-11-27).** There's no final logo yet. These plain, name-only assets use Telegram's look without its logo, so the public pieces stay within Telegram's rules while the real logo waits.

## Palette

| Role | Colour |
|---|---|
| Primary, top of gradient | `#40A8FA` |
| Primary, bottom of gradient | `#2382E8` |
| Text and mark | `#FFFFFF` |

The background is one blue going from light (top) to dark (bottom), with white on top. That's two colours in total.

## Type

- **Name and tagline:** Avenir Next, Bold for the name and Medium for the tagline (bundled with macOS). The PNGs are the finals; the SVGs need the font installed to look the same.
- **Bot photo and landing icon:** the "T" is drawn with plain shapes, so no font is needed.

## Files

| File | Size | Use | How to apply |
|---|---|---|---|
| `miniapp/cover.png` | 640×360 | Mini App photo for t.me/TunegramSyncBot/listen | BotFather → `/editapp` → pick the app → edit photo |
| `bot/photo.png` | 640×640 | @TunegramSyncBot profile picture (recommended; readable as a small circle) | BotFather → `/setuserpic` |
| `bot/photo-wordmark.png` | 640×640 | Alternative bot picture with the full name (hard to read at chat-list size) | Same as above |
| `landing/icon.svg` | 88×88 | Optional replacement for the speech-bubble icon in `backend/src/landing.ts` | Swap the inline SVG, then redeploy the Worker |

Each PNG has an SVG source next to it.

## Rules to keep

- Telegram API Terms §2.4: a third-party app must not use the official Telegram logo. §2.3: the app's title must not contain "Telegram" unless it reads "Unofficial Telegram …". Telegram's general style (blue, white, layout) is fine.
- **App icon:** development builds keep the stock icon from the upstream Telegram code for now. Replace it before any build is shared with other people.
