# Third-party notices

## baken

The Pioneer library writer (`core/PioneerPdb.*`, `core/PioneerAnlz.*`,
`core/PioneerSettings.*`) is ported from baken,
https://github.com/M-Igashi/baken (`crates/baken-export`), which is
distributed under the MIT License:

```
MIT License

Copyright (c) 2025 Jesse Pinkman (JPHFA)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Format descriptions used as reference

- Deep Symmetry's crate-digger documentation of `export.pdb` and the ANLZ
  analysis files (https://github.com/Deep-Symmetry/crate-digger), used as a
  specification only.
- The rekordbox hot cue colour table of Deep Symmetry's beat-link
  (https://github.com/Deep-Symmetry/beat-link), colour values only.
- The default *SETTING.DAT values are the ones rekordbox itself writes into an
  empty export (as found in rekordcrate's test data,
  https://github.com/Holzhaus/rekordcrate); rekordcrate is also used, outside
  this program, by `tests/validate-with-rekordcrate.sh` to read back what the
  writer produces.

## serato-tags (format documentation)

core/Serato.cpp writes Serato's tag and crate formats as documented by
serato-tags, https://github.com/Holzhaus/serato-tags (no code copied).

Copyright 2019 Jan Holthuis. MIT License.
