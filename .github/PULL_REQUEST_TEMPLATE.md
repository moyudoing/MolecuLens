## What this changes

<!-- One or two sentences. Link the issue it fixes: "Fixes #123". -->

## Type of change

- [ ] Bug fix
- [ ] New feature / representation / format
- [ ] Refactor (no behaviour change)
- [ ] Performance
- [ ] Documentation / figures
- [ ] Build or packaging

## How it was verified

<!-- Commands, results, test counts. -->

```text
build:      Debug / Release, compiler and version
tests:      moleculens_tests — <cases> / <assertions>, all passed
cli check:  the exact render command used, if relevant
```

## Rendering impact

Pick one and prove it:

- [ ] **Output unchanged** — same render command before and after, md5 identical
      (before `…`, after `…`).
- [ ] **Output changed intentionally** — what changed, why, and which figures in `docs/figs_v2/`
      were regenerated.
- [ ] Not rendering related.

## Checklist

- [ ] `moleculens_tests` passes locally and the case/assertion counts did not go down.
- [ ] New behaviour has a test (or the PR says why it cannot be tested).
- [ ] `python cmake/check_tree.py` passes (≤ 3 `.cpp/.h` pairs per directory).
- [ ] Dependencies still point one way: `entity ← engine ← app / cli`.
- [ ] Touched docs are updated, including `docs/_internal/` if a default or behaviour changed.
- [ ] README kept in sync on both languages when user-facing text changed.
- [ ] Commits are signed off (`git commit -s`) and licensed under Apache-2.0.

---

<details>
<summary>中文填写要点</summary>

- **改了什么**：一两句话说清，并关联 issue（`Fixes #123`）。
- **怎么验证的**：写清 Debug / Release、编译器、`moleculens_tests` 的用例数与断言数。
- **渲染影响**：必须三选一——输出没变（贴改前改后 md5）、有意改变（说明原因与重建了哪些插图）、
  与渲染无关。
- **自查**：测试数与断言数只增不减；新行为有测试；目录粒度检查通过；依赖方向没破；
  文档（含 `docs/_internal/` 变更记录）已更新；README 中英同步；提交已 `-s` 签署。

</details>
