// Not the U8g2 library. Arduino_GFX switches its U8g2-font decoder on with
// `#if __has_include(<U8g2lib.h>)` and then uses nothing from it -- the
// decoder is its own. This empty header turns the switch without pulling in
// a 39MB font source for the eleven Helvetica arrays in helv.h.
// ponytail: if Arduino_GFX ever does call into U8g2, add olikraus/U8g2 to
// lib_deps and delete this file.
#pragma once
