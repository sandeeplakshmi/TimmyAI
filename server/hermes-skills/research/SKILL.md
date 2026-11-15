---
name: research
description: "'Does X exist?' web research with sources, saved to a topic."
version: 1.0.0
author: the Timmy contributors
license: Apache-2.0
metadata:
  hermes:
    tags: [research, web search, prior art, sources, ideas]
    category: research
    requires_toolsets: [web]
---

# Research ("does this exist?")

Before the user starts a new idea, check whether it (or something close) already exists, summarize what's out there with sources, and save the findings to the matching topic in `$TIMMY_HOME/topics/ (default ~/.timmy/topics/)`.

## When to Use

- "Does X exist?", "is anyone already doing X?", "research X", "what's out there for X?", "find me options for X"
- They describe a new project or idea and asks whether it's worth building.

## Procedure

1. Restate the idea in one line and pick 3–5 search queries: the idea itself, the problem it solves, likely product names or categories, and "open source" / "github" variants for software ideas.
2. Run `web_search` for each query. Open the 3–6 most relevant results with `web_extract` to confirm what they actually do; don't trust snippets alone.
3. Answer in this shape (short; for voice keep only the verdict and top 2 options):
   - **Verdict:** exists / partly exists / doesn't seem to exist, in one sentence.
   - **Closest matches:** up to 5, each: name, what it does, cost (free / paid / open source), link.
   - **Gap:** what none of them do that their idea would.
   - **Suggested next step:** one line.
   - **Sources:** the URLs you used.
4. Save to the topic (read `$TIMMY_HOME/topics/README.md` to find it; use the topics skill rules):
   - Add one bullet under `## Context so far`: `- Research <YYYY-MM-DD>: <verdict>; closest: <name> (<link>), <name> (<link>).`
   - Append a Log line: `- <YYYY-MM-DD>: Researched "<question>": <verdict>.`
   - If no topic fits, ask whether to create one ("add a topic for …") before saving.

## Pitfalls
- Cite only pages you actually opened or saw in results. Never invent product names, prices or links.
- Prefer recent sources and say when something looks old or discontinued.
- Free tools only; don't sign up for anything or use paid search APIs.
- Don't paste long quotes from pages. Summarize in your own words.

## Verification
- Every claim in the answer maps to a listed source, and the topic file has the new bullet and Log line.
