# Releasing Timmy

## Checklist

1. `scripts/check-licenses.sh` and `scripts/check-private.sh` are clean (with
   your `~/.timmy/denylist.txt` in place), and CI is green.
2. Review the whole diff since the last release yourself: no personal names,
   addresses, Wi-Fi names, host names, device IDs, keys or tokens, and no
   third-party branding outside `NOTICE` and the README credits.
3. Build the public firmware with no add-ons (`TIMMY_EXTRA_COMPONENTS= idf.py build`)
   and check `strings build/timmy-gadget.bin` for anything private.
4. Check every screenshot and the demo clip: no notifications, Wi-Fi names,
   addresses, real names, browser tabs or menu-bar details in frame.
5. Update `NOTICE` when a bundled third-party component is added or removed.
6. If anything private ever leaked (in any commit, even one later reverted):
   rotate the key or token at once, revoke device keys
   (`server/gadget_server.py --revoke`), and rewrite or delete the published
   history.

## Never

- Never commit `~/.timmy`, `.env`, `keys/`, `*.pem`, logs, `build*/` or `managed_components/`.
- Never bypass the pre-commit hook (`--no-verify`).
- Never publish a firmware image built with private add-ons.
- Never put API keys in the firmware, the web page or command lines.
- Never add the upstream vendor's names, logos or assets.
