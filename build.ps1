# Builds the standalone ASI plugin: dist\SetTeamLives.asi
# (VS 18 Community, same toolchain as the UE4SS variant).
$ErrorActionPreference = "Stop"

$projectRoot = $PSScriptRoot
$outAsi = Join-Path $projectRoot "dist\SetTeamLives.asi"
$buildDir = "C:\jjkcc-build\force-gauge-lives-asi"

Import-Module "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "C:\Program Files\Microsoft Visual Studio\18\Community" -SkipAutomaticLocation -DevCmdArguments "-arch=amd64"

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path $outAsi) | Out-Null

$objs = @()
foreach ($src in @("asi_entry.cpp", "force_gauge.cpp")) {
    $objFile = Join-Path $buildDir ($src -replace '\.cpp$','.obj')
    $source = Join-Path $projectRoot "cpp\$src"
    cl.exe /nologo /std:c++latest /EHsc /MD /O2 /c $source "/Fo$objFile"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $objs += $objFile
}

$linkOut = Join-Path $buildDir "SetTeamLives.asi"
link.exe /nologo /DLL $objs user32.lib "/OUT:$linkOut"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Copy-Item -Force $linkOut $outAsi
Write-Host "Built $outAsi"
