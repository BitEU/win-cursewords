# Bulk-fetch free .puz crossword puzzles via xword-dl.
# Usage:
#   .\fetch.ps1                  # default: last 60 days
#   .\fetch.ps1 -Days 30
#   .\fetch.ps1 -Days 365 -Outlets lat,uni,nd
#
# Skips outlets that need auth (NYT). Errors per-puzzle are non-fatal —
# missing-date / paywall responses just get logged and we keep going.

param(
    [int]$Days = 60,
    [string[]]$Outlets = @(
        # Daily outlets that support --date and don't require auth.
        'atl',   # Atlantic (weekly-ish)
        'lat',   # LA Times (daily)
        'latm',  # LA Times Mini (daily)
        'nd',    # Newsday (daily)
        'tny',   # The New Yorker (daily Mon-Fri)
        'tnym',  # The New Yorker Mini (daily)
        'uni',   # Universal (daily)
        'usa',   # USA Today (daily)
        'wp',    # Washington Post (Sun, occasionally)
        'pop',   # Daily Pop (daily)
        'club',  # Crossword Club (weekly)
        'pzm',   # Puzzmo (daily)
        'pzmb',  # Puzzmo Big (weekly)
        'sdp',   # Simply Daily (daily)
        'sdpc',  # Simply Daily Cryptic (daily)
        'sdpq',  # Simply Daily Quick (daily)
        'prince',      # Daily Princetonian (variable)
        'prince-mini', # Daily Princetonian Mini
        'vult'   # Vulture 10x10 (weekly)
    )
)

$ErrorActionPreference = 'Continue'
$start = (Get-Date).Date
$end = $start.AddDays(-$Days + 1)

$total = 0
$ok = 0
$fail = 0
$skipped = 0

# Subdir per outlet keeps things tidy.
foreach ($o in $Outlets) {
    $dir = Join-Path $PSScriptRoot $o
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
}

Write-Host "Fetching $Days days across $($Outlets.Count) outlets..." -ForegroundColor Cyan

for ($d = $start; $d -ge $end; $d = $d.AddDays(-1)) {
    $dateStr = $d.ToString('M/d/yy')
    foreach ($o in $Outlets) {
        $total++
        $dir = Join-Path $PSScriptRoot $o
        $namePattern = Join-Path $dir ($o + '-' + $d.ToString('yyyyMMdd') + '*.puz')
        if (Get-ChildItem $namePattern -ErrorAction SilentlyContinue) {
            $skipped++
            continue
        }
        $outName = Join-Path $dir ($o + '-' + $d.ToString('yyyyMMdd') + '.puz')
        $stderr = & xword-dl $o --date $dateStr -o $outName 2>&1
        if ($LASTEXITCODE -eq 0 -and (Test-Path $outName)) {
            $ok++
            Write-Host "  [ok]   $o $dateStr" -ForegroundColor Green
        } else {
            $fail++
            # Most failures = "no puzzle for that date" — only print on -Verbose.
            Write-Verbose "  [fail] $o $dateStr -> $stderr"
        }
    }
}

Write-Host ""
Write-Host "Done. tried=$total ok=$ok fail=$fail skipped=$skipped" -ForegroundColor Cyan
$count = (Get-ChildItem -Path $PSScriptRoot -Recurse -Filter *.puz | Measure-Object).Count
Write-Host "Total .puz files in puzzles/: $count" -ForegroundColor Cyan
