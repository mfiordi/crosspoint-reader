# Google Drive Sync — Setup Guide

This guide walks you through everything you need to use **Settings → System → Google Drive
Sync**: creating the OAuth credentials (Client ID + Client Secret) and finding your Folder ID.
You only do this once.

> ## ⚠️ Read this first — security warning
>
> Google Drive has **no way to grant access to a single folder**. The credentials you create
> here give the device **read access to the *entire* Google Drive of whatever account you sign
> in with** — not just the books folder. The device's code only ever looks at your chosen
> folder, but the stored login token *could* read the whole account if someone pulled it off
> the SD card.
>
> **Strong recommendation: create a brand-new, separate Google account just for your books**,
> and use it for all the steps below. Put (or share) only your books folder in that account.
> That way, even in the worst case, a leaked token exposes nothing but books — never your
> personal Drive, email, or photos.

---

## Part 1 — Create the OAuth credentials (≈10 minutes)

Do all of this **while signed in to your books-only Google account** (see the warning above).

### 1. Create a Google Cloud project
1. Go to <https://console.cloud.google.com/>.
2. Top bar → project dropdown → **New Project**. Name it anything (e.g. `crosspoint-reader`)
   and create it. Make sure it's selected afterwards.

### 2. Enable the Google Drive API
1. Left menu → **APIs & Services → Library**.
2. Search **Google Drive API**, open it, click **Enable**.

### 3. Configure the consent screen
1. **APIs & Services → OAuth consent screen**.
2. User type: **External** → **Create**.
3. Fill only the required fields (App name, your email for support + developer contact). You
   can leave everything optional blank. **Save and Continue.**
4. **Scopes:** you can skip adding scopes here (the device requests the read-only scope
   itself). **Save and Continue.**
5. **Test users:** click **Add Users** and add your books account's email. **Save and
   Continue.** (Leaving the app in "Testing" mode is fine for personal use — no Google review
   needed.)

### 4. Create the OAuth client (the Client ID + Secret)
1. **APIs & Services → Credentials → Create Credentials → OAuth client ID**.
2. **Application type: `TV and Limited Input devices`.** *(This type is required — it's what
   enables the on-device code flow. Don't pick "Web" or "Desktop".)*
3. Give it a name and click **Create**.
4. A dialog shows your **Client ID** and **Client Secret**. Copy both somewhere safe — you'll
   type them into the device. (You can re-open them anytime from the Credentials page.)

---

## Part 2 — Find your Folder ID

1. In a browser, open <https://drive.google.com/> (signed in as your books account).
2. Create or open the folder that will hold the books you want on the device.
3. Look at the URL while inside the folder:

   ```
   https://drive.google.com/drive/folders/1AbCdEfGhIjKlMnOpQrStUvWxYz
                                           └──────────── this part ────────────┘
   ```

   The string after `/folders/` is your **Folder ID**. Copy it.

> If you're using a separate books account but your books currently live in your main account,
> either move/copy the folder into the books account, or **share** the folder *to* the books
> account (right-click → Share), then grab the Folder ID while signed in as the books account.

---

## Part 3 — Enter everything on the device

1. On the device: **Settings → System → Google Drive Sync**.
2. The first time, it asks for three things via the on-screen keyboard — enter them in order:
   - **Drive Client ID**
   - **Drive Client Secret**
   - **Drive Folder ID**
   These are saved to the SD card, so you only type them once.
3. The device connects to **WiFi** (pick your network).
4. **Authorize:** the screen shows a short **code**, a **URL**, and a **QR code**. On your
   phone, open the URL (or scan the QR), sign in **as your books account**, enter the code, and
   approve read-only access.
5. The device lists your folder and downloads the books. Done!

**Next time** you just open Google Drive Sync again — it remembers your login and only
downloads books that are new or changed.

---

## Notes & troubleshooting

- **Books are downloaded as-is.** The device does not shrink or convert images during sync, so
  put already-optimized books in the folder. (You can optimize them with the device's web
  uploader before placing them in Drive.)
- **"Access denied" / sign-in fails:** make sure the Google account you authorize with is
  listed as a **Test user** on the consent screen (Part 1, step 3.5), and that you signed in as
  that same account on your phone.
- **Nothing downloads:** confirm the Folder ID is correct and that the books are *directly*
  inside that folder (sub-folders aren't scanned). Only `.epub`, `.txt`, and `.xtc`/`.xtch`
  files are picked up.
- **Re-typing credentials:** they're stored on the SD card. Deleting `/.crosspoint/gdrive.json`
  resets the configuration.
- **Changing accounts:** if you ever need to re-authorize, delete `/.crosspoint/gdrive.json`
  and run Google Drive Sync again.
