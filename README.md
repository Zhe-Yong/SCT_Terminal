# SuperCommandTerminal Readme File
![Static Badge](https://img.shields.io/badge/build-passing-brightgreen?style=for-the-badge) ![GitHub last commit](https://img.shields.io/github/last-commit/Zhe-Yong/SCT_Terminal)

# Still updating

## Layout

Source code, resources, build scripts, documentation, and generated build/release
outputs are under `Code/`. The repository root keeps `.vscode/`, `README.md`, and
`LICENSE`.

```text
SuperCommandTerminal/
├── .vscode/     VS Code debug/build tasks (targets Code/)
├── Code/        source, resources, tools, build/, release/
├── LICENSE
└── README.md
```

Build from the repository root:

```powershell
.\Code\tools\build-release.ps1 -Arch x64 -Zip
```