# DeckLink SDK 16.0, Linux headers

`include/` is the `Linux/include` directory of Blackmagic Design's Desktop
Video SDK 16.0, unmodified: the headers and `DeckLinkAPIDispatch.cpp` that
sensor's DeckLink driver builds against (`VS_WITH_DECKLINK`). The 24.04 image
copies them to `/opt/decklink-sdk-16.0/include` and sets
`DeckLinkSDK_ROOT=/opt/decklink-sdk-16.0`. Why they are kept here: DECISIONS.md,
the 2026-11-01 entry on them.

Each file carries Blackmagic's copyright notice and licence statement, which
must stay with every copy; they are redistributed under that statement and
the SDK's EULA (https://www.blackmagicdesign.com/EULA/DeckLinkSDK). Nothing
here is the core's, so no formatting hook or editor setting rewrites them
(`.pre-commit-config.yaml`, `.editorconfig`).

`SHA256SUMS` lists each file's SHA256 as the SDK ships it, and the image
tests hold every file to it and to its licence statement
(`../../test_check_dockerfiles.py`). To move to a newer SDK, replace
`include/` with its `Linux/include`, regenerate the list from `include/`'s
parent with
`(cd include && LC_ALL=C ls | while read f; do shasum -a 256 "$f" | sed "s|  |  include/|"; done) > SHA256SUMS`,
and rename the directory, the `COPY` and `DeckLinkSDK_ROOT` for its version.
