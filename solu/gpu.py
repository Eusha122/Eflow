"""Sharing the graphics card politely.

The brain (qwen3:8b, 8k context) needs about 6.2 GB of the 8 GB card when it is loaded whole. Chrome, VS Code, WhatsApp,
Discord... all draw with the same card, and when its memory runs out THEY crash or close (seen: an NVIDIA driver reset
and VS Code crash dumps while the brain sat in memory for 30 minutes). So:

* the brain is let go after KEEP of not being used (it reloads in ~2-4 s when needed again);
* when it is loaded, RESERVE_GB of the card's memory stays free for everything else: if there is not enough room, only
  as many layers as fit go to the card and the rest run on the processor (slower answers, but nothing crashes);
* the decision is made only while the brain is NOT loaded (a different layer count makes Ollama reload it), and it is
  SHARED through a file: every Solu process (Eflow's helper, a test, a second copy) sends the same layer count. Without
  that, a second process asked for "all of it", Ollama reloaded the whole brain onto a full card and VS Code crashed.
"""
import json
import os
import subprocess
import threading
import time
import urllib.request

KEEP = "3m"                     # how long the brain stays in memory after its last use
RESERVE_GB = 1.5                # always left free for the user's other apps
# per model: layers (blocks + output), GB per layer (weights + context cache), GB it needs beyond its layers
SIZES = {"qwen3:0.6b": (29, 0.03, 0.2), "qwen3:1.7b": (29, 0.07, 0.2), "qwen3:4b-instruct": (37, 0.1, 0.3),
         "qwen3:8b": (37, 0.175, 0.2), "qwen3:14b": (41, 0.28, 0.3), "qwen3:30b-a3b": (49, 0.45, 0.4),
         "qwen3-vl:4b": (37, 0.12, 1.5)}
API = "http://127.0.0.1:11434/api"
SHARED = os.path.join(os.environ.get("LOCALAPPDATA", "."), "Eflow", "gpu_layers.json")
SMI = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "nvidia-smi.exe")

_lock = threading.Lock()
_layers = {}                    # model -> the num_gpu chosen when it was loaded (None = all of it)
_loaded_at = {}                 # model -> (time checked, loaded?)


def free_vram_gb():
    """Free memory on the graphics card in GB, or None when it cannot be read (no NVIDIA card, no driver tool)."""
    try:
        out = subprocess.run([SMI, "--query-gpu=memory.free", "--format=csv,noheader,nounits"], capture_output=True, text=True,
                             timeout=4, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout
        return int(out.split()[0]) / 1024.0
    except Exception:                                                                 # noqa: BLE001
        return None


def is_loaded(model):
    now = time.monotonic()
    seen = _loaded_at.get(model)
    if seen and now - seen[0] < 3.0:
        return seen[1]
    try:
        with urllib.request.urlopen(API + "/ps", timeout=2) as resp:
            names = {m.get("name") for m in json.loads(resp.read()).get("models", [])}
        loaded = model in names
    except Exception:                                                                 # noqa: BLE001
        loaded = False
    _loaded_at[model] = (now, loaded)
    return loaded


def _shared(model, layers=...):
    """Read (or with layers, write) the layer count every Solu process uses for this model. ... = nothing recorded."""
    try:
        with open(SHARED, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        data = {}
    if layers is ...:
        return data[model] if model in data else ...
    data[model] = layers
    try:
        os.makedirs(os.path.dirname(SHARED), exist_ok=True)
        with open(SHARED + ".tmp", "w", encoding="utf-8") as f:
            json.dump(data, f)
        os.replace(SHARED + ".tmp", SHARED)
    except OSError:
        pass
    return layers


def options(model, opts):
    """The request options with a layer count that leaves room for everything else on the card."""
    with _lock:
        if is_loaded(model):                                                          # loaded, maybe by another process: use ITS count
            known = _shared(model)
            if known is not ...:
                _layers[model] = known
            elif model not in _layers:                                                # loaded by an older Solu: never ask for more
                _layers[model] = _loaded_layers(model)
        else:                                                                         # deciding is only safe before it loads
            free = free_vram_gb()
            layers_n, layer_gb, fixed_gb = SIZES.get(model, (37, 0.17, 0.6))
            if free is None or free - RESERVE_GB >= fixed_gb + layers_n * layer_gb:
                _layers[model] = None                                                 # it fits whole
            else:
                _layers[model] = max(0, min(layers_n, int((free - RESERVE_GB - fixed_gb) / layer_gb)))
                print(f"   [gpu] {free:.1f} GB free on the card: {_layers[model]} of {layers_n} layers of {model} there, the rest on the processor")
            _shared(model, _layers[model])
            _loaded_at[model] = (time.monotonic(), True)                              # this call loads it
        layers = _layers.get(model)
    out = dict(opts)
    if layers is not None:
        out["num_gpu"] = layers
    return out


_installed = {}                 # model -> installed in Ollama? (asked once)


def installed(model):
    if model not in _installed:
        try:
            with urllib.request.urlopen(API + "/tags", timeout=2) as resp:
                names = {m.get("name") for m in json.loads(resp.read()).get("models", [])}
            _installed[model] = model in names
        except Exception:                                                             # noqa: BLE001
            return False
    return _installed[model]


def _fits(model, free):
    layers_n, layer_gb, fixed_gb = SIZES.get(model, (37, 0.17, 0.6))
    return free is None or free - RESERVE_GB >= fixed_gb + layers_n * layer_gb


def brain(big, fast):
    """Which brain answers now. The big one only when it fits WHOLE on the card: split with the processor it took 30 s
    to start answering, while the fast one (whole on the card) starts in well under a second. A loaded brain is kept."""
    if not installed(fast):
        return big
    if is_loaded(big):
        return big
    if is_loaded(fast):
        return fast
    return big if _fits(big, free_vram_gb()) else fast


def _loaded_layers(model):
    """How many layers the loaded model has on the card, from what Ollama reports (None = all of it)."""
    try:
        with urllib.request.urlopen(API + "/ps", timeout=2) as resp:
            m = next(x for x in json.loads(resp.read()).get("models", []) if x.get("name") == model)
        if m["size_vram"] >= m["size"] * 0.98:
            return None
        layers_n = SIZES.get(model, (37,))[0]
        return int(round(layers_n * m["size_vram"] / m["size"]))
    except Exception:                                                                 # noqa: BLE001
        return None


def body(model, opts, keep=KEEP, **rest):
    """A request body for /api/chat or /api/generate: how long to keep it, and the polite layer count."""
    return {"model": model, "keep_alive": keep, "options": options(model, opts), **rest}


def unload(model):
    """Let go of the card now (Solu switched off or closing)."""
    try:
        req = urllib.request.Request(API + "/generate", json.dumps({"model": model, "keep_alive": 0}).encode(), {"Content-Type": "application/json"})
        urllib.request.urlopen(req, timeout=1.2).read()               # Eflow waits 1.5 s for us to quit
    except Exception:                                                                 # noqa: BLE001
        pass
    _loaded_at.pop(model, None)
