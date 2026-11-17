param(
    [ValidateSet('x64', 'x86')]
    [string]$Arch = 'x64'
)
$ErrorActionPreference = 'Stop'
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
if (!(Test-Path -LiteralPath $vcvars)) {
    throw "vcvarsall.bat not found at $vcvars"
}

$outDir = Join-Path $PSScriptRoot "build\$Arch"
New-Item -ItemType Directory -Force $outDir | Out-Null
Copy-Item -Force (Join-Path $PSScriptRoot "BonDriver_WebOS.ini") (Join-Path $outDir "BonDriver_WebOS.ini")

$cmdFile = Join-Path $outDir "build.cmd"
$lines = @(
    '@echo off',
    "call `"$vcvars`" $Arch",
    'if errorlevel 1 exit /b 1',
    "cd /d `"$PSScriptRoot`"",
    "cl /nologo /LD /EHsc /std:c++17 /utf-8 /W4 /O2 /MT /DUNICODE /D_UNICODE BonDriver_WebOS.cpp /Fo:`"$outDir\BonDriver_WebOS.obj`" /Fe:`"$outDir\BonDriver_WebOS.dll`" /link /def:BonDriver_WebOS.def ws2_32.lib",
    'if errorlevel 1 exit /b 1',
    "cl /nologo /EHsc /std:c++17 /utf-8 /W4 /O2 /MT /DUNICODE /D_UNICODE test_bondriver.cpp /Fo:`"$outDir\test_bondriver.obj`" /Fe:`"$outDir\test_bondriver.exe`"",
    'if errorlevel 1 exit /b 1',
    "cl /nologo /EHsc /std:c++17 /utf-8 /W4 /O2 /MT /DUNICODE /D_UNICODE wake_tv.cpp /Fo:`"$outDir\wake_tv.obj`" /Fe:`"$outDir\wake_tv.exe`"",
    'exit /b %errorlevel%'
)
[System.IO.File]::WriteAllLines($cmdFile, $lines, [System.Text.Encoding]::ASCII)
& $env:ComSpec /d /c $cmdFile
if ($LASTEXITCODE -ne 0) { throw "Build failed for $Arch" }
Write-Output "Built $Arch successfully in $outDir"
