#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""A small TTS server for Muse gadgets built with CONFIG_MUSE_LOCAL_TTS.

    POST /tts   body: the reply's text (UTF-8)   header: X-Key: <your key>
    -> 200 audio/mpeg, streamed as it's synthesised

Speech comes from Microsoft Edge's online voices through the edge-tts package.
Set MUSE_TTS_KEY (required), and optionally MUSE_TTS_VOICE and PORT.
"""
import asyncio
import os
import re

import edge_tts
from aiohttp import web

KEY = os.environ.get("MUSE_TTS_KEY", "")
VOICE = os.environ.get("MUSE_TTS_VOICE", "en-US-AriaNeural")
MAX_CHARS = 2000


def clean(text: str) -> str:
    """Drops what shouldn't be read aloud: code, URLs and Markdown marks."""
    text = re.sub(r"```.*?```", "", text, flags=re.S)
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)   # links and images: keep their text
    text = re.sub(r"https?://\S+", "", text)
    text = re.sub(r"[*_`#>|~]+", "", text)
    text = re.sub(r"^\s*[-+]\s+", "", text, flags=re.M)      # list bullets
    return re.sub(r"\s+", " ", text).strip()[:MAX_CHARS]


async def tts(request: web.Request) -> web.StreamResponse:
    if KEY and request.headers.get("X-Key") != KEY:
        raise web.HTTPUnauthorized()
    text = clean(await request.text())
    if not text:
        raise web.HTTPBadRequest(text="empty")
    resp = web.StreamResponse(headers={"Content-Type": "audio/mpeg"})
    await resp.prepare(request)
    async for chunk in edge_tts.Communicate(text, VOICE).stream():
        if chunk["type"] == "audio":
            await resp.write(chunk["data"])
    await resp.write_eof()
    return resp


async def tts_pcm(request: web.Request) -> web.Response:
    if KEY and request.headers.get("X-Key") != KEY:
        raise web.HTTPUnauthorized()
    text = clean(await request.text())
    if not text:
        raise web.HTTPBadRequest(text="empty")

    mp3 = bytearray()
    try:
        async for chunk in edge_tts.Communicate(text, VOICE).stream():
            if chunk["type"] == "audio":
                mp3.extend(chunk["data"])
    except Exception as exc:
        raise web.HTTPBadGateway(text=f"edge-tts: {type(exc).__name__}") from exc
    if not mp3:
        raise web.HTTPBadGateway(text="empty audio")

    proc = await asyncio.create_subprocess_exec(
        "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
        "-f", "mp3", "-i", "pipe:0", "-vn", "-ac", "1", "-ar", "16000",
        "-f", "s16le", "-acodec", "pcm_s16le", "pipe:1",
        stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    try:
        pcm, _ = await asyncio.wait_for(proc.communicate(bytes(mp3)), timeout=30)
    except asyncio.TimeoutError as exc:
        proc.kill()
        await proc.wait()
        raise web.HTTPGatewayTimeout(text="audio conversion timed out") from exc
    if proc.returncode != 0 or not pcm:
        raise web.HTTPBadGateway(text="MP3 to PCM conversion failed")
    if len(pcm) & 1:
        pcm = pcm[:-1]
    return web.Response(
        body=pcm,
        headers={
            "Content-Type": "application/octet-stream",
            "X-Audio-Format": "s16le;rate=16000;channels=1",
        },
    )


app = web.Application(client_max_size=64 * 1024)
app.router.add_post("/tts", tts)
app.router.add_post("/tts-pcm", tts_pcm)

if __name__ == "__main__":
    web.run_app(app, host="0.0.0.0", port=int(os.environ.get("PORT", "8020")))
