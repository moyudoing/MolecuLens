# Contributing to MolecuLens

Thanks for taking the time to contribute. This document covers the setup, the conventions this
codebase enforces, and the review checklist a change has to pass.

**[中文版](CONTRIBUTING.zh-CN.md)**

## Ways to contribute

* **Bug reports** — crashes, wrong geometry, incorrect colours, CLI/exporter mistakes, format
  reading failures. Use the [bug report form](.github/ISSUE_TEMPLATE/bug_report.yml) and attach the
  smallest structure or trajectory that reproduces the problem.
* **Feature requests** — open a [feature request](.github/ISSUE_TEMPLATE/feature_request.yml) first
  for anything larger than a fix, so the design can be discussed before code is written.
* **Docs and figures** — the long-form docs are Chinese; corrections, English translations and
  better figures are all welcome.
* **Code** — start from an issue and keep the change reviewable (see *Pull requests* below).

## Before you start

1. Search the existing issues, [ROADMAP](docs/_internal/ROADMAP-渲染与DFT视图.md) and
   [ISSUES](docs/_internal/ISSUES-已知问题登记.md). Several known problems already have a plan.
2. For a non-trivial change, open an issue and agree on the approach first. Changes that touch the
   command stream or the render pipeline are much easier to review when the design is written down.
3. Read [REMAINING_WORK.md](REMAINING_WORK.md); it records the build/environment conventions and the
   "do not redo" list.

## Development setup

Requirements, versions and platform notes live in [DEPENDENCIES.md](DEPENDENCIES.md). Short version:

* CMake ≥ 3.21, C++20 compiler (MSVC 2022 / GCC 11+ / Clang 14+), [vcpkg](https://github.com/microsoft/vcpkg), Vulkan SDK
* Optional: `ffmpeg` on `PATH` (or `MOLECULENS_FFMPEG`) for MP4 / WebM / GIF export

```bash
git clone https://github.com/moyudoing/MolecuLens.git
cd MolecuLens
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Debug -j

./build/src/app/Debug/moleculens_app data/EPI.pdb     # GUI
./build/src/cli/Debug/moleculens-cli info data/EPI.pdb
./build/tests/Debug/moleculens_tests                  # test suite
```

Build in `Debug` while working and check `Release` before opening a pull request — performance work
must be measured in `Release`.

## Code conventions

* **Naming** — classes `PascalCase`, methods `snake_case`, members `lowercase_`, namespace `moleculens`.
* **Dependency direction** — `moleculens_entity ← moleculens_engine ← app / cli`. Nothing in
  `entity` may include engine or app headers.
* **Directory granularity** — at most three `.cpp/.h` pairs per directory, checked by
  `python cmake/check_tree.py`. Split into a subdirectory instead of growing a file.
* **Protocol headers** — `src/engine/cmd/cmd.h` and `src/engine/persist/mlscene/mlscene.h` must
  compile standalone (no other project headers).
* **Command stream** — any change that invalidates a cached representation must call `bumpSerial()`;
  the renderer relies on serials to decide what to re-upload.
* **No silent behaviour changes** — if a default changes, say so in the PR description and in
  `docs/_internal/`.

## Rendering changes must be bit-exact

This is the rule the project holds itself to, and the one reviewers check hardest:

1. Before editing, render a baseline and record its md5:

   ```bash
   ./build/src/cli/Release/moleculens-cli render data/EPI.pdb --rep ballstick \
       --size 960 --height 540 --out before.png
   md5sum before.png     # Windows: certutil -hashfile before.png MD5
   ```

2. After the change, render the same command and compare. Geometry-preserving refactors (BVH vs
   linear traversal, threaded vs scalar QuickSurf kernels, mesh caching) must produce an identical
   PNG.
3. If the pixels are *supposed* to change, say so explicitly, regenerate the affected figures in
   `docs/figs_v2/`, and note it in the changelog.

The test suite already encodes this for the tracer and the QuickSurf kernels; extend it when you add
a new path.

## Tests

* Run `moleculens_tests` (or `ctest --test-dir build`) before pushing. The suite is currently
  **187 cases / 193,781 assertions**; the count must not go down.
* New behaviour needs a test. New representations, exporters, format readers and scheduling rules
  are all covered by existing tests and make good templates.
* Bug fixes should come with the regression test that would have caught the bug.

## Commits

Conventional Commits, with a scope naming the layer or area:

```text
feat(engine): shared sphere mesh with per-instance transforms
fix(app): keep the timeline visible when the viewport is narrow
docs(readme): add the CLI cookbook
test(engine): BVH and linear traversal produce identical pixels
refactor(cmd): split command replay from mesh rebuild
build(cmake): bump vcpkg baseline
```

Rules of thumb:

* One logical change per commit; keep formatting-only changes separate.
* Explain *why* in the body when the diff is not self-explanatory. Chinese or English are both fine.
* Do not commit build output, `build_*/`, `_test/`, `.monitor/`, or generated figures you did not mean
  to refresh.

## Pull requests

1. Branch from `master`, name it after the change (`feat/shared-sphere-mesh`, `fix/timeline-layout`).
2. Keep it reviewable. A PR that mixes a refactor with a behaviour change is hard to verify against
   the bit-exact rule; split it.
3. Fill in the [pull request template](.github/PULL_REQUEST_TEMPLATE.md), including the commands you
   ran and the test counts.
4. State explicitly whether rendering output changed, and paste the before/after md5 when it did not
   change.
5. Do not force-push a branch under review; add commits and let the maintainer decide about squashing.
6. CI (.github/workflows/quality.yml) checks documentation links, forbidden product names and
   the directory-granularity rule. The C++ suite still runs locally before review — do not ask the
   reviewer to discover breakage a local run would have shown.

## Documentation

* Behaviour changes update the affected document under `docs/`, the internal changelog in
  `docs/_internal/`, and — when user-visible — the README feature table.
* Long-form documentation is written in Chinese; the README is bilingual (`README.md` English,
  `README.zh-CN.md` Chinese). Keep both languages in sync when you touch them.
* Screenshots in `docs/figs_v2/` should be reproducible from a command line; mention the command in
  the PR if you add one. The demo gallery in `docs/06-开源数据集与演示.md` is generated this way.
* New files under `datasets/` must be redistributable (public domain, CC0/CC-BY, MIT/BSD). Never add
  GPL, NC, ND or unclear-licence data; update `datasets/README.md` with source, licence and SHA-256.

## License and sign-off

The project is licensed under **Apache-2.0**. By submitting a pull request you agree to license your
contribution under the same terms (DCO style). Please sign your commits:

```bash
git commit -s -m "fix(engine): ..."
```

The sign-off certifies that you wrote the code or have the right to submit it. Do not paste code
from projects with incompatible licenses; if you adapt an algorithm from a paper or another
codebase, note the source and its license in the PR.

Two files in the repository are deliberately outside the Apache-2.0 grant and must stay that way:
the Noto Sans SC font subset (SIL OFL 1.1, `data/font/OFL.txt`) and the MDAnalysis XTC fixture
(GPL-2.0+, see `THIRD_PARTY_NOTICES.md`).

## Conduct

Keep discussions technical and civil. Disagreement about a design is welcome; personal attacks,
harassment and dismissive "just fix it" comments are not.
