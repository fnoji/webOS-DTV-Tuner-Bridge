# tv_libs（リポジトリには含まれません）

`tv-bridge/Makefile` は、**TV 本体の glibc に合わせてリンクする**ため、以下の共有ライブラリを
このディレクトリに置く前提です。

```
tv_libs/
├─ libc.so.6
├─ libpthread.so.0
├─ libdl.so.2
└─ ld-linux.so.3
```

LG ファームウェアからの抽出物のため**再配布できません**。各自の TV（root 化済み）から取得してください。

## 取得例

TV 上で（root シェル）:

```sh
mkdir -p /tmp/tvlibs
cp -L /lib/libc.so.6 /lib/libpthread.so.0 /lib/libdl.so.2 /lib/ld-linux.so.3 /tmp/tvlibs/
```

PC へは任意の方法で（`scp` / `adb pull` / `nc` など）:

```sh
scp -r root@<TVのIP>:/tmp/tvlibs ./tv_libs
```

## 注意

- バージョンは機種・ファームウェアに依存します（検証機: glibc 2.24 / soft-float ARM EABI）。
- `ld-linux.so.3` は soft-float 版です。hard-float（`ld-linux-armhf.so.3`）の機種では
  Makefile の `--dynamic-linker` とライブラリを差し替えてください。
- 抽出したバイナリは `.gitignore` で除外されています。**コミットしないでください。**
