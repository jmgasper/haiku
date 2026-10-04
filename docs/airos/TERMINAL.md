# Links in Terminal

air/OS's Terminal opens web links the way other terminals do, and lets
command line programs that sign in through a browser (Claude Code's `claude`,
for one) open that browser.

## Opening a link

Hold **Alt** and click a link to open it in the default browser, or Alt and
right-click it for *Open link* and *Copy link location*. Holding Alt over a
link underlines it and shows the link pointer. (The left Alt key is the
Command key of a PC keyboard in Haiku's default key layout; with the Keymap
preference's "Windows/Linux" shortcut layout it is Ctrl instead.) Haiku's Terminal always
had this for single-line links. Three things kept it from working on the
links that matter:

- **Links that a program broke into lines itself.** A program that lays out
  its own screen breaks a long address at the right margin and starts a new
  line, rather than letting the terminal wrap it. Claude Code prints its
  sign-in address this way, so Alt+click opened the first line of it, a
  truncated address, and the other lines were not links at all. An address
  that runs to the right margin of a full line now continues on the next line
  if that starts with address characters, in either direction from the line
  clicked (`HyperLinkState::_ExtendURLOverLineBreaks()`).
- **OSC 8 hyperlinks longer than 512 bytes.** Operating system commands were
  collected in a 512-byte buffer and dropped when they did not fit, and the
  rest of the command was then printed as text: a long address showed up as
  garbage before the link text. Commands are now collected up to 1 MiB and one
  that is longer is read to its end and dropped (`TermParse::
  _ReadOperatingSystemControl()`). An escape sequence that cuts a command
  short is carried out instead of being swallowed, and CAN and SUB abandon one.
  The `id=` parameter of a hyperlink was copied six characters too long.
- **No highlight for OSC 8 links.** The pointer changed, but the link range
  was left at (0, 0). The range is now every cell around the pointer that
  carries the same link, continuing over line ends that are filled to the
  margin, which is how a program repeats a link it has wrapped.

## Programs that open a browser

Terminal now starts the shell with:

| Variable | Value | Why |
| --- | --- | --- |
| `BROWSER` | `/bin/open` | Programs that open a web page run `$BROWSER <address>`, and fall back to `xdg-open`, which Haiku does not have. `open` hands the address to the preferred application for its scheme (Summit for http and https). |
| `FORCE_HYPERLINK` | `1` | Node.js programs using the common `supports-hyperlinks` check only print OSC 8 hyperlinks for terminals they know by name. Terminal understands them, so it says so. |

Neither is changed when it is already set, so a profile can override them.
`FORCE_HYPERLINK` also applies when such a program's output goes to a pipe or
a file; set it to `0` in the profile if that is unwanted.

## Copying from a program (OSC 52)

Terminal now takes `ESC ] 52 ; <selection> ; <base64 text> BEL` (or ST) and
puts the text on the system clipboard, as xterm, kitty, iTerm2 and Windows
Terminal do. Claude Code's "c to copy" on its sign-in screen uses it. A
program asking for the clipboard's contents (`?` instead of the text) is not
answered: what is on the clipboard is not a program's business.

## Claude Code's sign-in

With the above, `claude` (Claude Code 2.1.112 on Node 26, see `nvm`) signs in
from Terminal as it does elsewhere:

1. It runs `$BROWSER` with the sign-in address, which redirects back to a
   server it runs on `localhost`, so the browser opens and finishes the sign-in
   by itself.
2. If that is not wanted, the address it prints is one OSC 8 link on every
   line: Alt+click any of them, or press `c` to copy the whole address.

## Tested

On the X399 workstation (2026-10-04), with the Terminal built from this
branch, over VNC (which cannot hold a modifier key itself; the left Command
key was held by sending `B_MODIFIERS_CHANGED` through the VNC server's input
injector):

- A 456-character address printed in 79-column lines each ended with `\r\n`:
  Alt+right-click on its fourth line, *Copy link location*, gave the whole
  address, byte for byte; Alt+click on its first line opened the whole address
  in Summit (the window title showed it to the end).
- The same address as an OSC 8 link repeated on each line: the whole address.
- Claude Code: `$BROWSER` was run with the `localhost` sign-in address; `c`
  put the whole manual sign-in address on the clipboard; *Copy link location*
  on its third line gave the whole address (an OSC 8 command of about 600
  bytes).
- The link ranges Terminal highlights were logged from a debug build: rows 2
  to 7 ending at column 61 for the six-line address, exactly its extent. The
  highlight itself did not show in VNC screenshots, for the stock Terminal
  either.
