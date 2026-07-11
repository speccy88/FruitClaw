# Fruit Jam USB CDC Console Notes

This note records the USB CDC/NSH console behavior fixed during Fruit Jam
bring-up.

## Enter Key Prompt Pile-up

Observed behavior:

- Pressing Enter repeatedly at an empty NSH prompt printed prompts on the same
  display line, for example `nsh> nsh> nsh>`.
- Commands still executed, but the terminal became visually confusing because
  readline accepted the line ending without echoing a line break.

Root cause:

- `apps/system/readline/readline_common.c` treated CR or LF as end-of-line,
  stored `\n` in the command buffer, and returned to NSH without echoing any
  end-of-line byte.
- USB CDC serial terminals do not all locally echo Enter, so the next NSH
  prompt was printed immediately after the previous prompt.
- Hosts that send CRLF also left the LF queued after readline returned on CR,
  so the next readline call could see the LF as a second empty command.

Fix:

- Echo `\n` when readline accepts CR or LF. The console output path maps this
  to `\r\n` on the Fruit Jam CDC console.
- Remember when a line ended with CR and discard a following LF at the start of
  the next readline call.

Verified image:

```text
/Users/fred/Documents/FruitClaw/nuttx/build-artifacts/fruitjam-readline-enter-lf.uf2
SHA256: 561dab885bc4d7ee81e277d79bf0c63224780c8e4e5872be3f688bae74a46147
```

Verified on `/dev/cu.usbmodem01` after flashing the image above:

- Four empty CR-only Enters produce four separate `nsh>` prompt lines.
- `ls /` after those empty Enters renders on its own command line and lists the
  root directories normally.
- CRLF command input no longer creates an extra empty prompt.
- Left-arrow insertion still works: `echo ac`, left arrow, `b`, Enter prints
  `abc`.
- Up-arrow history still recalls and reruns the previous command.
