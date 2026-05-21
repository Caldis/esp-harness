## Summary

<!-- One paragraph: what does this PR do, and *why*? -->

## Type of change

<!-- Tick one (or more, if genuinely multi-faceted): -->

- [ ] Bug fix (non-breaking change which fixes an issue)
- [ ] New feature (non-breaking change which adds capability)
- [ ] Breaking change (would cause existing usage to need an update)
- [ ] Documentation only
- [ ] CI / build / chore

## Affected artifacts

<!-- Which top-level directory does this touch? -->

- [ ] `components/aurora-harness/`
- [ ] `tools/esp-harness/`
- [ ] `examples/aurora/`
- [ ] `sim-base/`
- [ ] `boards/`
- [ ] `docs/`
- [ ] Other: ___

## Checklist

- [ ] I have read [`CONTRIBUTING.md`](./.github/CONTRIBUTING.md)
- [ ] I have read the relevant section of [`AGENT.md`](./AGENT.md) on where things go
- [ ] My PR has a single conceptual change (or, if multiple, they are tightly related and explained in the summary)
- [ ] I have updated any affected documentation in the same PR
- [ ] My change passes `esp-harness test` locally (3 integration tests)
- [ ] If I changed a public API in `aurora-harness`, I bumped its `idf_component.yml::version` and added a CHANGELOG entry
- [ ] If I added a toolkit command, it's registered in `commands/manifest.py::TOOLKIT_COMMANDS` (the manifest lint will fail otherwise)
- [ ] If I broke any sim diff golden intentionally, I committed the refreshed BMPs in the same PR

## How tested

<!-- What did you actually run? -->

```
e.g.
esp-harness test
# 3 passed in 10.30s

esp-harness sim diff --scenes halo,foo
# all 13 scenes within threshold
```

## Related issues / discussions

<!-- Closes #123, addresses #456, etc. -->

## Screenshots / GIFs

<!-- For UI-affecting changes. Drop a before/after if behaviour visibly differs. -->
