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
    Write-Host "Waiting for file to be sent from HazeL..."

    # Read first line to create file name (this is blocking, so script will stop here until it receives a file
    $file = $port.ReadLine()
    Write-Host "`nFile detected: $file"

    # now set a timeout for reads, so that end of file can be detected
    $port.ReadTimeout = 500

    $outbuffer = [System.Collections.ArrayList]@()

    Write-Host "Reading data from HazeL..."

    $numlines = 0

    while($true) {
        try {
            $line = $port.ReadLine()
        }
        # Consider "End of file" to mean that it's taken longer than 5 seconds to receive a new line
        catch [System.TimeoutException] {
            Write-Host ""
            break
        }
        $numlines++

        # Every dot means 100 lines of data have been read
        if($numlines % 100 -eq 0) {Write-Host -NoNewline "."}
        if($numlines % (25*100) -eq 0) {Write-Host ""}
        $outbuffer.Add("$line`n") > $null
    }
    Add-Content -Path (Force-Resolve-Path $file) -Value $outbuffer -NoNewline
    Write-Host "Done writing data to $file`n`nReady to download next file"
    $port.ReadTimeout = -1
}
while ($port.IsOpen)