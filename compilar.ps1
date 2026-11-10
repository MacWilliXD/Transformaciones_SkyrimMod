# Compila el plugin y, con -Instalar, lo copia junto a su .ini a Data\SKSE\Plugins, y el plugin de
# Skyrim Platform que sincroniza la raza con SkyMP a Data\Platform\Plugins.
#
#   .\compilar.ps1
#   .\compilar.ps1 -Instalar
#
# Igual que en RolePlayHub: el PATH va antes de vcvars64.bat y VCPKG_ROOT después,
# porque vcvars lo pisa con el vcpkg de Visual Studio.

[CmdletBinding()]
param([switch]$Instalar)

$ErrorActionPreference = 'Stop'
$raiz = $PSScriptRoot

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -products * -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No encuentro Visual Studio con C++." }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$vcpkg = $env:VCPKG_ROOT
if (-not $vcpkg -or $vcpkg -like '*\VC\vcpkg*' -or -not (Test-Path (Join-Path $vcpkg 'vcpkg.exe'))) {
    foreach ($c in @("$env:USERPROFILE\vcpkg", 'C:\vcpkg', 'C:\dev\vcpkg')) {
        if (Test-Path (Join-Path $c 'vcpkg.exe')) { $vcpkg = $c; break }
    }
}
if (-not (Test-Path (Join-Path $vcpkg 'vcpkg.exe'))) { throw "No encuentro vcpkg." }

$cmakeBin = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaBin = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$comando = @(
    "set `"PATH=$cmakeBin;$ninjaBin;$(Split-Path $vswhere);%PATH%`""
    "call `"$vcvars`" >nul"
    "set `"VCPKG_ROOT=$vcpkg`""
    "cd /d `"$raiz`""
    "cmake --preset release"
    "cmake --build --preset release"
) -join ' && '

# Los avisos de cmake van por stderr: con 'Stop', PowerShell los tomaría por un fallo.
$ErrorActionPreference = 'Continue'
& cmd /c "$comando 2>&1"
$ErrorActionPreference = 'Stop'
if ($LASTEXITCODE -ne 0) { Write-Host "Fallo la compilacion." -ForegroundColor Red; exit $LASTEXITCODE }

$dll = 'C:\Users\ASUS\build\Transformaciones\release\Transformaciones.dll'
Write-Host "Compilado: $dll" -ForegroundColor Green

# El script de Papyrus se compila con el compilador y las fuentes de la instalación del Creation Kit.
$ck = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition'
$fuentesJuego = Join-Path $ck 'Data\Source\Scripts'
$pex = Join-Path $raiz 'Scripts'
& (Join-Path $ck 'Papyrus Compiler\PapyrusCompiler.exe') 'TransformacionesAPI.psc' `
    "-f=$(Join-Path $fuentesJuego 'TESV_Papyrus_Flags.flg')" "-i=$(Join-Path $raiz 'Scripts\Source');$fuentesJuego" "-o=$pex"
if ($LASTEXITCODE -ne 0) { Write-Host "Fallo el script de Papyrus." -ForegroundColor Red; exit $LASTEXITCODE }

$sync = Join-Path $raiz 'Platform\Plugins\transformaciones-sync.js'

# El paquete para repartir, con la misma estructura que la carpeta Data.
$paquete = Join-Path $raiz 'paquete\Transformaciones'
foreach ($d in 'SKSE\Plugins', 'Scripts', 'Platform\Plugins') { New-Item -ItemType Directory -Force (Join-Path $paquete $d) | Out-Null }
Copy-Item $dll, (Join-Path $raiz 'Transformaciones.ini') (Join-Path $paquete 'SKSE\Plugins') -Force
Copy-Item (Join-Path $pex 'TransformacionesAPI.pex') (Join-Path $paquete 'Scripts') -Force
Copy-Item $sync (Join-Path $paquete 'Platform\Plugins') -Force
Compress-Archive -Path (Join-Path $paquete '*') -DestinationPath (Join-Path $raiz 'paquete\Transformaciones.zip') -Force
Write-Host "Paquete: $(Join-Path $raiz 'paquete\Transformaciones.zip')" -ForegroundColor Green

if ($Instalar) {
    # La copia de Eldervale (1.6.1170, con SKSE y el cliente de SkyMP).
    $juego = 'C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition Eldervale'
    $plugins = Join-Path $juego 'Data\SKSE\Plugins'
    Copy-Item $dll $plugins -Force
    # El .ini instalado guarda la cámara de cada raza: no se pisa, solo se le añaden las secciones nuevas.
    $iniRepo = Join-Path $raiz 'Transformaciones.ini'
    $iniJuego = Join-Path $plugins 'Transformaciones.ini'
    if (-not (Test-Path $iniJuego)) {
        Copy-Item $iniRepo $plugins -Force
    } else {
        $instalado = Get-Content $iniJuego -Raw -Encoding UTF8
        $bloques = [regex]::Split((Get-Content $iniRepo -Raw -Encoding UTF8), '(?m)^(?=\[)') | Where-Object { $_.Trim() }
        foreach ($bloque in $bloques) {
            $cabecera = ($bloque -split "`r?`n")[0].Trim()
            if ($instalado -notmatch "(?m)^$([regex]::Escape($cabecera))\s*$") {
                Add-Content $iniJuego ("`r`n" + $bloque.TrimEnd()) -Encoding UTF8
                Write-Host "Seccion nueva en el .ini instalado: $cabecera"
            }
        }
    }
    Copy-Item (Join-Path $pex 'TransformacionesAPI.pex') (Join-Path $juego 'Data\Scripts') -Force
    Copy-Item $sync (Join-Path $juego 'Data\Platform\Plugins') -Force
    Write-Host "Instalado en: $juego\Data" -ForegroundColor Green
}
