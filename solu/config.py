"""Solu's settings that other modules share: the local model server, the models and the context size."""

import os

# Where the speech models live. Installed copies of Eflow keep them in a writable folder of their own (the app's own
# folder is read-only), and set this; a development checkout just has them in solu\models.
MODELS = os.environ.get("EFLOW_SOLU_MODELS") or "models"


def model(name):
    return os.path.join(MODELS, name)


OLLAMA = "http://127.0.0.1:11434/api/chat"

# The brain. Eflow's guided setup picks one to suit the PC (a 0.6B on a laptop with no graphics card, a 30B on a
# 24 GB card) and passes it here; a development checkout, set up by hand, keeps the 8B this was written around.
BRAIN = os.environ.get("EFLOW_SOLU_BRAIN") or "qwen3:8b"
# The stand-in used when the big brain will not fit whole on the card. A brain already this small IS the stand-in:
# naming a second model the PC never downloaded would just make every answer wait for a failed lookup.
_SMALL = ("qwen3:0.6b", "qwen3:1.7b", "qwen3:4b-instruct")
FAST = os.environ.get("EFLOW_SOLU_FAST") or (BRAIN if BRAIN in _SMALL else "qwen3:4b-instruct")
CTX = 8192                                      # one context size for every brain call: a change makes Ollama reload the model
VISION = "qwen3-vl:4b"                          # only used for questions about pictures; it is swapped in and out of the GPU
