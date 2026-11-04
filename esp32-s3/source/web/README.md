# Controller Portal Files

The portal renders native HTML forms on the server. `page()` and the `write*()`
helpers in `main/network.cpp` assemble the overview, agents, and settings pages
from this tree; separate static page copies are not needed.

- `templates/shell/`: document head, navigation, notices, and footer.
- `templates/overview/`, `stats/`, and `summary/`: overview sections.
- `templates/agent/` and `agents/`: agent controls and filtering.
- `templates/schedule/` and `settings/`: configuration forms.
- `templates/{current,checked,disabled,hidden}.html`: native HTML attributes.
- `assets/css/{portal,preview}.css`: stylesheets.
- `assets/js/{portal,preview}.js`: progressive enhancement and live preview.
- `images/device.png`: controller image exposed at `/device.png`.
- `images/device-layout.json`: measured angled CAD aperture and physical pixel plane.
  `preview.css` maps the live canvas into this plane with an affine transform;
  the physical 348 x 233 mm matrix stays centered in the 348 x 248 mm aperture.
  The body is horizontally mirrored, but the canvas keeps a positive-determinant
  transform so the animation is not reflected. Layout uses a 768 x 550 coordinate
  space; the PNG has a 1536 x 1100 raster for high-density displays. CAD exports
  use fine tessellation and 4x supersampling, then alpha-aware 2x downsampling.

## Rendering Contract

`Html::file(relativePath)` streams a literal fragment through `openWebFile()`.
`Html::render(relativePath, callback)` replaces numbered slots such as `{{0}}`
by invoking `callback(unsigned token)` in file order. Both use bounded buffers;
they do not load whole pages. Templates must use paired braces and one to three
decimal digits per slot. Callback output is not parsed again as a template.

Dynamic names, SSIDs, and notices use `Html::escaped()`. Keep untrusted values
out of raw text and formatting calls. Attribute selectors `@current`, `@checked`,
and `@disabled` are internal arguments to `Html::attribute()`; they select files
and must never appear in rendered output. An empty selector emits nothing.

Keep markup in templates, styling in CSS, and event handlers in JavaScript.
Preserve native form methods, actions, names, and values so configuration works
without JavaScript. The visible title is `Codex`. Preserve the legacy title
inside the early HTML comment in `templates/shell/document-head.html`: older
agents scan the first response bytes for that marker during device discovery.

## Performance Contract

Status polling stays sequential at five-second intervals with timeout/backoff;
hidden pages and page-cache navigation stop requests. Agent rows are updated
only when their reported state or local edit state changes. Ranking rows are
keyed by agent ID and reused; sorting or membership changes alone reorder them.
Unsaved form input remains local and is never replaced by polling.

Public CSS/JS/image assets revalidate with bounded ETags derived from
the running firmware SHA, verified web bundle SHA and asset identity. A matching
validator returns a bodyless 304. Stable URLs intentionally do not use long
freshness lifetimes, so replacement firmware cannot leave an old portal cached.
Status, frames, HTML and agent update artifacts remain non-cacheable.

Preview optimizations affect browser drawing only, not LED output, color
conversion, normalization or physical 30 FPS. No playback queue is introduced.

`boot.js` is the sole inline-script exception: `page()` streams the small,
constant filesystem source inside the first 1024-byte HTML chunk, before
stylesheets, so no blocking script fetch can postpone its safety watchdog.
It hides the assembling shell only when
JavaScript is available. The loader releases after window load and two layout
frames, or after an eight-second safety deadline. Missing JavaScript leaves
native forms visible; page-cache restores never repeat the loader. Background
status polls do not disable refresh buttons; a human click can adopt an existing
request without starting a duplicate. The pixel-meteor favicon is served from
`/favicon.svg` with the same bounded conditional cache as other static assets.

## Integration And Checks

Filesystem paths are relative to the active web root selected by `web_files.hpp`.
Mount points and bundle slot directories are intentionally hidden from callers.
The HTTP server exposes five explicit `/assets/css/` and `/assets/js/` resources;
templates and this README are not public routes.

`build_web_bundle.py` packages this tree deterministically, with a 256-file limit.
Regenerate the bundle after changing templates or assets. Run the portal and
preview JavaScript suites plus the native firmware network contract tests.
Verify overview, agents, and settings with JavaScript enabled and disabled,
including escaped values, active navigation, checked schedule days, disabled
maintenance controls, mobile layout, and live preview.

## Lifetime ranking integration

Render two unframed sections using the existing weekly streaming boundaries.
For lifetime emit `lifetime/open.html`, then escaped literal
`За доступними журналами агентів`, then `lifetime/table.html`, then the
same four-cell row markup as weekly, then `lifetime/close.html`.
As for weekly, close.html ends the table wrapper only: the server must emit
its own empty paragraph with `data-lifetime-empty` (hidden only when rows
exist), then close the section. All three fragments have no numbered slots.
Use `lifetime-ranking`, `lifetime-heading`, and `data-lifetime-*`; retain
weekly IDs/selectors unchanged. Shared weekly CSS classes are intentional.
The weekly period remains Monday–Sunday in Kyiv.

`/status` supplies independent `weekly` and `lifetime` objects. Lifetime is
`{entries:[{id,name,color,tokens}],dirty,save_failed}`. Tokens must be
nonnegative safe JS integers; unsafe numbers are rejected, never rounded
into fabricated totals. Percentages use exact BigInt sums. Missing ranking
objects show unavailable text, not zero. Manual refresh is a human click
using the existing `/status` request and sequential polling machinery.
