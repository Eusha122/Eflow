"""Solu's settings that other modules share: the local model server, the models and the context size."""

import os

# Where the speech models live. Installed copies of Eflow keep them in a writable folder of their own (the app's own
# folder is read-only), and set this; a development checkout just has them in solu\models.
MODELS = os.environ.get("EFLOW_SOLU_MODELS") or "models"


def model(name):
    return os.path.join(MODELS, name)


OLLAMA = "http://127.0.0.1:11434/api/chat"
BRAIN = "qwen3:8b"
FAST = "qwen3:4b-instruct"                               # answers instead when the big brain does not fit whole on the GPU
CTX = 8192                                      # one context size for every brain call: a change makes Ollama reload the model
VISION = "qwen3-vl:4b"                          # only used for questions about pictures; it is swapped in and out of the GPU
