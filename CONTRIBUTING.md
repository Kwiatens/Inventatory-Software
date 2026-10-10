# Contributing to Inventatory

Thank you for taking the time to contribute. Inventatory is in public beta, and a bug report with clear
steps helps as much as a code change.

## Start with an issue

For a bug, open an issue with the steps to reproduce it, the Inventatory version and your operating system.
For a larger change, open an issue first so the approach can be agreed before you write the code.

Security problems do not go in issues. See [SECURITY.md](SECURITY.md).

## Build and test

- Windows: follow the build steps in [docs/public-beta.md](docs/public-beta.md).
- Linux (Ubuntu 24.04): the required packages and optional runtime services are in
  [docs/linux-support.md](docs/linux-support.md).

Build and test both the Debug and the Release configuration before you open a pull request. The continuous
integration runs the same commands, plus a Clang build and a sanitizer build, on every pull request.
Add a focused test for every behavior you change. `inventatory_tests --list` shows the named tests, and
`inventatory_tests --filter <text>` runs a subset.

## Code

- The code is C++17. Match the code around your change: two-space indentation, the existing naming, and the
  ownership boundaries in [AGENTS.md](AGENTS.md).
- Keep platform-specific code behind the existing platform files and preprocessor guards.
- Anything shown on screen follows [docs/ui-style-guide.md](docs/ui-style-guide.md). UI changes also
  follow [docs/ui-contributor-guide.md](docs/ui-contributor-guide.md).

## Commits and pull requests

- Write each commit subject as `type(scope): imperative summary`, with a lowercase summary and no full stop,
  ideally 72 characters or fewer. Example: `fix(storage): preserve inventory history during restore`.
  Allowed types are `feat`, `fix`, `refactor`, `perf`, `style`, `test`, `docs`, `build`, `ci`, `chore` and
  `revert`.
- Keep one logical change per commit.
- If a change is visible to users, add an entry under `[Unreleased]` in [CHANGELOG.md](CHANGELOG.md). Write
  the entry in plain terms of what users can see or do, not as a list of code changes.

## Data and secrets

Never commit credentials, tokens, device identifiers, real inventory data or local file paths. This applies
to test fixtures and screenshots too.

## Licensing

Inventatory is licensed under GPL-3.0-only (see [LICENSE](LICENSE)). By contributing, you agree that your
contribution is licensed under the same terms. Bundled third-party code and artwork are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Add an entry when you bring in new code or artwork.

## Automated coding agents

[AGENTS.md](AGENTS.md) is the operating guide for automated coding agents, and it is also the reference for
people. It describes the repository layout, the invariants that must be preserved, and the test expectations.

## Conduct

Participation in this project is governed by the [Code of Conduct](CODE_OF_CONDUCT.md).
