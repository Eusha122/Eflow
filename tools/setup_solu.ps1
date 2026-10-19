# Sets up Solu, Eflow's local voice assistant: a Python environment, the speech models and the language model.
# Run once:  powershell -ExecutionPolicy Bypass -File tools\setup_solu.ps1
param([switch]$SkipOllama)
$ErrorActionPreference = 'Stop'
$solu = Join-Path (Split-Path $PSScriptRoot -Parent) 'solu'

Write-Host '1/3  Python environment...'
if (-not (Test-Path "$solu\venv\Scripts\python.exe")) {
    if (Get-Command py -ErrorAction SilentlyContinue) { py -3.11 -m venv "$solu\venv" } else { python -m venv "$solu\venv" }
}
& "$solu\venv\Scripts\python.exe" -m pip install --quiet --upgrade pip
& "$solu\venv\Scripts\python.exe" -m pip install --quiet -r "$solu\requirements.txt"

Write-Host '2/3  Voice and speech models (about 400 MB)...'
New-Item -ItemType Directory -Force "$solu\models" | Out-Null
$files = @{
    'kokoro-v1.0.onnx' = 'https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/kokoro-v1.0.onnx'
    'voices-v1.0.bin'  = 'https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/voices-v1.0.bin'
    'silero_vad.onnx'  = 'https://github.com/snakers4/silero-vad/raw/master/src/silero_vad/data/silero_vad.onnx'
}
foreach ($name in $files.Keys) {
    if (-not (Test-Path "$solu\models\$name")) { Invoke-WebRequest $files[$name] -OutFile "$solu\models\$name" }
}

if (-not $SkipOllama) {
    Write-Host '3/3  Language model (Ollama, about 5 GB)...'
    if (-not (Get-Command ollama -ErrorAction SilentlyContinue)) { Write-Warning 'Install Ollama first: https://ollama.com'; exit 1 }
    ollama pull qwen3:8b
    ollama pull qwen3:4b-instruct   # the fast brain, used when the 8B does not fit whole on the graphics card (about 2.5 GB)
    ollama pull qwen3-vl:4b      # screen help: questions about pictures (about 3 GB)
}
Write-Host 'Done. Switch Solu on in Settings (the gear on the island) (Whisper downloads itself the first time).'
