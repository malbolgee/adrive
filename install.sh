#!/usr/bin/env bash

if [ -n "$SSH_CLIENT" ] || [ -n "$SSH_TTY" ]; then
    directory="/localrepo/$USER/.local/bin/"
else
    directory="$HOME/.local/bin/"
fi

function install() {
    create_directory
    create_symlink
    export_shortcut
}

function create_directory() {
    if [ ! -d "$directory" ]; then
        mkdir -p "$directory"
    fi
}

function create_symlink() {
    ln -sf "$(pwd)/adrive" "$directory/adrive"
}

function export_shortcut() {
    local export_line="export PATH=\"\$PATH:$directory\""
    if ! grep -Fxq "$export_line" "$HOME/.bashrc"; then
        echo "$export_line" >> "$HOME/.bashrc"
        echo "line $export_line successfully added to bashrc"
    else
        echo "line $export_line already exists in bashrc"
    fi
}

install
