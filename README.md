# Brainux on PW-H1

PW-H1 / V690_H1の追加アプリからLinuxへ移行する実験用ローダー。
`AppContext.exec_path` と同じフォルダーからカーネル・DTBを読み込むため、本体／SDのどちらに配置しても使える。

**診断ログの保存、メモリマッピング、Linuxからinitramfsへの到達は実機確認済み。専用予約領域への移動後に画面破損が一度解消したが、USB PHY初期化の変更後に白いノイズが再発。diag 12はUSBを無効化した表示切り分け用の暫定版。SDからのrootfs起動は未確認。**
ネイティブのキーボード・タッチ・音声・電源管理は未対応。diag 12ではUSBシリアルも使えないため、Linux起動後のシェル入力はできない。

## 生成物と起動

`make image` は `target/APP/Brainux/` に次を生成する。

- `AppMain.bin`: 純正OS上で動くローダー。
- `BOOT.BIN`: ARM Linux Image・initramfs・PW-H1 DTB・SHA-256。
- `index.din`: 空の追加アプリ識別ファイル。
- `FONT-LICENSE.txt`: Reflexのフォントライセンス。

フォルダー全体を本体またはSDの追加アプリ置き場へコピーする。
起動時に `BOOT.BIN verified` が出たら、Enter、続いてBでLinuxへ移る。Escは移行前のキャンセル。
移行後は純正OSへreturnしない。戻す場合は本体のリセット操作が必要。
読み込み・サイズ・チェックサム・DTB・マッピングのエラーではLinuxを起動しない。
周辺機器の停止中に異常を検出した場合はpanic画面で停止する。この段階では純正OSには戻れない。

USB有効時は、起動したinitramfsがUSB CDC ACMを公開する構成（diag 12では無効）。
Linuxホストでは `/dev/ttyACM*`、Windowsでは対応するCOMポートを開く（115200）。
**initramfsのUSBコンソールは診断用の認証なしrootシェル**。
起動コンソールは本体画面。`0x66e00000` の専用RGB565バッファをLCDIFの連続表示で使う。
表示用の1 MiBを `reserved-memory` / `no-map` で通常のメモリ利用から除外する。
転送不足からの自動復帰を有効にし、純正OSの補助表示レイヤーは停止する。
UARTの配線は未確認のため、既定では有効にしない。

## ビルド

必要なもの:

- Rust（edition 2024対応）、`armv7a-none-eabi` ターゲット。
- 隣の `../brain_5th_hack` にあるReflex。参照時のコミットは
  `0b9553617cdc5dba191cac4a1b5fc55ba8b31d36`。path依存なのでローカルの変更も反映される。
- GNU Make、Ruby、Git、`llvm-objcopy-20`。
- カーネル用: `arm-linux-gnueabi-gcc` と対応するbinutils・libc開発ファイル、
  ホストCコンパイラ、bison、flex、bc、OpenSSL開発ファイル。

```sh
rustup target add armv7a-none-eabi
make fetch       # LinuxとBusyBoxのソースをtarget配下へ取得
make image       # カーネル、initramfs、DTB、ローダー、BOOT.BIN
make check
cargo run --locked --features host --bin bundle-check -- target/APP/Brainux/BOOT.BIN
```

Linuxは `brain-hackers/linux-brain` の `5e99ce4dc22d77bb029133d7aa6483486c684f20`、
BusyBoxは `1_36_1` を使う。既存ディレクトリがある場合、`make fetch` は上書きしない。
`make linux` は指定したLinuxソースへ `kernel/pwh1.patch` とボードファイルを適用する。
作業中の別チェックアウトではなく、この用途の専用コピーを指定すること。

```sh
LINUX=/path/to/linux BUSYBOX_SRC=/path/to/busybox \
  CROSS_COMPILE=arm-linux-gnueabi- JOBS=4 make linux
make loader CARGO=/path/to/cargo OBJCOPY=/path/to/llvm-objcopy
```

整形は `make fmt`（Rust）。CはLinuxのスタイル、Ruby・シェルは既存の小さなスクリプト形式を使う。
通常の検査はARM向けclippy。ELFのエントリー・コード／データ／ページテーブル／ペイロード領域はリンク時ASSERTで検査する。
`bundle-check` はローダーと同じ検証コードをホストで実行する。

## Brainuxのrootfsへ進む

最初の `BOOT.BIN` には診断用BusyBox initramfsを含む。Debian/Brainuxのrootfs自体は含めない。
[buildbrain](https://github.com/brain-hackers/buildbrain) で作成したARM用Brainux rootfsをSDのext4パーティションへ配置する。
既存のi.MX283用カーネル・DTBは使わず、このリポジトリのカーネルを使う。

本体の内蔵eMMCはLinuxのDTで無効化している。Linux側の `mmcblk0` はSD用コントローラーのみ。
SDのフォーマット・イメージ書き込みはこのリポジトリのスクリプトでは行わない。

rootfsをSDへ配置する前に、展開した**コピー**へUSBログインサービスを追加する。

```sh
ruby scripts/prepare-rootfs.rb /path/to/extracted-brainux-rootfs
```

既存のサービスファイルがある場合は停止する。このサービスは通常のログインを使うため、
Brainuxの既存ユーザー・パスワードが必要。
初期シェルから次を実行する（実際のSDパーティションを確認すること）。

```sh
boot-root /dev/mmcblk0p2
```

rootfsを最初は読み取り専用でマウントし、chroot内でinitの存在を確認してから、
PID 1が診断シェルを終了し `switch_root` で起動する。
rootfs側のinitやfstabが書き込み可能に再マウントする場合がある。
終了前には `sync` を実行する。自動電源断は未対応。

## 診断アプリ

`make` は従来の `target/APP/BrainuxProbe/` を生成する。
起動するとアプリと同じフォルダーへ `BUXPROBE.TXT` を保存する。
左右／ページキーで閲覧、Enterで同じ採取結果を再保存、Escで終了する。
同名の診断ファイルは上書きする。採取対象のMMIO自体は読み書きしない。

実機ログと解析の根拠は [docs/bringup.md](docs/bringup.md)、
ローダーのメモリ配置・移行手順・制限は [docs/boot.md](docs/boot.md) を参照。
