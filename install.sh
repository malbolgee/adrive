#!/usr/bin/env bash

if [ -n "$SSH_CLIENT" ] || [ -n "$SSH_TTY" ]; then
    directory="/localrepo/$USER/.local/bin/"
else
    directory="$HOME/.local/bin/"
fi

# Ensure install directory exists
mkdir -p "$directory"

# Create symlink to adrive binary
ln -sf "$(pwd)/adrive" "$directory/adrive"

# Export install directory in PATH via bashrc if not already present
export_line="export PATH=\"\$PATH:$directory\""
if ! grep -Fxq "$export_line" "$HOME/.bashrc"; then
    echo "$export_line" >> "$HOME/.bashrc"
    echo "line $export_line successfully added to bashrc"
else
    echo "line $export_line already exists in bashrc"
fi
