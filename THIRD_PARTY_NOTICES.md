# Third-party notices

`monero-solo-stratum` itself is distributed under the MIT License in
`LICENSE`. The source archive contains the following third-party works. Their
upstream license files remain alongside their source and control if this
summary differs from them.

## monero-stratum-pow-verifier

- Project: `SeriousPassenger/monero-stratum-pow-verifier`
- Revision: `856c015de433a23fe45d88a18dc08c821e50f1cb`
- Package version: 0.1.0
- License: MIT
- Included license: `third_party/monero-stratum-pow-verifier/LICENSE`

Copyright (c) 2026 SeriousPassenger, <seriouspassenger@proton.me>

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

## RandomX

- Project: `tevador/RandomX`
- Revision: `6c4340ba4561aec9a3611c1aedf9931239777fb3` (v1.2.2)
- License: BSD 3-Clause
- Included license: `third_party/monero-stratum-pow-verifier/third_party/RandomX/LICENSE`

Copyright (c) 2018-2019, tevador <tevador@gmail.com>

Copyright (c) 2014-2019, The Monero Project

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

RandomX also contains build-required code derived from the Argon2 reference
source package, including its Blake2 implementation. The corresponding files
under `src/argon2*`, `src/blake2/`, and the related cache initialization code
carry this notice:

- Original package: `P-H-C/phc-winner-argon2`
- Dedication: CC0
- Copyright: 2015 Daniel Dinu, Dmitry Khovratovich, Jean-Philippe Aumasson,
  and Samuel Neves

The Windows build support includes `vcxproj/h2inc.ps1`, distributed under the
MIT License with copyright held by the .NET Foundation and Contributors. Its
complete MIT notice is retained at the top of that file.

## JSON for Modern C++

- Project: `nlohmann/json`
- Revision: `9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03` (v3.11.3)
- License: MIT
- Included license: `third_party/nlohmann-json/LICENSE.MIT`

Copyright (c) 2013-2022 Niels Lohmann

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

Only the upstream MIT library headers and build metadata required by this
server are included. Upstream tests, fuzz fixtures, and tools are not part of
the release archive.

The included headers also incorporate the following attributed components, as
recorded by the upstream README and source headers:

- UTF-8 decoder by Björn Hoehrmann, copyright 2008-2009, MIT License;
- modified Grisu2 algorithm by Florian Loitsch, copyright 2009, MIT License;
- Hedley by Evan Nemerson, CC0-1.0; and
- portions of Google Abseil, copyright 2018 The Abseil Authors, Apache License
  2.0.

The nlohmann/json release subset therefore includes the complete Apache 2.0
text at `third_party/nlohmann-json/LICENSES/Apache-2.0.txt` in addition to
`LICENSE.MIT`. Installed packages carry these as
`nlohmann-json-Apache-2.0.txt` and `nlohmann-json-LICENSE.MIT` in the project
documentation directory. The MIT terms printed above cover the upstream MIT
components; the upstream README retains their individual attributions.

## SQLite and sqlite-amalgamation wrapper

- Wrapper project: `azadkuh/sqlite-amalgamation`
- Wrapper revision: `15d0ff10ebc7e7225eced1de84bb52137000899b`
- Included SQLite: 3.38.2, source ID
  `d33c709cc0af66bc5b6dc6216eba9f1f0b40960b9ae83694c986fbf4c1d6f08f`
- Wrapper license: BSD 3-Clause
- SQLite core status: public domain
- Included wrapper license: `third_party/sqlite-amalgamation/LICENSE`

The SQLite amalgamation contains SQLite's public-domain dedication. The
wrapper repository's BSD notice is retained verbatim in its included `LICENSE`.
Installed packages carry that notice as `sqlite-amalgamation-LICENSE` in the
project documentation directory.

## Reference-only provenance

Monero v0.18.5.1 at
`4f92268d7c16741cfb41e5bbe2aa46cc260a9ea5` is the BSD-licensed consensus
reference for independently implemented address, CryptoNote serialization,
block-hash, and difficulty behavior. It is not vendored or linked.

`SeriousPassenger/xmrig-proxy` branch `improvised-daemon-mining` at
`fe6977291b5bea14e88579e867987e759c96d584` is GPLv3 and was consulted only as
a behavioral/test-oracle reference. No source from that project is included,
linked, mechanically translated, or relicensed in this MIT tree.
