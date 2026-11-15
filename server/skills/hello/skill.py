# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""The smallest useful skill: answers, and plays a look from its eyes.json on the board."""


def handle(kit, req):
    # req.text: what was said; req.match: the pattern's named groups; req.source: board, web or brain
    look = "sparkle" if req.match.get("extra") == "stars" else "wave"
    kit.eyes(look, secs=4)            # eyes.json names it; shown on the board, if one is online
    name = kit.config.get("name")     # from the 'general' section of the local config
    return f"Hello{', ' + name if name else ''}! This is the hello skill."
