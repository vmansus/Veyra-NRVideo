param([Parameter(Mandatory=$true)][string]$Context)
# Windows PowerShell 5.1 / .NET. No elevation, execution-policy override or
# network request. This helper only consumes the already verified ZIP.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$work = Split-Path -LiteralPath $Context
$log = Join-Path $work 'update.log'
$changed = [System.Collections.Generic.List[string]]::new()
$backup = $null
$root = $null
$committed = $false
function Log([string]$text) { Add-Content -LiteralPath $log -Value $text -Encoding UTF8 }
function SafeName([string]$name) {
    if ([string]::IsNullOrWhiteSpace($name) -or $name.Contains('\') -or $name.Contains(':') -or $name.StartsWith('/')) { throw 'Unsafe payload path' }
    foreach ($part in $name.Split('/')) {
        if (!$part -or $part -eq '.' -or $part -eq '..' -or $part -match '[<>"|?*\x00-\x1f]' -or $part -match '[. ]$' -or
            $part -match '^(?i:CON|PRN|AUX|NUL|CONIN\$|CONOUT\$|CLOCK\$|COM[0-9]|LPT[0-9])(?:\.|$)') { throw 'Unsafe payload component' }
    }
    return $name
}
function SafePath([string]$directory, [string]$name) {
    $null = SafeName $name
    $path = [IO.Path]::GetFullPath((Join-Path $directory $name))
    $base = [IO.Path]::GetFullPath($directory).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (!$path.StartsWith($base, [StringComparison]::OrdinalIgnoreCase)) { throw 'Payload escapes directory' }
    # Check all ancestors, including the install directory and its parents.
    $cursor = $path
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse point in update path' }
        }
        $parent = Split-Path -LiteralPath $cursor
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
    return $path
}
function Records($manifest) {
    $rows = @{}
    foreach ($row in $manifest.files) {
        $name = SafeName ([string]$row.path)
        if ($name -eq 'package-manifest.json' -or $rows.ContainsKey($name) -or [string]$row.sha256 -notmatch '^[a-fA-F0-9]{64}$' -or
            [long]$row.size -lt 0) { throw 'Invalid manifest record' }
        $rows[$name] = $row
    }
    if (!$rows.ContainsKey('veyra_qml_ui.exe')) { throw 'Missing application executable' }
    return $rows
}
function ValidateFiles([string]$directory, $rows) {
    foreach ($name in $rows.Keys) {
        $file = SafePath $directory $name
        if (!(Test-Path -LiteralPath $file -PathType Leaf) -or (Get-Item -LiteralPath $file).Length -ne [long]$rows[$name].size -or
            (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $rows[$name].sha256) { throw "File identity mismatch: $name" }
    }
}
try {
    $c = Get-Content -LiteralPath $Context -Raw -Encoding UTF8 | ConvertFrom-Json
    $root = [IO.Path]::GetFullPath([string]$c.directory)
    $exe = SafePath $root 'veyra_qml_ui.exe'
    if ($exe -ne [IO.Path]::GetFullPath([string]$c.executable) -or $c.flavor -notin @('NVIDIA','AMD') -or
        [string]$c.version -notmatch '^\d+\.\d+\.\d+$' -or [string]$c.sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'Invalid update context' }
    $owner = Get-Process -Id ([int]$c.pid)
    if ($owner.Path -ne $exe) { throw 'Application process identity mismatch' }
    $null = SafePath $root 'package-manifest.json'
    $old = Get-Content -LiteralPath (Join-Path $root 'package-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($old.gpuPackage -ne $c.flavor -or [version]$old.version -ge [version]$c.version) { throw 'Installed package flavor/version mismatch' }
    $oldRows = Records $old
    # Customized/replaced DLLs and local edits must never be silently overwritten.
    ValidateFiles $root $oldRows
    if ((Get-FileHash -LiteralPath $c.archive -Algorithm SHA256).Hash -ne $c.sha256) { throw 'Archive SHA-256 mismatch' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $stage = Join-Path $work 'stage'
    $null = New-Item -ItemType Directory -Path $stage
    $zip = [IO.Compression.ZipFile]::OpenRead([string]$c.archive)
    try {
        $names = @{}
        $total = 0L
        foreach ($entry in $zip.Entries) {
            $name = SafeName $entry.FullName.TrimEnd('/')
            if ($names.ContainsKey($name) -or (($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) { throw 'Duplicate entry or symbolic link' }
            $names[$name] = $entry
            $total += $entry.Length
            if ($total -gt 24GB -or $names.Count -gt 20000) { throw 'Archive exceeds update limits' }
        }
        $manifests = @($zip.Entries | Where-Object { $_.FullName -match '^[^/]+/package-manifest\.json$' })
        if ($manifests.Count -ne 1 -or $manifests[0].Length -gt 16MB) { throw 'Missing unique package manifest' }
        $prefix = $manifests[0].FullName.Split('/')[0] + '/'
        $reader = [IO.StreamReader]::new($manifests[0].Open())
        try { $new = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
        if ($new.gpuPackage -ne $c.flavor -or $new.version -ne $c.version) { throw 'New package version/flavor mismatch' }
        $newRows = Records $new
        $expected = @{}
        foreach ($name in $newRows.Keys) { $expected[$prefix + $name] = $true }
        $expected[$prefix + 'package-manifest.json'] = $true
        foreach ($entry in $zip.Entries) {
            if ($entry.FullName.EndsWith('/')) { continue }
            if (!$expected.ContainsKey($entry.FullName)) { throw 'Unlisted archive file' }
            $relative = $entry.FullName.Substring($prefix.Length)
            $destination = SafePath $stage $relative
            $null = New-Item -ItemType Directory -Path (Split-Path -LiteralPath $destination) -Force
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $false)
        }
    } finally { $zip.Dispose() }
    ValidateFiles $stage $newRows
    # Verify destination collisions before asking the running app to exit.
    foreach ($name in $newRows.Keys) {
        $destination = SafePath $root $name
        if ((Test-Path -LiteralPath $destination) -and !$oldRows.ContainsKey($name)) { throw "Unowned destination collision: $name" }
    }
    # Backup is on the install volume; permissions/space must work before exit.
    $backup = Join-Path $root ('.veyra-update-backup-' + [Guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $backup
    foreach ($name in @($oldRows.Keys) + @('package-manifest.json')) {
        $source = SafePath $root $name
        $destination = SafePath $backup $name
        $null = New-Item -ItemType Directory -Path (Split-Path -LiteralPath $destination) -Force
        Copy-Item -LiteralPath $source -Destination $destination
    }
    Log "Validated package; backup: $backup"
    if (Test-Path -LiteralPath (Join-Path $work 'cancel')) { throw 'Update canceled' }
    Set-Content -LiteralPath (Join-Path $work 'ready') -Value 'ready' -Encoding ASCII
    # Wait for the captured process object, not a new process reusing its PID.
    $deadline = [DateTime]::UtcNow.AddMinutes(5)
    while (!$owner.WaitForExit(200)) {
        if ((Test-Path -LiteralPath (Join-Path $work 'cancel')) -or [DateTime]::UtcNow -gt $deadline) { throw 'Update canceled or application did not exit' }
    }
    if (Test-Path -LiteralPath (Join-Path $work 'cancel')) { throw 'Update canceled' }
    # Recheck the old files after exit: changes made during teardown also count.
    ValidateFiles $root $oldRows
    $operations = @($newRows.Keys) + @($oldRows.Keys | Where-Object { !$newRows.ContainsKey($_) }) + @('package-manifest.json')
    foreach ($name in $operations) {
        $destination = SafePath $root $name
        $changed.Add($name)
        if ($newRows.ContainsKey($name) -or $name -eq 'package-manifest.json') {
            $source = SafePath $stage $name
            $null = New-Item -ItemType Directory -Path (Split-Path -LiteralPath $destination) -Force
            Copy-Item -LiteralPath $source -Destination $destination -Force
        } else { Remove-Item -LiteralPath $destination -Force }
    }
    ValidateFiles $root $newRows
    $committed = $true
    Log 'UPDATE SUCCEEDED. Previous package retained in backup directory.'
    # Use a JSON argument vector: preserve spaces/quotes in data-dir arguments.
    function QuoteArg([string]$arg) {
        return '"' + ([regex]::Replace([regex]::Replace($arg, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1')) + '"'
    }
    $arguments = @($c.arguments | ForEach-Object { QuoteArg ([string]$_) }) -join ' '
    $start = [Diagnostics.ProcessStartInfo]::new($exe, $arguments)
    $start.WorkingDirectory = $root
    $start.UseShellExecute = $false
    $null = [Diagnostics.Process]::Start($start)
} catch {
    Log ('UPDATE FAILED: ' + $_.Exception.Message)
    if (!$committed -and $backup -and $changed.Count) {
        try {
            for ($i = $changed.Count - 1; $i -ge 0; --$i) {
                $name = $changed[$i]
                $destination = SafePath $root $name
                $original = SafePath $backup $name
                if (Test-Path -LiteralPath $original -PathType Leaf) { Copy-Item -LiteralPath $original -Destination $destination -Force }
                elseif (Test-Path -LiteralPath $destination -PathType Leaf) { Remove-Item -LiteralPath $destination -Force }
            }
            Log 'ROLLBACK SUCCEEDED. Original package restored.'
        } catch { Log ('ROLLBACK FAILED. Recover original files from ' + $backup + ': ' + $_.Exception.Message) }
    }
    Set-Content -LiteralPath (Join-Path $work 'failed') -Value 'failed' -Encoding ASCII
    exit 1
}
