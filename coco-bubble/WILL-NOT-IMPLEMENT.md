# Will Not Implement

Disk BASIC features this ROM deliberately leaves out, and why. Each of
these reports `?FC` or is documented as a limitation; none is coming.

| Feature | Reason |
|---|---|
| **BACKUP** | The bubble backs 512 sectors against a floppy's 630. A whole-disk copy would be unidirectional at best and lossy from floppy to bubble. `COPY` moves files in both directions, which is what is actually needed. |
| **DOS** | No OS-9 support for now. |
| **DSKINI track formatting and the skip factor** | Out of scope. Floppies are supported as a convenience for moving files, not as a primary medium; format a floppy under Disk BASIC. `DSKINI n` on a floppy keeps its present meaning - rewrite the allocation table and directory of an already-formatted disk. |
| **Floppy motor on/off timer** | DECB turns the motor off from its 60 Hz interrupt handler. Doing the same here means an IRQ hook that moves SCS* to the floppy controller's slot from interrupt context and back. Not worth it for a convenience device; the motor runs from the first access until reset. |
| **A fourth floppy drive** | DECB's drives 0-3 are four floppies; here drive 0 is the bubble, so drives 1-3 reach physical floppies 0-2 and the fourth drive-select line (DSKREG bit 6) is unreachable. Reaching it would need a drive number 4, which DECB's own syntax rejects (`DRIVE`, filenames and `DSKI$`/`DSKO$` all stop at 3), so it cannot be done without breaking parity. Three floppies. |

Consequences the public documents should state plainly, as limitations
rather than as pending work: `BACKUP` and `DOS` are `?FC`; a new floppy
must be formatted elsewhere; the floppy motor stays on.

Not on this list: direct-access files (`OPEN "D"`, `FIELD`,
`LSET`/`RSET`, `GET#`/`PUT#`, `MKN$`/`CVN`, `FILES`) were considered,
approved and built.
