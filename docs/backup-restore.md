# Inventatory backup and restore

Use **Backup data** to create a timestamped bundle. The bundle is first built
in a unique staging directory and is published only after it has been fully
validated. The SQLite file is produced with SQLite's online-backup API, so it
is a consistent snapshot even while the application is using WAL or a
transaction is in progress. The bundle contains the
SQLite inventory database (including BOM projects), activity history, printer
configuration, quick-label configuration, and sanitized application settings.
Each file is listed in `manifest.tsv` with its size and SHA-256 hash.

Inventory commit history is stored inside `inventory.db`, so every backup and
restore preserves the complete local version history automatically. Older
databases receive an `Initial inventory` baseline the first time Inventatory
opens them; existing `activity.tsv` and movement records are retained as-is and
are not converted into commits.

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

Validation is read-only: it never migrates, creates, or otherwise changes the
candidate database. The manifest has exact rows, 64-hex-digit SHA-256 hashes,
and an allowlist of files; unknown files and symbolic links are rejected.

Restore uses unique same-volume staging and protected old-data paths. A
restore journal next to the application settings records each activation
phase. Startup should call the restore-recovery helper before opening the
workspace so an interrupted activation is either completed safely or rolled
back. Data-directory and application-settings files can be on different
volumes, so the journal is required; the operation is not presented as one
cross-volume atomic transaction. Failed cleanup leaves the journal and
recoverable old artifacts in place and reports the cleanup failure.

The optional filesystem-operation hooks in the transfer API are test-only
fault-injection seams. Production code passes no hooks; manifest, database,
and settings validation always runs in the transfer implementation itself.
