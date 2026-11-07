# Runs a plug-in's real installer on this machine, checks the installed plug-in with the validator and
# all the tests, then uninstalls it and checks that it's gone. CI runs this before publishing a release.
# It writes to Program Files, so it needs admin rights and is meant for CI machines.
#
# Usage: test-installer.ps1 -Name <name> -Installer <Setup.exe> -Validator <validator.exe> -Tests <tests program>
#        [-ExtraLicenses <file names that the plug-in's installer adds to the usual ones, separated by commas>]

param(
    [Parameter(Mandatory)] [string] $Name,
    [Parameter(Mandatory)] [string] $Installer,
    [Parameter(Mandatory)] [string] $Validator,
    [Parameter(Mandatory)] [string] $Tests,
    [string] $ExtraLicenses = ''
)

$ErrorActionPreference = 'Stop'

function Assert-That ([bool] $Condition, [string] $What) {
    if (-not $Condition) {
        Write-Output "::error::Installer test failed: $What"
        exit 1
    }
    Write-Output "ok   $What"
}

$bundle = Join-Path $env:CommonProgramW6432 "VST3\$Name.vst3"
$binary = Join-Path $bundle "Contents\x86_64-win\$Name.vst3"
$licenses = Join-Path $bundle 'Contents\Resources\Licenses'
$uninstaller = Join-Path $env:ProgramW6432 "Hadronyche\$Name\unins000.exe"

Assert-That (-not (Test-Path $bundle)) 'nothing is installed yet'

$install = Start-Process -FilePath $Installer -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
Assert-That ($install.ExitCode -eq 0) 'the installer succeeds'

Get-ChildItem -Recurse -Force $bundle | ForEach-Object { Write-Output "  $($_.FullName)" }

Assert-That (Test-Path $binary) 'the plug-in is in the VST3 folder'
Assert-That (Test-Path (Join-Path $bundle 'Contents\Resources\moduleinfo.json')) 'its module info is there'
$extra = @($ExtraLicenses -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$licenseNames = @("$Name-LICENSE.txt", 'VST3-SDK-LICENSE.txt', 'VSTGUI-LICENSE.txt', 'HadronycheCore-LICENSE.txt') + $extra + @('NOTICE.txt')
foreach ($licenseName in $licenseNames) {
    Assert-That (Test-Path (Join-Path $licenses $licenseName)) "$licenseName is there"
}
Assert-That (Test-Path $uninstaller) 'the uninstaller is outside the VST3 folder'

& $Validator $bundle | Select-Object -Last 2
Assert-That ($LASTEXITCODE -eq 0) 'the validator passes on the installed plug-in'

& $Tests $binary | Select-Object -Last 1
Assert-That ($LASTEXITCODE -eq 0) 'all the tests pass on the installed plug-in'

Start-Process -FilePath $uninstaller -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait

# The uninstaller hands over to a temporary copy of itself, so it finishes after this returns.
$deadline = (Get-Date).AddSeconds(60)
while ((Test-Path $bundle) -or (Test-Path $uninstaller)) {
    if ((Get-Date) -gt $deadline) { break }
    Start-Sleep -Seconds 1
}

Assert-That (-not (Test-Path $bundle)) 'uninstalling removes the plug-in'
Assert-That (-not (Test-Path $uninstaller)) 'uninstalling removes the uninstaller'
