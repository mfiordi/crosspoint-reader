# Google Drive Sync — Setup Guide

This guide walks you through everything you need to use **Google Drive Sync**: creating a
Google **service account**, downloading its key, sharing your books folder with it, and finding
your Folder ID. You only do this once.

> ## ✅ Why a service account (and why it's safe)
>
> The device authenticates with a **service account** — a robot Google account with its own
> key. Crucially, a service account starts with an **empty** Drive and can only see what you
> **explicitly share with it**. So you share **only your books folder**, and the key on the
> device can read **nothing else** — not your email, photos, or the rest of your Drive. If the
> SD card is ever lost, you just revoke the key in the console. This is both simpler (no phone
> sign-in) and safer than the old approach.

---

## Part 1 — Create the service account & key (≈10 minutes)

You can do this with your normal Google account — there's no whole-Drive exposure risk here.

### 1. Create a Google Cloud project
1. Go to <https://console.cloud.google.com/>.
2. Top bar → project dropdown → **New Project**. Name it anything (e.g. `crosspoint-reader`)
   and create it. Make sure it's selected afterwards.

### 2. Enable the Google Drive API
1. Left menu → **APIs & Services → Library**.
2. Search **Google Drive API**, open it, click **Enable**.

### 3. Create the service account
1. **APIs & Services → Credentials → Create Credentials → Service account**.
2. Give it a name (e.g. `crosspoint-reader`) and click **Create and Continue**.
3. You can skip the optional "grant access" / "grant users" steps — click **Done**.

### 4. Create a JSON key
1. On the **Credentials** page, click your new service account (under "Service Accounts").
2. Open the **Keys** tab → **Add Key → Create new key** → key type **JSON** → **Create**.
3. A `.json` file downloads to your computer. **Keep it safe** — this is the credential you'll
   paste onto the device. (You can create more keys, or delete/revoke this one, anytime.)
4. Open the JSON file in a text editor and note the **`client_email`** value (looks like
   `crosspoint-reader@your-project.iam.gserviceaccount.com`). You'll share the folder with it
   next.

---

## Part 2 — Share your books folder & find its Folder ID

1. In a browser, open <https://drive.google.com/> with the account that holds your books.
2. Create or open the folder that will hold the books you want on the device.
3. **Share it with the service account:** right-click the folder → **Share**, paste the
   service account's **`client_email`** from Part 1 step 4, set it to **Viewer**, and send.
   *(There's no email inbox; sharing just grants the robot account read access.)*
4. Find the **Folder ID** from the URL while inside the folder:

   ```
   https://drive.google.com/drive/folders/1AbCdEfGhIjKlMnOpQrStUvWxYz
                                           └──────────── this part ────────────┘
   ```

   The string after `/folders/` is your **Folder ID**. Copy it.

---

## Part 3 — Configure the device (via a file on the SD card)

Typing long OAuth IDs on the e-ink keyboard is painful, so configuration is done by editing a
small JSON file on the SD card from your PC.

1. On the device, open **Google Drive Sync** — it's in two places: **Home → File Transfer →
   Google Drive Sync** (quicker), or **Settings → System → Google Drive Sync**. The first time,
   it creates the file `/.crosspoint/gdrive.json` on the SD card and shows *"Config file
   created — edit /.crosspoint/gdrive.json on your PC, then run again."* Press **Back**.
2. Power off the device, take out the SD card, and put it in your PC.
3. Open **`/.crosspoint/gdrive.json`** in a text editor. It looks like this:

   ```json
   {
     "_instructions": "Paste your Google service-account JSON ...",
     "folderId": "",
     "syncFolder": "/",
     "serviceAccount": {
       "client_email": "",
       "private_key": "",
       "token_uri": "https://oauth2.googleapis.com/token"
     }
   }
   ```

   Fill it in from the key file you downloaded in Part 1:
   - `folderId` → the Folder ID from Part 2.
   - `serviceAccount.client_email` → copy `client_email` from your downloaded `.json`.
   - `serviceAccount.private_key` → copy `private_key` from your downloaded `.json` **including
     the `-----BEGIN PRIVATE KEY-----...` text and all the `\n`** exactly as it appears (it's
     one long line with `\n` in it — paste it verbatim).
   - `token_uri` → leave as-is.
   - `syncFolder` (optional) → where on the SD card to save books; leave as `"/"` for the root.

   > **Tip:** the easiest way is to open the downloaded key `.json`, copy the whole
   > `client_email` and `private_key` values straight across. Keep the `\n` sequences in the
   > private key — the device converts them to real line breaks.

   Save the file. (Leave `_instructions` as-is; it's ignored.)
4. Put the SD card back in the device and open **Google Drive Sync** again. The device reads
   your file and — for security — **scrambles the private key so it can't be read off the card**
   (it'll look like gibberish afterward; that's expected and only works on this device).
5. The device connects to **WiFi** (pick your network), syncs its clock, and signs in with the
   service-account key. No phone or code needed.
6. The device lists your folder and downloads the books. Done!

**Next time** you just open Google Drive Sync again — it only downloads books that are new or
changed. No editing needed.

---

## Notes & troubleshooting

- **The on-screen error tells you the cause.** Instead of a generic message, the device shows
  the failing step and reason — e.g. *"Authenticate: HTTP 400 [invalid_grant: Invalid JWT…]"*
  (usually a clock or key issue) or *"List folder: request failed — check the Folder ID and that
  the account can see it"*. The same messages are appended to **`/.crosspoint/gdrive_log.txt`**
  on the SD card, so you can read the history on your PC.
- **Books are downloaded as-is.** The device does not shrink or convert images during sync, so
  put already-optimized books in the folder. (You can optimize them with the device's web
  uploader before placing them in Drive.)
- **"Config incomplete":** one of `folderId` / `serviceAccount.client_email` /
  `serviceAccount.private_key` is still empty in the file — edit it on your PC and run again.
- **"Config file invalid":** the JSON is malformed (a missing quote or comma — easy to do when
  pasting the private key). Fix it in the editor (or delete the file and let the device recreate
  the template), then run again.
- **"Authenticate: …invalid_grant…" or signing fails:** usually the **private key** wasn't
  pasted correctly (make sure the whole `-----BEGIN…END PRIVATE KEY-----` value with its `\n`
  is intact), or the **device clock couldn't sync** over NTP (check internet access — a
  captive-portal WiFi will block it).
- **Lists nothing / "could not list folder":** confirm you **shared the folder with the service
  account's `client_email`** (Part 2 step 3), the **Folder ID** is correct, and the books are
  *directly* inside that folder (sub-folders aren't scanned). Only `.epub`, `.txt`, and
  `.xtc`/`.xtch` files are picked up.
- **Reconfiguring / rotating the key:** delete `/.crosspoint/gdrive.json` and run Google Drive
  Sync again — the device recreates the editable template. Revoke a leaked key from the
  Cloud console's service-account **Keys** tab.
