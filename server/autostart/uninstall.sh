#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
# Remove Timmy's auto-start: the web-page gateway hook and the Hermes gateway login service.
export PATH="$HOME/.local/bin:$PATH"
rm -rf "$HOME/.hermes/hooks/timmy-web"
hermes gateway uninstall
echo "Timmy no longer starts at login. Start it by hand with ./start-timmy.sh"
