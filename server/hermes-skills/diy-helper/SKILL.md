---
name: diy-helper
description: "Turn measurements into DIY cut lists, shopping lists and build steps."
version: 1.0.0
author: the Timmy contributors
license: Apache-2.0
metadata:
  hermes:
    tags: [diy, woodworking, cut list, shopping list, cabinets, measurements]
    category: productivity
    requires_toolsets: [terminal]
---

# DIY helper

Turns your measurements and goals into a buildable plan: a parts list, a sheet-goods cut layout, a shopping list and step-by-step instructions.

Nesting script (plain Python, no extra packages):

```
python3 "$TIMMY_REPO/server/hermes-skills/diy-helper/scripts/cutlist.py" < parts.json
```

Input is JSON (inches): `{"sheet": {"length": 96, "width": 48}, "kerf": 0.125, "parts": [{"name", "qty", "length", "width", "material", "linear": true|false}]}`. Sheet parts are nested per material. `"linear": true` parts (face frames, cleats, trim) are totalled in feet and 8 ft boards. The output gives sheets needed, yield, rip-strip layout and any parts too big for a sheet.

## When to Use

- "I measured the closet: 72 wide, 24 deep, 90 high, make me a cut list"
- "What do I need to buy for …", "how do I build …", "plan the cabinets"

## Procedure

1. Read the topic file for context (what it stores, style, material, tools). If key inputs are missing, ask at most three short questions: inside dimensions (W × D × H), what it stores, and material (default ¾" plywood carcasses, ¼" backs).
2. Design the parts, stating each assumption on one line: material thickness (¾" ply is really 23/32"), toe-kick, clearance (about ¼" less than the opening in each direction), shelf spacing, and box count (keep boxes ≤ 36" wide, so shelves don't sag and boxes fit through doors).
3. Write the parts JSON and run `cutlist.py`. Double-check that the parts add up to the overall dimensions.
4. Produce the plan in this order:
   - **Overview:** box layout with overall sizes.
   - **Cut list:** a table of part, qty, length × width, material.
   - **Sheet layout:** per sheet, the rip widths and crosscuts from the script.
   - **Shopping list:** sheets, lumber, edge banding (total feet), hardware (hinges, slides, shelf pins, screws, pocket screws), finish. Give rough price ranges only if they ask, and say they're estimates.
   - **Steps:** numbered, in build order, with the tool for each step. Flag safety points (rip cuts on large sheets: use a track/circular saw with a guide, have help carrying sheets, wear eye and ear protection).
5. Save: write the plan to `topics/<topic>-plan.md` (overwrite the previous version, it's the current plan). In the topic file, add or refresh a Context bullet `- Current plan: [<topic>-plan.md](<topic>-plan.md) (<date>, <one-line summary>)` and append a Log line. Use the topics skill rules.
6. For voice replies, give only the headline: number of sheets, the main boards and the first step. Point to the plan file for details.

## Pitfalls
- Never guess measurements. Use their numbers, and ask if one is missing.
- Note when the room is finished (drywall, flooring, outlets): plan scribe strips and avoid blocking outlets and vents.
- Keep units in inches with fractions to 1/16" in the final cut list (e.g. 23 1/4").
- This is planning help, not structural or electrical advice. Say so if they ask about load-bearing walls or wiring.

## Verification
- Every part in the cut list appears in the script output, and the shopping list sheet count matches it.
