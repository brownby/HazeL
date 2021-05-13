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
do {
    #Tee-Object -InputObject $port.ReadLine() -FilePath .\HazeL_data_raw.txt -Append
    Tee-Object -InputObject $port.ReadLine() -FilePath (Force-Resolve-Path ".\HazeL_data_raw.txt") -Append
}
while ($port.IsOpen)