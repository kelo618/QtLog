param(
    [string]$Message = "Update QtLog",
    [string]$Remote = "origin"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Invoke-Git {
    param([Parameter(Mandatory = $true)][string[]]$GitArguments)

    & git @GitArguments
    if ($LASTEXITCODE -ne 0) {
        throw "git $($GitArguments -join ' ') failed with exit code $LASTEXITCODE."
    }
}

$repositoryPath = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location -LiteralPath $repositoryPath

Invoke-Git -GitArguments @("rev-parse", "--is-inside-work-tree")

$branch = (& git branch --show-current).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($branch)) {
    throw "The repository must be on a named branch before it can be pushed."
}

& git remote get-url $Remote *> $null
if ($LASTEXITCODE -ne 0) {
    throw "Git remote '$Remote' does not exist."
}

Invoke-Git -GitArguments @("add", "-A")

& git diff --cached --quiet
$hasChanges = $LASTEXITCODE -eq 1
if ($LASTEXITCODE -gt 1) {
    throw "Unable to inspect staged changes."
}

if ($hasChanges) {
    Invoke-Git -GitArguments @("commit", "-m", $Message)
} else {
    Write-Host "No local changes need to be committed."
}

Invoke-Git -GitArguments @("push", $Remote, $branch)
