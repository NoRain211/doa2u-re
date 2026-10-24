# SPDX-License-Identifier: GPL-3.0-or-later
# Adapted from committed DOA3 tools/launcher.ps1 at 26672b6.
Add-Type -AssemblyName System.Windows.Forms
$ErrorActionPreference = 'Stop'
trap { [Windows.Forms.MessageBox]::Show("$_", 'DOA2U launcher') | Out-Null; exit 1 }
$root = Split-Path $PSScriptRoot -Parent
$settingsPath = Join-Path $root 'private\launcher.json'
$heights = 480, 720, 1080, 1440, 2160
$samples = 1, 2, 4, 8, 16, 32
$settings = @{ height = [Windows.Forms.Screen]::PrimaryScreen.Bounds.Height; msaa = 32; smaa = $true; widescreen = $true; skipIntro = $false; surround = $false }
try {
    $saved = Get-Content $settingsPath -Raw -ErrorAction Stop | ConvertFrom-Json
    if ($heights -contains $saved.height) { $settings.height = [int]$saved.height }
    if ($samples -contains $saved.msaa) { $settings.msaa = [int]$saved.msaa }
    foreach ($key in 'smaa', 'widescreen', 'skipIntro', 'surround') {
        if ($saved.$key -is [bool]) { $settings[$key] = $saved.$key }
    }
} catch {}

$form = New-Object Windows.Forms.Form
$form.Text = 'Dead or Alive 2 Ultimate'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.AutoScaleMode = 'Font'
$form.Font = New-Object Drawing.Font 'Segoe UI', 10
$form.ClientSize = New-Object Drawing.Size 360, 324

function Add-Choice($text, $top, $items, $index) {
    $label = New-Object Windows.Forms.Label -Property @{ Text = $text; Left = 16; Top = $top + 4; Width = 112 }
    $box = New-Object Windows.Forms.ComboBox -Property @{ Left = 136; Top = $top; Width = 208; DropDownStyle = 'DropDownList'; AccessibleName = $text.Replace('&', '') }
    $box.Items.AddRange($items)
    $box.SelectedIndex = [Math]::Max(0, $index)
    $form.Controls.AddRange(@($label, $box))
    $box
}
$heightNames = [string[]]($heights | ForEach-Object {
    if ($_ -eq 480) { '480p (windowed)' } elseif ($_ -eq 2160) { '2160p (4K)' } else { "$($_)p" }
})
$nearest = @($heights | Where-Object { $_ -le [int]$settings.height }).Count - 1
$resolution = Add-Choice '&Resolution' 16 $heightNames $nearest
$msaaNames = [string[]]($samples | ForEach-Object { if ($_ -eq 1) { 'Off' } else { "$($_)x" } })
$msaa = Add-Choice '&MSAA' 56 $msaaNames ([Array]::IndexOf($samples, [int]$settings.msaa))
$smaa = New-Object Windows.Forms.CheckBox -Property @{ Text = '&SMAA Ultra'; Left = 136; Top = 96; Width = 208; Checked = $settings.smaa }
$widescreen = New-Object Windows.Forms.CheckBox -Property @{ Text = '&Widescreen (16:9)'; Left = 136; Top = 128; Width = 208; Checked = $settings.widescreen }
$skipIntro = New-Object Windows.Forms.CheckBox -Property @{ Text = 'Skip &intro movies'; Left = 136; Top = 160; Width = 208; Checked = $settings.skipIntro }
$surround = New-Object Windows.Forms.CheckBox -Property @{ Text = 'S&urround 5.1'; Left = 136; Top = 192; Width = 208; Checked = $settings.surround }
$note = New-Object Windows.Forms.Label -Property @{ Text = 'Unsupported MSAA falls back to GPU support.'; Left = 16; Top = 232; Width = 328; Height = 40 }
$play = New-Object Windows.Forms.Button -Property @{ Text = '&Play'; Left = 264; Top = 280; Width = 80; DialogResult = 'OK' }
$cancel = New-Object Windows.Forms.Button -Property @{ Text = 'Cancel'; Left = 176; Top = 280; Width = 80; DialogResult = 'Cancel' }
$form.Controls.AddRange(@($smaa, $widescreen, $skipIntro, $surround, $note, $cancel, $play))
$form.AcceptButton = $play
$form.CancelButton = $cancel
if ($form.ShowDialog() -ne 'OK') { $form.Dispose(); exit 0 }

$height = $heights[$resolution.SelectedIndex]
$count = $samples[$msaa.SelectedIndex]
$wideOption = if ($widescreen.Checked) { '--widescreen' } else { '--no-widescreen' }
$smaaOption = if ($smaa.Checked) { '--smaa' } else { '--no-smaa' }
$surroundOption = if ($surround.Checked) { '--surround' } else { '--no-surround' }
$introOption = @(if ($skipIntro.Checked) { '--skip-intro' })
New-Item -ItemType Directory -Force (Split-Path $settingsPath) | Out-Null
[pscustomobject]@{ height = $height; msaa = $count; smaa = $smaa.Checked; widescreen = $widescreen.Checked; skipIntro = $skipIntro.Checked; surround = $surround.Checked } |
    ConvertTo-Json | Set-Content $settingsPath -Encoding ASCII
$form.Dispose()
Set-Location $root
& python (Join-Path $root 'tools\run_game.py') --play --resolution $height --msaa $count $wideOption $smaaOption $surroundOption @introOption
exit $LASTEXITCODE
