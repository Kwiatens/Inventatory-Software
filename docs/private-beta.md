# Private beta installation

The Inventatory repository and its GitHub Releases remain private. Each tester must be invited to `Kwiatens/Inventatory-Software` and use their own authenticated GitHub account.

Install [GitHub CLI](https://cli.github.com/), then run `gh auth login` and authenticate with the invited account. In CMD, download and start the current installer with:

```cmd
if exist "%TEMP%\Inventatory-install" rmdir /s /q "%TEMP%\Inventatory-install"
mkdir "%TEMP%\Inventatory-install"
gh release download --repo Kwiatens/Inventatory-Software --pattern "Install-Inventatory.*" --dir "%TEMP%\Inventatory-install" --clobber
call "%TEMP%\Inventatory-install\Install-Inventatory.cmd"
```

The installer verifies the release ZIP against `SHA256SUMS.txt`, installs only for the current Windows user, preserves inventory data during updates, then launches Inventatory after a three-second countdown. Press any key during the countdown to cancel the launch. Run the same command later to update. The installed application performs a daily private-release check through the authenticated GitHub CLI session; it never sends inventory or Scan R1 data.

To uninstall, run `Uninstall-Inventatory.cmd` from `%LOCALAPPDATA%\Programs\Inventatory`. It keeps inventory data and settings unless you explicitly choose to remove them.
