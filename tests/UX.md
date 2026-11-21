# Shell and connection workflow verification

CloudpilotEmu ran a Tungsten E2 and Palm TX at 100 MIPS against a disposable local OpenSSH server. The E2 tests covered a clean install and an upgrade from a saved session with an encrypted Ed25519 key. The TX tests used a copy of an existing password profile.

| Flow | Observed result |
| --- | --- |
| E2 first connection | **Set up now** opened the seed importer. The trust dialog showed the host, port, key type, and complete fingerprint. Copying it and pasting it into a shell draft preserved the `SHA256:` prefix. |
| E2 clipboard | Copying the terminal viewport and using **Paste into line** put the output in the input field. The shell prompt stayed at the same command before any explicit send. |
| E2 and TX scrollback | **History** showed earlier lines and changed the toolbar to **Older**, **Newer**, and **Back to live**. Returning to live restored the prompt and the normal toolbar. On the E2, **Follow output** also returned to live when a delayed command produced output. |
| E2 and TX session menu | Fullscreen showed the exit hint on first use and removed it automatically. A tap restored the toolbar. The TX restored the closed input area in landscape. Disconnect asked for confirmation and the E2 remained connected after Cancel. |
| E2 appearance and reconnect | Dark-on-light and amber presets updated the preview and the open shell. On relaunch the last successful connection was already filled in. The imported-key profile returned to the passphrase prompt directly from **Connect**. |
| TX settings and input area | Opening Appearance and Behavior, returning to Appearance, and saving back to the shell preserved the closed input area in landscape. |
| TX host-key replacement | The client blocked a connection to a replacement key before user authentication. **Saved key** showed the earlier fingerprint. After explicitly forgetting that pin, the next connection showed the replacement ECDSA P-256 fingerprint and required **Trust** before accepting the password. |

On the TX with the regular font, `stty size` reported 28×53 in portrait with the input area open, 28×74 in landscape with it closed, and 32×80 in landscape fullscreen. The toolbar and settings were exercised with emulator taps. Physical stylus use remains unverified.
