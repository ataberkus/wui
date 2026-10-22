# Third-party notices

Prepared source checkouts retain their complete upstream license files. The
selected FreeType alternative is the FreeType License (FTL); its checkout also
retains the alternative GPL-2.0-only notice. HarfBuzz, utf8proc, and doctest
retain their upstream MIT notices in their respective checkouts.

The distributed Inter Regular and SemiBold assets retain the exact `LICENSE.txt`
from the Inter 4.1 release archive in `assets/fonts/LICENSE.txt`.

The distributable notice file is `THIRD_PARTY_NOTICES.txt` at the repository
root. `tools/collect_notices.py --write` generates it from wui's `LICENSE`,
these checkouts' license files and the Inter license; CTest `wui-notices`
fails when it is stale. doctest is test-only and is not listed there.
