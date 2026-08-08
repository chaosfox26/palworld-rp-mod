# Getting started — for a first-timer

Written for someone who has never compiled anything. You already have a GitHub
account, UE4SS working in Palworld on Steam, and the backend deployed and updated
to v2.6.1. That is most of the hard part done.

## Where am I?

Tick these off as you go. It is fine to stop anywhere and come back.

- [x] **Part 1** — Node.js and Claude Code installed *(done: node v24.19.0, claude 2.1.226)*
- [x] **Part 2** — key and backend URL collected *(done: key verified and already written into `src/key.h`)*
- [ ] **Part 3** — repo pushed to GitHub, CI running
- [ ] **Part 4** — builds going green
- [ ] **Part 5** — mod installed into Palworld
- [ ] **Part 6** — first run in game

**You are at the start of Part 3.**

---

## Read this before anything else

**The code in this folder has never been compiled.** Nobody has run it through a
Windows compiler yet. That means:

- The first build **will** fail. Probably the second and third too.
- That is the expected path, not a mistake you made.
- You are not going to fix those errors. Claude Code is. Your job is to install a
  couple of things, paste some prompts, and tell it what you see.

Budget a couple of sessions, not twenty minutes. Nothing here can break your game
or your backend — the worst case is a mod that refuses to load.

**A word on what "compiling" means here**, since it comes up constantly below:
the `.cpp` and `.hpp` files in `src/` are human-readable instructions. A compiler
turns them into `PalworldRpChat.dll`, which is the thing Palworld can actually
load. Compiling is that translation step. It fails when the instructions have a
mistake in them, and the error messages are how you find the mistake.

---

## Part 1 — Install two things (about 15 minutes, once) ✅ DONE

Kept here for reference and in case you ever set this up on another machine.

Open **PowerShell**: press `Win`, type `powershell`, press Enter.

### 1.1 Node.js

```powershell
winget install OpenJS.NodeJS.LTS
```

**Close PowerShell and open a new one** when it finishes. This matters — the new
window is the only one that knows Node exists.

Check it worked:

```powershell
node --version
```

**Any version number is success.** `v22`, `v24`, whatever winget gave you — it
does not need to match anything. Only "not recognized" means the install failed.

### 1.2 Claude Code

```powershell
npm install -g @anthropic-ai/claude-code
```

Check:

```powershell
claude --version
```

A line like `2.1.226 (Claude Code)` means you are done.

### The warnings you will see here are fine

`npm` prints three things at this step that all look worse than they are.

**`npm warn allow-scripts ... not yet covered by allowScripts`**

Newer npm refuses to run a package's own setup script unless you say so, as a
supply-chain precaution. It is telling you it skipped one, not that anything
broke. **If `claude --version` printed a version, the install is working — ignore
this.**

Only if Claude Code later misbehaves in some way you cannot explain, re-run it
with the setup script allowed:

```powershell
npm install -g --allow-scripts=@anthropic-ai/claude-code @anthropic-ai/claude-code
```

**`npm notice New major version of npm available`**

Cosmetic. Do not update npm right now — a major version bump has its own way of
going wrong, and there is nothing here that needs it. Leave it for a quiet day.

**`added 2 packages in 7s`**

That is the success line.

### What you did NOT need to install

No compiler, no Visual Studio, no C++ anything. That is the whole point of
building in the cloud. Claude Code will install Git and the GitHub tool itself
when it gets to them, so you do not need those either.

---

## Part 2 — Collect two things from your backend (5 minutes) ✅ DONE

**Already handled.** Your key was verified — the base64 and hex forms agree and it
is exactly 32 bytes — and written into `src/key.h`. Your backend URL was already
the default in `Scripts/main.lua`, so there is nothing to configure. You can skip
straight to Part 3.

Keep the rest of this section for reference.

You need the encryption key and the backend URL. Both come from the machine your
backend runs on, the same place you ran the installer.

On that server:

```bash
npx -y github:chaosfox26/palworld-rp-backend keygen
npx -y github:chaosfox26/palworld-rp-backend url
```

`keygen` prints a key. `url` prints something like
`https://212-28-190-199.sslip.io`.

**Copy both into a text file for now.** You will paste them in Part 4.

### Check the backend is reachable from this PC

Worth 20 seconds, because it separates "the mod is broken" from "the mod cannot
reach the server" later. In a browser, visit your URL with these on the end:

| Address | What you should see |
|---|---|
| `…/health` | `{"ok":true,"uptimeSeconds":…}` |
| `…/info` | a longer block with `limits`, `encryption`, `registrationOpen` |

If `/health` works, TLS and Caddy are fine. If `/info` shows
`"required": true` under `encryption`, the mod is already configured for that —
that is the expected setting.

### Two things about the key

**Every player needs a mod built with the same key**, or they cannot read each
other's messages. It is not a password to anything, but changing it later means
everyone reinstalls the mod, so pick one and keep it.

**Do not regenerate it once people are playing.** `keygen` gives you a new key
every time you run it. Run it once, save the output, and use that.

### You do not need to rebuild when you change server settings

Since you have been working on the backend: the mod reads the server's limits from
`/info` every 60 seconds. Message size, chat radius, party size, password minimum,
whether registration is open — change any of those in the admin overrides and the
mod picks it up on its own, mid-session, no rebuild. Only changing the **key**
needs a new build.

---

## Part 3 — Hand the project to Claude Code

In PowerShell:

```powershell
cd D:\Claude\Test\palworld-rp-mod
claude
```

It will ask you to sign in on first run. Use the same account you use for Claude.

**A few things about talking to Claude Code**, since this is your first time:

- It will ask permission before running commands that change things. Reading files
  it does just does. When it asks, "yes" is usually right — you can read what it
  wants to run before approving.
- It works in this folder only. It cannot touch your Palworld install or your
  backend unless you point it there.
- `/clear` starts a fresh conversation if it gets confused. `Ctrl+C` twice exits.
- It has already been briefed: `CLAUDE.md` in this folder tells it the whole
  architecture, the backend contract, and that you are new to this. You do not
  need to explain any of that.

Now paste this in, exactly:

> Read CLAUDE.md first. There is a broken `.git` folder from a failed init —
> delete it, then set up a clean git repo. Install Git and the GitHub CLI if they
> are missing. Create a **private** GitHub repo called `palworld-rp-mod`, push,
> and confirm the Actions workflow started. Do not change any source files yet.

**Why private:** this repo will contain your encryption key and your backend URL.
Private costs nothing and GitHub Actions works the same. If you ever make it
public, generate a new key first.

When it says the workflow is running, go look: on github.com open your repo and
click the **Actions** tab. You will see three jobs.

| Job | What it is | Expect |
|---|---|---|
| `host-tests` | checks the maths and message formatting | ✅ green |
| `windows-crypto` | checks the encryption on a real Windows machine | ❌ red at first |
| `windows-dll` | builds the actual mod file | ❌ red at first, marked "expected" |

**Two red marks here is success.** It means the cloud machines ran and told us
what is wrong. Red on `host-tests` would be surprising — mention it if you see it.

---

## Part 4 — The build loop

This is where most of the time goes. The pattern is always the same: Claude Code
changes something, pushes, waits for the cloud to build, reads the errors, changes
something again.

Paste this:

> Work through the CI failures in the order given in CLAUDE.md. Start with
> `windows-crypto` because it has no UE4SS dependency. Use `gh run list` and
> `gh run view --log-failed` to read the build logs yourself. Push a fix, wait for
> the run, read the result, repeat. Tell me when `windows-crypto` is green, and
> tell me if you get stuck on the same error twice.

Each cycle takes roughly 3–5 minutes of waiting. Claude Code can read the logs
directly, so you do not need to copy anything from the website.

When `windows-crypto` goes green, that is a real milestone: the encryption is
proven to work and to interoperate with what your server expects.

Then:

> Now get `windows-dll` producing a DLL. The reconnaissance step in the workflow
> prints the real UE4SS layout — read that log before editing xmake.lua.

**Your key is already in `src/key.h`**, so there is nothing to do here. It was
verified before being written, which matters: a mistyped key produces a mod that
encrypts perfectly and is unreadable by everyone else, with no error to explain
why.

### If it gets stuck

Two signs to watch for: the same error twice in a row, or more than about six
failed runs with no progress. Either way, say:

> We seem stuck. Summarise what you have tried, what the current error is, and
> what you think the options are. Do not push another change yet.

Then bring that summary back to me here and I will look at it.

---

## Part 5 — Install the mod into Palworld

When `windows-dll` is green there will be a downloadable file on the Actions run
page, at the bottom, under **Artifacts** — a zip called `PalworldRpChat`.

Download and unzip it. Inside you will find `PalworldRpChat.dll` and a `Scripts`
folder.

Your UE4SS mods live here (Steam default):

```
C:\Program Files (x86)\Steam\steamapps\common\Palworld\Pal\Binaries\Win64\ue4ss\Mods\
```

Open that folder. You will recognise it — it has the mods you already installed.

Create a folder called `PalworldRpChat` and arrange it like this:

```
Mods\PalworldRpChat\
├── dlls\
│   └── main.dll          <- PalworldRpChat.dll, RENAMED to main.dll
└── Scripts\
    ├── main.lua
    └── json.lua
```

The rename is not optional — UE4SS looks for `dlls\main.dll` specifically.

Then open `Mods\mods.txt` in Notepad and add this line:

```
PalworldRpChat : 1
```

**Check the shape against a mod that already works.** Look at one of your existing
mods' folders and match it. UE4SS has changed this layout between versions, and
yours is the version that counts. If it differs from what I described above,
trust yours and tell me.

### Point the mod at your backend

Launch Palworld once, then quit. That creates `rp-config.json` next to
`main.lua`. Open it in Notepad and set `backendUrl` to the URL from Part 2:

```json
{ "backendUrl": "https://212-28-190-199.sslip.io" }
```

---

## Part 6 — First run

Launch Palworld and load into a world.

1. The UE4SS console window should show lines starting with `[RP]`.
2. Press **F9**. The chat window should appear.
3. Type a character name and password, click **Register**. Names are lowercase
   letters, numbers, `_` and `-`, between 2 and 24 characters.
4. Status should go to **Ready**.
5. Type `hello` and press Send. It goes to local chat.
6. Type `/help` to see every command.

To confirm it truly works you need a second player with the same mod and the same
key, standing near you in game. Local chat only reaches people within about 3000
units — roughly shouting distance.

### If nothing happens

Check in this order, and tell me what you find:

- **No `[RP]` lines in the UE4SS console** — the mod did not load. Folder layout
  or `mods.txt`.
- **`[RP]` lines but F9 does nothing** — the DLL loaded but the window did not
  register.
- **Window opens, but Register fails** — the backend is unreachable. Open your
  backend URL in a browser with `/health` on the end; you should see
  `{"ok":true,...}`.
- **`[unreadable: ...]` on incoming messages** — key mismatch. That player's mod
  was built with a different key.

---

## One thing to be honest with your players about

Local chat position is reported by each player's own game, so a modified client
can claim to be standing anywhere and listen in from across the map. That is a
deliberate trade-off in this design, not a bug. Whispers and passcode-protected
channels are the private options. Say so in your mod description — and do not
call this end-to-end encrypted, because every player has the same key.

---

## Quick reference

| I want to... | Do this |
|---|---|
| Start Claude Code | `cd D:\Claude\Test\palworld-rp-mod` then `claude` |
| See if the build passed | GitHub repo → Actions tab |
| Get the built mod | Actions → click the run → Artifacts at the bottom |
| Change commands or wording | Edit `Scripts\main.lua`, no rebuild needed |
| Change anything in `src\` | Needs a rebuild — push and wait for CI |
| Check the backend is alive | Visit your URL + `/health` in a browser |
| See the server's current limits | Visit your URL + `/info` in a browser |
| Resume this guide after a break | Check the "Where am I?" list at the top |
| Change a server limit | Admin overrides on the backend — no rebuild needed |
| Change the encryption key | Needs a rebuild, and every player reinstalls |
