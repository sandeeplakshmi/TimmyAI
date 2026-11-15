# Your Timmy folder (TIMMY_HOME, default ~/.timmy)

Everything personal lives here, outside the Timmy repository, and is never committed:

- `.env`                 your settings (see the repo's .env.example)
- `keys/`                device keys for the desk gadget (gadget-keys.json, mode 600)
- `briefings/`           daily briefings the brain writes
- `topics/`              your notes, one markdown file per topic
- `skill-state/`         local skills' state and your skill settings (config.yaml)
- `addons/<name>/`       private server add-ons: addon.py with register(api), plus any scripts
- `firmware-extras/`     private firmware components (extra avatars, ...), built in when present
- `denylist.txt`         strings that must never reach the public repo (checked before every commit)
- `allowed-hosts.txt`    extra host names the web page may be reached as, one per line
- `gadget-status.json`   when each board last connected (written by the server)

Back it up with the repo's scripts/backup-private.sh; restore with scripts/restore-private.sh.
