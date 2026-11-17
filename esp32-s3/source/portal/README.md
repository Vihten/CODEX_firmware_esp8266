# Portal JavaScript sources

Edit these fragments, then run `python firmware/esp32s3/portal/generator.py`
from the repository root. Commit both the fragments and the generated
`web/assets/js/portal.js` and `main/portal_strings.hpp`. Existing JS tests execute
that generated script.

| Fragment | Responsibility |
| --- | --- |
| `context.js` | Shared state, DOM/format helpers, authentication shutdown |
| `commands.js` | Dirty forms, acknowledgements, command transport and submission |
| `settings.js` | Agent section navigation, device-settings dialog and original-form movement |
| `stats.js` | Rankings, quota metadata and controller-status rendering |
| `agents.js` | Agent roster reconciliation, sorting, filtering and rendering |
| `controls.js` | Input, change, click and reset interactions |
| `lifecycle.js` | Status polling, visibility/navigation and final startup |

`generator.py` owns the explicit assembly order and one surrounding closure.
Fragments share lexical bindings: do not add wrappers, imports or exports.
Declarations may refer to later bindings inside callbacks; do not invoke them
before initialization. Final startup belongs in `lifecycle.js`, after all
other fragments. Source-boundary comments in the generated script make browser
stack traces easy to map back to the editable source.

Build integration, for the owner of `build_web_bundle.py`:

```python
from portal.generator import sync

sync(web_root)  # before enumerating/hash/compressing the web tree
```

`sync(web_root)` writes `web_root/assets/js/portal.js` and its sibling
`web_root.parent/main/portal_strings.hpp`. It returns whether either changed
and preserves mtimes for unchanged files. `sync(web_root, check=True)` checks
both outputs and raises `ValueError` on stale or missing output without writing
either. `render()` returns deterministic UTF-8/LF script bytes; `render_header()`
returns the generated C++ header bytes. All paths default relative to the
generator, not the working directory. Staging destinations can be selected
with `--web-root PATH`.

## Shared Ukrainian messages and release source

`messages.uk.json` is the single authority for `missing`, `on`, `off`, `online`,
`offline`, `remainingLastData`, `resetUnknown`, `ageUnknown`, `stale`,
`resetPrefix`, `agePrefix`, `ageSuffix`, `seconds`, `minutes` and `hours`.
The generator replaces the `// @portal-strings` marker in `context.js` with a
frozen `const portalStrings` object. Do not maintain a second JS dictionary.
`main/portal_strings.hpp` exposes matching UTF-8 values as
`portal_strings::missing` (and the other keys), each a `static constexpr char[]`
with no heap allocation or runtime initialization. The C++ consumer owner can
include `"portal_strings.hpp"`; consumer and fixture includes are separate work.

Metadata is empty when quota is unknown (or remaining is outside 0..100).
Known quotas retain reset, age and stale metadata, including known 0%.
The matching C++ renderer should return before metadata when `!quota.known`;
otherwise use `resetPrefix` plus `%H:%M` in Kyiv time when known; omit an unknown
reset instead of displaying a placeholder. Reset metadata explicitly describes
the five-hour window. Ages below five seconds read "Оновлено щойно". Format other ages as
`agePrefix` + floored seconds/minutes/hours + the matching unit + `ageSuffix`,
or `ageUnknown`. Append `stale` only when the known quota is stale. Separate
parts with ` · `. This keeps C++ first paint and JS polling wording consistent.

Release-source packaging must retain the complete `portal/` directory,
including `messages.uk.json` and `generator.py`, plus the generated
`main/portal_strings.hpp` and `web/assets/js/portal.js`. For a staged firmware
tree, copy the source directory too and call `sync(staged_root / "web")` before
packaging. The dictionary is build-time source, not a browser-fetched asset.

CI freshness check: `python firmware/esp32s3/portal/generator.py --check`.
Assembly adds no runtime requests, script tags or dependencies. The browser
still receives one portal script, preserving the four-socket budget.
