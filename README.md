# Brainux on PW-H1

PW-H1 / V690_H1の追加アプリからLinuxへ移行する実験用ローダー。
`AppContext.exec_path` と同じフォルダーからカーネル・DTBを読み込むため、本体／SDのどちらに配置しても使える。

**診断ログの保存、メモリマッピング、Linuxからinitramfsへの到達は実機確認済み。`nohlt` を追加したdiag 13では2回連続で表示が安定したとの報告あり。長時間の安定性とSDからのrootfs起動は未確認。**

現在の生成物は **diag 54（SDの起動パーティション指定を修正するfix-sd）**。指定したSDパーティションがない場合は、エラーと認識済みパーティションを表示する。diag 50でWKPUのタイマー起床を有効にし、通常idleをPSTOP3へ変更。メモリ・画面用クロックは維持する。待機からの自動復帰・入力・ふた開閉・sleep 10と短時間の放置は実機確認済み。英字・Enter・Shiftに加え、Space・Backspace・矢印・PgUp/PgDn・マイナスをLinuxコンソールへ送る。
A=`01`・Q=`11`・Enter=`25`・Shift=`2e`を実機確認し、残りの英字は連番と推定している。
`~ #` で `ls` や `dmesg` を入力してEnterを押す。Shiftとの同時押しで大文字になる。
diag 15で本体キーボードから入力できたとの実機報告あり。あA1で英字／数字・記号モードを切り替える。配置は`keymap`で確認できる。
`keycodes` と入力してEnterを押すと、約20秒間キー番号を表示してシェルへ戻る。
確認するキーを1個ずつ0.5秒ほど押して離す。
通知キュー対策で入力回数による遅延の解消を実機確認。追加デバウンスを削除し、長押しの自動キーリピートを有効にした。
`wfi-test` でCPU待機時間・タイマー割り込み・生キー状態を確認できる（停止時はリセットが必要）。diag 50ではWKPU channel 3を有効にし、通常idleでも待機する。`SLEEP` がキー操作なしで約1 tick以内に戻ること、`sleep 10`、入力、ふた開閉、表示の安定性を確認する。
同時押しは3キーまで実機確認済み。4キーは未対応。
diag 47でふたを開いた後の入力復帰を実機確認。diag 48は `backlight test` で2秒間消灯して再点灯する依頼を送れる。
`backlight on` / `backlight off` / `backlight status` も使用可能。diag 48の点灯・消灯は実機確認済み。diag 49では閉じると消灯し、開くと手動設定へ戻る（実機未確認）。明るさの段階変更は未対応。
上段6キーはF1〜F6、戻るはEsc、音声はCtrl、あA1は英字／数字・記号の切り替え。音声+CでCtrl+C、Shift+マイナスで `_`。
ローダー `power mask 1` で純正APIによる電源・ふた操作の停止抑止を追加（電源キー後の入力継続を実機確認）。更新対象は `AppMain.bin`。
電源キーは `1c` と実機確認済み（M4コードとも一致）。Linux入力イベントには未割り当て。USBは無効。diag 30は起動直後から入力できなくなったため、diag 31でSMC1変更を撤去し `nohlt` を復元した。
diag 25の端子操作で自動再起動を実機確認。diag 26ではこの処理を`reboot`に割り当てた（コマンド変更後は実機未確認）。電源キーなしで純正OSを起動し直す。電源断は未対応。
タッチ・音声・通常動作中の省電力制御は未対応。詳細は [boot.md](docs/boot.md) を参照。

## 生成物と起動

`make image` は `target/APP/Brainux/` に次を生成する。

- `AppMain.bin`: 純正OS上で動くローダー。
- `BOOT.BIN`: ARM Linux Image・initramfs・PW-H1 DTB・SHA-256。
- `index.din`: 空の追加アプリ識別ファイル。
- `FONT-LICENSE.txt`: Reflexのフォントライセンス。

フォルダー全体を本体またはSDの追加アプリ置き場へコピーする。
起動時に `BOOT.BIN verified` が出たら、Enter、続いてBでLinuxへ移る。Escは移行前のキャンセル。
移行後は純正OSの元の実行状態へreturnしない。diag 21から `reboot` で純正OSを起動し直す経路を追加した（実機未確認）。停止する場合は本体のリセット操作が必要。
読み込み・サイズ・チェックサム・DTB・マッピングのエラーではLinuxを起動しない。
周辺機器の停止中に異常を検出した場合はpanic画面で停止する。この段階では純正OSには戻れない。

USB有効時は、起動したinitramfsがUSB CDC ACMを公開する構成（diag 13では無効）。
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
cat /proc/partitions
fix-sd
boot-root /dev/mmcblk0p2
```

`fix-sd` は `/dev/mmcblk0p2` の `boot.mount` に残る旧指定 `/dev/mmcblk1p1` を
`/dev/mmcblk0p1` へ修正する。修正済みなら変更せず終了する。SDへの保存は一度でよい。

rootfsを最初は読み取り専用でマウントし、chroot内でinitの存在を確認してから、
検査後に書き込み可能へ再マウントし、`/dev`・`/proc`・`/sys`・`/run` を準備する。
PID 1が診断シェルを終了し `switch_root` で起動する。配布rootfsのfstabには
ルートの再マウント設定がないため、書き込み可能な状態で引き渡す。
マウント移動に失敗した場合は、移動済みのマウントを逆順に戻す。
終了前には `sync` を実行する。自動電源断は未対応。

## 診断アプリ

`make` は従来の `target/APP/BrainuxProbe/` を生成する。
起動するとアプリと同じフォルダーへ `BUXPROBE.TXT` を保存する。
左右／ページキーで閲覧、Enterで同じ採取結果を再保存、Escで終了する。
同名の診断ファイルは上書きする。採取対象のMMIO自体は読み書きしない。

実機ログと解析の根拠は [docs/bringup.md](docs/bringup.md)、
ローダーのメモリ配置・移行手順・制限は [docs/boot.md](docs/boot.md) を参照。
