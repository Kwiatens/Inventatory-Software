# Shared project standards

These rules apply to both Inventatory repositories: the application
([Inventatory-Software](https://github.com/Kwiatens/Inventatory-Software)) and the
Scan R1 firmware ([Inventatory-Firmware](https://github.com/Kwiatens/Inventatory-Firmware)).
Each repository's `AGENTS.md` and `CONTRIBUTING.md` add the rules for its own code.
They may add rules, but must not contradict this file.

Change a rule here only when the change applies to both repositories.

## Licensing and copyright

- Inventatory is licensed under GPL-3.0-only. The full text is in `LICENSE`.
  The copyright holder is Paweł Kwiatkowski.
- Every new first-party source file (C, C++ or Python) starts with these two
  lines, placed after a shebang line if the file has one:

  ```text
  // SPDX-FileCopyrightText: <year> Paweł Kwiatkowski
  // SPDX-License-Identifier: GPL-3.0-only
  ```

  Python files use `#` instead of `//`.
- Documentation and configuration files are covered by the repository `LICENSE`.
- Third-party code keeps its own license and notice, is listed in
  `THIRD_PARTY_NOTICES.md`, and never carries the Inventatory header.

## Commits

- Format: `type(scope): subject`.
- Types: `feat`, `fix`, `refactor`, `perf`, `style`, `test`, `docs`, `build`,
  `ci`, `chore`, `revert`.
- Shared scopes:
  - `build`: build system, packaging, CI workflows and their action pins, and
    the Dependabot configuration.
  - `docs`: files under `docs/`.
  - `project`: repository root files (README, CONTRIBUTING, SECURITY, LICENSE,
    CHANGELOG, AGENTS.md), issue and pull request templates, CODEOWNERS, and
    this file.
  - `release`: the release process, versioning and release notes.

  Each repository lists its own domain scopes in its `AGENTS.md`. Existing
  commits keep the scopes they were written with.
- Subject: imperative present tense, lowercase first word after the colon, no
  full stop, 72 characters or fewer. Name the outcome, not the tool or the
  process.
- Body: add one when the reason, a compatibility or security consequence, or a
  migration matters. Wrap it at 72 characters.
- One logical change per commit. Keep formatting-only or unrelated cleanup in a
  separate commit.
- Breaking changes: put `!` after the scope and add a `BREAKING CHANGE:` footer.
- Commit messages, branch names and pull requests never contain prompts,
  conversation identifiers, local paths, credentials, device identifiers or real
  inventory data.

## Changelog

- `CHANGELOG.md` follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
  Use the `Added`, `Changed`, `Deprecated`, `Removed`, `Fixed` and `Security`
  subsections.
- Entries describe what someone running the product sees or can do. Leave out
  refactors, tests, build and CI changes, documentation edits, and the names of
  internal functions and files.
- A released version is headed `## [vX.Y.Z] - YYYY-MM-DD`, matching its release
  tag.

- Entries reach `CHANGELOG.md` through the pull request's Changelog section, not by
  editing the file on a feature branch. The maintainer approves the wording, and the
  release copies the approved entries into `CHANGELOG.md` as written.

## Secrets and logging

- Passwords, tokens, pairing secrets, HMAC keys, API credentials and similar
  values are never written to logs, console output, commits, test fixtures,
  screenshots, error messages or issue reports.
- Secrets are stored only in the store that the repository's `AGENTS.md` names.
  They never go into plain configuration files.
- Scanned codes, raw device buffers and authenticated request bodies are not
  logged either.
- A repository that has a logging check in CI keeps that check passing.

## Verification and reporting

- Name the level of verification that actually ran: compiled only, host or unit
  tests executed, or hardware or live integration executed. Never describe a
  compile-only result as passed.
- List the checks you could not run and say why.
- The final report states what changed and why, which files were touched, which
  commands ran and their results, which checks were not run, and any remaining
  risks.

## Issues, pull requests and security

- Security problems are never reported in public issues or pull requests. Use
  the repository's `SECURITY.md`.
- Issues and pull requests use the forms and templates in `.github/`.

## Release and repository settings

Release steps and the GitHub repository settings are in each repository's
`docs/releasing.md`.
