# Releasing

This page covers two things: the steps for cutting a release of the application,
and the GitHub repository settings that the maintainer sets by hand. The shared
rules for changelog entries and versions are in
[project-standards.md](project-standards.md).

## Cutting a release

1. Check that `main` passes CI: the `linux`, `linux-runtime`, `linux-clang`,
   `linux-sanitizers`, `secret-logging` and `windows` jobs.
2. Set the version in `project(InventatorySoftware VERSION x.y.z)` in
   `CMakeLists.txt`.
3. In `CHANGELOG.md`, move the `[Unreleased]` entries under a new
   `## [x.y.z] - YYYY-MM-DD` heading, using the standard subsections. A
   `**Highlights**` block, when the release has one, moves with them and stays
   directly under the heading (see [AGENTS.md](../AGENTS.md)).
4. Create the tag `vX.Y.Z` on that commit and push it. Release candidates use
   `vX.Y.Z-rc.N`. The release workflow rejects any other tag format.
5. The `Public beta release` workflow (`.github/workflows/release.yml`) runs on
   the tag, builds the Windows and Linux packages, and publishes the GitHub
   release. The Linux package is built in an AlmaLinux 9 container, and
   `tools/check-linux-abi.sh` fails the job when its binary needs a glibc newer
   than 2.34, a libstdc++ newer than GCC 11's, or a versioned libcurl. Its notes
   come from the matching `CHANGELOG.md` section. A
   `release-notes/<tag>.md` file overrides them, and so does the notes input when
   the workflow is started by hand. If none of these exists, GitHub's generated
   notes are used.
6. Download the published Linux and Windows packages and check that they report
   the new version.

## Repository settings

These settings are not stored in the repository. Set them before the repository
is made public, or right after.

- [ ] Repository description and topics.
- [ ] Private vulnerability reporting (Settings, Code security), because
      [SECURITY.md](../SECURITY.md) points reporters to it.
- [ ] Dependabot alerts and security updates, secret scanning, and push
      protection.
- [ ] Protect `main`: require pull requests and the CI jobs listed above, and
      disallow force pushes and branch deletion.
- [ ] Default workflow token permission read-only, and approval required for
      workflows from first-time contributors (Settings, Actions).
- [ ] Repository variable `SCAN_FIRMWARE_REPOSITORY` (owner/repo, for example
      `Kwiatens/Inventatory-Firmware`). Set it only once that repository is
      public. Release builds check it for Scan R1 firmware updates, and a
      private repository cannot be checked, so leave the variable unset until then.
- [ ] Community profile (`/community`) shows every item complete.
