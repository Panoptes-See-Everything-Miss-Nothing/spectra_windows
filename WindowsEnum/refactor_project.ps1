# Refactoring Script: Split GetWindowsApps into modular files

Write-Host "=" *60 -ForegroundColor Cyan
Write-Host "Refactoring Windows Info Gathering Project" -ForegroundColor Cyan
Write-Host "=" *60 -ForegroundColor Cyan
Write-Host ""

# New files created (already exist in file system)
$newFiles = @(
    "src\WindowsInfoGathering\WindowsInfoGathering.h",
    "src\WindowsInfoGathering\MachineInfo.h",
    "src\WindowsInfoGathering\MachineInfo.cpp",
    "src\WindowsInfoGathering\RegistryUtils.h",
    "src\WindowsInfoGathering\RegistryUtils.cpp",
    "src\WindowsInfoGathering\UserProfiles.h",
    "src\WindowsInfoGathering\UserProfiles.cpp",
    "src\WindowsInfoGathering\Win32Apps.h",
    "src\WindowsInfoGathering\Win32Apps.cpp",
    "src\WindowsInfoGathering\AppXPackages.h",
    "src\WindowsInfoGathering\AppXPackages.cpp"
)

# Files to remove (old monolithic files)
$oldFiles = @(
    "src\WindowsInfoGathering\GetWindowsApps.h",
    "src\WindowsInfoGathering\GetWindowsApps.cpp"
)

Write-Host "Step 1: Verifying new files exist..." -ForegroundColor Yellow
foreach ($file in $newFiles) {
    if (Test-Path $file) {
        Write-Host "  ✓ $file" -ForegroundColor Green
    } else {
        Write-Host "  ✗ $file NOT FOUND!" -ForegroundColor Red
    }
}

Write-Host ""
Write-Host "Step 2: Files to be removed from project:" -ForegroundColor Yellow
foreach ($file in $oldFiles) {
    if (Test-Path $file) {
        Write-Host "  - $file" -ForegroundColor Gray
    }
}

Write-Host ""
Write-Host "=" *60 -ForegroundColor Cyan
Write-Host "Next Steps (Manual):" -ForegroundColor Cyan
Write-Host "=" *60 -ForegroundColor Cyan
Write-Host ""
Write-Host "1. In Visual Studio, right-click 'WindowsInfoGathering' folder" -ForegroundColor White
Write-Host "2. Add -> Existing Item... -> Select all new .cpp and .h files" -ForegroundColor White
Write-Host ""
Write-Host "3. Right-click old files and Remove:" -ForegroundColor White
Write-Host "   - GetWindowsApps.h" -ForegroundColor Gray
Write-Host "   - GetWindowsApps.cpp" -ForegroundColor Gray
Write-Host ""
Write-Host "4. Build -> Rebuild Solution" -ForegroundColor White
Write-Host ""
Write-Host "Files are ready in src\WindowsInfoGathering\" -ForegroundColor Green
