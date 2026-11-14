# Converts one sound for the SmartSwitchD2 speaker (MAX98357A + a small speaker), see README 2a.
#
#   .\convert_sound.ps1 -In my_sound.wav -Out data\my_sound.mp3
#
# Two passes, because loudnorm cannot judge a sound of a few seconds in one go:
#   1. shape it for the speaker (trim, high-pass, presence, compression) and measure how loud
#      the result is;
#   2. bring it to the target loudness with one fixed gain, limit the peaks, encode.
# Any input ffmpeg reads works (wav, mp3, ...). Uses ffmpeg from PATH, or set $env:FFMPEG to its full path.
param(
    [Parameter(Mandatory = $true)][string]$In,
    [Parameter(Mandatory = $true)][string]$Out,
    # Keep a quiet intro: skip the start-silence trim.
    [switch]$KeepIntro,
    # Where the bass is cut. Lower keeps more of the low "m"/"n" sounds of speech (a word
    # ending in one can sound clipped at 350), at the cost of a little more rattle.
    [int]$HighPass = 350
)

# No $ErrorActionPreference = 'Stop': Windows PowerShell would turn ffmpeg's ordinary stderr
# report (the loudness measurement) into an error. Failures are checked by exit code below.
$ff = $env:FFMPEG
if (-not $ff) { $ff = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source }
if (-not $ff) { throw 'ffmpeg not found: put it on PATH or set $env:FFMPEG' }

$TARGET_LUFS = -14.0   # every sound equally loud
$LIMIT = 0.79          # peaks at -2 dBFS before encoding; MP3 overshoots by up to ~1 dB

# Shaping for a small speaker:
#  - highpass 350 Hz, twice (24 dB/octave): a small speaker cannot play below its resonance
#    (~400-800 Hz); bass only makes the cone rattle and uses up the amplifier for nothing.
#  - +3 dB around 3 kHz: where speech is understood, and where a small speaker does well.
#  - compression 3:1 above -24 dB: quiet parts come up, so it sounds louder without clipping.
$shape = @()
if (-not $KeepIntro) { $shape += 'silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0' }
$shape += "highpass=f=$HighPass", "highpass=f=$HighPass",
          'equalizer=f=3000:t=q:w=1.2:g=3',
          'acompressor=threshold=0.063:ratio=3:attack=5:release=100:makeup=2'
$shape = $shape -join ','

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("convert_sound_{0}.wav" -f [guid]::NewGuid())
try {
    & $ff -hide_banner -loglevel error -y -i $In -ac 1 -ar 16000 -af $shape $tmp
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg could not read or shape $In" }

    $report = & $ff -hide_banner -nostats -i $tmp -af ebur128 -f null NUL 2>&1 | Out-String
    $m = [regex]::Matches($report, 'I:\s+(-?[\d.]+) LUFS')
    if ($m.Count -eq 0) { throw "could not measure the loudness of $In" }
    $measured = [double]$m[$m.Count - 1].Groups[1].Value
    $gain = [math]::Round($TARGET_LUFS - $measured, 2)

    $finish = "volume=${gain}dB,alimiter=limit=${LIMIT}:level=false"
    & $ff -hide_banner -loglevel error -y -i $tmp -af $finish -ac 1 -ar 16000 -b:a 32k -map_metadata -1 $Out
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg could not encode $Out" }
    "{0}: measured {1} LUFS, gain {2} dB -> {3} ({4} KB)" -f (Split-Path $In -Leaf), $measured, $gain, $Out,
        [math]::Round((Get-Item $Out).Length / 1KB, 1)
}
finally {
    Remove-Item $tmp -ErrorAction SilentlyContinue
}
