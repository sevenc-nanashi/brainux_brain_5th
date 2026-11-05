# PW-H1 / V690_H1 の起動調査

## 実装状況

`BrainuxProbe` は純正OS上の診断アプリ。実機のアドレス変換は静的解析と一致した。
その後、[起動ローダーとカーネル](boot.md) を実装した。以下は調査時点の記録。
Linuxへの実機移行とM4との共存は引き続き未確認。

## 実機ログ

ユーザー提供の [初回ログ](logs/pwh1-probe-v1.txt) を保存した。
ファームウェア名はProbeの固定ラベルで、自動検出の結果ではない。

- MIDR=`410fc075`: Cortex-A7 r0p5。
- CPSR=`a000011f`: ARM / Systemモード。IRQ・FIQは有効。
  Linuxへ渡す前に割り込みを禁止し、SVCモードへ切り替える必要がある。
- SCTLR=`00c5187d`: MMU、D-cache、I-cacheが有効。
- TTBCR=`0`: short-descriptor形式。TTBR0/1=`60500019`、テーブル基底は`60500000`。
- アプリ領域15 MiB、RAM別名128 MiB、RAM恒等マッピング113 MiBは、
  **全4 KiBページ**で静的解析の変換と一致した。実装RAM容量の破壊検査ではない。
- スタックはVA=`91eff844` → PA=`604ff844`、ヒープの採取時点のアドレスは
  VA=`8c010364` → PA=`60210364`。アプリコードの領域外にあるため、
  MMU切替後に既存のスタックやRustの所有値へアクセスしてはいけない。
- CNTFRQ=`0`: この値からgeneric timerの周波数を得られない。
  実際のカウンターの稼働・周波数と、Linux側のタイマー選択は別途確認が必要。
- MMIOの恒等変換を確認したが、レジスターへのアクセスやDMA停止の成功は未確認。

ファイルの生成・採取は成功。アプリ終了後の画面復帰などはこのログだけでは確認できない。

## 参照元

| ソース | 確認したリビジョン／場所 |
| --- | --- |
| ローカルReflex | `../brain_5th_hack/crates/reflex`、`0b9553617cdc5dba191cac4a1b5fc55ba8b31d36` |
| [U-Boot PW-H1設定](https://github.com/brain-hackers/u-boot-brain/blob/e8fc0d0cf39d9cd06245ef1777d1cf54258e5cb6/include/configs/pwh1.h) | `e8fc0d0cf39d9cd06245ef1777d1cf54258e5cb6` |
| [Linux PW-H1 DT](https://github.com/brain-hackers/linux-brain/blob/5e99ce4dc22d77bb029133d7aa6483486c684f20/arch/arm/boot/dts/imx7ulp-pwh1.dts) | `5e99ce4dc22d77bb029133d7aa6483486c684f20` |
| [既存MMU実験](https://github.com/brain-hackers/lab/tree/master/x1) | `injector/disable_mmu.S`、`spray/top.S` |
| [ARM Linux起動規約](https://docs.kernel.org/arch/arm/booting.html) | DMA停止、物理アドレス、CPUモードとキャッシュの要件 |

調査用クローンはローカルの `__gi_lab`、`__gi_uboot`、`__gi_linux`、
`__gi_buildbrain` に置いてある。Git管理外で、ビルドの依存先にはしていない。

## RAMの静的解析

V690_H1 `system.bin` のオフセット `0xce0c4` 以降に、
長さ・物理アドレス・仮想アドレス・属性の並びがある。
以下はローカルの `../brain_5th_hack/__gi_system.disasm` とダンプで確認した値。
実行時の変換結果は別途照合する必要がある。

| 表の位置 | 長さ | 物理アドレス | 仮想アドレス | 用途／解釈 |
| --- | --- | --- | --- | --- |
| `0xce0c4` | `0x08000000` | `0x60000000` | `0x30000000` | 128 MiBのRAM別名 |
| `0xce394` | `0x00500000` | `0x60a00000` | `0x60a00000` | Reflexの表示バッファを含む |
| `0xce3a8` | `0x07100000` | `0x60f00000` | `0x60f00000` | RAMの恒等マッピング |
| `0xce470` | `0x00f00000` | `0x67100000` | `0x90000000` | 追加アプリの15 MiB領域 |
| `0xce484` | `0x00f00000` | `0x67100000` | `0x91000000` | 同じ領域の別名 |

U-Bootの `PHYS_SDRAM_SIZE=SZ_128M` と上記のRAMマッピング長は整合する。
ただし、表だけで実装RAM容量や全領域の利用可否が確定するわけではない。
`0x90500000` のReflex `.data/.bss` はこの表では物理 `0x67600000` となる。

Reflexの画面は `0x60a00008`、854×480、RGB565。
パネル向きの配置は480×854で、ランドスケープの `(x,y)` は
`x*480+(479-y)` 番目の16-bitピクセルへ対応する。
表示要求は `0x40aa0004` へ1を書き込む処理がある。
Linuxのsimple-framebufferを置くだけで表示更新できるとは未確認。

## 既存コードとの差分

- Linuxの `imx7ulp-pwh1.dts` はRAMを **1 GiB** (`0x40000000`) と宣言し、
  `0x9ff00000` 付近にRPMsg領域を予約している。128 MiBという他の資料と一致しない。
- U-Boot側のsimple-framebufferは `0x62000000`。Reflexの `0x60a00008` と異なる。
  LCDの実際の読み出し先・RUN制御と合わせた確認が必要。
- 既存DTにはWi-Fi、BT、PMIC、MIPIパネルなどのEVK由来の設定が残っている。
  名前がPW-H1でも、各ノードを実機対応済みとは扱えない。
- U-Bootの環境変数保存先はMMC。移植版では媒体を特定し、
  起動確認中は内蔵eMMCへ環境を書かない構成にする必要がある。
- `lab/x1/injector` は固定サイズのロード・コピーとL1キャッシュの無効化を行う実験。
  戻り値、転送量、dirty line、コピー先と稼働中OSの重なりの扱いを見直す必要がある。

## 移植時の確認項目（起動経路は実装済み、実機検証待ち）

1. ProbeでCPUモード・ページテーブル形式・RAM別名・アプリ全ページの変換を照合する。
2. 純正OSが使うDMA、USB、SD、表示、watchdog、MUを確認し、
   書き込み途中のストレージがない状態で停止する手順を確定する。
   Cortex-M4がRAMへアクセスし続けるかも確認する。
3. カーネル・DTB・起動トランポリンの配置を決める。
   純正OS稼働中は所有していないRAMへペイロードをコピーしない。
   キャッシュ全階層のclean、物理アドレスでの命令フェッチ、MMU無効化を扱う。
4. 検証済みのRAM容量・予約領域だけを持つPW-H1用DTとi.MX7ULPカーネルを作る。
   `r0=0`、`r1=~0`、`r2=DTBの物理アドレス`、ARM/SVC、IRQ/FIQ無効で移行する。
5. UARTまたは検証済みの画面出力でカーネルログを採取する。
   SDの配線・電源を確認し、まずinitramfs、次にBrainuxのrootfsへ接続する。
6. 第5世代のキーボード、タッチ、電源・終了処理を実装して実用動作を確認する。

PEGの入力イベントは純正OSのサービスなので、LinuxがOSを置き換えた後には利用できない。
ReflexのキーコードをLinuxのキーマップへ転記するだけでは入力対応は完了しない。
