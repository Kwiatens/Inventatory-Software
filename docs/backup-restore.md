# Inventatory backup and restore

Use **Backup data** to create a timestamped bundle. The bundle contains the
SQLite inventory database (including BOM projects), activity history, printer
configuration, quick-label configuration, and sanitized application settings.
Each file is listed in `manifest.tsv` with its size and SHA-256 hash.

DigiKey secrets remain in Windows Credential Manager and are never exported.
The Scan R1 token and device configuration are also excluded. Restoring a
bundle is an explicit **Restore backup** action: Inventatory validates every
listed file and the database before activation, creates an automatic
pre-restore backup, and leaves the active workspace unchanged if validation or
activation fails.

After a successful restore, the Scan R1 token is rotated, replay state is
cleared, and the paired-device identity is removed. The device must be paired
again. DigiKey credentials remain local to the PC; if they are not available on
that machine, the DigiKey setup page prompts for re-entry.
