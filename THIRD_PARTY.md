# Third-party components

This project bundles or depends on the following third-party software. Game assets and
game code are **not** included; see `LICENSE` for repository code and `web/public/licenses.html`
for notices on the deployed site.

| Component | Role | License |
|-----------|------|---------|
| [iced-x86](https://github.com/icedland/iced) | x86 decode/lift in `recomp/` (npm `iced-x86`) | MIT |
| [Emscripten](https://emscripten.org/) | WebAssembly toolchain for `runtime/` player engine | MIT |
| [dlmalloc](https://github.com/emscripten-core/emscripten/tree/main/system/lib/dlmalloc) | Guest heap (`third_party/dlmalloc/`) | Public domain |
| [Vite](https://vite.dev/) | `web/` production bundle | MIT |
| [Playwright](https://playwright.dev/) | Headless browser checks in `tools/` (dev only) | Apache-2.0 |
| [Wrangler](https://developers.cloudflare.com/workers/wrangler/) | Cloudflare Pages deploy (`tools/deploy.sh`) | MIT |

Detective Barbie (Mattel Media, 1998) is a separately purchased product; this repository does not
redistribute it.
