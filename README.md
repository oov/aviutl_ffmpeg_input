ffmpeg_input
============

[FFmpeg](https://www.ffmpeg.org/) を利用した AviUtl 用の入力プラグインです。  
以下のような特徴があります。

- `movie-ffmpeg.mp4` などのようにファイル名が `-ffmpeg` が終わるものしか読み込まない
  - L-SMASH Works との併用を想定しており、ファイル名によって使い分けができます
- 巨大なファイルの読み込みがそれなりに速い代わりにシークが不正確
  - 精度が重要な場合は従来通り L-SMASH Works をご利用ください
  - ファイル内容やカット地点によって影響の大きさは変わると思われます
- InputPipePlugin 相当の機能を最初から内包
  - 動画の読み込みを別プロセスで行うため AviUtl のメモリ空間を必要以上に圧迫しません
  - ハンドルキャッシュにより同じ動画ファイルを読み込んだオブジェクトの切り替えが高速です
- 64bit 版の FFmpeg を利用
  - ライセンス周辺の諸問題をクリアするために OpenH264 を動的リンクした LGPL 版の FFmpeg を同梱しています  
    以下のリポジトリーでビルドしています  
    https://github.com/oov/ffmpeg-openh264-win/releases  
    このファイルのライセンスについては ffmpeg64/FFMPEG_LICENSE.txt を参照してください。
  - Cisco-provided binary の OpenH264 を ffmpeg64/bin/ フォルダー内に同梱しています。  
    このファイルのライセンスについては ffmpeg64/OPENH264_BINARY_LICENSE.txt を参照してください。

ffmpeg_input の動作には AviUtl version 1.00 以降と拡張編集 version 0.92 以降が必要です。

なお、このプラグインの作成には [AviUtlFFmpegDecoder](https://github.com/kusaanko/AviUtlFFmpegDecoder) を大いに参考にさせて頂きました。  
この場を借りてお礼申し上げます。

注意事項
--------

ffmpeg_input は無保証で提供されます。  
ffmpeg_input を使用したこと及び使用しなかったことによるいかなる損害について、開発者は責任を負いません。

これに同意できない場合、あなたは ffmpeg_input を使用することができません。

This software uses libraries from the [FFmpeg project](https://www.ffmpeg.org/) under the LGPLv2.1.  
Copyright (c) 2003-2022 the FFmpeg developers.

This software uses [OpenH264](https://github.com/cisco/openh264) binary that released from Cisco Systems, Inc.  
OpenH264 Video Codec provided by Cisco Systems, Inc.  
Copyright (c) 2014 Cisco Systems, Inc. All rights reserved.

ダウンロード
------------

https://github.com/oov/aviutl_ffmpeg_input/releases

インストール
------------

1. `ffmpeg_input.64aui` と `ffmpeg_input-brdg64.aui` と `ffmpeg64` を、  
   `aviutl.exe` と同じ場所か、`plugins` フォルダー内のどちらかにまとめて配置
2. 配置したファイルがセキュリティソフトにブロックされないよう、適切に除外設定を行ってください
3. AviUtl のメニューから `ファイル`→`環境設定`→`入力プラグイン優先度の設定` を選び、  
   `FFmpeg Video Reader Bridge` を `L-SMASH Works File Reader` や `InputPipePlugin` より上に配置してください

設定
----

`ファイル`→`環境設定`→`入力プラグインの設定`→`FFmpeg Video Reader Bridgeの設定` を選ぶと設定ダイアログが表示されます。

### 全体に関わる設定

#### ファイル名が "-ffmpeg" で終わるファイルだけ読み込む

ファイル名に `hello-ffmpeg.mp4` のように `-ffmpeg` がついているファイルだけをこのプラグインでの読み込み対象にします。  
他の読み込みプラグインと併用する場合に有用です。  
デフォルトで有効です。

#### 優先するデコーダー

優先的に使用したいデコーダーがある場合に、それらをカンマで区切って指定します。  
先に書いたものが優先されます。

例えば `h264_cuvid,h264_qsv,libopenh264` と指定すると、`h264` のファイルを読み込もうとしたときに `h264_cuvid` で開き、それに失敗した場合は `h264_qsv` で、それでも駄目なら `libopenh264` で開きます。

#### リソース管理モード

拡張編集で読み込んだ動画ファイルが参照されなくなったあとも内部的に保持したままにすることで、次に必要になった際に再利用するための設定です。

- 通常
  - 特殊な動作を行いません
- ハンドルキャッシュ
  - 既に開いている動画ファイルが再度開かれようとした場合に、同じファイルを再利用することで、読み込み時間を短縮します
  - 通常の利用時には効率がいいですが、同じ動画ファイルを同時に表示するような使い方をした場合、劇的に遅くなります
  - これがデフォルト設定です
- ハンドルプール
  - 動画ファイルを閉じる際、実際には閉じず覚えておくことで、再利用時の読み込み時間を短縮します
  - 拡張編集の環境設定で「動画ファイルのハンドル数」を2まで減らすとプールを有効に活用できますが、他の入力プラグインのパフォーマンスが劇的に悪くなります
  - 通常の利用時のパフォーマンスはハンドルプールより悪いですが、同じ動画ファイルを同時に表示しても動作します
  - 試しに作ってみたけど思ったより良くなかったです

#### ハンドルキャッシュ数

「同じ動画ファイルの別のフレームを同時に表示する」といった使い方をした場合に、パフォーマンスが著しく低下するのを緩和する設定です。  
シーンチェンジの前後に同じ動画ファイルがある場合も該当します。

数字を大きくすると消費メモリが大きくなっていくため、必要最低限の数字に設定するのが望ましいです。  
同じ動画ファイルを同時に2つ表示するなら `2`、もしその状態でシーンチェンジを使うなら、その2倍である `4` が最低限の設定です。

### 映像

#### カラーフォーマット変換時のスケーリングアルゴリズム

これは通常の拡大縮小時の処理が変わる設定ではありません。  
初期設定である `fast bilinear` は速度と品質のバランスが良いアルゴリズムです。

### 音声

#### 音ズレ軽減

何も対策しない場合、動画を途中から再生すると音ズレが発生します。  
ここで設定を行うことで動作を改善できます。

ただし、ここでの設定は現時点では音声データをデコードせずにサンプル数が取得できる場合にのみ有効です。  
また、作成したインデックスは現時点ではファイルには保存されないため、動画を読み込むたびに作成処理が行われます。  
作成中にはダイアログなどは出さず、作成済みの領域に随時アクセス可能になります。

- なし
  - 何も対策を行いません
  - この設定で開発中に遭遇したズレは1フレーム程度です
  - これがデフォルト設定です
- リラックス
  - 音声のインデックスを作成します
  - 動画ファイルへの書き出し中の挙動は「正確」と同じです
  - 編集時にまだ作成されてない領域を表示しようとした場合は、ズレを許容します
  - このモードでの動作には AviUtl のバージョン 1.00 または 1.10 が必要です
- 正確
  - 音声のインデックスを作成します
  - まだ作成されてない領域を表示しようとした場合は、インデックスが作成されるまで待機します

#### 位相を反転（デバッグ用）

読み込んだ音声データの位相を反転させます。

Credits
-------

ffmpeg_input is made possible by the following open source softwares.

### Acutest

https://github.com/mity/acutest

The MIT License (MIT)

Copyright © 2013-2019 Martin Mitáš

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the “Software”),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.

### AviUtlPluginSDK License

http://spring-fragrance.mints.ne.jp/aviutl/

The MIT License

Copyright (c) 1999-2012 Kenkun

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

### AviUtlFFmpegDecoder

https://github.com/kusaanko/AviUtlFFmpegDecoder

Apache License

                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

   TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

   1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

   2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

   3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

   4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

   5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

   6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

   7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

   8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

   9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

   END OF TERMS AND CONDITIONS

   APPENDIX: How to apply the Apache License to your work.

      To apply the Apache License to your work, attach the following
      boilerplate notice, with the fields enclosed by brackets "[]"
      replaced with your own identifying information. (Don't include
      the brackets!)  The text should be enclosed in the appropriate
      comment syntax for the file format. We also recommend that a
      file or class name and description of purpose be included on the
      same "printed page" as the copyright notice for easier
      identification within third-party archives.

   Copyright [yyyy] [name of copyright owner]

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

### FFmpeg

https://www.ffmpeg.org/

This software uses libraries from the FFmpeg project under the LGPLv2.1.  
Copyright (c) 2003-2022 the FFmpeg developers.

See ffmpeg64/FFMPEG_LICENSE.txt for details.

### hashmap.c

https://github.com/tidwall/hashmap.c

NOTICE: This program used a modified version of hashmap.c.  
        https://github.com/oov/hashmap.c/tree/simplify

The MIT License (MIT)

Copyright (c) 2020 Joshua J Baker

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

### OpenH264

https://github.com/cisco/openh264

This software uses OpenH264 binary that released from Cisco Systems, Inc.  
OpenH264 Video Codec provided by Cisco Systems, Inc.  
Copyright (c) 2014 Cisco Systems, Inc. All rights reserved.

See ffmpeg64/OPENH264_BINARY_LICENSE.txt for details.

### TinyCThread

https://github.com/tinycthread/tinycthread

NOTICE: This program used a modified version of TinyCThread.  
        https://github.com/oov/tinycthread

Copyright (c) 2012 Marcus Geelnard
              2013-2016 Evan Nemerson

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
    claim that you wrote the original software. If you use this software
    in a product, an acknowledgment in the product documentation would be
    appreciated but is not required.

    2. Altered source versions must be plainly marked as such, and must not be
    misrepresented as being the original software.

    3. This notice may not be removed or altered from any source
    distribution.
