# Packaging

```powershell
.\scripts\build.ps1 -Configuration Release
.\scripts\package.ps1
```

Output:

- `out\Release\x64\ForgeConductorApp\ForgeConductor.exe`
- `dist\ForgeConductor-0.8.0-win-x64.msi`

The MSI installs to `Program Files\ForgeConductor` and adds a Start Menu shortcut. Uninstall from Apps & features. User data in `%USERPROFILE%\.forge-conductor` is not removed.
