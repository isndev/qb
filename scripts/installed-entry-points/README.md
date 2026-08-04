# Entry-point TUs for `check-installed-headers.sh` (phase 2)

One file per **public entry point**. Each is a complete program, includes **exactly one**
qb header, and ODR-uses that header's headline API so the result is a **link**, not a parse.

The rule that makes these worth having: *one header per file*. The gate that existed before
these put `<qb/actor.h>` and `<qb/main.h>` in one TU, and that single combination is what hid
both `qb::Actor::push<E>` being emitted by no TU reachable from `<qb/main.h>` and
`forget_frame_if_current` being declared non-`inline`. Adding a second `#include` to any file
here is how this gate stops working.

Adding an entry point: copy the closest file, change the one `#include`, and make `main()`
ODR-use something the archive actually has to supply. A TU that only *declares* things links
against an empty archive and proves nothing.
