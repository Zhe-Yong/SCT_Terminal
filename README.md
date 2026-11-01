# SuperCommandTerminal Readme File
![Static Badge](https://img.shields.io/badge/build-passing-brightgreen?style=for-the-badge) ![GitHub last commit](https://img.shields.io/github/last-commit/Zhe-Yong/SCT_Terminal)

# Still updating

## Layout

Source code, resources, build scripts, and documentation are under `Code/`.
Build output goes to `Code/build/` for debug builds; all release output is under
the project-root `release/` folder, split into `normal/` and `random/`.

```text
SuperCommandTerminal/
├── .vscode/     VS Code debug/build tasks (targets Code/)
├── Code/        source, resources, tools, build/
├── release/
│   ├── normal/  normal release builds and ZIPs
│   └── random/  random-title release builds and ZIPs
├── LICENSE
└── README.md
```

Build a normal release from the repository root:

```powershell
.\Code\tools\build-release.ps1 -Arch all -Zip
```

Build the random-title release (16-character random window/console title,
executables still named `SCT64.exe` / `SCT32.exe`):

```powershell
.\Code\tools\build-release.ps1 -Arch all -RandomTitle -Zip
```

Normal releases are written to `release/normal/`; random-title releases are
written to `release/random/`. Each folder contains its own `SCT_*.zip` archives.