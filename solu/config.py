"""Solu's settings that other modules share: the local model server, the models and the context size."""

OLLAMA = "http://127.0.0.1:11434/api/chat"
BRAIN = "qwen3:8b"
FAST = "qwen3:4b-instruct"                               # answers instead when the big brain does not fit whole on the GPU
CTX = 8192                                      # one context size for every brain call: a change makes Ollama reload the model
VISION = "qwen3-vl:4b"                          # only used for questions about pictures; it is swapped in and out of the GPU
