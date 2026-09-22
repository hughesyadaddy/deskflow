# Mouser heap-class sampling in `fleet-soak` (needs `sudo -n heap`)

`tools/fleet-soak sample --heap-classes` records, per sample, the live
instance counts of the leak fingerprint classes (`CGEvent`,
`CGSEventAppendix`, `HIDEvent`, `NSXPCConnection`, `GPProcessMonitor`,
`CGImage`) by running the Mouser repo tool
`$FLEET_MOUSER_ROOT/tools/mouser-heap-classes --pid <pid>` (default
`~/Desktop/Mouser`). `fleet-soak report --class-slope-max 10` then fails when
any class grows faster than 10 instances/h.

## Why sudo

The tool wraps `heap -s <pid>` (Xcode command-line tools), which must attach
to the Mouser process. The fleet Mouser bundle is signed with the hardened
runtime and **without** `get-task-allow` (that entitlement resets TCC and is
for debug builds only), so `heap` can attach only as root. `fleet-soak`
therefore invokes the tool as `sudo -n <tool> --pid <pid>` and checks
`sudo -n true` once at start-up:

- passwordless sudo available: every sample carries `classes` and
  `heap_total_bytes`;
- not available (or the tool is missing): every sample has `classes: null`,
  and exactly one notice is printed at start-up -- never per sample. The soak
  itself continues (footprint, ports, cpu are unaffected). `report
  --class-slope-max` then fails with "no samples with `classes`", which is
  the intended signal that the gate could not be evaluated.

The tool's own exit 2 (heap missing, process gone, unexpected `heap` table)
is likewise a null sample, not a soak failure.

## Seat setup (root step, once per Mac)

Allow the soak user to run `heap` without a password. `visudo -f` into a
drop-in so it survives upgrades:

    sudo visudo -f /etc/sudoers.d/fleet-soak-heap

with the single line (replace the user):

    alexhughes ALL=(root) NOPASSWD: /usr/bin/heap

`tools/mouser-heap-classes` runs `heap` by absolute path; `sudo -n <tool>`
needs the tool itself allowed too when it is not a symlink into `/usr/bin`,
so the practical line for the fleet is:

    alexhughes ALL=(root) NOPASSWD: /usr/bin/heap, /Users/alexhughes/Desktop/Mouser/tools/mouser-heap-classes

Verify: `sudo -n true && sudo -n ~/Desktop/Mouser/tools/mouser-heap-classes --name Mouser`.

## Reading the result

    tools/fleet-soak report --in soak.jsonl --proc mouser --window 24 \
        --slope-max 0.1 --cap 200 --class-slope-max 10

The text report prints `classes/h: CGEvent=... NSXPCConnection=...`; the JSON
report carries `class_slopes_h` and `class_samples`.
