# Third-party notices

MeshCore itself is released under the MIT License (see `license.txt`). It also
carries third-party material that has its own licence terms. This file records
the vendored components that do not otherwise announce themselves — so that
their origin and terms are discoverable without reading the source — together
with the notices their licences ask to be retained. Those terms apply to the
identified component only, not to MeshCore as a whole.

This is not a complete dependency audit. Libraries resolved by PlatformIO at
build time are not listed here, and neither are the vendored trees that already
ship their own licence file (for example `lib/ed25519`,
`arch/esp32/AsyncElegantOTA`, `arch/stm32/Adafruit_LittleFS_stm32/src/littlefs`)
or their own file headers.

## Semtech LR2021 patch RAM image

File: `src/helpers/radiolib/LR2021Pram.h`

560 words — 2240 bytes — of firmware patch RAM for the Semtech LR2021 radio,
taken verbatim from Semtech's `lr20xx_driver` v2.0.2 as distributed in
[Lora-net/usp](https://github.com/Lora-net/usp) at tag `v1.1.2-feature-202604`,
path `smtc_rac_lib/radio_drivers/lr20xx_driver/inc/lr20xx_pram_lr2021.h`. Only
the formatting differs; the values are upstream's.

The image serialised as 560 little-endian 32-bit words — 2240 bytes, no header,
no padding, no separators — has sha256:

```
a828f5bac9f1309bc02b06f0885d5061ebd91510f40ade951b34c4b23d6edc3f
```

Upstream ships that array with no per-file copyright notice of any kind. The
notice reproduced below is the one carried — verbatim, year included — by the
neighbouring files of the same driver release, among them `lr20xx_pram_load.h`,
the loader for this exact image. Semtech's own years are inconsistent upstream
(the repository's top-level `LICENSE.txt` says 2025, other headers in the same
driver say 2022); the adjacent loader is used here as the closest match.

On patents: unlike BSD-3-Clause, the Clear BSD License expressly disclaims any
implied patent licence. Neither licence grants an express one — Clear BSD
differs only in saying so.

The notice is recorded here for source-level provenance. Clause 2 below
additionally asks that it be reproduced in the materials accompanying a binary
redistribution, so anyone publishing a MeshCore binary built with this image
should carry the notice alongside it.

```
The Clear BSD License
Copyright Semtech Corporation 2026. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted (subject to the limitations in the disclaimer
below) provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Semtech corporation nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY
THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT
NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SEMTECH CORPORATION BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```
