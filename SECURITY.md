# Security policy

## Supported versions

Security fixes are made for the latest release of Inventatory. Before you report a problem, check
whether it still happens in the latest release, if you can do that safely.

## Reporting a vulnerability

Do not open a public issue or pull request for a security problem. Report it privately:

1. Open the **Security** tab of this repository on GitHub.
2. Choose **Report a vulnerability** to open a private security advisory.

If private reporting is not available to you, contact the maintainer through the
[Kwiatens GitHub profile](https://github.com/Kwiatens) and ask for a private channel. Do not describe the
issue there.

Include what is needed to reproduce the problem:

- the Inventatory version (`inventatory --version`), your operating system and your terminal;
- the steps that trigger the problem, and what you expected to happen;
- the impact you observed, and any logs with secrets removed.

Do not include scanner tokens, Wi-Fi passwords, DigiKey credentials or real inventory data. Test only on
your own computer, with your own scanner and your own data.

## Scope

In scope:

- The Scan R1 device service: the local HTTP service, Bluetooth Low Energy provisioning, and the
  HMAC-authenticated request protocol with replay protection. The design is described in
  [docs/scanner-transport-security.md](docs/scanner-transport-security.md).
- Storage of secrets. The scanner token and DigiKey credentials belong in Windows Credential Manager or the
  Linux Secret Service. They must never appear in configuration files, logs, backups or the inventory
  database.
- Backup and restore integrity, including manifest validation and any path that could lose or overwrite
  inventory data.
- Verification of update and installer packages.

Known limitation, documented in the design notes: the scanner protocol authenticates requests and detects
replay, but it does not encrypt their contents. A way to bypass authentication, replay protection or the
private-network boundary is in scope.

Out of scope: problems that need physical access to an unlocked computer or control of a logged-in user
account, and vulnerabilities in third-party software such as FTXUI, SQLite or libcurl. Report those to the
upstream project. Tell us as well if Inventatory makes them worse.
