# Contributing to Inventatory

Thank you for your interest in helping with the development of Inventatory!

Here is a guide on how to contribute to the project.

## Start with an issue

For a bug, open an issue with the steps to reproduce it, the Inventatory version and your operating system (exact version would really help).
For a larger change, open an issue first so the approach can be agreed before you write the code.

Security problems do not go in issues. See [SECURITY.md](SECURITY.md).

## Build and test

- Windows: build with Visual Studio 2022 or newer and CMake 3.20 or newer.
- Linux: the release is built in AlmaLinux 9 (glibc 2.34, GCC 11), as set in `.github/workflows/ci.yml`. Install the build dependencies for your distribution:
  - Ubuntu or Debian: `build-essential cmake pkg-config libsecret-1-dev libcurl4-openssl-dev libssl-dev libglib2.0-dev git`.
  - Fedora, AlmaLinux or RHEL: `gcc-c++ make cmake pkgconf-pkg-config libsecret-devel libcurl-devel openssl-devel glib2-devel git`.
- A binary built on a newer distribution can need a newer glibc than the release supports. Check a local build with `bash tools/check-linux-abi.sh <binary>`.

Build and test both the Debug and the Release configuration before you open a pull request. The continuous
integration runs the same commands, plus a Clang build, a sanitizer build and a secret-logging check, on every
pull request. Add a focused test for every behavior you change. `inventatory_tests --list` shows the named
tests, and `inventatory_tests --filter <text>` runs a subset.

## Code

- The code is C++17. Match the code around your change: two-space indentation, the existing naming, and the
  ownership boundaries in [AGENTS.md](AGENTS.md).
- Keep platform-specific code behind the existing platform files and preprocessor guards.
- Anything shown on screen follows [docs/ui-style-guide.md](docs/ui-style-guide.md).
- New source files carry the SPDX header described in [docs/project-standards.md](docs/project-standards.md).

## Commits and pull requests

- Keep one logical change per commit.
- If a change is visible to users, add an entry under `[Unreleased]` in [CHANGELOG.md](CHANGELOG.md). Write
  the entry in plain terms of what users can see or do, not as a list of code changes.

## Data, secrets and logging

Never commit credentials, tokens, device identifiers, real inventory data or local file paths. This applies
to test fixtures and screenshots too. Passwords, tokens and other credentials are never written to logs or
console output. The `secret-logging` CI job rejects console output that names a credential.

## Licensing

Inventatory is licensed under GPL-3.0-only (see [LICENSE](LICENSE)). By contributing, you agree that your
contribution is licensed under the same terms. Bundled third-party code and artwork are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Add an entry when you bring in new code or artwork.

## Automated coding agents

[AGENTS.md](AGENTS.md) is the operating guide for automated coding agents, and it is also the reference for
people. It describes the repository layout, the invariants that must be preserved, and the test expectations.
