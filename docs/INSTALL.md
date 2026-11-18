# インストールガイド

対象: 管理者権限（root）を取得済みの LG webOS TV（動作確認: 55UK7500PJA / webOS 4.4.3-22 Rockhopper）
パッケージ: `release/tv-ipk/webos_dtv_tunerbridge.ipk`

---

## 0. 前提

| 項目 | 必須 | 理由 |
|---|---|---|
| **root 化** | 必須 | `tvservice` への `.so` 注入（ptrace）に 管理者権限が必要 |
| **Homebrew Channel (webosbrew)** | 必須 | ipk のインストールと `luna://org.webosbrew.hbchannel.service/exec` による起動 |
| **Quick Start+ (設定 → 一般)** | WOL を使う場合 | スタンバイ中の LAN 疎通と Magic Packet 復帰 |
| **PC** | 必須 | BonDriver_WebOS を使う場合は Windows / EPGStation を使う場合は任意の OS |

> root 化と Homebrew Channel は別物です。Homebrew だけでは `tvservice` に注入できません。

---

## 1. TV 側のセットアップ

### 1-1. ipk をインストール

Homebrew Channel の「Install」から ipk を指定するか、PC から:

```sh
ares-install -d <TVのIPまたはデバイス名> webos_dtv_tunerbridge.ipk
```

インストール先（ファーム書き換え領域ではありません）:

```
/media/developer/apps/usr/palm/applications/io.github.webostuner.app/
├─ appinfo.json
├─ index.html
└─ native/
   ├─ webos_tuner_d        # デーモン本体
   ├─ webos_inject         # 注入ツール
   ├─ libwebos_tuner.so    # tvservice 内で動くブリッジ
   └─ tunerctl.sh          # 起動/停止/自動起動/電源モード管理
```

**この時点ではデーモンは起動しません。**

### 1-2. アプリを起動して「起動 (Start)」

TV のホーム画面から **「webOS DTV Tuner」** を開き、**起動 (Start)** を押します。

初回起動時に以下が行われます。

1. `webos_inject` が `tvservice` を検出して `libwebos_tuner.so` を注入（メモリ上のみ）
2. LS2 の特権ロールを登録（後述の 3 ファイル）
3. チューナー/EPG/B-CAS の初期化、HTTP `:40772` と TCP `:40580` の待受開始

画面に IP と `TCP:40580 / HTTP:40772` が表示され、状態が「稼働中」になれば成功です。

### 1-3. 常時使う場合

- **自動起動 ON**: TV 起動時にデーモンを自動起動（`/var/lib/webosbrew/init.d/90-webostuner` を作成）
- **電源待機モード**: 用途に合わせて選択
  - `ActiveStandby維持`（既定）: TV OFF でも裏で稼働。復帰待ち 0 秒
  - `スリープ`: TV OFF で停止。WOL で復帰（`wake_tv` / BonDriver）
  - `完全シャットダウン`: TV OFF で完全電源断。WOL 復帰の可否は機種・ファームに依存（復帰しない場合はリモコン/本体ボタン）

### 1-4. 動作確認

```sh
curl http://<TVのIP>:40772/api/status
curl http://<TVのIP>:40772/api/version
curl http://<TVのIP>:40772/api/channels
```

ブラウザで `http://<TVのIP>:40772/` を開くと管理画面（状態・ログ・電源操作）が使えます。

---

## 2. TV 側に書き込まれるもの（変更点の全量）

### 2-1. 追加されるファイル（既存ファイルの書き換えは行いません）

| パス | 内容 | 作成タイミング | 削除方法 |
|---|---|---|---|
| `/media/developer/apps/.../io.github.webostuner.app/` | アプリ本体・バイナリ | ipk インストール | アプリ削除 |
| `/var/luna-service2-dev/roles.d/io.github.webostuner.daemon.role.json`<br>`/var/luna-service2/roles.d/...`（dev 無効時） | LS2 特権ロール（`outbound: *`） | 初回 Start | アンインストール準備 |
| `/var/luna-service2-dev/client-permissions.d/io.github.webostuner.daemon.perm.json` | LS2 クライアント権限 | 初回 Start | 同上 |
| `/var/luna-service2-dev/manifests.d/io.github.webostuner.daemon.json` | LS2 マニフェスト | 初回 Start | 同上 |
| `/var/lib/webosbrew/init.d/90-webostuner` | 自動起動フック | 自動起動 ON | 自動起動 OFF / 準備 |
| `/media/developer/apps/.../native/power_mode.conf` | 電源モードの保存 | モード変更時 | アプリ削除 |
| `/tmp/webos_tuner_d.pid` `.log` `.lock` | 実行時ファイル | 起動時 | 再起動で消滅 |

### 2-2. 実行時のみ（ディスクには残りません）

| 対象 | 内容 |
|---|---|
| `tvservice` のメモリ空間 | 注入された `libwebos_tuner.so`（TV 再起動で消滅） |
| `/proc/lg/sdec/dump/input` `buffer_size` | 選局・バッファ設定の書き込み（procfs） |
| `/proc/<pid>/…` | 注入時の ptrace 制御 |

### 2-3. 変更しないもの

- LG 純正のバイナリ（`tvservice`、`libhal_lg115x.so`、`libkadaptor.so` など）
- システムパーティションの既存設定ファイル
- B-CAS カードの内容（読み取りと APDU 中継のみ）

> `/var` 配下への書き込みは「追加」であり、`uninstall-prep` で削除できます。

---

## 3. アンインストール

1. アプリの **「アンインストール準備」** を押す（誤操作防止のため **5 秒以内に 2 回**）
   - デーモン停止
   - `init.d/90-webostuner` 削除
   - LS2 の role / perm / manifest 削除 + `ls-control scan-services`
2. アプリを削除（Homebrew Channel の Uninstall、または `ares-install -r`）
3. **TV を再起動**（`tvservice` 内に残った注入 `.so` を消すため）

---

## 4. 本体アップデートで Homebrew が外れた場合

### 起こると考えられる挙動

| 状況 | 挙動 |
|---|---|
| root を失った | 注入に失敗 → デーモンは起動せず終了。アプリは「停止中」表示 |
| `/var` が初期化された | LS2 ロールと自動起動フックが消える（残留なし）。次回起動から何も動かない |
| `/media/developer` が残った | アプリ本体は残るが、ランチャー登録が消えてアイコンが出ない場合がある |
| 自動起動フックだけ残った | フック先の `appinfo.json` が無ければ即 `exit 0`（無害） |
| ファームで `/proc/lg/sdec/*` が変わった | 注入は成功しても選局できず、TS が流れない |

**TV の通常動作への影響はありません。** 既存のシステムファイルを書き換えていないため、起動不能・ブートループ等にはなりません。

### `/var` は本当に消えるのか（実機での確認方法）

`/var` の実体は機種・ファームで異なります。再起動で残るか、ファーム更新で消えるかを切り分けるには:

```sh
ls -l /var/lib/webosbrew/init.d/90-webostuner   # フックが存在するか
df -h /var                                      # どのパーティションか
mount | grep -E ' /var(/| )'                    # tmpfs か永続FSか
ls -ld /var                                     # シンボリックリンクか
```

| 観察結果 | 意味 |
|---|---|
| `tmpfs` | **再起動で毎回消える**（LS2 ファイルと自動起動フックも毎回作り直しが必要） |
| `ubifs` / `ext4` の永続パーティション | 通常の再起動では**残る**。電源断でも保持 |
| ファーム更新後 | ルートFSごと差し替わるため、**ほぼ確実に消える**（`/media` のアプリだけ残ることが多い） |

> 参考: このプロジェクトは電源モード（`power_mode.conf`）を `/var` ではなく**アプリ直下（`/media/developer`）**に保存しているため、`/var` が消えてもモード設定だけは残ります。

### 復旧手順

1. 再度 root 化
2. Homebrew Channel を再導入
3. ipk を再インストール（上書きで可）
4. アプリを開いて「起動 (Start)」→ 必要なら「自動起動 ON」
5. 古い LS2 ファイルが残っている場合は、アプリの「アンインストール準備」→ 再インストール

---

## 5. PC 側のセットアップ

### 5-1. BonDriver_WebOS（TVTest などで視聴）

1. `release/pc-bondriver/x64/` または `x86/` の `BonDriver_WebOS.dll` と `BonDriver_WebOS.ini` を使用ソフトへコピー
2. 既定は `Host=auto`（UDP 自動検出）、`Tuner=auto`（空きチューナー）
3. 2 チューナー同時使用は DLL を別名コピー（例: `BonDriver_WebOS_T2.dll`）

### 5-2. WOL ツール（任意）

- `wake_tv.bat`: `wake_tv.exe` を呼ぶだけのラッパー
- `wake_tv.exe -w`: Magic Packet 送信 + 起動待機（最大 15 秒）
- `wake_tv.exe -o`: 送信 + 待機 + ブラウザで WebUI を開く
- MAC は `BonDriver_WebOS.ini` の `LastMac` / `LastEthMac` / `WakeMac` から取得。未設定の場合は
  `wake_tv.exe <MACアドレス> -w` のように引数で指定（ハードコードの既定値はありません）
- EPGStation の `config.yml` の `recordingPreStartCommand` に `'C:\TVTest\wake_tv.exe -w'` を指定すると、スリープ運用でも録画前に復帰できます

### 5-3. EPGStation（録画サーバー）

- `config.yml` の `mirakurunPath` に `http://<TVのIP>:40772` を指定
- 録画はサービス単位ストリーム（PID 絞り込み済み）で行われます

---

## 6. トラブルシュート

| 症状 | 確認点 |
|---|---|
| アプリが「停止中」 | root 権限 / Homebrew Channel の exec サービス / `/tmp/webos_tuner_d.log` |
| 注入失敗 | `pidof tvservice` でプロセスが居るか、ファームでシンボルが変わっていないか |
| 選局できない | アンテナ・B-CAS 挿入・`/api/tuners` の `lock` |
| EPG が欠ける / 空になる | 起動直後はアイドル時の自動 `EIT` 巡回スキャン（GR → BS → CS）が一巡するまで少し待つと埋まります。`/api/epg/status` と `/api/log` でスキャン状況を確認してください（チャンネル一覧自体は TV 側でのチャンネルスキャンが必要です） |
| WOL で復帰しない | Quick Start+ が ON か、スリープモードになっているか（完全シャットダウンは機種により復帰不可） |
| ブラウザから 403 | Host ヘッダ検証。IP 直指定でアクセスする（`lgwebostv.local` 等の名前は不可） |
| 電源モードが変わらない | TV 内アプリから操作する（PC ブラウザからの `/api/power` は POST 必須） |

---

## 7. 制限事項

- Tuner 0 は TV 本体の選局と連動（TV 点灯中は低優先度の割り当てを 503 で拒否）
- 完全シャットダウンからの WOL 復帰は機種・ファームに依存
- ロゴ画像・番組詳細の `video` / `audios` は未対応
- 検証機種は 1 機種のみ（HAL シンボル依存のため他機種は未保証）
