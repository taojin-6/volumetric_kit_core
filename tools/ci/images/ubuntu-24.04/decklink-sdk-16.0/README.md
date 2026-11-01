# DeckLink SDK 16.0, Linux headers

`include/` is the `Linux/include` directory of Blackmagic Design's Desktop
Video SDK 16.0, unmodified: the headers and `DeckLinkAPIDispatch.cpp` that
sensor's DeckLink driver builds against (`VS_WITH_DECKLINK`). The 24.04 image
copies them to `/opt/decklink-sdk-16.0/include` and sets
`DeckLinkSDK_ROOT=/opt/decklink-sdk-16.0` (`../Dockerfile`). They are kept
here because the SDK's download sits behind a form, so no job can fetch it.

Each file carries Blackmagic's copyright notice and licence statement, which
must stay with every copy; they are redistributed under that statement and
the SDK's EULA (https://www.blackmagicdesign.com/EULA/DeckLinkSDK). Nothing
here is the core's, and the repository's formatting hooks leave the directory
alone (`.pre-commit-config.yaml`).

`SHA256SUMS` lists each file's SHA256 as the SDK ships it; the image tests
check the files against it (`../../test_check_dockerfiles.py`). To move to a
newer SDK, replace `include/` with its `Linux/include`, regenerate the list
from `include/`'s parent with
`(cd include && LC_ALL=C ls | while read f; do shasum -a 256 "$f" | sed "s|  |  include/|"; done) > SHA256SUMS`,
and rename the directory, the `COPY` and `DeckLinkSDK_ROOT` for its version.
No image has Desktop Video, the library the SDK loads at run time: the
driver's SDK smoke test checks that refusal there.
