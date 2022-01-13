function Force-Resolve-Path {
    <#
    .SYNOPSIS
        Calls Resolve-Path but works for files that don't exist.
    .REMARKS
        From http://devhawk.net/blog/2010/1/22/fixing-powershells-busted-resolve-path-cmdlet
    #>
    param (
        [string] $FileName
    )

    $FileName = Resolve-Path $FileName -ErrorAction SilentlyContinue `
                                       -ErrorVariable _frperror
    if (-not($FileName)) {
        $FileName = $_frperror[0].TargetObject
    }

    return $FileName
}

$COM = [System.IO.Ports.SerialPort]::getportnames()

$port = new-Object System.IO.Ports.SerialPort $COM,9600,None,8,one
$port.open()

$datetime = Get-Date -UFormat %y%m%d_%H%M%S

$file = "HazeL_$datetime.txt"
do {
    $line = $port.ReadLine();
    Add-Content -Path (Force-Resolve-Path $file) -Value "$line`n" -PassThru -NoNewline
}
while ($port.IsOpen)