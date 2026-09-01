# Third-Party Notices

## SKSE Menu Framework 3

SFSE Menu Framework directly adapts implementation behavior and assets from
[SKSE Menu Framework 3 by SkyrimThiago at commit
`928e01ab459822a8d233ab99f0419ea1de23c775`](https://github.com/QTR-Modding/SKSE-Menu-Framework-3/tree/928e01ab459822a8d233ab99f0419ea1de23c775).

The theme loader behavior and `classic.json`, `modern.json`, and
`skyrimDefault.json`, together with the font discovery, fallback, and live
rebuild request sequence, are used under GNU GPL version 3. The complete
license is included in
[COPYING](COPYING).

These SKSE Menu Framework-derived portions remain GPL-3.0-only. The separate
exceptions in this repository do not relicense those portions.

## Dear ImGui 1.90.8

SFSE Menu Framework vendors Dear ImGui at commit
`6f7b5d0ee2fe9948ab871a530888a6dc5c960700` and adapts its DirectX 12 font
texture creation/upload sequence for checked, per-generation live replacement.
Its FreeType builder is compiled through a narrow project bridge that selects
an OpenType variable-weight coordinate without changing public ImGui layouts.

The MIT License (MIT)

Copyright (c) 2014-2024 Omar Cornut

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

## Toggle Dialogue Camera SF

The verified vtable-hook installation and guarded rollback pattern is adapted
from [Toggle Dialogue Camera SF by Quantumyilmaz at commit
`8021fa934591aac1c71266cc4abc5cb1c24e28d7`](https://github.com/QTR-Modding/ToggleDialogueCameraSF/tree/8021fa934591aac1c71266cc4abc5cb1c24e28d7).

That source is GPL-3.0-or-later with its Modding Exception and GPL-3.0 Linking
Exception. Its license and exception files are byte-identical to the
[COPYING](COPYING) and [EXCEPTIONS](EXCEPTIONS) files shipped here.

## OSF UI

The Starfield cursor, control-layer, simulation-pause, and native-main-thread
queue ownership-transfer protocols are adapted from
[OSF UI by ozooma10 at commit
`14b7565bbc7689b07fdccdb74525b9505f9f0dd6`](https://github.com/ozooma10/osf-ui/tree/14b7565bbc7689b07fdccdb74525b9505f9f0dd6).

OSF UI is GPL-3.0 with the following Modding Exception and GPL-3.0 Linking
Exception. The complete GPL version 3 text is included in [COPYING](COPYING).
The exception text is reproduced verbatim:

    This Program is intended to be used with and to modify existing code
    (the "Modded Code"). The purpose of this exception is to address issues
    when an open source modding community interacts with potentially
    proprietary code. In addition, the modding community often uses libraries
    (the "Modding Libraries") under licenses that may be incompatible with
    the GPL ("Modding Library Licenses").

                                Modding Exception

    As an exception, the author gives You the additional right to link the
    code of this Program with the existing code that this Program is
    intended to be used with or modify and to distribute linked combinations
    including the two, subject to the limitations in this paragraph. Modded
    Code permitted under this exception may link to the code of this Program
    without causing the Modded Code and portion of the combined work
    corresponding to the Modded Code to be covered by the GNU General Public
    License. You must obey the GNU General Public License in all respects for
    all of the Program code and other code used in conjunction with the
    Program except the Modded Code covered by this exception. If you modify
    this file, you may extend this exception to your version of the file, but
    you are not obligated to do so. If you do not wish to provide this
    exception without modification, you must delete this exception statement
    from your version and license this file solely under the GPL without
    exception.

              GPL-3.0 Linking Exception (with Corresponding Source)

             Additional permission under GNU GPL version 3 section 7

    If you modify this Program, or any covered work, by linking or combining
    it with Modding Libraries (or a modified version thereof), containing
    parts covered by the terms of Modding Library Licenses, the licensors of
    this Program grant you additional permission to convey the resulting work.
    Corresponding Source for a non-source form of such a combination shall
    include the source code for the parts of Modding Libraries used as well as
    that of the covered work.

## CommonLibSF and commonlib-shared

SFSE Menu Framework links the
[QTR CommonLibSF fork at commit
`7b71b944285771c1478faa5568bf2a3fe1b70471`](https://github.com/QTR-Modding/commonlibsf/tree/7b71b944285771c1478faa5568bf2a3fe1b70471),
including fork modifications by Quantumyilmaz, and its pinned
[commonlib-shared dependency at commit
`5470284e964d5510aa001dca3e0bb5548b6356a4`](https://github.com/libxse/commonlib-shared/tree/5470284e964d5510aa001dca3e0bb5548b6356a4).

CommonLibSF is GPL-3.0-or-later with the Modding Exception and GPL-3.0 Linking
Exception; its license and exception files are byte-identical to
[COPYING](COPYING) and [EXCEPTIONS](EXCEPTIONS). commonlib-shared is GPL-3.0
with the same exception text reproduced in the OSF UI section above. A
distributed binary must be accompanied by the corresponding source required
by those licenses.

Portions inherited from CommonLibSSE and CommonLibSSE-NG remain available
under the following MIT license:

MIT License

Copyright (c) 2018 Ryan-rsm-McKenzie

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

## spdlog 1.16.0

SFSE Menu Framework statically links
[spdlog 1.16.0 at commit
`486b55554f11c9cccc913e11a87085b2a91f706f`](https://github.com/gabime/spdlog/tree/486b55554f11c9cccc913e11a87085b2a91f706f).

The MIT License (MIT)

Copyright (c) 2016 Gabi Melman.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

## Jost 3.7

The unmodified `Jost-500-Medium.ttf` and `Jost-400-Book.ttf` are
redistributed from
[indestructible-type/Jost at commit
`35f141c970538f1ed0f235789c19156b3ce2a762`](https://github.com/indestructible-type/Jost/tree/35f141c970538f1ed0f235789c19156b3ce2a762).

Copyright 2020 The Jost Project Authors (https://github.com/indestructible-type/Jost)

Jost is licensed under the SIL Open Font License, Version 1.1. The complete
license is installed alongside the font as
`Data/SFSE/Plugins/Fonts/Jost-OFL.txt` and retained in
[`public/SFSE/Plugins/Fonts/Jost-OFL.txt`](public/SFSE/Plugins/Fonts/Jost-OFL.txt).

## Space Grotesk 2.0.0

The unmodified `SpaceGrotesk-Medium.ttf` and
`SpaceGrotesk[wght].ttf` are redistributed from
[floriankarsten/space-grotesk release 2.0.0 at commit
`7220f5d04813fe83babe76d4fd23e02275021280`](https://github.com/floriankarsten/space-grotesk/tree/7220f5d04813fe83babe76d4fd23e02275021280).

Copyright 2020 The Space Grotesk Project Authors (https://github.com/floriankarsten/space-grotesk)

Space Grotesk is licensed under the SIL Open Font License, Version 1.1. The
complete license is installed alongside the font as
`Data/SFSE/Plugins/Fonts/SpaceGrotesk-OFL.txt` and retained in
[`public/SFSE/Plugins/Fonts/SpaceGrotesk-OFL.txt`](public/SFSE/Plugins/Fonts/SpaceGrotesk-OFL.txt).

## FreeType 2.14.3

The framework statically links FreeType 2.14.3 from the official
[`VER-2-14-3` source](https://gitlab.freedesktop.org/freetype/freetype/-/tree/VER-2-14-3)
to rasterize ImGui fonts with TrueType hinting.

This software is based in part on the work of the FreeType Team.

Portions of this software are copyright © 2026 The FreeType Project
(https://freetype.org). All rights reserved.

FreeType is used under the FreeType License. The complete license is installed
as `Data/SFSE/Plugins/Fonts/FreeType-FTL.txt` and retained in
[`public/SFSE/Plugins/Fonts/FreeType-FTL.txt`](public/SFSE/Plugins/Fonts/FreeType-FTL.txt).

## xmake-repo

The repository-local FreeType package recipe is adapted from the FreeType
recipe in [xmake-io/xmake-repo at commit
`4033f542417b761af67a9bfc2a3bac3cf8c6b6f1`](https://github.com/xmake-io/xmake-repo/tree/4033f542417b761af67a9bfc2a3bac3cf8c6b6f1/packages/f/freetype).
That recipe is used under Apache License 2.0.

A package repository based on xmake

Copyright 2017-2018 The Xmake Open Source Community

This product includes software developed by The Xmake Open Source Community
(https://xmake.io/).

The complete source-distribution attribution is retained in
[`xmake-packages/LICENSE.xmake-repo.md`](xmake-packages/LICENSE.xmake-repo.md)
and
[`xmake-packages/NOTICE.xmake-repo.md`](xmake-packages/NOTICE.xmake-repo.md).

## JSON for Modern C++ 3.11.3

Copyright (c) 2013-2022 Niels Lohmann

MIT License

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
