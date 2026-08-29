# menu-register.ps1 —— Windows 11 新版右键级联菜单注册（稀疏 MSIX + IExplorerCommand）
#
# 流程（官方 learn.microsoft.com/.../grant-identity-to-nonpackaged-apps 与
#       .../integrate-packaged-app-with-file-explorer）：
#   1. 自签名证书（CurrentUser\My，代码签名 EKU）——不存在则创建，可复用
#   2. 证书公钥导入信任库（优先 CurrentUser\TrustedPeople；失败尝试 LocalMachine 需管理员）
#   3. AppxManifest 填入 Publisher（=证书 Subject）→ makeappx 打稀疏包（仅清单）
#   4. signtool 签名 → Add-AppxPackage -ExternalLocation <本目录>
#   5. 资源管理器右键文件 → 「nx 解压」级联（新右键与“显示更多选项”均可见）
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File menu-register.ps1            # 注册/更新
#   powershell -ExecutionPolicy Bypass -File menu-register.ps1 -Remove    # 移除
param(
    [switch]$Remove,
    [string]$PackageDir = ""    # menupkg 目录（含模板与脚本）；默认脚本所在目录
)

$ErrorActionPreference = "Stop"
if (-not $PackageDir) { $PackageDir = $PSScriptRoot }
$DistDir = Split-Path -Parent $PackageDir            # dist\nx（ExternalLocation，含 nx.exe/nxshell.dll）
$PkgName = "nx.zunzip"

function Find-Tool([string]$name) {
    $roots = @("${env:ProgramFiles(x86)}\Windows Kits\10\bin", "$env:ProgramFiles\Windows Kits\10\bin")
    foreach ($r in $roots) {
        if (Test-Path $r) {
            $hit = Get-ChildItem $r -Recurse -Filter $name -ErrorAction SilentlyContinue |
                Where-Object { $_.DirectoryName -match "\\x(64|86)$" } |
                Sort-Object FullName -Descending | Select-Object -First 1
            if ($hit) { return $hit.FullName }
        }
    }
    return $null
}

if ($Remove) {
    Write-Host "[menu] 移除稀疏包……"
    Get-AppxPackage -Name $PkgName -ErrorAction SilentlyContinue | Remove-AppxPackage
    # 清理旧版 HKCU 平级项（升级路径）
    foreach ($k in "nxExtractHere", "nxExtractInto", "nxExtract") {
        Remove-Item "HKCU:\Software\Classes\*\shell\$k" -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host "[menu] 已移除（若菜单仍显示请重启资源管理器）"
    exit 0
}

if (-not (Test-Path "$DistDir\nx.exe"))  { throw "缺少 $DistDir\nx.exe" }
if (-not (Test-Path "$DistDir\nxshell.dll")) { throw "缺少 $DistDir\nxshell.dll" }

# ---- 1. 自签名证书（可复用）----
$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue |
    Where-Object { $_.Subject -eq "CN=nx-shell" } | Select-Object -First 1
if (-not $cert) {
    Write-Host "[menu] 创建自签名证书……"
    $cert = New-SelfSignedCertificate -Type Custom -Subject "CN=nx-shell" `
        -KeyUsage DigitalSignature -FriendlyName "nx shell menu" `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
    if (-not $cert) { throw "证书创建失败" }
} else {
    Write-Host "[menu] 复用已有证书 $($cert.Thumbprint)"
}

# ---- 2. 信任证书（CurrentUser 优先，失败走 LocalMachine）----
$trusted = Get-ChildItem Cert:\CurrentUser\TrustedPeople -ErrorAction SilentlyContinue |
    Where-Object { $_.Thumbprint -eq $cert.Thumbprint }
if (-not $trusted) {
    $cerPath = Join-Path $env:TEMP "nx-shell.cer"
    [IO.File]::WriteAllBytes($cerPath, $cert.RawData)
    try {
        Import-Certificate -FilePath $cerPath -CertStoreLocation Cert:\CurrentUser\TrustedPeople | Out-Null
        Write-Host "[menu] 证书已信任（当前用户）"
    } catch {
        try {
            Import-Certificate -FilePath $cerPath -CertStoreLocation Cert:\LocalMachine\TrustedPeople | Out-Null
            Write-Host "[menu] 证书已信任（本机，需管理员）"
        } catch {
            throw "证书信任失败：请以管理员运行，或手动将 $cerPath 导入 TrustedPeople"
        }
    }
} else {
    Write-Host "[menu] 证书已在 TrustedPeople"
}

# ---- 3. 清单 + makeappx ----
$makeappx = Find-Tool "makeappx.exe"
$signtool = Find-Tool "signtool.exe"
if (-not $makeappx -or -not $signtool) { throw "未找到 makeappx/signtool（需 Windows SDK）" }

$buildDir = Join-Path $PackageDir "build"
New-Item -ItemType Directory -Force -Path "$buildDir\Assets" | Out-Null
Copy-Item "$PackageDir\Assets\logo.png" "$buildDir\Assets\logo.png" -Force
(Get-Content "$PackageDir\AppxManifest.template.xml" -Raw -Encoding UTF8) `
    -replace '\$\{PUBLISHER\}', $cert.Subject |
    Set-Content "$buildDir\AppxManifest.xml" -Encoding UTF8

$msix = Join-Path $buildDir "nx-menu.msix"
Remove-Item $msix -Force -ErrorAction SilentlyContinue
$mkOut = & $makeappx pack -o -nv -d $buildDir -p $msix 2>&1
if ($LASTEXITCODE -ne 0) { throw "makeappx 失败: $mkOut" }

# ---- 4. 签名（导出 PFX 后按文件签名）----
$pfxPath = Join-Path $env:TEMP "nx-shell.pfx"
$pw = ConvertTo-SecureString -String "nxshell" -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath $pfxPath -Password $pw -Force | Out-Null
$sgOut = & $signtool sign -fd SHA256 -p $pfxPath -p nxshell $msix 2>&1
if ($LASTEXITCODE -ne 0) { throw "signtool 失败: $sgOut" }
Remove-Item $pfxPath -Force -ErrorAction SilentlyContinue

# ---- 5. 注册（先卸旧）----
Get-AppxPackage -Name $PkgName -ErrorAction SilentlyContinue | Remove-AppxPackage
Add-AppxPackage -Path $msix -ExternalLocation $DistDir
foreach ($k in "nxExtractHere", "nxExtractInto", "nxExtract") {
    Remove-Item "HKCU:\Software\Classes\*\shell\$k" -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "[menu] 已注册：资源管理器右键文件 → nx 解压（级联：解压到当前目录 / 解压到指定目录…）"
Write-Host "[menu] 若未显示，请重启资源管理器（任务管理器 → Windows 资源管理器 → 重新启动）"
