#!/bin/sh
# Launch llama-server for the model the bot selected (Task J).
#
# The bot never manages llama-server itself: /model rewrites /models/.current_model
# with the name of a model folder (a folder directly under MODELS_DIR that holds a
# server.args file). Run this script under systemd or tmux so a supervisor relaunches
# llama-server; set MODEL_RELOAD_CMD in bot.env to make /model trigger that relaunch.
# Invoke it as `sh scripts/run-model.sh` (no exec bit required) or chmod +x it first.
#
# The full llama-server argument list for a model lives in <folder>/server.args,
# shell-style (values may be quoted, e.g. inline JSON). It is written by hand; the bot
# only reads it.
set -eu

MODELS_DIR="${MODELS_DIR:-/models}"
LLAMA_SERVER_BIN="${LLAMA_SERVER_BIN:-/home/yang-wenli/llama/mtp/build/bin/llama-server}"

NAME="$(cat "$MODELS_DIR/.current_model" 2>/dev/null || true)"
[ -n "$NAME" ] || NAME="flashnext"

if [ ! -d "$MODELS_DIR/$NAME" ]; then
    echo "run-model: no such model folder: $MODELS_DIR/$NAME" >&2
    exit 1
fi
ARGS_FILE="$MODELS_DIR/$NAME/server.args"
if [ ! -f "$ARGS_FILE" ]; then
    echo "run-model: model '$NAME' has no server.args: $ARGS_FILE" >&2
    exit 1
fi

# shellcheck disable=SC2046  # server.args is intentionally word-split (may hold quoted values)
eval exec "$LLAMA_SERVER_BIN" "$(cat "$ARGS_FILE")"
