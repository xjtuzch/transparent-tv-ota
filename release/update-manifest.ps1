<#
  生成 release/manifest.json

  用法示例(在 release 目录下执行):
    .\update-manifest.ps1 -Build 31 -Version 31 `
      -BinPath .\TransparentScreen.ino.bin `
      -AssetUrl "https://github.com/你的用户名/你的仓库/releases/download/v31/TransparentScreen.ino.bin"
#>
param(
    [Parameter(Mandatory = $true)][int]$Build,
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$BinPath,
    [Parameter(Mandatory = $true)][string]$AssetUrl
)

if (-not (Test-Path $BinPath)) {
    Write-Error "找不到固件文件: $BinPath"
    exit 1
}

$md5 = (Get-FileHash -Algorithm MD5 -Path $BinPath).Hash.ToLower()

$manifest = [ordered]@{
    build   = $Build
    version = $Version
    url     = $AssetUrl
    md5     = $md5
}

$json = $manifest | ConvertTo-Json
$json | Set-Content -Path "manifest.json" -Encoding utf8

Write-Host "manifest.json 已生成:"
Write-Host $json
Write-Host ""
Write-Host "下一步: git add manifest.json && git commit -m \"manifest: build $Build\" && git push"

