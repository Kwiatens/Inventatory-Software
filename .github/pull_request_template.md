## Summary

<!-- What changes for the user or the maintainer, and why. -->

## Testing

- [ ] Debug and Release tests pass (`ctest --output-on-failure`), or the Windows commands in `docs/public-beta.md` pass
- [ ] Each behavior change has a focused test in `tests/`
- [ ] UI changes were checked at 100x30 and by keyboard and mouse (if applicable)

## Checklist

- [ ] One logical change per commit, with `type(scope): summary` subjects
- [ ] `CHANGELOG.md` has an entry under `[Unreleased]` for user-visible changes
- [ ] The diff contains no credentials, tokens, device identifiers, real inventory data or local paths
