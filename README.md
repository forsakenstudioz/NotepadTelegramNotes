# Telegram Notes for Notepad++

A Notepad++ plugin (64-bit) that connects Notepad++ to **your own private Telegram bot**:

- **Notepad++ → phone:** send the current note, the selected text or a todo list to your phone. Code is sent as a formatted code block; long notes arrive as a file.
- **Phone → Notepad++:** type a note to your bot on your phone and open it in Notepad++ with one click.
- **Ask from your phone:** send `/list` to see your notes, `/get name` to get one, and keep a `/todo` and `/grocery` list.
- **Works while your PC is off (optional, free):** with the small cloud bot from [part 2](#part-2-keep-the-bot-running-when-your-pc-is-off-free) the bot answers 24/7. Without it, the bot only answers while Notepad++ is open.

Only **your** Telegram account can use the bot. Messages from anyone else are ignored.

---

## Part 1: Setup (about 5 minutes, one time only)

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
| **Connect to cloud (works when PC is off)** | Hands the bot over to your cloud bot ([part 2](#part-2-keep-the-bot-running-when-your-pc-is-off-free)). |
| **Add this file to cloud notes** | Makes a file outside the notes folder available to `/get` too. It uploads again every time you save it. |
| **Sync with cloud now** | Uploads changed notes and downloads phone notes and lists right away (this also happens by itself every minute). |
| **Help** | Short reminder of all of this. |

After sending, the bottom-left of the Notepad++ status bar says **"Telegram: sent ..."**. If something goes wrong, a message explains why.

Tip: give any of these a keyboard shortcut in **Settings > Shortcut Mapper > Plugin commands**.

### From your phone

Without the cloud bot, Notepad++ must be open on your PC for the bot to answer. With the cloud bot it answers any time.

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
| `/del name` | deletes a note from the cloud (cloud bot only) |
| `/ping` | checks that Notepad++ is online |
| `/help` | shows all commands |

Good to know:

- If Notepad++ is closed, your messages wait on Telegram (up to 24 hours) and are handled as soon as you open Notepad++ again.
- `/get` reads the **saved** file from disk, so save a tab (Ctrl+S) before asking for it from your phone.
- `todo.txt` and `grocery.txt` are ordinary text files in `Documents\Telegram Notes`. You can edit them in Notepad++ too.
- Want phone notes to pop open in Notepad++ automatically? Set `AutoOpenPhoneNotes=1` in the settings file.

---

## Part 2: Keep the bot running when your PC is off (free)

Normally the bot "lives" inside Notepad++, so it stops answering when your PC is off. This part moves the bot to **Cloudflare Workers**, a free service that runs small programs on the internet around the clock. You don't need a credit card, and you only do this once (about 10 minutes).

Do **part 1 first** (bot created, phone connected). Then:

### Step 5: Create a free Cloudflare account

1. Go to [dash.cloudflare.com/sign-up](https://dash.cloudflare.com/sign-up) and sign up with your email. Confirm the email they send you.
2. If it asks what you want to do or offers a paid plan, skip it. The free plan is enough.

### Step 6: Create the Worker (the cloud bot)

1. In the Cloudflare dashboard, open **Workers & Pages** in the left menu (on some screens it's under **Compute**).
2. Click **Create** (or **Create application**), then choose **Start with Hello World!**.
3. Name it `telegram-notes` and click **Deploy**.
4. Click **Edit code**. Delete everything in the editor, then paste the whole content of [`cloud/worker.js`](cloud/worker.js) (on GitHub, open the file and click the **copy** icon at the top right of the code).
5. Click **Deploy** (top right).

### Step 7: Give it a place to store your notes

1. In the left menu, open **Storage & Databases > KV** (sometimes called **Workers KV**).
2. Click **Create** (or **Create a namespace**), name it `telegram-notes`, and click **Add**/**Create**.
3. Go back to **Workers & Pages**, open your `telegram-notes` Worker, then **Settings > Bindings**.
4. Click **Add**, choose **KV namespace**, and fill in:
   - **Variable name:** `NOTES` (exactly like this, in capitals)
   - **KV namespace:** `telegram-notes`
5. Click **Save** (or **Deploy**).

### Step 8: Check it works

1. On the Worker's page, find its address. It looks like `https://telegram-notes.yourname.workers.dev`. Click it.
2. Your browser should say **"Telegram Notes cloud is running."** If it says "Almost there: add a KV namespace binding named NOTES", go back to step 7.

### Step 9: Connect Notepad++ to it

1. In Notepad++ open **Plugins > Telegram Notes > Settings (bot token)**.
2. At the bottom, find `CloudUrl=` and paste your Worker address after it:

   ```
   CloudUrl=https://telegram-notes.yourname.workers.dev
   ```

   (If you installed an older version of the plugin and there's no `CloudUrl=` line, click **Connect to cloud** once and it's added for you.)
3. Save (**Ctrl+S**).
4. Click **Plugins > Telegram Notes > Connect to cloud (works when PC is off)**. You should see **"Done! ... now runs in the cloud"**.

That's it. Send `/ping` to your bot: it answers "I'm online (in the cloud)", even after you shut down your PC.

### How it works now

- **Your notes:** every file in your notes folder (`Documents\Telegram Notes`) is uploaded when you save it, and when Notepad++ starts. Use **Add this file to cloud notes** for files that live somewhere else. `/get` on your phone gives you the last uploaded version.
- **Phone notes:** what you type to the bot waits safely in the cloud (no time limit). Notepad++ downloads it whenever it's open; open it with **Fetch notes from phone**.
- **Todo and grocery lists:** they live in the cloud, and Notepad++ keeps `todo.txt` and `grocery.txt` in your notes folder up to date. You can also edit those files in Notepad++; when you save, your version goes to the cloud. The lists you already had are copied to the cloud the first time you connect.
- **Sending from Notepad++** (Send note, selection, todo) works the same as before.
- **Your SyncKey** (in the settings file) is the password between Notepad++ and your cloud bot. Keep it private like the bot token.

The free Cloudflare plan allows about 1,000 saved changes a day (note uploads plus list changes from your phone), which is far more than normal use needs.

To stop using the cloud bot, delete the address after `CloudUrl=` and save. Notepad++ then takes the bot back and works like before.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| No **Telegram Notes** under Plugins | The DLL must be at `...\Notepad++\plugins\TelegramNotes\TelegramNotes.dll` (folder and file names must match). This build is for **64-bit** Notepad++ (check **? > About Notepad++**). |
| "Telegram does not recognise this bot token" | Copy the token again from BotFather, the whole thing including the part before the `:`. Save the settings with Ctrl+S. |
| "Connect my phone" doesn't find you | Make sure you opened **your bot** (not BotFather) and pressed **Start** or sent "hi", then click **Retry**. |
| The bot doesn't answer on the phone | Notepad++ must be open on your PC and the PC online. Send `/ping` to check. |
| "This bot is already connected to something else" | Another program uses the same bot. Make a new bot with `/newbot` in BotFather and use its token. |
| You lost or leaked the token | In BotFather send `/revoke`, pick your bot, and paste the new token into the settings. With the cloud bot, click **Connect to cloud** again afterwards. |
| "Connect to cloud" says "Unexpected answer from the cloud" | Check `CloudUrl` is exactly the address from step 8, and that you deployed the Telegram Notes code (step 6.4–6.5), not the Hello World example. |
| "This cloud bot is already connected to another Notepad++" | The Worker remembers the first Notepad++ that connected. On a new PC, copy the `SyncKey=` line from the old PC's settings. Or reset it: in Cloudflare open **KV > telegram-notes**, delete the entry called `config`, and connect again. |
| A note doesn't show up in `/list` (cloud bot) | Only files in the notes folder (or added with **Add this file to cloud notes**) are uploaded, and only text files up to 5 MB. Save the file, then click **Sync with cloud now**. |

Settings are stored in `%AppData%\Notepad++\plugins\config\TelegramNotes.ini`.

---

## Building from source

The plugin is one C++ file using only Windows' built-in WinHTTP, so it needs no extra libraries. The cloud bot is the single file `cloud/worker.js`, pasted into Cloudflare as-is.

- **On Linux / WSL:** `sudo apt-get install g++-mingw-w64-x86-64 zip`, then `sh build.sh`. The DLL is written to `dist/TelegramNotes/`.
- **On GitHub:** the workflow in `.github/workflows/build.yml` builds the DLL on every push (download it from the run's **Artifacts**). Pushing a tag like `v1.0` also attaches `TelegramNotes-x64.zip` to a GitHub Release.
