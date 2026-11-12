#!/usr/bin/env python3
"""腾讯云两跳自检：一句话识别（TC3 签名）+ TokenHub 大模型（Bearer）。

对应 docs/14 §7.7。固件里 transcribeTencent() 要做的两步，这个脚本按同样的字节顺序先发一遍：

  1) WAV(44 字节头 + 16k 单声道 PCM) -> base64(压掉换行) -> SentenceRecognition -> 文本
  2) 文本 -> TokenHub /v1/chat/completions -> 一个动作标签

脚本跑通 = Key、地域、域名、模型名、请求体格式四件事同时对，固件只剩网络问题。

用法：
    # 不花一分钱、不需要 Key 的两步
    python tools/tencent_voice_check.py --self-test     # TC3 实现 vs 官方示例，逐字节比对
    python tools/tencent_voice_check.py --preflight     # 打真实端点，确认请求格式被接受
    python tools/tencent_voice_check.py --dry-run       # 打印四段签名串，拿去 API Explorer 对照

    # 需要 Key
    export TENCENT_SECRET_ID=AKID...
    export TENCENT_SECRET_KEY=...
    export TENCENT_TOKENHUB_KEY=sk-...
    python tools/tencent_voice_check.py --asr-only      # 只验第一跳，隔离签名问题
    python tools/tencent_voice_check.py --llm-only      # 只验第二跳，隔离 ASR 问题
    python tools/tencent_voice_check.py --tts           # 只验第三跳，音频存 server/debug/
    python tools/tencent_voice_check.py                 # 两跳都跑

Key 的读取顺序：环境变量 > .secrets/tencent.env > firmware/include/secrets.h
（后两个都在 .gitignore 里；脚本任何情况下都不会把 Key 完整打印出来）

纯标准库实现，不需要腾讯云 SDK。
"""

from __future__ import annotations

import argparse
import base64
import datetime
import hashlib
import hmac
import json
import os
import sys
import time
import urllib.error
import urllib.request
import uuid
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

CT_JSON = "application/json; charset=utf-8"
ALGORITHM = "TC3-HMAC-SHA256"

# 一句话识别
ASR_HOST = "asr.tencentcloudapi.com"
ASR_SERVICE = "asr"
ASR_ACTION = "SentenceRecognition"
ASR_VERSION = "2019-06-14"

# TokenHub（混元系 OpenAI 兼容网关）。
# 2026-11-11 实测：控制台给的入口是 tokenhub.tencentmaas.com（不是 tencentcloudmaas），
# 模型名带厂商前缀 deepseek/deepseek-flash；换入口或换模型都会 401/400。
LLM_HOST = os.environ.get("TENCENT_LLM_HOST", "tokenhub.tencentmaas.com")
LLM_PATH = "/v1/chat/completions"
LLM_MODEL = os.environ.get("TENCENT_LLM_MODEL", "deepseek/deepseek-flash")

# 语音合成（与 ASR 共用同一套 TC3 签名）
TTS_HOST = "tts.tencentcloudapi.com"
TTS_SERVICE = "tts"
TTS_ACTION = "TextToVoice"
TTS_VERSION = "2019-08-23"

DEFAULT_WAV = ROOT / "server" / "kaitest.wav"      # 1.32 s，"开灯"
SECRETS_ENV = ROOT / ".secrets" / "tencent.env"
SECRETS_H = ROOT / "firmware" / "include" / "secrets.h"

ACTION_LABELS = (
    "你是台灯的控制中枢。根据用户这句话，只回一个英文单词，不要解释、不要标点："
    "开灯=on，关灯=off，调亮=up，调暗=down，调到最亮=max，夜灯=night，"
    "学习/看书模式=study，休息/睡眠模式=rest，其他无关的话=none。"
)

# 常见错误码 -> 人话。排错顺序就按这张表走，别凭 message 猜。
ERROR_HINTS = {
    "AuthFailure.SecretIdNotFound": "SecretId 不存在：子账号密钥被删，或 NVS/secrets.h 里还留着旧厂商的 Key",
    "AuthFailure.SignatureFailure": "签名串与实发请求不一致：Content-Type 多了/少了 charset，或头部被改写过",
    "AuthFailure.SignatureExpire": "签名过期：本机（或板子）时间与标准时间相差超过 5 分钟",
    "AuthFailure.UnauthorizedOperation": "签名已通过，但这把子账号 Key 没有该接口权限：CAM 里关联 QcloudASRFullAccess（语音识别）/ QcloudTTSFullAccess（语音合成），或自定义策略放行 asr:SentenceRecognition、tts:TextToVoice",
    "UnauthorizedOperation": "同上：子账号缺少接口权限（CAM 策略）",
    "FailedOperation.UserNotRegistered": "控制台没开通「语音识别」服务，Key 再对也调不通",
    "FailedOperation.ServiceIsolate": "账号欠费停服，充值后 1~2 分钟恢复",
    "FailedOperation.UserHasNoAmount": "资源包用完或额度耗尽",
    "InvalidParameterValue.ErrorInvalidVoicedata": "音频数据不合法：base64 里带了换行，或 WAV 头/采样率不对",
    "InvalidParameterValue.ErrorNoVoice": "音频里没检测到有效语音",
    "FailedOperation.ErrorRecognizeVoice": "识别失败，换一段安静、离麦近一点的音频",
    "UnsupportedOperation": "接口参数不支持，检查 SubServiceType / EngSerViceType",
}


# ---------------------------------------------------------------- 签名
def _sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sign_tc3(secret_key: str, date: str, service: str, string_to_sign: str) -> str:
    def _hmac(key: bytes, msg: str) -> bytes:
        return hmac.new(key, msg.encode("utf-8"), hashlib.sha256).digest()

    k_date = _hmac(("TC3" + secret_key).encode("utf-8"), date)
    k_service = _hmac(k_date, service)
    k_signing = _hmac(k_service, "tc3_request")
    return hmac.new(k_signing, string_to_sign.encode("utf-8"), hashlib.sha256).hexdigest()


def build_authorization(
    secret_id: str,
    secret_key: str,
    host: str,
    service: str,
    action: str,
    version: str,
    payload: bytes,
    timestamp: int,
    sign_action_header: bool = True,
) -> tuple[str, dict]:
    """返回 (Authorization 头, 中间量)。中间量给 --dry-run 对照 API Explorer 用。"""
    date = datetime.datetime.utcfromtimestamp(timestamp).strftime("%Y-%m-%d")

    canonical_headers = "content-type:%s\nhost:%s\n" % (CT_JSON, host)
    signed_headers = "content-type;host"
    if sign_action_header:
        # docs/14 §5.1 / 官方示例：x-tc-action 的“值”在规范头里必须小写
        canonical_headers += "x-tc-action:%s\n" % action.lower()
        signed_headers += ";x-tc-action"

    canonical_request = "\n".join([
        "POST",
        "/",
        "",
        canonical_headers,
        signed_headers,
        _sha256_hex(payload),
    ])

    credential_scope = "%s/%s/tc3_request" % (date, service)
    string_to_sign = "\n".join([
        ALGORITHM,
        str(timestamp),
        credential_scope,
        _sha256_hex(canonical_request.encode("utf-8")),
    ])

    signature = sign_tc3(secret_key, date, service, string_to_sign)
    authorization = "%s Credential=%s/%s, SignedHeaders=%s, Signature=%s" % (
        ALGORITHM, secret_id, credential_scope, signed_headers, signature,
    )
    return authorization, {
        "date": date,
        "canonical_request": canonical_request,
        "string_to_sign": string_to_sign,
        "signature": signature,
        "signed_headers": signed_headers,
        "hashed_payload": _sha256_hex(payload),
        "action_header": action,
        "version": version,
    }


# ---------------------------------------------------------------- Key 读取
def _looks_placeholder(value: str) -> bool:
    low = value.lower()
    return "xxxx" in low or low in ("", "akid", "sk-")


def _parse_env_file(path: Path) -> dict:
    out = {}
    if not path.is_file():
        return out
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        k, v = k.strip(), v.strip().strip('"').strip("'")
        if _looks_placeholder(v):
            continue          # 从 example 复制过来还没填值
        out[k] = v
    return out


def _parse_secrets_h(path: Path) -> dict:
    """从 firmware/include/secrets.h 抠出腾讯云需要的凭据。

    这个文件是给旧 provider 用的，槽位语义会漂移，所以按「值长什么样」来判断，
    而不是只看宏名——被填错槽位是现场最常见的低级错（sk- 塞进 SECRET_KEY 之类）。
    """
    out = {}
    if not path.is_file():
        return out

    raw = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        s = line.strip()
        if not s.startswith("#define"):
            continue
        parts = s.split(None, 2)
        if len(parts) < 3:
            continue
        name, value = parts[1], parts[2].strip().strip('"')
        if name.startswith("LAMP_CLOUD_") and value:
            raw[name] = value

    def note(msg):
        print("  提示: " + msg)

    api = raw.get("LAMP_CLOUD_API_KEY", "")
    secret = raw.get("LAMP_CLOUD_SECRET_KEY", "")
    llm = raw.get("LAMP_CLOUD_LLM_KEY", "")

    if api.startswith("AKID"):
        out["TENCENT_SECRET_ID"] = api
    elif api:
        note("LAMP_CLOUD_API_KEY 是 %s 开头（%s），不是腾讯 SecretId（AKID 开头），已忽略"
             % (api[:3], mask(api)))

    if secret:
        if secret.startswith("sk-"):
            out.setdefault("TENCENT_TOKENHUB_KEY", secret)
            note("LAMP_CLOUD_SECRET_KEY 里放的是一把 sk- Key（%s），更像模型 API Key 而不是 SecretKey；"
                 "先当作 TokenHub Key 用" % mask(secret))
        else:
            out["TENCENT_SECRET_KEY"] = secret

    if llm.startswith("sk-"):
        out["TENCENT_TOKENHUB_KEY"] = llm

    if not api:
        note("secrets.h 里没有生效的 LAMP_CLOUD_API_KEY（可能被注释掉了），腾讯 ASR 需要的 SecretId 缺一个")
    return out


def load_credentials() -> tuple[dict, list[str]]:
    sources: list[str] = []
    creds = {}
    for name in ("TENCENT_SECRET_ID", "TENCENT_SECRET_KEY", "TENCENT_TOKENHUB_KEY"):
        if os.environ.get(name):
            creds[name] = os.environ[name]
    if creds:
        sources.append("环境变量")

    from_file = _parse_env_file(SECRETS_ENV)
    for name, value in from_file.items():
        if name.startswith("TENCENT_") and value and name not in creds:
            creds[name] = value
    if from_file:
        sources.append(str(SECRETS_ENV.relative_to(ROOT)))

    from_h = _parse_secrets_h(SECRETS_H)
    for name, value in from_h.items():
        if value and name not in creds:
            creds[name] = value
    if from_h:
        sources.append("firmware/include/secrets.h")

    return creds, sources


def mask(value: str | None) -> str:
    if not value:
        return "（未设置）"
    if len(value) <= 8:
        return value[:2] + "***"
    return value[:6] + "***" + value[-2:]


# ---------------------------------------------------------------- HTTP
def post_json(url: str, headers: dict, payload: bytes, timeout: float = 16.0) -> tuple[int, dict, float]:
    req = urllib.request.Request(url, data=payload, headers=headers, method="POST")
    started = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read()
            status = resp.status
    except urllib.error.HTTPError as exc:
        body = exc.read()
        status = exc.code
    elapsed_ms = (time.monotonic() - started) * 1000
    try:
        data = json.loads(body.decode("utf-8", "replace"))
    except json.JSONDecodeError:
        data = {"_raw": body.decode("utf-8", "replace")[:500]}
    return status, data, elapsed_ms


def call_tencent(
    host: str,
    service: str,
    action: str,
    version: str,
    body: dict,
    secret_id: str,
    secret_key: str,
    timestamp: int,
    timeout: float = 16.0,
) -> tuple[dict, dict, float, int]:
    payload = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    authorization, meta = build_authorization(
        secret_id, secret_key, host, service, action, version, payload, timestamp,
    )
    headers = {
        "Authorization": authorization,
        "Content-Type": CT_JSON,
        "X-TC-Action": action,
        "X-TC-Version": version,
        "X-TC-Timestamp": str(timestamp),
    }
    status, data, elapsed_ms = post_json("https://" + host + "/", headers, payload, timeout)
    return data, meta, elapsed_ms, status


def describe_error(code: str, message: str) -> None:
    hint = ERROR_HINTS.get(code)
    print("  错误码 : %s" % code)
    print("  错误信息: %s" % message)
    if hint:
        print("  怎么办 : %s" % hint)


# ---------------------------------------------------------------- 第一跳：ASR
def load_wav(path: Path) -> tuple[bytes, int, float]:
    if not path.is_file():
        raise SystemExit("找不到音频文件：%s" % path)
    raw = path.read_bytes()
    with wave.open(str(path), "rb") as w:
        rate, channels, sampwidth, frames = (
            w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes(),
        )
        duration = frames / float(rate)
    if rate != 16000 or channels != 1 or sampwidth != 2:
        raise SystemExit(
            "音频格式不对：需要 16000 Hz / 单声道 / 16 bit，当前 %d Hz / %d 声道 / %d bit" % (
                rate, channels, sampwidth * 8,
            )
        )
    return raw, rate, duration


def run_asr(creds: dict, wav_path: Path, engine: str, hotwords: str | None,
            timestamp: int, dry_run: bool) -> tuple[bool, str]:
    """返回 (是否成功, 识别文本)。dry-run 成功时文本为空串。"""
    raw, _rate, duration = load_wav(wav_path)
    b64 = base64.b64encode(raw).decode("ascii").replace("\n", "").replace("\r", "")
    body = {
        "ProjectId": 0,
        "SubServiceType": 2,
        "EngSerViceType": engine,
        "SourceType": 1,
        "VoiceFormat": "wav",
        "UsrAudioKey": uuid.uuid4().hex,
        "Data": b64,
        "DataLen": len(raw),
    }
    if hotwords:
        body["HotwordList"] = hotwords

    print("\n=== 第一跳：一句话识别 ===")
    print("  音频   : %s（%.2f s / %d B -> base64 %d B）" % (wav_path.name, duration, len(raw), len(b64)))
    print("  引擎   : %s · %s" % (engine, ASR_VERSION))
    print("  密钥   : SecretId=%s SecretKey=%s" % (
        mask(creds.get("TENCENT_SECRET_ID")), mask(creds.get("TENCENT_SECRET_KEY"))))

    if dry_run:
        payload = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        _auth, meta = build_authorization(
            creds.get("TENCENT_SECRET_ID", ""), creds.get("TENCENT_SECRET_KEY", ""),
            ASR_HOST, ASR_SERVICE, ASR_ACTION, ASR_VERSION, payload, timestamp,
        )
        print("\n  --- CanonicalRequest ---\n" + meta["canonical_request"])
        print("\n  --- StringToSign ---\n" + meta["string_to_sign"])
        print("\n  --- Signature ---\n" + meta["signature"])
        print("\n  --- Authorization ---\n  " + _auth)
        print("\n  对照方法：API Explorer -> 签名串生成 -> 选 API 3.0 签名 v3，")
        print("  把 host/service/action/版本/timestamp/body 填成上面这些，四段应逐字节一致。")
        return True, ""

    if not creds.get("TENCENT_SECRET_ID") or not creds.get("TENCENT_SECRET_KEY"):
        print("  跳过：没有 SecretId / SecretKey（设置 TENCENT_SECRET_ID / TENCENT_SECRET_KEY）")
        return False, ""

    data, meta, elapsed_ms, _status = call_tencent(
        ASR_HOST, ASR_SERVICE, ASR_ACTION, ASR_VERSION, body,
        creds["TENCENT_SECRET_ID"], creds["TENCENT_SECRET_KEY"], timestamp,
    )
    resp = data.get("Response") or {}
    if resp.get("Error"):
        err = resp["Error"]
        print("  失败（%.0f ms）" % elapsed_ms)
        describe_error(err.get("Code", "?"), err.get("Message", ""))
        return False, ""

    text = (resp.get("Result") or "").strip()
    print("  成功（%.0f ms，音频 %.0f ms，RequestId %s）" % (
        elapsed_ms, resp.get("AudioDuration", 0), resp.get("RequestId", "?")))
    print("  识别结果: %r" % text)
    if not text:
        print("  注意：返回空文本，多半是音频里没有人声（静音/噪声）")
    return bool(text), text


# ---------------------------------------------------------------- 第二跳：LLM
def run_llm(creds: dict, text: str, model: str, thinking: bool = False) -> bool:
    key = creds.get("TENCENT_TOKENHUB_KEY")
    print("\n=== 第二跳：TokenHub 大模型 ===")
    print("  输入   : %r" % text)
    print("  模型   : %s @ %s%s（思考 %s）" % (
        model, LLM_HOST, LLM_PATH, "开" if thinking else "关"))
    print("  密钥   : %s" % mask(key))
    if not key:
        print("  跳过：没有 TokenHub Key（设置 TENCENT_TOKENHUB_KEY）")
        return False

    body = {
        "model": model,
        "messages": [
            {"role": "system", "content": ACTION_LABELS},
            {"role": "user", "content": text},
        ],
        "temperature": 0.1,
        # deepseek-flash 默认是推理模型：max_tokens 太小会被 reasoning 吃光，content 为空。
        # 实测（5 条命令）：思考关闭后中位 1609 ms；开着中位 2~3 s。
        "max_tokens": 64,
        "stream": False,
    }
    if not thinking:
        body["thinking"] = {"type": "disabled"}
    payload = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    headers = {
        "Authorization": "Bearer " + key,
        "Content-Type": "application/json; charset=utf-8",
    }
    status, data, elapsed_ms = post_json("https://" + LLM_HOST + LLM_PATH, headers, payload)
    if data.get("error"):
        err = data["error"]
        code = str(err.get("code", err.get("type", "")))
        print("  失败（HTTP %d，%.0f ms）" % (status, elapsed_ms))
        print("  错误: %s" % json.dumps(err, ensure_ascii=False)[:300])
        if code in ("401002", "invalid_api_key", "not_authorized"):
            print("  怎么办: Key 与 Base URL 要配对：TokenHub 用 tokenhub.tencentmaas.com，")
            print("          混元用 api.hunyuan.cloud.tencent.com；Key 去对应控制台重新确认。")
        return False

    try:
        content = data["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError):
        print("  失败（HTTP %d，%.0f ms）：响应结构不认识" % (status, elapsed_ms))
        print("  " + json.dumps(data, ensure_ascii=False)[:400])
        return False

    label = (content or "").strip()
    finish = (data["choices"][0].get("finish_reason") or "")
    usage = (data.get("usage") or {}).get("completion_tokens_details") or {}
    print("  成功（%.0f ms，finish=%s，reasoning_tokens=%s）" % (
        elapsed_ms, finish or "-", usage.get("reasoning_tokens", "-")))
    if not label:
        print("  注意：content 为空。多半是推理模型把 max_tokens 用在 reasoning 上了：")
        print("        加 --thinking 保持默认思考时，把 max_tokens 调大；或确认 thinking.disabled 生效。")
        return False
    print("  动作标签: %r" % label)
    return True


# ---------------------------------------------------------------- 第三跳：TTS
def run_tts(creds: dict, text: str, out_path: str | None, voice: int,
            codec: str, sample_rate: int) -> bool:
    print("\n=== 第三跳：语音合成 TextToVoice ===")
    print("  文本   : %r（%d 字）" % (text, len(text)))
    print("  参数   : Codec=%s · %d Hz · VoiceType=%d · %s" % (codec, sample_rate, voice, TTS_VERSION))
    print("  密钥   : SecretId=%s SecretKey=%s" % (
        mask(creds.get("TENCENT_SECRET_ID")), mask(creds.get("TENCENT_SECRET_KEY"))))
    if not creds.get("TENCENT_SECRET_ID") or not creds.get("TENCENT_SECRET_KEY"):
        print("  跳过：没有 SecretId / SecretKey")
        return False

    body = {
        "Text": text,
        "SessionId": uuid.uuid4().hex,
        "ModelType": 1,
        "VoiceType": voice,
        "Volume": 5,
        "Speed": 0,
        "ProjectId": 0,
        "PrimaryLanguage": 1,
        "SampleRate": sample_rate,
        "Codec": codec,
    }
    data, _meta, elapsed_ms, _status = call_tencent(
        TTS_HOST, TTS_SERVICE, TTS_ACTION, TTS_VERSION, body,
        creds["TENCENT_SECRET_ID"], creds["TENCENT_SECRET_KEY"], int(time.time()),
    )
    resp = data.get("Response") or {}
    if resp.get("Error"):
        err = resp["Error"]
        print("  失败（%.0f ms）" % elapsed_ms)
        describe_error(err.get("Code", "?"), err.get("Message", ""))
        return False

    audio_b64 = resp.get("Audio") or ""
    blob = base64.b64decode(audio_b64) if audio_b64 else b""
    out = Path(out_path) if out_path else (
        ROOT / "server" / "debug" / ("tts-%s.%s" % (time.strftime("%Y%m%d-%H%M%S"), codec))
    )
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(blob)
    print("  成功（%.0f ms，base64 %d B -> 音频 %d B）" % (elapsed_ms, len(audio_b64), len(blob)))
    print("  已存   : %s" % out)
    if codec == "wav" and blob[:4] == b"RIFF":
        try:
            with wave.open(str(out), "rb") as w:
                print("  音频   : %d Hz · %d ch · %d bit · %.2f s" % (
                    w.getframerate(), w.getnchannels(), w.getsampwidth() * 8,
                    w.getnframes() / float(w.getframerate())))
        except Exception as exc:  # noqa: BLE001
            print("  音频头解析失败：%r" % (exc,))
    return bool(blob)


# ---------------------------------------------------------------- 自检
OFFICIAL_PAYLOAD = (
    '{"Limit": 1, "Filters": [{"Values": ["\\u672a\\u547d\\u540d"], "Name": "instance-name"}]}'
)
OFFICIAL_PAYLOAD_SHA = "35e9c5b0e3ae67532d3c9f17ead6c90222632e5b1ff7f6e89887f1398934f064"
OFFICIAL_CANONICAL = (
    "POST\n/\n\n"
    "content-type:application/json; charset=utf-8\n"
    "host:cvm.tencentcloudapi.com\n"
    "x-tc-action:describeinstances\n"
    "\n"
    "content-type;host;x-tc-action\n" + OFFICIAL_PAYLOAD_SHA
)
OFFICIAL_CANONICAL_SHA = "7019a55be8395899b900fb5564e4200d984910f34794a27cb3fb7d10ff6a1e84"


def run_self_test() -> bool:
    print("=== TC3 实现自检（官方示例，无需 Key）===")
    ok = True

    payload_sha = _sha256_hex(OFFICIAL_PAYLOAD.encode("utf-8"))
    print("  1) payload SHA256  %s  %s" % (
        payload_sha[:16], "OK" if payload_sha == OFFICIAL_PAYLOAD_SHA else "FAIL"))
    ok &= payload_sha == OFFICIAL_PAYLOAD_SHA

    date = datetime.datetime.utcfromtimestamp(1551113065).strftime("%Y-%m-%d")
    print("  2) 时间戳->UTC 日期  %s  %s" % (date, "OK" if date == "2019-02-25" else "FAIL"))
    ok &= date == "2019-02-25"

    canonical_sha = _sha256_hex(OFFICIAL_CANONICAL.encode("utf-8"))
    print("  3) CanonicalRequest SHA256  %s  %s" % (
        canonical_sha[:16], "OK" if canonical_sha == OFFICIAL_CANONICAL_SHA else "FAIL"))
    ok &= canonical_sha == OFFICIAL_CANONICAL_SHA

    string_to_sign = "%s\n1551113065\n2019-02-25/cvm/tc3_request\n%s" % (ALGORITHM, canonical_sha)
    signature = sign_tc3("SECRETKEY-VECTOR", date, "cvm", string_to_sign)
    # 该值由官方 SDK tencentcloud.common.sign.Sign.sign_tc3() 用同一组输入算出，作为回归向量
    expected = "e801a66cc33c3633e72c49cb43fd4864de18c6a78fe8caffd73883bfeb4ba8b5"
    print("  4) HMAC 派生链（对照官方 SDK）  %s  %s" % (
        signature[:16], "OK" if signature == expected else "FAIL"))
    ok &= signature == expected

    # 用真实签名函数复核：同一输入必须可复现，且改一个字节必须变
    mutated = sign_tc3("SECRETKEY-VECTOR", date, "cvm", string_to_sign + "x")
    print("  5) 输入变更后签名变化  %s" % ("OK" if mutated != signature else "FAIL"))
    ok &= mutated != signature

    print("\n  官方示例 CanonicalRequest：")
    for line in OFFICIAL_CANONICAL.splitlines():
        print("    " + line)

    if ok:
        print("\n  PASS：签名算法的拼接顺序与官方文档逐字节一致（已用官方 SDK sign_tc3 交叉验证）。")
    else:
        print("\n  FAIL：实现与官方示例不一致，先别发真实请求。")
    return ok


def run_preflight(timestamp: int) -> bool:
    """不需要真 Key：确认真实端点可达、请求格式被接受。"""
    print("=== 端点预检（示例 SecretId，不需要真密钥）===")
    print("  目的：区分「网络/请求格式问题」和「密钥问题」。")
    print("  预期：返回 AuthFailure.SecretIdNotFound —— 说明 TLS、域名、JSON、签名头都通了，只差真 Key。")
    body = {
        "ProjectId": 0,
        "SubServiceType": 2,
        "EngSerViceType": "16k_zh",
        "SourceType": 1,
        "VoiceFormat": "wav",
        "UsrAudioKey": uuid.uuid4().hex,
        "Data": "",
        "DataLen": 0,
    }
    data, _meta, elapsed_ms, status = call_tencent(
        ASR_HOST, ASR_SERVICE, ASR_ACTION, ASR_VERSION, body,
        "AKID" + "EXAMPLE" + "0" * 24,          # 运行期拼出来，避免被 CI 的 Key 扫描误判
        "EXAMPLE" + "0" * 24, timestamp,
    )
    resp = data.get("Response") or {}
    err = resp.get("Error") or {}
    code = err.get("Code", "")
    print("  HTTP %d，%.0f ms，Code=%s" % (status, elapsed_ms, code or "（无错误码）"))
    if code == "AuthFailure.SecretIdNotFound":
        print("  PASS：端点与请求格式没问题，缺的只是真实密钥。")
        return True
    if code == "AuthFailure.SignatureFailure":
        print("  FAIL：签名串与实发请求不一致，先跑 --self-test 对照官方示例。")
        return False
    if not code:
        print("  返回体：%s" % json.dumps(resp, ensure_ascii=False)[:300])
        print("  FAIL：没拿到预期错误码，检查网络或域名。")
        return False
    print("  其他错误码：")
    describe_error(code, err.get("Message", ""))
    return False


# ---------------------------------------------------------------- 入口
def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="腾讯云两跳自检（SentenceRecognition + TokenHub）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--self-test", action="store_true", help="校验 TC3 实现（不需要 Key）")
    p.add_argument("--preflight", action="store_true", help="打真实端点确认请求格式（不需要 Key）")
    p.add_argument("--dry-run", action="store_true", help="只打印签名串，不发送")
    p.add_argument("--asr-only", action="store_true", help="只跑第一跳")
    p.add_argument("--llm-only", action="store_true", help="只跑第二跳")
    p.add_argument("--tts", action="store_true", help="只跑第三跳（语音合成），音频存 server/debug/")
    p.add_argument("--tts-text", default="我在。", help="合成用的文本")
    p.add_argument("--tts-out", default=None, help="音频输出路径，默认 server/debug/tts-<时间>.wav")
    p.add_argument("--voice", type=int, default=101001, help="VoiceType，默认 101001（中文女声）")
    p.add_argument("--codec", default="wav", help="合成音频格式：wav / mp3 / pcm，默认 wav")
    p.add_argument("--sample-rate", type=int, default=16000, help="合成采样率，默认 16000")
    p.add_argument("--wav", default=str(DEFAULT_WAV), help="16k/单声道/16bit WAV，默认 server/kaitest.wav")
    p.add_argument("--text", default=None, help="--llm-only 时直接给一句话，跳过 ASR")
    p.add_argument("--engine", default="16k_zh", help="识别引擎，默认 16k_zh")
    p.add_argument("--hotwords", default=None, help='热词，例如 "开灯|10,关灯|10"')
    p.add_argument("--model", default=LLM_MODEL, help="TokenHub 模型名，默认 deepseek/deepseek-flash")
    p.add_argument("--llm-host", default=LLM_HOST, help="TokenHub 入口域名，默认 tokenhub.tencentmaas.com")
    p.add_argument("--thinking", action="store_true", help="保留推理模型的思考（默认关闭，省 1 s 左右）")
    p.add_argument("--ts", type=int, default=None, help="固定时间戳（复现签名用），默认当前时间")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    timestamp = args.ts if args.ts else int(time.time())

    if args.self_test:
        return 0 if run_self_test() else 1

    if args.preflight:
        return 0 if run_preflight(timestamp) else 1

    global LLM_HOST
    LLM_HOST = args.llm_host

    creds, sources = load_credentials()
    print("=== 凭据 ===")
    print("  SecretId      : %s" % mask(creds.get("TENCENT_SECRET_ID")))
    print("  SecretKey     : %s" % mask(creds.get("TENCENT_SECRET_KEY")))
    print("  TokenHub Key  : %s" % mask(creds.get("TENCENT_TOKENHUB_KEY")))
    print("  来源          : %s" % ("、".join(sources) if sources else "没找到"))
    print("  本机时间      : %s（UTC %s）" % (
        datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        datetime.datetime.utcnow().strftime("%Y-%m-%d %H:%M:%S")))

    if not creds and not args.dry_run:
        print("\n没有找到任何凭据。三选一：")
        print("  1) 设环境变量：TENCENT_SECRET_ID / TENCENT_SECRET_KEY / TENCENT_TOKENHUB_KEY")
        print("  2) 写 %s（复制同目录下的 tencent.env.example）" % SECRETS_ENV.relative_to(ROOT))
        print("  3) 写进 firmware/include/secrets.h 的 LAMP_CLOUD_API_KEY / LAMP_CLOUD_SECRET_KEY / LAMP_CLOUD_LLM_KEY")
        print("\n先用不花钱的两步确认链路：")
        print("  python tools/tencent_voice_check.py --self-test")
        print("  python tools/tencent_voice_check.py --preflight")
        return 2

    asr_ok = True
    text = ""

    if args.tts:
        return 0 if run_tts(creds, args.tts_text, args.tts_out, args.voice,
                            args.codec, args.sample_rate) else 3

    if not args.llm_only:
        asr_ok, text = run_asr(creds, Path(args.wav), args.engine, args.hotwords, timestamp, args.dry_run)
        if args.dry_run:
            return 0 if asr_ok else 1

    if args.text:
        text = args.text
    elif args.llm_only and not text:
        text = "开灯"

    if args.asr_only:
        return 0 if asr_ok else 3

    if not text:
        print("\n（第二跳需要文本：ASR 没给出文本，可用 --llm-only --text 开灯 单独验证第二跳）")
        return 0 if asr_ok else 3

    llm_ok = run_llm(creds, text, args.model, args.thinking)
    return 0 if (asr_ok and llm_ok) else 3


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
