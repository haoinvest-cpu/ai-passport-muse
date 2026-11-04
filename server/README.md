# Muse TTS 服务端

一台局域网里的小机器（树莓派之类）跑这个脚本，把固件的回复文本念出来。固件
`muse_tts.c` 只负责把文本 POST 过来、把回来的 PCM 直接喂给喇叭，不碰 MP3。

语音由微软 Edge 的在线声音合成（`edge-tts` 包），再交给 `ffmpeg` 转成设备能
直接播放的格式。

## 依赖

- Python 3（脚本用 `asyncio` 和 `aiohttp`）
- `ffmpeg`：必须在 `PATH` 里，`/tts-pcm` 靠它把 MP3 转成 PCM

```sh
# Debian / Raspberry Pi OS
sudo apt install ffmpeg python3 python3-venv

cd server
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
```

## 环境变量

| 变量 | 默认值 | 说明 |
|---|---|---|
| `MUSE_TTS_KEY` | 空 | 可选。设了以后，请求必须带 `X-Key: <同一个值>`，否则 401。留空表示不校验，只适合可信局域网。 |
| `MUSE_TTS_VOICE` | `en-US-AriaNeural` | Edge TTS 的声音名。中文回复用 `zh-CN-XiaoxiaoNeural` 之类。 |
| `PORT` | `8020` | 监听端口，脚本固定绑 `0.0.0.0`。 |

## 启动

```sh
cd server
. .venv/bin/activate
MUSE_TTS_VOICE=zh-CN-XiaoxiaoNeural PORT=8020 python3 muse-tts-server.py
```

前面挂 systemd 或 `nohup` 随你，重启后自动拉起来即可。板子和这台机器必须在
同一个能互相访问的网段，防火墙要放行 TCP 8020。

## 接口

两个都是 `POST`，请求体是纯文本（UTF-8），最长 2000 字符，超出部分截断。脚本
会先删掉代码块、URL 和 Markdown 记号再念。

| 路径 | 返回 | 用途 |
|---|---|---|
| `/tts` | `audio/mpeg`，边合成边发 | 给别的播放器/调试用 |
| `/tts-pcm` | `application/octet-stream`，`X-Audio-Format: s16le;rate=16000;channels=1` | 固件用的就是它：16 kHz 单声道 16 位小端 PCM |

设了 `MUSE_TTS_KEY` 时，两个接口都要带 `X-Key` 头。

## 自测

```sh
# MP3：应该直接听到声音
curl -sS -X POST --data-binary '你好，这是一次语音测试。' \
  http://127.0.0.1:8020/tts -o /tmp/reply.mp3

# PCM：16 kHz 单声道，每 32000 字节一秒
curl -sS -X POST --data-binary '你好，这是一次语音测试。' \
  http://127.0.0.1:8020/tts-pcm -o /tmp/reply.pcm
ls -l /tmp/reply.pcm
```

带上 key 时加 `-H 'X-Key: 你的key'`。

## 接到固件上

把地址写进 board overlay 的 `esp32/devices/sdkconfig.muse-passport`：

```
CONFIG_MUSE_TTS_PCM_URL="http://192.168.1.10:8020/tts-pcm"
```

`server/` 和固件不共享配置，改完重新编译、刷入才生效。

## English

A small HTTP service for the Passport's spoken replies. It needs Python 3 and
`ffmpeg`; install the Python packages with `pip install -r requirements.txt`.
`MUSE_TTS_KEY` is optional (when empty, `X-Key` is not checked),
`MUSE_TTS_VOICE` defaults to `en-US-AriaNeural`, and `PORT` defaults to 8020.
`POST /tts` returns streamed MP3; `POST /tts-pcm` returns 16 kHz mono s16le PCM,
which is what the firmware plays. Start it with
`MUSE_TTS_VOICE=zh-CN-XiaoxiaoNeural PORT=8020 python3 muse-tts-server.py` and
point `CONFIG_MUSE_TTS_PCM_URL` at it.
