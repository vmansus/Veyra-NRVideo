# Online updates

Settings → About → Check queries the latest **stable** release of
`Likely7/Veyra-NRVideo`, the existing upstream release channel. It does not
automatically check on startup, request GitHub credentials, download a source
archive, or downgrade a newer installed version. The installed
`package-manifest.json` determines NVIDIA versus AMD, independently of the
active graphics adapter. Unrecognized/custom builds use the manual release link.
Version comparison uses the manifest's numeric package version when available;
the official 2.0.7 packages' retained `test-r2` display label must not cause an
endless offer to reinstall the same stable package.

Download streams the matching `Veyra-<version>-<vendor>-win64-portable.zip`
to a private temporary directory and checks its exact size and GitHub asset
SHA-256 digest. Missing digests, unsuccessful HTTP responses, TLS errors,
corrupt/incomplete downloads and non-upstream download URLs prevent installation.
System proxy settings are supported; TLS certificate verification stays enabled.
Cancellation leaves the installed application untouched.

After stopping playback and export, choose **Exit and install**. A local Windows
PowerShell 5.1 helper validates and extracts the package, verifies every manifest
file, rejects unsafe/duplicate paths and reparse points, checks the destination,
and backs up the current package before acknowledging readiness. Veyra closes
only after this acknowledgement, through its existing normal teardown. The helper
waits for that captured process to exit, replaces package-owned files, removes
obsolete package-owned files and restarts Veyra with its original arguments.
No elevation or execution-policy override is requested.

Files outside the old/new package manifests, including user settings, remain
untouched. A user-replaced DLL, any modified/missing package file, or an unknown
file colliding with a new package path causes refusal rather than silent
overwrite. Use a fresh manual extraction for those installations. Updates also
require a writable install folder and enough space for extraction and a complete
backup. A filesystem failure during replacement triggers rollback. The backup
`.veyra-update-backup-<id>` remains beside the application for recovery; remove it
manually only after verifying the new version. The private `veyra-update-*`
temporary folder holds `update.log`, the archive and extraction; retain its log
when reporting an update failure. If rollback itself fails, the log identifies
the retained backup for manual recovery. Successful replacement is not a claim
that all GPU backends in the new version passed hardware validation.

The updater requires `Qt6Network.dll` and `tls/qschannelbackend.dll`; staging and
package checks now enforce both. Publication must continue to include
`package-manifest.json`, exact per-file hashes, `gpuPackage`, a stable numeric
`version`, and asset SHA-256 digests. Existing 2.0.7 release assets expose digests.
First-time migration from older applications still requires a manual extraction
of a release containing this updater.

## Tests

The updater has a separate Qt 6.8+ Core/Network/Test build, avoiding the Windows
player/GPU dependencies for policy and networking-state tests:

```sh
cmake -S tests/updater -B <external-build-dir> -G Ninja
cmake --build <external-build-dir> --parallel 2
ctest --test-dir <external-build-dir> --output-on-failure
python -B tests/updater/installer_tests.py --pwsh <powershell> --owner <external-build-dir>/veyra_update_owner --output <new-external-test-dir>
```

The Windows product CMake also exposes `veyra_online_updater_tests`. Run the
installer tests with **Windows PowerShell 5.1** and a Windows-built owner fixture
(`veyra_update_owner.exe`) before release. A Linux/PowerShell 7 run covers the
actual script's extraction, manifest checks, process handshake, preservation,
restart arguments and rollback in disposable folders, but cannot validate Windows
file locking, ACLs, Schannel deployment, full MSVC linkage or real player teardown.

For release acceptance, use two disposable Windows NVIDIA/AMD packages with a
private data directory: verify cancellation, download/TLS failures, custom DLL
refusal, running playback/export refusal, normal exit, replacement/restart,
locked-file rollback and user profile preservation. Check the cold-extracted
package includes the network DLL/TLS plugin. Those Windows checks were **not
executed** in the Linux cloud environment; this change is a draft for review.
