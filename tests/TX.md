# Palm TX verification

CloudpilotEmu ran a Palm TX at 100 MIPS with a supplied TX session, a disposable SD card, and native NetLib redirection to an isolated OpenSSH server. The same PRC also ran on the Tungsten E2.

One live SSH session with the regular 6×10 font reported the following dimensions via `stty size`.

| Display mode | Rows × columns |
| --- | ---: |
| Portrait, input area open | 29×53 |
| Portrait, input area closed | 41×53 |
| Landscape, input area closed | 29×74 |
| Landscape, input area open | 29×53 |
| Portrait, fullscreen | 48×53 |

Leaving portrait fullscreen restored the open input area and status bar. Rotating back to portrait with the input area open returned the session to 29×53.

With the small 5×8 font, landscape reported 36×89 with the input area closed and 40×96 in fullscreen. The fullscreen view rendered 270 numbered lines and a marker on its bottom row near the right edge. After leaving fullscreen, the scroll controls showed earlier output and the DIA remained closed.

Running `exit` from landscape fullscreen restored the status bar and returned to the connection form. After dismissing the closure message, the input area was still closed and could be reopened from the status bar.

## Reproduce

```sh
cp-uarm --device tx --net-native --mips 100 --sd test-card.img tx-test.session
```

Install the PRC into a copy of an initialized TX session and connect to a local SSH fixture as described in the README. Run `stty size`, tap the system input-area arrow or rotation icon, and run `stty size` again in the same shell. Tap **Full** to hide the toolbar and system chrome, then tap the shell to return.

Verification used an emulator. Physical Palm TX hardware was unavailable.
