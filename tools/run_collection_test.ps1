param(
    [ValidateSet('baseline', 'legacy', 'balanced', 'held', 'png-none', 'png-store', 'capture-only', 'input-only')]
    [string]$Variant = 'png-store',
    [string]$WindowTitle = 'PUBG',
    [ValidateRange(1, 3600)][int]$Seconds = 90,
    [ValidateRange(0, 3599)][double]$WarmupSeconds = 5,
    [int]$GameProcessId = 0,
    [ValidateRange(1, 8192)][int]$RoiWidth = 640,
    [ValidateRange(1, 8192)][int]$RoiHeight = 640,
    [ValidateRange(1, 3600000)][int]$PeriodicMs = 5000,
    [string]$Exe = '',
    [switch]$Interactive
)
$ErrorActionPreference = 'Stop'
if ($Interactive) {
    $choices = @('legacy', 'balanced', 'held', 'png-none', 'capture-only', 'input-only', 'png-store', 'baseline')
    Write-Host '1 legacy       - original capture behavior'
    Write-Host '2 balanced     - release DXGI on pause; fewer window probes'
    Write-Host '3 held         - balanced + retain frame until next capture'
    Write-Host '4 png-none     - held + lossless PNG without row filtering'
    Write-Host '5 capture-only - held capture, no PNG files'
    Write-Host '6 input-only   - input and scheduler, no DXGI or PNG files'
    Write-Host '7 png-store    - held + lossless PNG without compression (larger files)'
    Write-Host '8 baseline     - game frame timings without collector'
    $selection = Read-Host 'Choose 1..8 [Enter = 7]'
    if ($selection) {
        if ($selection -notmatch '^[1-8]$') { throw 'Choose a number from 1 to 8.' }
        $Variant = $choices[[int]$selection - 1]
    }
    $titleAnswer = Read-Host "Window title substring [Enter = $WindowTitle]"
    if ($titleAnswer) { $WindowTitle = $titleAnswer }
}
$taskRoot = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $taskRoot 'build/windows-msvc/Release/pubg_vision_app.exe' }
Write-Host "Variant: $Variant; wall-clock duration: $Seconds seconds; ROI: ${RoiWidth}x${RoiHeight}"
if ($Variant -eq 'baseline') {
    Write-Host 'Switch to the game. Baseline runs for the configured duration; Ctrl+C retains partial results.'
} else {
    Write-Host 'Switch to the game. F8 pauses/resumes, F9 stops early. Use the same scene and click pattern.'
}
Write-Host 'Every game frame is recorded by PresentMon. The first warmup seconds are excluded from the summary.'
if ($Variant -in @('capture-only', 'input-only')) {
    Write-Host 'Diagnostic only: no PNG images will be saved.' -ForegroundColor Yellow
}
$runnerArgs = @((Join-Path $PSScriptRoot 'run_experiment.py'), '--variant', $Variant,
    '--window-title', $WindowTitle, '--seconds', "$Seconds", '--warmup-seconds',
    $WarmupSeconds.ToString([System.Globalization.CultureInfo]::InvariantCulture),
    '--roi-width', "$RoiWidth", '--roi-height', "$RoiHeight", '--periodic-ms', "$PeriodicMs", '--exe', $Exe)
if ($GameProcessId) { $runnerArgs += @('--game-process-id', "$GameProcessId") }
& python @runnerArgs
if ($LASTEXITCODE -ne 0) { throw "Experiment failed (code $LASTEXITCODE). Partial results are retained; see the logs." }
