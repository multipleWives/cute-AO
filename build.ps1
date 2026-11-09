param(
    [string]$AESDKRoot = $env:AESDKRoot
)

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio Installer's vswhere.exe was not found."
}

$vsRoot = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
$msbuild = Join-Path $vsRoot "MSBuild\Current\Bin\amd64\MSBuild.exe"
if (-not $vsRoot -or -not (Test-Path -LiteralPath $msbuild)) {
    throw "Visual Studio MSBuild was not found. Install Visual Studio with Desktop development with C++."
}
if (-not $AESDKRoot -or -not (Test-Path -LiteralPath (Join-Path $AESDKRoot "Examples\Headers\AE_Effect.h"))) {
    throw "After Effects SDK not found at: $AESDKRoot"
}

& $msbuild "$PSScriptRoot\Win\CuteAO.vcxproj" /m /p:Configuration=Release /p:Platform=x64 /p:AESDKRoot="$AESDKRoot"
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "Built: $PSScriptRoot\dist\cute-AO.aex"
