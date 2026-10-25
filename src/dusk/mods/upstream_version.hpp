#pragma once

// aleks2 (1.0.2): the upstream Dusklight version this tree's mod ABI matches, reported to mods as
// svc_host->version. Mods gate on upstream's numbering (Twilight HD HUD 2.7.x needs "1.4.1.402 or newer"); the
// port's own 1.0.x would read as older than any upstream build. The port version stays in the updater, the UI,
// the NACP and the logs.
//
// Keep it in step with the layout reference: abi_parity.py runs against an upstream checkout at v2.0.3 + 2
// (34a144a), and provenance-residue.py's UPSTREAM_REF names the same tag. Moving the base means changing all three.
#define DUSK_UPSTREAM_VERSION "2.0.3"
