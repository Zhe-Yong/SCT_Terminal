# SuperCommandTerminal Readme File
![Static Badge](https://img.shields.io/badge/build-passing-brightgreen?style=for-the-badge) ![GitHub last commit](https://img.shields.io/github/last-commit/Zhe-Yong/SCT_Terminal)

# Still updating

## Layout

Source code, resources, and build scripts are under `Code/`; project
documentation is at the repository root. Build output goes to `Code/build/` for
debug builds; all release output is under the project-root `release/` folder,
split into `normal/` and `random/`.

```text
SuperCommandTerminal/
├── .vscode/          VS Code debug/build tasks (targets Code/)
├── Code/             source, resources, tools, build/
├── release/
│   ├── normal/       normal release builds and ZIPs
│   └── random/       random-identity release builds and ZIPs
├── ADD_COMMAND.md    command maintenance guide
├── CHANGELOG.md      change log
├── DLL.md            plugin authoring guide
├── LICENSE
└── README.md
```

Documentation:

- `ADD_COMMAND.md` describes command registration, argument schemas, and maintenance conventions.
- `DLL.md` describes the plugin ABI, exports, host API, build steps, and signing requirements.
- `CHANGELOG.md` records notable behavior changes.

Build a normal release from the repository root:

```powershell
.\Code\tools\build-release.ps1 -Arch all -Zip
```

Build the random release (16-character random window/console title and runtime
process image name; distributed executables are still `SCT64.exe` /
`SCT32.exe`):

```powershell
.\Code\tools\build-release.ps1 -Arch all -RandomTitle -Zip
```

At startup the random build briefly runs `SCT64.exe` / `SCT32.exe`, copies
itself to `%LOCALAPPDATA%\SCT\runtime\<random>.exe`, launches that copy, and
then exits the original process. The random copy has no `SCT` prefix. Old
runtime copies are cleaned on the next launch, and plugins, `autoexec.sct`,
NSudo, and PPL DLLs are loaded from the original release folder.

Normal releases are written to `release/normal/`; random releases are
written to `release/random/`. Each folder contains its own `SCT_*.zip` archives.