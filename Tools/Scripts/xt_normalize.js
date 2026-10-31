#!/usr/bin/env node
// Blanks, in place, what two builds of the same tree never agree on, so their outputs can be compared byte
// for byte (Docs/design/52-web-wasm.md §52.5, 15 §15.10): the compile time of an .xtir's META chunk with that
// chunk's CRC (exactly what bundle::irFileHash() leaves out) and a bundle manifest's `builtAt`. Shared by
// Tools/Scripts/check_wasm.sh (native vs wasm) and Tools/Scripts/check_compilers.sh (GCC vs Clang).
// Usage: node Tools/Scripts/xt_normalize.js <file or dir>...
// Not a C++ file: never run clang-format on it.
'use strict';
const fs = require('fs');
const path = require('path');
function xtir(file) {
  const b = fs.readFileSync(file);
  if (b.length < 16 || b.toString('latin1', 0, 4) !== 'XTIR') return;
  const count = b.readUInt32LE(8);
  let o = 16;
  for (let i = 0; i < count && o + 12 <= b.length; ++i) {
    const type = b.toString('latin1', o, o + 4);
    const length = b.readUInt32LE(o + 4);
    if (type === 'META' && length >= 16 && o + 12 + 16 <= b.length) {
      b.fill(0, o + 8, o + 12);        // the chunk's payload CRC
      b.fill(0, o + 12 + 8, o + 12 + 16);  // IrMeta::compileTimeUnix
    }
    o += 12 + length;
  }
  fs.writeFileSync(file, b);
}
function manifest(file) {
  const text = fs.readFileSync(file, 'utf8');
  fs.writeFileSync(file, text.replace(/("builtAt":\s*)"[^"]*"/g, '$1"<builtAt>"'));
}
function walk(p) {
  if (!fs.existsSync(p)) return;
  if (fs.statSync(p).isDirectory()) {
    for (const e of fs.readdirSync(p)) walk(path.join(p, e));
  } else if (p.endsWith('.xtir')) {
    xtir(p);
  } else if (path.basename(p) === 'manifest.json') {
    manifest(p);
  }
}
process.argv.slice(2).forEach(walk);
