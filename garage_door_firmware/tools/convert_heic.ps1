Add-Type -AssemblyName System.Runtime.WindowsRuntime
$src = 'D:\mimo\garage.heic'
$dst = 'D:\mimo\garage.jpg'

$null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
$null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Graphics.Imaging, ContentType = WindowsRuntime]
$null = [Windows.Graphics.Imaging.BitmapEncoder, Windows.Graphics.Imaging, ContentType = WindowsRuntime]
$null = [Windows.Storage.Streams.InMemoryRandomAccessStream, Windows.Storage.Streams, ContentType = WindowsRuntime]
$null = [Windows.Storage.FileAccessMode, Windows.Storage, ContentType = WindowsRuntime]

function Await($WinRtTask, $ResultType) {
    $asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and
        $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
    } | Select-Object -First 1)
    if ($null -eq $asTask) {
        $asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
            $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1
        } | Select-Object -First 1
    }
    $netTask = $asTask.MakeGenericMethod($ResultType).Invoke($null, @($WinRtTask))
    $netTask.Wait(-1) | Out-Null
    $netTask.Result
}

try {
    $getFile = [Windows.Storage.StorageFile]::GetFileFromPathAsync($src)
    $file = Await $getFile ([Windows.Storage.StorageFile])
    Write-Host "File: $($file.Path)"

    $open = $file.OpenAsync([Windows.Storage.FileAccessMode]::Read)
    $stream = Await $open ([Windows.Storage.Streams.IRandomAccessStream])
    Write-Host "Stream size: $($stream.Size)"

    $decoderAsync = [Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)
    $decoder = Await $decoderAsync ([Windows.Graphics.Imaging.BitmapDecoder])
    Write-Host "Width x Height: $($decoder.PixelWidth) x $($decoder.PixelHeight)"

    $bitmapAsync = $decoder.GetSoftwareBitmapAsync()
    $softwareBitmap = Await $bitmapAsync ([Windows.Graphics.Imaging.SoftwareBitmap])

    $outStream = New-Object Windows.Storage.Streams.InMemoryRandomAccessStream
    $encoderAsync = [Windows.Graphics.Imaging.BitmapEncoder]::CreateAsync(
        [Windows.Graphics.Imaging.BitmapEncoder]::JpegEncoderId,
        $outStream
    )
    $encoder = Await $encoderAsync ([Windows.Graphics.Imaging.BitmapEncoder])
    $encoder.SetSoftwareBitmap($softwareBitmap)

    $asTaskAction = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and
        $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction'
    } | Select-Object -First 1)
    $flushAsync = $encoder.FlushAsync()
    $flushTask = $asTaskAction.Invoke($null, @($flushAsync))
    $flushTask.Wait(-1) | Out-Null

    $outStream.Seek(0)
    $reader = New-Object Windows.Storage.Streams.DataReader($outStream)
    $loadAsync = $reader.LoadAsync($outStream.Size)
    $loadTask = Await $loadAsync ([uint32])

    $bytes = New-Object byte[] ([uint32]$outStream.Size)
    $reader.ReadBytes($bytes)
    [System.IO.File]::WriteAllBytes($dst, $bytes)
    Write-Host "OK: $dst size=$($bytes.Length)"
} catch {
    Write-Host "ERROR: $($_.Exception.GetType().FullName): $($_.Exception.Message)"
    Write-Host $_.ScriptStackTrace
    exit 1
}
