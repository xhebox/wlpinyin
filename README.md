# wlpinyin

[WIP] experimental minimal wayland IME

No gui. Candidate are showed in text area.  Cuz i really can not find a working native Chinese IME, so i made this.

Now everything is hardcoded, but i except it can be configured like dwm. It is overkill to add IPC or dynamic config in such a project, though, i am writing it with modularity in my mind.

It will not be too hard to add new input backend engine, enable it by pkgconf check and ifdef at compilation time.

**Since libpinyin has very little document, the libpinyin engine currently can not remember user phrases and correctly train the database.**
