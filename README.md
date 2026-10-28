# Telegram Notes for Notepad++

A Notepad++ plugin (64-bit) that connects Notepad++ to **your own private Telegram bot**:

- **Notepad++ → phone:** send the current note, the selected text or a todo list to your phone. Code is sent as a formatted code block; long notes arrive as a file.
- **Phone → Notepad++:** type a note to your bot on your phone and open it in Notepad++ with one click.
- **Ask from your phone:** while Notepad++ is open, send `/list` to see your notes, `/get name` to get one, and keep a `/todo` and `/grocery` list.

Only **your** Telegram account can use the bot. Messages from anyone else are ignored.

---

## Setup (about 5 minutes, one time only)

### Step 1: Create your bot in Telegram (on your phone)

A "bot" is just a special Telegram account that a program (this plugin) controls. Telegram makes them for free with its official bot called **BotFather**.

1. Open Telegram and tap the search icon.
2. Search for **BotFather**. Pick the one with the **blue verified tick**, named `@BotFather`.
3. Tap **Start**.
4. Send this message: `/newbot`
5. BotFather asks for a **name**. Type anything, for example: `My Notepad`
6. BotFather asks for a **username**. It must be unique and end in `bot`, for example: `zoro_notepad_bot`. If it's taken, try another.
7. BotFather replies with a long **token**, which looks like this:

   ```
   7123456789:AAHk3xQf9Lw2...about 35 more characters...
   ```

   This token is the password of your bot. **Do not share it with anyone.** You will copy it into Notepad++ in Step 3, so it helps to open Telegram on your PC too (web.telegram.org or Telegram Desktop) so you can copy and paste it.

### Step 2: Install the plugin in Notepad++

1. Download **TelegramNotes-x64.zip** and unzip it. You get a folder called `TelegramNotes` with `TelegramNotes.dll` inside.
2. **Close Notepad++.**
3. Open this folder in File Explorer (paste it into the address bar):

   ```
   C:\Program Files\Notepad++\plugins
   ```

4. Copy the whole **`TelegramNotes`** folder into it. Windows asks for administrator permission; click **Continue**.
   It must end up exactly like this:

   ```
   C:\Program Files\Notepad++\plugins\TelegramNotes\TelegramNotes.dll
   ```

5. Start Notepad++. You now have **Plugins > Telegram Notes**, and a small window asks whether you want to set it up. Click **Yes**.

> If Windows says the file is blocked: right-click `TelegramNotes.dll` > **Properties** > tick **Unblock** > OK, then start Notepad++ again.

### Step 3: Paste your token

1. A settings file named `TelegramNotes.ini` opens in Notepad++. (Later you can always open it with **Plugins > Telegram Notes > Settings (bot token)**.)
2. Find the line `BotToken=` and paste your token right after the `=`, with no spaces:

   ```
   BotToken=7123456789:AAHk3xQf9Lw2...
   ```

3. Save the file with **Ctrl+S**.

### Step 4: Connect your phone

1. On your phone, search Telegram for your new bot's username (for example `@zoro_notepad_bot`), open it and tap **Start**.
2. In Notepad++ click **Plugins > Telegram Notes > Connect my phone**.
3. The plugin finds your message and asks **"Is this you?"**. Check your name and click **Yes**.
4. Your bot sends you a "Connected to Notepad++!" message. You're done.

(This step saves your private **chat id** in the settings file. That number is how the bot knows to only listen to you.)

---

## Using it

### From Notepad++ (Plugins > Telegram Notes)

| Menu item | What it does |
|---|---|
| **Send note to Telegram** | Sends the whole current tab. Code files arrive as a code block; notes longer than about 3800 characters arrive as a file. |
| **Send selection to Telegram** | Sends only the text you selected. |
| **Send as todo list** | Sends the tab (or the selection) as a checklist: one task per line. `- [x] done` lines are shown ticked, lines ending in `:` become headings. |
| **Send note as .txt file** | Always sends the tab as a file attachment. |
| **Fetch notes from phone** | Opens everything you typed to your bot in a new tab. |
| **Open notes folder** | Opens `Documents\Telegram Notes`, where `todo.txt` and `grocery.txt` live. |
| **Settings (bot token)** | Opens the settings file. |
| **Connect my phone** | Links your Telegram account (Step 4). |
| **Help** | Short reminder of all of this. |

After sending, the bottom-left of the Notepad++ status bar says **"Telegram: sent ..."**. If something goes wrong, a message explains why.

Tip: give any of these a keyboard shortcut in **Settings > Shortcut Mapper > Plugin commands**.

### From your phone (Notepad++ must be open on your PC)

Tap the **/** button (or the **Menu** button) in the bot's chat to see the commands.

| Send this | And the bot... |
|---|---|
| any normal text | keeps it for **Fetch notes from phone** in Notepad++ |
| `/list` | lists the notes open in Notepad++ and the files in your notes folder, numbered |
| `/get 2` or `/get shopping` | sends you that note |
| `/todo` | shows your todo list |
| `/todo buy milk` | adds an item (one item per line if you send several lines) |
| `/done 2` | ticks item 2 (send again to un-tick) |
| `/grocery eggs, bread` | adds to the grocery list (commas separate items) |
| `/got 1` | ticks grocery item 1 |
| `/clear todo` or `/clear grocery` | removes the ticked items |
| `/ping` | checks that Notepad++ is online |
| `/help` | shows all commands |

Good to know:

- If Notepad++ is closed, your messages wait on Telegram (up to 24 hours) and are handled as soon as you open Notepad++ again.
- `/get` reads the **saved** file from disk, so save a tab (Ctrl+S) before asking for it from your phone.
- `todo.txt` and `grocery.txt` are ordinary text files in `Documents\Telegram Notes`. You can edit them in Notepad++ too.
- Want phone notes to pop open in Notepad++ automatically? Set `AutoOpenPhoneNotes=1` in the settings file.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| No **Telegram Notes** under Plugins | The DLL must be at `...\Notepad++\plugins\TelegramNotes\TelegramNotes.dll` (folder and file names must match). This build is for **64-bit** Notepad++ (check **? > About Notepad++**). |
| "Telegram does not recognise this bot token" | Copy the token again from BotFather, the whole thing including the part before the `:`. Save the settings with Ctrl+S. |
| "Connect my phone" doesn't find you | Make sure you opened **your bot** (not BotFather) and pressed **Start** or sent "hi", then click **Retry**. |
| The bot doesn't answer on the phone | Notepad++ must be open on your PC and the PC online. Send `/ping` to check. |
| "This bot is already connected to something else" | Another program uses the same bot. Make a new bot with `/newbot` in BotFather and use its token. |
| You lost or leaked the token | In BotFather send `/revoke`, pick your bot, and paste the new token into the settings. |

Settings are stored in `%AppData%\Notepad++\plugins\config\TelegramNotes.ini`.

---

## Building from source

The plugin is one C++ file using only Windows' built-in WinHTTP, so it needs no extra libraries.

- **On Linux / WSL:** `sudo apt-get install g++-mingw-w64-x86-64 zip`, then `sh build.sh`. The DLL is written to `dist/TelegramNotes/`.
- **On GitHub:** the workflow in `.github/workflows/build.yml` builds the DLL on every push (download it from the run's **Artifacts**). Pushing a tag like `v1.0` also attaches `TelegramNotes-x64.zip` to a GitHub Release.
