# Third-Party Licenses

This document lists all third-party dependencies used by **occt-kernel-wasm**, their
versions, and their applicable licenses.

---

## Open CASCADE Technology (OCCT)

- **Website:** https://dev.opencascade.org/
- **Repository:** https://git.dev.opencascade.org/repos/occt.git
- **Version:** 7.8.x (or as configured in `scripts/build-occt.sh`)
- **License:** GNU Lesser General Public License v2.1 with OCCT Exception

### OCCT License Summary

OCCT is distributed under the LGPL-2.1 with the following exception (the "OCCT Exception"):

> Linking this library statically or dynamically with other modules is making a
> combined work based on this library. Thus, the terms and conditions of the GNU
> Lesser General Public License cover the whole combination.
>
> As a special exception, the copyright holders of this library give you permission
> to link this library with independent modules to produce an executable, regardless
> of the license terms of these independent modules, and to copy and distribute the
> resulting executable under terms of your choice, provided that you also meet, for
> each linked independent module, the terms and conditions of the license of that
> module. An independent module is a module which is not derived from or based on
> this library. If you modify this library, you may extend this exception to your
> version of the library, but you are not obligated to do so. If you do not wish to
> do so, delete this exception statement from your version.

This exception is equivalent to the "Classpath Exception" used by the GNU Classpath
project and permits linking OCCT into non-GPL (including proprietary) applications
without those applications being considered derivative works of OCCT.

### Obligations

If you redistribute a binary that incorporates OCCT (including via WebAssembly), you must:

1. Provide the OCCT source code or a written offer to provide it on request.
2. Preserve the copyright notices in all copies.
3. Include a copy of the LGPL-2.1 license text.

The full text of LGPL-2.1 is available at:
https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html

---

## Emscripten

- **Website:** https://emscripten.org/
- **Repository:** https://github.com/emscripten-core/emscripten
- **License:** MIT (toolchain) / University of Illinois/NCSA Open Source License (LLVM)

Emscripten is the compiler toolchain used to compile C++ to WebAssembly. The runtime
portions embedded in the output `.js` file are covered by the MIT license.

---

## Node.js (runtime only, not distributed)

- **Website:** https://nodejs.org/
- **License:** MIT

---

## TypeScript (dev dependency)

- **Website:** https://www.typescriptlang.org/
- **License:** Apache-2.0

---

## Jest (dev dependency – testing only)

- **Website:** https://jestjs.io/
- **License:** MIT

---

## ts-jest (dev dependency – testing only)

- **Website:** https://kulshekhar.github.io/ts-jest/
- **License:** MIT

---

*For the complete, machine-readable list of runtime npm dependencies, run `npm ls --all` after `npm install`.*
