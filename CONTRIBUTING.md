# Contributing to Timmy

Thanks for helping. Pull requests are welcome.

## Pull requests

1. Fork the repo and create your branch from `main`.
2. Run `scripts/install-hooks.sh` once, so every commit runs the checks.
3. If you add code that should be tested, add tests. If you change behaviour,
   update the docs.
4. Before you open the PR, run `scripts/check-licenses.sh` and
   `scripts/check-private.sh`, and build the firmware if you touched it
   (`cd firmware && idf.py build`).

## Rules for every change

- **New files carry an Apache-2.0 SPDX header**, in the file's comment style:

      SPDX-License-Identifier: Apache-2.0
      Copyright 2026 the Timmy contributors

- **Files that came from the upstream SDK keep their original copyright
  header unchanged.** When you change one, make sure it carries the line
  `Modified by the Timmy contributors, 2026` under that header (the checks
  enforce it).
- **No third-party branding.** Don't add the upstream vendor's or product's
  names, logos, avatars or other assets to the code, UI strings or docs. The
  only places the upstream project is named are the attribution in `NOTICE`,
  the credit in `README.md`, and the original copyright headers.
- **No personal data.** Nothing from `~/.timmy` (your keys, `.env`, notes,
  memory, device keys) and no Wi-Fi names, IP addresses, host names, e-mail
  addresses or real names in code, comments, tests, screenshots or commit
  messages. Private add-ons belong in `~/.timmy`, not here
  (`docs/CUSTOMIZING.md`).
- Bundled third-party code keeps its license file, and `NOTICE` lists it.

## Issues

Use GitHub issues for bugs, with steps to reproduce. For security problems,
use the repository's private vulnerability reporting instead of a public issue.

## License

By contributing to Timmy, you agree that your contributions will be licensed
under the Apache License 2.0 in the `LICENSE` file at the root of this
repository.
