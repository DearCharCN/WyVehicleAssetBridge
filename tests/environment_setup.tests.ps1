# Integration checks for the compiled setup form. Never installs dependencies,
# shows a visible window, or writes the real repository's local configuration.
# Run: powershell.exe -NoProfile -STA -File tests\environment_setup.tests.ps1
param([string]$AssemblyPath = '', [string]$SourceConfigRoot = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([Threading.Thread]::CurrentThread.ApartmentState -ne 'STA') {
    throw 'Run this test with Windows PowerShell 5.1 -STA.'
}
$repoRoot = Split-Path -Parent $PSScriptRoot
if (!$AssemblyPath) { $AssemblyPath = Join-Path $repoRoot 'WyVehicleAssetBridgeManager.exe' }
if (!(Test-Path -LiteralPath $AssemblyPath -PathType Leaf)) {
    throw 'Build WyVehicleAssetBridgeManager.exe before running the setup integration tests.'
}
. (Join-Path $PSScriptRoot 'test_support.ps1')
Add-Type -AssemblyName System.Windows.Forms,System.Drawing,System.Web.Extensions
$assembly = [Reflection.Assembly]::LoadFrom((Resolve-Path -LiteralPath $AssemblyPath).ProviderPath)
$configType = $assembly.GetType('WyVehicleAssetBridge.Setup.LocalConfig', $true)
$detectionType = $assembly.GetType('WyVehicleAssetBridge.Setup.Detection', $true)
$formType = $assembly.GetType('WyVehicleAssetBridge.Setup.SetupForm', $true)
$staticFlags = [Reflection.BindingFlags]'Static,Public,NonPublic'
$instanceFlags = [Reflection.BindingFlags]'Instance,Public,NonPublic'
$buildRoot = Join-Path $repoRoot 'ue_plugins\build'
$fixture = Join-Path $buildRoot ('tests-setup-' + [Guid]::NewGuid().ToString('N'))
Assert-TestChild $fixture $buildRoot
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
$forms = New-Object 'System.Collections.Generic.List[System.Windows.Forms.Form]'
$previousContext = [Threading.SynchronizationContext]::Current
$script:passed = 0
$script:failed = 0

function Assert-True([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
}

function Test-Case([string]$Name, [scriptblock]$Action) {
    try { & $Action; $script:passed++; Write-Host "PASS $Name" }
    catch { $script:failed++; Write-Host "FAIL $Name : $($_.Exception.Message)"; Write-Host $_.ScriptStackTrace }
}

function Invoke-Static([Type]$Type, [string]$Name, [object[]]$Arguments) {
    # PowerShell wraps some returned strings/dictionaries in PSObject. Reflection
    # uses the CLR binder, so explicitly unwrap arguments without enumerating them.
    $values = [Array]::CreateInstance([object], $Arguments.Count)
    for ($i = 0; $i -lt $Arguments.Count; $i++) {
        $values[$i] = if ($null -eq $Arguments[$i]) { $null } else { $Arguments[$i].PSObject.BaseObject }
    }
    return $Type.GetMethod($Name, $staticFlags).Invoke($null, $values)
}

function Get-FormField($Form, [string]$Name) {
    return $formType.GetField($Name, $instanceFlags).GetValue($Form)
}

function Copy-Paths($Source) {
    $copy = New-Object 'System.Collections.Generic.Dictionary[string,string]'
    foreach ($key in $Source.Keys) { $copy[$key] = $Source[$key] }
    return ,$copy
}

function New-HiddenForm([string]$Root, $Paths) {
    New-Item -ItemType Directory -Path $Root -Force | Out-Null
    $constructor = $formType.GetConstructor($instanceFlags, $null, [Type[]]@([string]), $null)
    $form = $constructor.Invoke([object[]]@($Root))
    $forms.Add($form)
    $form.ShowInTaskbar = $false
    $form.CreateControl()
    $null = $form.Handle
    [Threading.SynchronizationContext]::SetSynchronizationContext((New-Object Windows.Forms.WindowsFormsSynchronizationContext))
    $formType.GetField('paths', $instanceFlags).SetValue($form, $Paths)
    return $form
}

function Wait-FormTask($Form, [string]$Method) {
    if ($Method -eq 'RefreshPaths') { $arguments = [object[]]@($null) } else { $arguments = [object[]]@() }
    $task = $formType.GetMethod($Method, $instanceFlags).Invoke($Form, $arguments)
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    while (!$task.IsCompleted) {
        [Windows.Forms.Application]::DoEvents()
        if ([DateTime]::UtcNow -gt $deadline) { throw "$Method did not complete within 45 seconds." }
        Start-Sleep -Milliseconds 20
    }
    $null = $task.GetAwaiter().GetResult()
    [Windows.Forms.Application]::DoEvents()
    Assert-True (!(Get-FormField $Form 'busy')) 'Form stayed busy after its task completed.'
    Assert-True (!$Form.Visible) 'Test made a form visible.'
}

function Get-AllControls([Windows.Forms.Control]$Control) {
    foreach ($child in $Control.Controls) { $child; Get-AllControls $child }
}

function Invoke-EnvironmentCheck($Fixture, [string[]]$Arguments=@()) {
    return Invoke-TestProcess (Join-Path $Fixture.Root 'WyVehicleAssetBridgeManager.exe') (@('check-env') + $Arguments) (Get-Location).ProviderPath
}

function Assert-EnvironmentReport([string]$Output) {
    foreach ($key in @('UE.EnginePath','UE.ProjectPath','Max.InstallPath','Build.CMakePath','Max.SdkPath','Build.WindowsSdkPath','Build.VisualStudioPath')) {
        Assert-True ($Output.Contains($key)) "Environment report omitted $key. Output: $Output"
    }
}

function Get-NamedControl($Form, [string]$Name) {
    $matches = @($Form.Controls.Find($Name, $true))
    Assert-True ($matches.Count -eq 1) "Missing or duplicate control: $Name"
    return $matches[0]
}

function Wait-PublishClick($Form, [string]$ButtonName, [bool]$Success=$true) {
    $navigation = Get-NamedControl $Form 'PublishNav'
    $navigation.Checked = $true
    [Windows.Forms.Application]::DoEvents()
    $button = Get-NamedControl $Form $ButtonName
    Assert-True ($button.Enabled) "$ButtonName is disabled despite a saved configuration."
    # PerformClick ignores hidden controls; invoke the actual click event while
    # retaining a fully hidden form and its normal async event handler.
    $click = [Windows.Forms.Button].GetMethod('OnClick', [Reflection.BindingFlags]'Instance,NonPublic')
    $null = $click.Invoke($button, [object[]]@([EventArgs]::Empty))
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    while ((Get-FormField $Form 'busy') -or (Get-FormField $Form 'publishing')) {
        [Windows.Forms.Application]::DoEvents()
        if ([DateTime]::UtcNow -gt $deadline) { throw "$ButtonName did not finish within 45 seconds." }
        Start-Sleep -Milliseconds 20
    }
    [Windows.Forms.Application]::DoEvents()
    Assert-True ($button.Enabled) 'Publish button was not re-enabled after completion.'
    $log = (Get-NamedControl $Form 'PublishLog').Text
    $status = (Get-NamedControl $Form 'PublishStatus').Text
    Assert-True ($log.Length -gt 0) 'Publishing produced no visible log text.'
    $successText = ([string][char]0x6210) + [char]0x529F
    $completedText = ([string][char]0x5B8C) + [char]0x6210
    $failureText = ([string][char]0x5931) + [char]0x8D25
    if ($Success) { Assert-True (($status.Contains($successText) -or $status.Contains($completedText)) -and !$status.Contains($failureText)) "Publish did not report success: $status`n$log" }
    else { Assert-True ($status.Contains($failureText)) "Publish did not report failure: $status`n$log" }
    Assert-True (!$Form.Visible) 'Publishing made the test window visible.'
}

function Get-LocalVisible([Windows.Forms.Control]$Control) {
    # Control.Visible includes ancestor visibility. Tests intentionally keep the
    # top-level form hidden, so read the local visibility bit from WinForms.
    $method = [Windows.Forms.Control].GetMethod('GetState', [Reflection.BindingFlags]'Instance,NonPublic', $null, [Type[]]@([int]), $null)
    return [bool]$method.Invoke($Control, [object[]]@([int]2))
}

function Wait-PublishVersionsApplied($Form, [long]$Before) {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        [Windows.Forms.Application]::DoEvents()
        $applied = [long](Get-FormField $Form 'publishVersionApplied')
        $request = [long](Get-FormField $Form 'publishVersionRequest')
        $pending = [bool](Get-FormField $Form 'publishVersionPending')
        $running = [bool](Get-FormField $Form 'publishVersionRefreshRunning')
        if ($applied -gt $Before -and $applied -eq $request -and !$pending -and !$running) { return }
        if ([DateTime]::UtcNow -gt $deadline) { throw "Version refresh did not settle. Before=$Before Applied=$applied Request=$request Pending=$pending Running=$running" }
        Start-Sleep -Milliseconds 20
    } while ($true)
}

function Refresh-PublishVersions($Form) {
    $before = [long](Get-FormField $Form 'publishVersionApplied')
    (Get-NamedControl $Form 'EnvironmentNav').Checked = $true
    (Get-NamedControl $Form 'PublishNav').Checked = $true
    Wait-PublishVersionsApplied $Form $before
}

function Assert-PublishVersionState($Form, [bool]$UeMismatch, [bool]$MaxMismatch) {
    $green = [Drawing.Color]$formType.GetField('Good', $staticFlags).GetValue($null)
    foreach ($item in @(@{Target='UE'; Mismatch=$UeMismatch}, @{Target='Max'; Mismatch=$MaxMismatch})) {
        $warning = Get-NamedControl $Form ('PublishMismatch' + $item.Target)
        $shown = (Get-LocalVisible $warning) -and ![String]::IsNullOrWhiteSpace($warning.Text)
        Assert-True ($shown -eq $item.Mismatch) "Unexpected mismatch hint for $($item.Target)."
        $button = Get-NamedControl $Form ('Publish' + $item.Target)
        Assert-True (($button.BackColor.ToArgb() -eq $green.ToArgb()) -eq $item.Mismatch) "Unexpected mismatch button color for $($item.Target)."
    }
    $all = Get-NamedControl $Form 'PublishAll'
    Assert-True ($all.BackColor.ToArgb() -eq [Drawing.Color]::White.ToArgb()) 'Publish All should keep its white background.'
    $changed = Get-NamedControl $Form 'PublishChanged'
    Assert-True ((Get-LocalVisible $changed) -eq ($UeMismatch -or $MaxMismatch)) 'Incremental action is not restricted to known mismatches.'
    if ($UeMismatch -or $MaxMismatch) {
        Assert-True ($changed.Enabled -and $changed.BackColor.ToArgb() -eq $green.ToArgb()) 'Incremental action is not enabled and green.'
        $allPosition = $Form.PointToClient($all.PointToScreen([Drawing.Point]::Empty))
        $changedPosition = $Form.PointToClient($changed.PointToScreen([Drawing.Point]::Empty))
        Assert-True ($changedPosition.X -ge $allPosition.X + $all.Width) 'Incremental action is not positioned to the right of Publish All.'
        $gap = $changedPosition.X - ($allPosition.X + $all.Width)
        $graphics = $Form.CreateGraphics()
        try { $maximumGap = [Math]::Ceiling(24 * $graphics.DpiX / 96.0) } finally { $graphics.Dispose() }
        Assert-True ($gap -le $maximumGap) "Incremental action is not adjacent to Publish All (gap $gap px, maximum $maximumGap px)."
    }
}

function Assert-InstalledHashLabel($Form, [string]$Target, [string]$Fingerprint) {
    $label = Get-NamedControl $Form ('PublishVersion' + $Target)
    Assert-True ($label.Text.Contains($Fingerprint.Substring(0,8))) "Installed $Target fingerprint is not shown in the version label."
    $tips = Get-FormField $Form 'tips'
    Assert-True ($tips.GetToolTip($label).Contains($Fingerprint)) "Full installed $Target fingerprint is missing from the tooltip."
}

try {
    # By default use complete fake installed tools, including version metadata.
    # A caller may explicitly provide a real config root for read-only validation.
    if (!$SourceConfigRoot) {
        $seedFixture = New-TestBridgeFixture $fixture 'valid environment seed' $repoRoot $AssemblyPath
        $SourceConfigRoot = $seedFixture.Root
    }
    $configured = Invoke-Static $configType Read @($SourceConfigRoot)
    $found = Invoke-Static $detectionType Scan @($SourceConfigRoot, $configured)
    $seed = Copy-Paths $configured
    foreach ($key in $found.Keys) {
        if (!$seed.ContainsKey($key) -or !$seed[$key]) { $seed[$key] = $found[$key] }
    }
    # Legacy INIs may name cmake rather than its absolute executable path.
    if ($seed['CMake'] -eq 'cmake') {
        $seed['CMake'] = (Get-Command cmake.exe -CommandType Application).Source
    }
    foreach ($key in @('Engine','Project','Max')) {
        $issue = Invoke-Static $configType ValidateApplication @($key, $seed[$key])
        if ($issue) { throw "Integration fixture requires an existing valid $key installation/path: $issue" }
    }
    foreach ($key in @('CMake','MaxSdk','WindowsSdk','Toolchain')) {
        $issue = Invoke-Static $detectionType Validate @($key, $seed[$key])
        if ($issue) { throw "Integration fixture requires an existing valid $key installation/path: $issue" }
    }

    $headless = New-TestBridgeFixture $fixture 'headless environment checks' $repoRoot $AssemblyPath
    $environmentItems = @(
        @{Key='UE.EnginePath'; Path=$headless.Engine},
        @{Key='UE.ProjectPath'; Path=$headless.Project},
        @{Key='Max.InstallPath'; Path=$headless.Max},
        @{Key='Build.CMakePath'; Path=$headless.CMake},
        @{Key='Max.SdkPath'; Path=$headless.MaxSdk},
        @{Key='Build.WindowsSdkPath'; Path=$headless.WindowsSdk},
        @{Key='Build.VisualStudioPath'; Path=$headless.VisualStudio}
    )

    Test-Case 'Headless check-env reports all seven valid paths without modifying configuration or building' {
        $original = [Convert]::ToBase64String([IO.File]::ReadAllBytes($headless.Config))
        $result = Invoke-EnvironmentCheck $headless
        Assert-True ($result.ExitCode -eq 0) ("Valid default INI failed check-env: " + $result.Output)
        Assert-EnvironmentReport $result.Output
        foreach ($item in $environmentItems) { Assert-True ($result.Output.Contains($item.Path)) "Report did not display the checked path for $($item.Key)." }
        Assert-True ($original -eq [Convert]::ToBase64String([IO.File]::ReadAllBytes($headless.Config))) 'Read-only environment check rewrote the default INI.'
        Assert-True (!(Test-Path -LiteralPath (Join-Path $headless.Root '.build.lock'))) 'Environment inspection acquired a build lock.'
        foreach ($path in @($headless.LatestUE,$headless.LatestMax,$headless.DeployUE,$headless.DeployMax)) {
            Assert-True (!(Test-Path -LiteralPath $path)) 'Environment inspection built or deployed a package.'
        }
        Assert-True (@(Get-ChildItem -LiteralPath (Join-Path $headless.Root 'events') -File).Count -eq 0) 'Environment inspection launched a fake compiler.'
        $commandHelp = Invoke-EnvironmentCheck $headless @('-Help')
        Assert-True ($commandHelp.ExitCode -eq 0 -and $commandHelp.Output.Contains('check-env')) 'check-env -Help did not return command usage.'
        $globalHelp = Invoke-TestProcess (Join-Path $headless.Root 'WyVehicleAssetBridgeManager.exe') @('--help') (Get-Location).ProviderPath
        Assert-True ($globalHelp.ExitCode -eq 0 -and $globalHelp.Output.Contains('check-env') -and $globalHelp.Output.Contains('publish')) 'Global help omitted an available command.'
    }

    Test-Case 'Headless check-env reports every invalid configured path and never substitutes installed alternatives' {
        $original = [IO.File]::ReadAllText($headless.Config)
        $invalid = $original
        $badPaths = @()
        foreach ($item in $environmentItems) {
            $iniKey = $item.Key.Substring($item.Key.LastIndexOf('.') + 1)
            $badPath = Join-Path $headless.Root ('missing installations\' + $item.Key.Replace('.','_'))
            $invalid = $invalid.Replace($iniKey + '=' + $item.Path, $iniKey + '=' + $badPath)
            $badPaths += $badPath
        }
        try {
            Write-TestFile $headless.Config $invalid
            $before = [Convert]::ToBase64String([IO.File]::ReadAllBytes($headless.Config))
            $result = Invoke-EnvironmentCheck $headless
            Assert-True ($result.ExitCode -ne 0) 'Seven invalid configured paths returned success.'
            Assert-EnvironmentReport $result.Output
            foreach ($path in $badPaths) { Assert-True ($result.Output.Contains($path)) "Invalid configured path was hidden or replaced: $path" }
            Assert-True ($before -eq [Convert]::ToBase64String([IO.File]::ReadAllBytes($headless.Config))) 'Invalid settings were silently repaired or overwritten.'
        } finally { Write-TestFile $headless.Config $original }
    }

    Test-Case 'Headless check-env honors a custom INI and resolves its relative paths from that file' {
        $original = [IO.File]::ReadAllText($headless.Config)
        $custom = Join-Path $headless.Root 'custom settings\environment.ini'
        $customText = $original
        foreach ($item in $environmentItems) {
            $iniKey = $item.Key.Substring($item.Key.LastIndexOf('.') + 1)
            $relative = '..\' + $item.Path.Substring($headless.Root.Length).TrimStart('\')
            $customText = $customText.Replace($iniKey + '=' + $item.Path, $iniKey + '=' + $relative)
        }
        Write-TestFile $custom $customText
        try {
            # A broken adjacent default proves that the explicitly selected file
            # is actually used, instead of merely accepting its command argument.
            Write-TestFile $headless.Config "[UE]`r`nProjectPath=missing-default-project`r`n"
            $defaultBefore = [Convert]::ToBase64String([IO.File]::ReadAllBytes($headless.Config))
            $customBefore = [Convert]::ToBase64String([IO.File]::ReadAllBytes($custom))
            $result = Invoke-EnvironmentCheck $headless @('-Config',$custom)
            Assert-True ($result.ExitCode -eq 0) ("Valid custom INI failed check-env: " + $result.Output)
            Assert-EnvironmentReport $result.Output
            foreach ($item in $environmentItems) { Assert-True ($result.Output.Contains($item.Path)) "Relative custom path resolved incorrectly for $($item.Key)." }
            Assert-True ($defaultBefore -eq [Convert]::ToBase64String([IO.File]::ReadAllBytes($headless.Config))) 'Custom check changed the adjacent default INI.'
            Assert-True ($customBefore -eq [Convert]::ToBase64String([IO.File]::ReadAllBytes($custom))) 'Custom check rewrote its selected INI.'
        } finally { Write-TestFile $headless.Config $original }
    }

    Test-Case 'Headless check-env discovers without a default INI but rejects an explicitly missing custom INI' {
        $original = [IO.File]::ReadAllText($headless.Config)
        $missing = Join-Path $headless.Root 'custom settings\not-created.ini'
        $explicit = Invoke-EnvironmentCheck $headless @('-Config',$missing)
        Assert-True ($explicit.ExitCode -ne 0 -and $explicit.Output.Contains($missing)) 'An explicitly missing custom INI did not report its path as an error.'
        Assert-True (!(Test-Path -LiteralPath $missing)) 'The explicit missing custom INI was created.'
        Remove-Item -LiteralPath $headless.Config
        try {
            $result = Invoke-EnvironmentCheck $headless
            Assert-EnvironmentReport $result.Output
            # Project is deliberately manual, so an unconfigured environment is
            # incomplete even when installed applications are auto-detected.
            Assert-True ($result.ExitCode -ne 0) 'An environment with no selected project returned all-valid.'
            Assert-True (!(Test-Path -LiteralPath $headless.Config)) 'Default-absent environment inspection created an INI.'
            Assert-True (@(Get-ChildItem -LiteralPath (Join-Path $headless.Root 'events') -File).Count -eq 0) 'Default-absent inspection launched a compiler.'
        } finally { Write-TestFile $headless.Config $original }
    }

    Test-Case 'First opening without an INI keeps Project pending and cannot save' {
        $root = Join-Path $fixture 'first open'
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        $paths = Invoke-Static $configType Read @($root)
        $form = New-HiddenForm $root $paths
        Wait-FormTask $form 'RefreshPaths'
        Assert-True ((Get-FormField $form 'paths')['Project'] -eq '') 'First opening guessed an unrelated UE project.'
        foreach ($key in @('Engine','Max')) {
            $detected = (Get-FormField $form 'paths')[$key]
            Assert-True ($detected.Length -gt 0) "First opening failed to discover the installed $key application."
            Assert-True ((Invoke-Static $configType ValidateApplication @($key, $detected)) -eq '') "Automatically detected $key application is invalid."
        }
        Assert-True (!(Get-FormField $form 'save').Enabled) 'Incomplete first configuration enabled Save.'
        Wait-FormTask $form 'SaveAsync'
        Assert-True (!(Test-Path -LiteralPath (Join-Path $root 'build_and_deploy.ini'))) 'Incomplete first configuration was written.'
        $buttons = @(Get-AllControls $form | Where-Object { $_ -is [Windows.Forms.Button] })
        foreach ($name in @('EngineAction','ProjectAction','MaxAction','CMakeAction','MaxSdkAction','WindowsSdkAction','ToolchainAction','CMakeBrowse','MaxSdkBrowse','WindowsSdkBrowse','ToolchainBrowse','Rescan','SaveConfig')) {
            Assert-True (@($buttons | Where-Object { $_.Name -eq $name }).Count -eq 1) "Missing or duplicate button: $name"
        }
    }

    Test-Case 'Seven valid selections automatically create a local INI and reopen correctly' {
        $root = Join-Path $fixture 'complete setup'
        $form = New-HiddenForm $root (Copy-Paths $seed)
        Wait-FormTask $form 'RefreshPaths'
        $file = Join-Path $root 'build_and_deploy.ini'
        Assert-True (Test-Path -LiteralPath $file -PathType Leaf) 'Seven valid selections did not auto-create the first INI.'
        Assert-True ((Get-FormField $form 'save').Enabled) 'All-valid configuration did not enable Save.'
        $saved = Invoke-Static $configType Read @($root)
        foreach ($key in $seed.Keys) { Assert-True ($saved[$key] -eq $seed[$key]) "Saved value differs for $key." }
        $reopened = New-HiddenForm $root $saved
        Wait-FormTask $reopened 'RefreshPaths'
        Assert-True ((Get-FormField $reopened 'save').Enabled) 'Reopening a valid INI did not enable Save.'
        foreach ($key in $seed.Keys) {
            $labels = @($reopened.Controls.Find($key + 'Path', $true))
            Assert-True ($labels.Count -eq 1 -and $labels[0].Text -eq $seed[$key]) "Reopened path was not displayed for $key."
        }
        $bitmap = New-Object Drawing.Bitmap $reopened.ClientSize.Width,$reopened.ClientSize.Height
        try { $reopened.DrawToBitmap($bitmap, $reopened.ClientRectangle) }
        finally { $bitmap.Dispose() }
    }

    Test-Case 'An existing INI preserves comments, custom keys and its build configuration' {
        $root = Join-Path $fixture 'existing config'
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        Invoke-Static $configType Save @($root, (Copy-Paths $seed)) | Out-Null
        $file = Join-Path $root 'build_and_deploy.ini'
        $text = [IO.File]::ReadAllText($file).Replace('MaxConfiguration=Release', 'MaxConfiguration=RelWithDebInfo')
        $text = "; keep this personal comment`r`n" + $text + "`r`n[Custom]`r`nKeepMe=unchanged`r`n"
        [IO.File]::WriteAllText($file, $text, (New-Object Text.UTF8Encoding $false))
        $before = [IO.File]::ReadAllBytes($file)
        $form = New-HiddenForm $root (Invoke-Static $configType Read @($root))
        Wait-FormTask $form 'RefreshPaths'
        Assert-True ([Convert]::ToBase64String($before) -eq [Convert]::ToBase64String([IO.File]::ReadAllBytes($file))) 'Detection silently rewrote an existing INI.'
        Wait-FormTask $form 'SaveAsync'
        $after = [IO.File]::ReadAllText($file)
        Assert-True ($after.Contains('; keep this personal comment')) 'Save removed personal comments.'
        Assert-True ($after.Contains('KeepMe=unchanged')) 'Save removed an unknown setting.'
        Assert-True ($after.Contains('MaxConfiguration=RelWithDebInfo')) 'Save discarded the existing build configuration.'
    }

    Test-Case 'Missing required application input cannot overwrite an existing INI' {
        $root = Join-Path $fixture 'missing project'
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        Invoke-Static $configType Save @($root, (Copy-Paths $seed)) | Out-Null
        $file = Join-Path $root 'build_and_deploy.ini'
        $before = [Convert]::ToBase64String([IO.File]::ReadAllBytes($file))
        $paths = Copy-Paths $seed
        $paths['Project'] = ''
        $form = New-HiddenForm $root $paths
        Wait-FormTask $form 'RefreshPaths'
        Assert-True (!(Get-FormField $form 'save').Enabled) 'Missing project did not disable Save.'
        Wait-FormTask $form 'SaveAsync'
        Assert-True ($before -eq [Convert]::ToBase64String([IO.File]::ReadAllBytes($file))) 'Missing project overwrote the old INI.'
    }

    Test-Case 'A nonempty invalid configured path is preserved even when a valid alternative exists' {
        $root = Join-Path $fixture 'stale configured dependency'
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        Invoke-Static $configType Save @($root, (Copy-Paths $seed)) | Out-Null
        $file = Join-Path $root 'build_and_deploy.ini'
        $stale = Join-Path $root 'removed installation\cmake.exe'
        $text = [IO.File]::ReadAllText($file).Replace('CMakePath=' + $seed['CMake'], 'CMakePath=' + $stale)
        [IO.File]::WriteAllText($file, $text, (New-Object Text.UTF8Encoding $false))
        $before = [Convert]::ToBase64String([IO.File]::ReadAllBytes($file))
        $form = New-HiddenForm $root (Invoke-Static $configType Read @($root))
        Wait-FormTask $form 'RefreshPaths'
        Assert-True ((Get-FormField $form 'paths')['CMake'] -eq $stale) 'Detection replaced a nonempty stale configured CMake path.'
        Assert-True (!(Get-FormField $form 'save').Enabled) 'Stale dependency was accepted as valid.'
        Assert-True ($form.Controls.Find('CMakePath', $true)[0].Text -eq $stale) 'UI hid the stale configured path.'
        Wait-FormTask $form 'SaveAsync'
        Assert-True ($before -eq [Convert]::ToBase64String([IO.File]::ReadAllBytes($file))) 'Invalid configured dependency overwrote the old INI.'
    }

    Test-Case 'All seven nonempty invalid selections stay visible and are never replaced by detection' {
        $root = Join-Path $fixture 'all stale selections'
        $paths = Copy-Paths $seed
        foreach ($key in @($paths.Keys)) {
            $suffix = if ($key -eq 'CMake') { 'cmake.exe' } elseif ($key -eq 'Project') { 'WangYue.uproject' } else { 'missing directory' }
            $paths[$key] = Join-Path $root "$key\$suffix"
        }
        $expected = Copy-Paths $paths
        $form = New-HiddenForm $root $paths
        Wait-FormTask $form 'RefreshPaths'
        foreach ($key in $expected.Keys) {
            Assert-True ((Get-FormField $form 'paths')[$key] -eq $expected[$key]) "Detection replaced the nonempty $key selection."
            Assert-True ($form.Controls.Find($key + 'Path', $true)[0].Text -eq $expected[$key]) "UI did not show the invalid $key selection."
        }
        Assert-True (!(Get-FormField $form 'save').Enabled) 'Invalid selections did not disable Save.'
        Wait-FormTask $form 'SaveAsync'
        Assert-True (!(Test-Path -LiteralPath (Join-Path $root 'build_and_deploy.ini'))) 'Invalid selections created a configuration.'
    }

    Test-Case 'Left navigation switches between environment and publishing with three publish actions' {
        $root = Join-Path $fixture 'navigation'
        $form = New-HiddenForm $root (Copy-Paths $seed)
        Wait-FormTask $form 'RefreshPaths'
        $environment = Get-NamedControl $form 'EnvironmentNav'
        $publish = Get-NamedControl $form 'PublishNav'
        Assert-True ($environment -is [Windows.Forms.RadioButton] -and $publish -is [Windows.Forms.RadioButton]) 'Navigation is not represented by page controls.'
        Assert-True ($environment.Checked -and !$publish.Checked) 'Environment is not the initial page.'
        $publish.Checked = $true
        [Windows.Forms.Application]::DoEvents()
        Assert-True ($publish.Checked -and !$environment.Checked) 'Publishing navigation did not switch exclusively.'
        foreach ($name in @('PublishUE','PublishMax','PublishAll')) {
            $button = Get-NamedControl $form $name
            Assert-True ($button -is [Windows.Forms.Button] -and $button.Enabled) "$name is unavailable."
        }
        Assert-True ((Get-NamedControl $form 'PublishLog').ReadOnly) 'Publish logs should be read-only.'
        $bitmap = New-Object Drawing.Bitmap $form.ClientSize.Width,$form.ClientSize.Height
        try { $form.DrawToBitmap($bitmap, $form.ClientRectangle) } finally { $bitmap.Dispose() }
        $environment.Checked = $true
        Assert-True ($environment.Checked -and !$publish.Checked) 'Could not navigate back to Environment.'
    }

    Test-Case 'UE publish button builds and deploys only UE with no Max configuration' {
        $publishFixture = New-TestBridgeFixture $fixture 'publish ue only' $repoRoot $AssemblyPath
        Write-TestConfiguration $publishFixture 'UE'
        Set-TestRun $publishFixture @('UE') 'gui-ue'
        $form = New-HiddenForm $publishFixture.Root (Invoke-Static $configType Read @($publishFixture.Root))
        Wait-FormTask $form 'RefreshPaths'
        Wait-PublishClick $form 'PublishUE'
        Assert-True ((Read-TestVersion $publishFixture.DeployUE) -eq 'gui-ue') 'UE button did not deploy the new package.'
        Assert-True (!(Test-Path -LiteralPath $publishFixture.DeployMax)) 'UE button deployed Max.'
        Assert-True (!(Test-Path -LiteralPath (Join-Path $publishFixture.Root 'events\Max.start'))) 'UE button launched Max build.'
    }

    Test-Case 'Max publish button builds and deploys only Max with no UE configuration' {
        $publishFixture = New-TestBridgeFixture $fixture 'publish max only' $repoRoot $AssemblyPath
        Write-TestConfiguration $publishFixture 'Max'
        Set-TestRun $publishFixture @('Max') 'gui-max'
        $form = New-HiddenForm $publishFixture.Root (Invoke-Static $configType Read @($publishFixture.Root))
        Wait-FormTask $form 'RefreshPaths'
        Assert-True (!(Get-FormField $form 'save').Enabled) 'Missing project unexpectedly allowed saving environment configuration.'
        Wait-PublishClick $form 'PublishMax'
        Assert-True ((Read-TestVersion $publishFixture.DeployMax) -eq 'gui-max') 'Max button did not deploy the new package.'
        Assert-True (!(Test-Path -LiteralPath $publishFixture.DeployUE)) 'Max button deployed UE.'
        Assert-True (!(Test-Path -LiteralPath (Join-Path $publishFixture.Root 'events\UE.start'))) 'Max button launched UE build.'
    }

    Test-Case 'Publish All deploys both; a failed rebuild preserves both installed versions and latest' {
        $publishFixture = New-TestBridgeFixture $fixture 'publish all' $repoRoot $AssemblyPath
        Set-TestRun $publishFixture @('UE','Max') 'gui-both'
        $form = New-HiddenForm $publishFixture.Root (Invoke-Static $configType Read @($publishFixture.Root))
        Wait-FormTask $form 'RefreshPaths'
        Wait-PublishClick $form 'PublishAll'
        foreach ($package in @($publishFixture.DeployUE,$publishFixture.DeployMax,$publishFixture.LatestUE,$publishFixture.LatestMax)) {
            Assert-True ((Read-TestVersion $package) -eq 'gui-both') 'Publish All failed to update both packages.'
        }
        Set-TestRun $publishFixture @('UE','Max') 'must-not-deploy' 'Max' '' 'gui-both'
        Wait-PublishClick $form 'PublishAll' $false
        foreach ($package in @($publishFixture.DeployUE,$publishFixture.DeployMax,$publishFixture.LatestUE,$publishFixture.LatestMax)) {
            Assert-True ((Read-TestVersion $package) -eq 'gui-both') 'Failed GUI build changed an installed/latest package.'
        }
    }

    Test-Case 'Installed fingerprints reflect source content and filenames, not touch times or BuildOnly' {
        $versioned = New-TestBridgeFixture $fixture 'version view lifecycle' $repoRoot $AssemblyPath
        Set-TestRun $versioned @('UE','Max') 'version-baseline'
        $form = New-HiddenForm $versioned.Root (Invoke-Static $configType Read @($versioned.Root))
        Wait-FormTask $form 'RefreshPaths'
        Wait-PublishClick $form 'PublishAll'
        Refresh-PublishVersions $form
        $baselineUE = Read-TestFingerprint $versioned.DeployUE
        $baselineMax = Read-TestFingerprint $versioned.DeployMax
        Assert-InstalledHashLabel $form 'UE' $baselineUE
        Assert-InstalledHashLabel $form 'Max' $baselineMax
        Assert-PublishVersionState $form $false $false

        $source = Get-TestSourceFile $versioned 'UE'
        $original = [IO.File]::ReadAllBytes($source)
        [IO.File]::SetLastWriteTimeUtc($source, [DateTime]::UtcNow.AddHours(1))
        Refresh-PublishVersions $form
        Assert-PublishVersionState $form $false $false
        $beforeChange = [long](Get-FormField $form 'publishVersionApplied')
        [IO.File]::AppendAllText($source, "`n// content change for version test`n", (New-Object Text.UTF8Encoding $false))
        # The open publishing page should notice a source edit without navigation.
        Wait-PublishVersionsApplied $form $beforeChange
        Assert-PublishVersionState $form $true $false
        Set-TestRun $versioned @('UE') 'build-only-version'
        $build = Invoke-TestBuild $versioned @('-Target','UE','-BuildOnly')
        Assert-True ($build.ExitCode -eq 0) $build.Output
        Assert-True ((Read-TestFingerprint $versioned.LatestUE) -ne $baselineUE) 'BuildOnly did not produce a new source fingerprint.'
        Refresh-PublishVersions $form
        Assert-InstalledHashLabel $form 'UE' $baselineUE
        Assert-PublishVersionState $form $true $false
        [IO.File]::WriteAllBytes($source, $original)
        Refresh-PublishVersions $form
        Assert-PublishVersionState $form $false $false

        $extra = Join-Path (Split-Path -Parent $source) 'FingerprintProbe.cpp'
        $renamed = Join-Path (Split-Path -Parent $source) 'FingerprintRenamed.cpp'
        Write-TestFile $extra '// additional source input'
        Refresh-PublishVersions $form
        Assert-PublishVersionState $form $true $false
        Set-TestRun $versioned @('UE') 'extra-source-installed'
        Wait-PublishClick $form 'PublishUE'
        Refresh-PublishVersions $form
        Assert-PublishVersionState $form $false $false
        Move-Item -LiteralPath $extra -Destination $renamed
        Refresh-PublishVersions $form
        Assert-PublishVersionState $form $true $false
        Move-Item -LiteralPath $renamed -Destination $extra
        Refresh-PublishVersions $form
        Assert-PublishVersionState $form $false $false
        Remove-Item -LiteralPath $extra
        Refresh-PublishVersions $form
        Assert-PublishVersionState $form $true $false
        Set-TestRun $versioned @('UE') 'original-sources-restored'
        Wait-PublishClick $form 'PublishUE'
        Refresh-PublishVersions $form
        Assert-PublishVersionState $form $false $false
        Assert-True ((Read-TestFingerprint $versioned.DeployUE) -eq $baselineUE) 'Restoring the original source tree did not restore its fingerprint.'
        Assert-True ((Read-TestFingerprint $versioned.DeployMax) -eq $baselineMax) 'UE source lifecycle changed the installed Max version.'
    }

    Test-Case 'Incremental button publishes only known changes and keeps both baselines on failure' {
        $versioned = New-TestBridgeFixture $fixture 'incremental gui' $repoRoot $AssemblyPath
        Set-TestRun $versioned @('UE','Max') 'incremental-baseline'
        $form = New-HiddenForm $versioned.Root (Invoke-Static $configType Read @($versioned.Root))
        Wait-FormTask $form 'RefreshPaths'
        Wait-PublishClick $form 'PublishAll'
        $oldUE = Read-TestFingerprint $versioned.DeployUE
        $oldMax = Read-TestFingerprint $versioned.DeployMax
        [IO.File]::AppendAllText((Get-TestSourceFile $versioned 'UE'), "`n// first incremental UE change`n", (New-Object Text.UTF8Encoding $false))
        Refresh-PublishVersions $form
        Assert-PublishVersionState $form $true $false
        Set-TestRun $versioned @('UE') 'incremental-ue-only'
        Wait-PublishClick $form 'PublishChanged'
        Refresh-PublishVersions $form
        Assert-PublishVersionState $form $false $false
        $newUE = Read-TestFingerprint $versioned.DeployUE
        Assert-True ($newUE -ne $oldUE -and (Read-TestFingerprint $versioned.DeployMax) -eq $oldMax) 'Incremental UE publish changed the wrong installed versions.'
        Assert-True (!(Test-Path -LiteralPath (Join-Path $versioned.Root 'events\Max.start'))) 'UE-only incremental button launched the Max build.'

        [IO.File]::AppendAllText((Get-TestSourceFile $versioned 'UE'), "`n// another UE change`n", (New-Object Text.UTF8Encoding $false))
        [IO.File]::AppendAllText((Get-TestSourceFile $versioned 'Max'), "`n// Max change`n", (New-Object Text.UTF8Encoding $false))
        Refresh-PublishVersions $form
        Assert-PublishVersionState $form $true $true
        Set-TestRun $versioned @('UE','Max') 'incremental-failure' 'Max'
        Wait-PublishClick $form 'PublishChanged' $false
        Refresh-PublishVersions $form
        Assert-PublishVersionState $form $true $true
        Assert-True ((Read-TestFingerprint $versioned.DeployUE) -eq $newUE -and (Read-TestFingerprint $versioned.DeployMax) -eq $oldMax) 'Failed incremental GUI publication changed an installed baseline.'
        Set-TestRun $versioned @('UE','Max') 'incremental-both-success'
        Wait-PublishClick $form 'PublishChanged'
        Refresh-PublishVersions $form
        Assert-PublishVersionState $form $false $false
        Assert-True ((Read-TestFingerprint $versioned.DeployUE) -ne $newUE -and (Read-TestFingerprint $versioned.DeployMax) -ne $oldMax) 'Successful dual incremental publish did not update both versions.'
    }

    Test-Case 'Unknown or unreadable versions show explicit states without incremental mismatch' {
        $unknown = New-TestBridgeFixture $fixture 'unknown version gui' $repoRoot $AssemblyPath
        $form = New-HiddenForm $unknown.Root (Invoke-Static $configType Read @($unknown.Root))
        Wait-FormTask $form 'RefreshPaths'
        Refresh-PublishVersions $form
        Assert-PublishVersionState $form $false $false
        $notInstalled = ([string][char]0x672A) + [char]0x5B89 + [char]0x88C5
        foreach ($target in @('UE','Max')) {
            Assert-True ((Get-NamedControl $form ('PublishVersion' + $target)).Text.Contains($notInstalled)) "No-install $target status is not explicit."
            Assert-True ((Get-NamedControl $form ('Publish' + $target)).Enabled) "Ordinary $target publish is disabled for a missing installation."
        }
        New-TestPackage $unknown.DeployUE UE 'old-unversioned'
        New-TestPackage $unknown.DeployMax Max 'old-unversioned'
        Refresh-PublishVersions $form
        Assert-PublishVersionState $form $false $false
        $notRecorded = ([string][char]0x672A) + [char]0x8BB0 + [char]0x5F55
        foreach ($target in @('UE','Max')) {
            Assert-True ((Get-NamedControl $form ('PublishVersion' + $target)).Text.Contains($notRecorded)) "Old unversioned $target status is not explicit."
        }
        Set-TestRun $unknown @('UE','Max') 'established-baseline'
        Wait-PublishClick $form 'PublishAll'
        Refresh-PublishVersions $form
        Assert-PublishVersionState $form $false $false
        Assert-InstalledHashLabel $form 'UE' (Read-TestFingerprint $unknown.DeployUE)
        Assert-InstalledHashLabel $form 'Max' (Read-TestFingerprint $unknown.DeployMax)
        $installedUE = Read-TestFingerprint $unknown.DeployUE
        $sourceLock = [IO.File]::Open((Get-TestSourceFile $unknown 'UE'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
        try {
            Refresh-PublishVersions $form
            Assert-PublishVersionState $form $false $false
            Assert-InstalledHashLabel $form 'UE' $installedUE
            $comparisonFailed = ([string][char]0x6E90) + [char]0x7801 + [char]0x6BD4 + [char]0x5BF9 + [char]0x5931 + [char]0x8D25
            Assert-True ((Get-NamedControl $form 'PublishVersionUE').Text.Contains($comparisonFailed)) 'Unreadable source did not retain its installed version with an explicit comparison failure.'
        } finally { $sourceLock.Dispose() }
        Refresh-PublishVersions $form
        Assert-PublishVersionState $form $false $false
    }
} finally {
    foreach ($form in $forms) { $form.Dispose() }
    [Threading.SynchronizationContext]::SetSynchronizationContext($previousContext)
    Remove-TestTree $fixture $buildRoot
}

Write-Host "Setup integration tests: $script:passed passed, $script:failed failed."
if ($script:failed) { exit 1 }
exit 0
