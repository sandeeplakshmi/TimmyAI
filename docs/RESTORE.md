# Restoring Timmy on a new Mac

1. **Back up the old Mac** (any time):

       scripts/backup-private.sh zip          # encrypted zip of ~/.timmy (asks for a password)
       # or
       scripts/backup-private.sh git          # local git history of ~/.timmy, keys/ and .env excluded

   Copy the zip (or push the git backup to a private remote you control). The
   git backup leaves out `keys/` and `.env` on purpose: keep those in the
   encrypted zip or your password manager.

2. **On the new Mac**, clone the repo, then restore before installing:

       scripts/restore-private.sh ~/timmy-private-YYYY-MM-DD.zip
       # or: scripts/restore-private.sh <your private git URL>   (then copy keys/ and .env back)

   It refuses to overwrite an existing `~/.timmy`.

3. **Install**: `./install.sh --yes` keeps your restored folder as it is,
   loads your add-ons and sets up the login service. Enter the API key again on
   the setup page (it lives in Hermes' settings, not in `~/.timmy`).

4. **Check**: `./install.sh doctor`. Firmware add-ons in
   `~/.timmy/firmware-extras` are built in by `./install.sh flash`.
