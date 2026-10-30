## 2026.10.29
This is the start of this file.

## 2026.10.29
- Added code-derived 8-character build IDs (CRC32 of the source SHA-256) and the `version [self|DLL_NAME]` command.
- Added plugin `dll_ver` reporting through `sct_plugin_dll_ver` and DLL FileVersion fallback.
- Changed elevation to close and relaunch SCT as the target identity instead of using helper sessions.
- Fixed relaunched elevated windows opening minimized or hidden.
- Updated the startup banner to derive its version and width automatically.
