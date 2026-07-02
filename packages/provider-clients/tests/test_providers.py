"""Provider-client shape tests with SDKs stubbed out."""

from __future__ import annotations

import sys
import types as pytypes
from pathlib import Path

import pytest

from ptv_providers import (
    AnthropicClient,
    AnthropicConfig,
    GoogleImageClient,
    GoogleImageConfig,
    ProviderCallError,
    ProviderConfigError,
    TripoClient,
    TripoConfig,
)
from ptv_providers.anthropic_client import _concat_text_blocks


# ---------------------------------------------------------------------------
# Anthropic
# ---------------------------------------------------------------------------

class _FakeUsage(dict):
    pass


class _FakeBlock:
    def __init__(self, text: str):
        self.type = "text"
        self.text = text


class _FakeMsg:
    def __init__(self, text: str):
        self.content = [_FakeBlock(text)]
        self.usage = _FakeUsage({"input_tokens": 1, "output_tokens": 2})


class _FakeMessages:
    def __init__(self, response_text: str):
        self._text = response_text
        self.last_kwargs: dict = {}

    def create(self, **kwargs):
        self.last_kwargs = kwargs
        return _FakeMsg(self._text)


class _FakeAnthropicSdk:
    def __init__(self, response_text: str):
        self._text = response_text
        self.last_instance: "_FakeAnthropicInstance" | None = None

        parent = self

        class _Instance:
            def __init__(self, api_key=None):
                self.api_key = api_key
                self.messages = _FakeMessages(parent._text)
                parent.last_instance = self

        self.Anthropic = _Instance


def test_anthropic_missing_key_raises():
    cfg = AnthropicConfig(api_key=None)
    c = AnthropicClient(cfg)
    c._sdk = _FakeAnthropicSdk('{"ok": true}')
    with pytest.raises(ProviderConfigError):
        c.generate_json(prompt="x", schema={"type": "object"})


def test_anthropic_structured_call_parses_json_and_uses_hard_model():
    cfg = AnthropicConfig(api_key="sk-test", hard_model="claude-opus-4-7")
    c = AnthropicClient(cfg)
    sdk = _FakeAnthropicSdk('{"foo": 1}')
    c._sdk = sdk
    result = c.generate_json(
        prompt="hi",
        schema={"type": "object", "properties": {"foo": {"type": "integer"}}},
    )
    assert result.data == {"foo": 1}
    assert result.model == "claude-opus-4-7"
    kwargs = sdk.last_instance.messages.last_kwargs
    assert kwargs["model"] == "claude-opus-4-7"
    assert kwargs["output_config"]["format"]["type"] == "json_schema"
    assert "schema" in kwargs["output_config"]["format"]


def test_anthropic_structured_call_rejects_bad_json():
    cfg = AnthropicConfig(api_key="sk-test")
    c = AnthropicClient(cfg)
    c._sdk = _FakeAnthropicSdk("not json")
    with pytest.raises(ProviderCallError):
        c.generate_json(prompt="x", schema={"type": "object"})


def test_anthropic_generate_code_allows_light_model_override():
    cfg = AnthropicConfig(api_key="sk-test", light_model="claude-sonnet-4-6")
    c = AnthropicClient(cfg)
    sdk = _FakeAnthropicSdk("// cpp code here")
    c._sdk = sdk
    r = c.generate_code(prompt="write code", model=cfg.light_model)
    assert r.model == "claude-sonnet-4-6"
    assert "// cpp code" in r.raw_text


def test_concat_text_blocks_handles_dicts_and_objects():
    assert _concat_text_blocks([]) == ""
    assert _concat_text_blocks([{"type": "text", "text": "a"}]) == "a"


# ---------------------------------------------------------------------------
# Google image
# ---------------------------------------------------------------------------

class _FakeGenImg:
    def __init__(self, data: bytes):
        class _Img:
            image_bytes = data

        self.image = _Img()


class _FakeImagenResp:
    def __init__(self, count: int, data: bytes):
        self.generated_images = [_FakeGenImg(data) for _ in range(count)]


class _FakeModels:
    def __init__(self, resp):
        self._resp = resp
        self.last_call: dict | None = None

    def generate_images(self, **kwargs):
        self.last_call = kwargs
        return self._resp

    def generate_content(self, **kwargs):
        self.last_call = kwargs
        # Simulate Gemini image part
        class _InlineData:
            data = b"PNGBYTES"
            mime_type = "image/png"

        class _Part:
            inline_data = _InlineData()

        class _Content:
            parts = [_Part()]

        class _Cand:
            content = _Content()

        class _Resp:
            candidates = [_Cand()]

        return _Resp()


class _FakeClient:
    def __init__(self, api_key: str | None = None):
        self.api_key = api_key
        self.models = _FakeModels(_FakeImagenResp(1, b"PNGBYTES"))


def _install_fake_genai(monkeypatch, client_cls=_FakeClient):
    genai_mod = pytypes.ModuleType("google.genai")
    genai_mod.Client = client_cls  # type: ignore[attr-defined]

    types_mod = pytypes.ModuleType("google.genai.types")

    class _Cfg:
        def __init__(self, **kwargs):
            for k, v in kwargs.items():
                setattr(self, k, v)

    types_mod.GenerateImagesConfig = _Cfg  # type: ignore[attr-defined]
    types_mod.GenerateContentConfig = _Cfg  # type: ignore[attr-defined]
    types_mod.ImageConfig = _Cfg  # type: ignore[attr-defined]

    google_pkg = pytypes.ModuleType("google")
    google_pkg.genai = genai_mod  # type: ignore[attr-defined]

    monkeypatch.setitem(sys.modules, "google", google_pkg)
    monkeypatch.setitem(sys.modules, "google.genai", genai_mod)
    monkeypatch.setitem(sys.modules, "google.genai.types", types_mod)


def test_google_imagen_writes_png(tmp_path, monkeypatch):
    _install_fake_genai(monkeypatch)
    cfg = GoogleImageConfig(api_key="key", imagen_model="imagen-4.0-generate-001")
    c = GoogleImageClient(cfg)
    with pytest.warns(DeprecationWarning, match="generate_with_imagen"):
        results = c.generate_with_imagen(
            prompt="a shiny cube",
            out_dir=tmp_path,
            number_of_images=1,
        )
    assert len(results) == 1
    r = results[0]
    assert r.path.exists()
    assert r.path.read_bytes() == b"PNGBYTES"
    assert r.model == "imagen-4.0-generate-001"


def test_google_gemini_writes_png(tmp_path, monkeypatch):
    _install_fake_genai(monkeypatch)
    cfg = GoogleImageConfig(
        api_key="key", gemini_image_model="gemini-3.1-flash-image"
    )
    c = GoogleImageClient(cfg)
    results = c.generate_with_gemini(prompt="a car", out_dir=tmp_path)
    assert len(results) == 1
    assert results[0].path.read_bytes() == b"PNGBYTES"
    assert results[0].model == "gemini-3.1-flash-image"


def test_google_generate_defaults_to_gemini(tmp_path, monkeypatch):
    _install_fake_genai(monkeypatch)
    cfg = GoogleImageConfig(api_key="key")
    c = GoogleImageClient(cfg)
    results = c.generate(prompt="a tree", out_dir=tmp_path)
    assert len(results) == 1
    assert results[0].model == "gemini-3.1-flash-image"


def test_google_missing_key_raises(monkeypatch, tmp_path):
    _install_fake_genai(monkeypatch)
    cfg = GoogleImageConfig(api_key=None)
    c = GoogleImageClient(cfg)
    with pytest.raises(ProviderConfigError):
        c.generate_with_gemini(prompt="x", out_dir=tmp_path)


# ---------------------------------------------------------------------------
# Tripo
# ---------------------------------------------------------------------------

class _FakeTripoSdk:
    def __init__(self, tmp_path: Path):
        self._tmp = tmp_path
        self.last_image_kwargs: dict | None = None

        outer = self

        class _AsyncCtx:
            def __init__(self, api_key: str):
                self.api_key = api_key

            async def __aenter__(self):
                return self

            async def __aexit__(self, *exc):
                return False

            async def image_to_model(self, **kwargs):
                outer.last_image_kwargs = kwargs
                return "fake-task-id"

            async def text_to_model(self, **kwargs):
                outer.last_image_kwargs = kwargs
                return "fake-task-id-text"

            async def wait_for_task(self, task_id, **kw):
                class _Task:
                    status = "success"

                return _Task()

            async def download_task_models(self, task, out_dir: str):
                p = Path(out_dir) / "model.glb"
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b"glb!")
                return {"pbr_model": str(p)}

        self.TripoClient = _AsyncCtx


def test_tripo_image_to_model_forces_texture_and_pbr(tmp_path):
    cfg = TripoConfig(api_key="tripo-key")
    c = TripoClient(cfg)
    fake = _FakeTripoSdk(tmp_path)
    c._sdk = fake

    img = tmp_path / "ref.png"
    img.write_bytes(b"PNG")
    result = c.image_to_model(image_path=img, out_dir=tmp_path / "out")

    assert result.task_id == "fake-task-id"
    assert result.status == "success"
    assert result.primary_glb is not None
    assert result.primary_glb.read_bytes() == b"glb!"

    kw = fake.last_image_kwargs
    # Quality defaults verified against Tripo's platform docs
    # (https://platform.tripo3d.ai/docs/generation, Apr 2026):
    # textured + PBR, image-aligned, detailed texture+geometry,
    # autofix on, oriented to the source image, latest v3.1 checkpoint.
    assert kw["texture"] is True
    assert kw["pbr"] is True
    assert kw["texture_alignment"] == "original_image"
    assert kw["texture_quality"] == "detailed"
    assert kw["geometry_quality"] == "detailed"
    assert kw["enable_image_autofix"] is True
    assert kw["auto_size"] is True
    assert kw["orientation"] == "align_image"
    assert kw["model_version"] == "v3.1-20260211"
    assert kw["image"] == str(img)


def test_tripo_text_to_model_carries_quality_defaults(tmp_path):
    cfg = TripoConfig(api_key="tripo-key")
    c = TripoClient(cfg)
    fake = _FakeTripoSdk(tmp_path)
    c._sdk = fake

    c.text_to_model(prompt="a brass key", out_dir=tmp_path / "out")
    kw = fake.last_image_kwargs
    assert kw["texture"] is True
    assert kw["pbr"] is True
    assert kw["texture_quality"] == "detailed"
    assert kw["geometry_quality"] == "detailed"
    assert kw["enable_image_autofix"] is True
    assert kw["model_version"] == "v3.1-20260211"
    # text_to_model has no ``orientation`` kwarg in the SDK signature,
    # so the wrapper must NOT inject one.
    assert "orientation" not in kw
    assert kw["prompt"] == "a brass key"


def test_tripo_geometry_quality_skipped_for_legacy_v25(tmp_path):
    cfg = TripoConfig(api_key="tripo-key", model_version="v2.5-20250123")
    c = TripoClient(cfg)
    fake = _FakeTripoSdk(tmp_path)
    c._sdk = fake

    img = tmp_path / "ref.png"
    img.write_bytes(b"PNG")
    c.image_to_model(image_path=img, out_dir=tmp_path / "out")
    kw = fake.last_image_kwargs
    # v2.5 rejects geometry_quality; the wrapper should drop it.
    assert "geometry_quality" not in kw
    # All other quality knobs should still be honored.
    assert kw["texture_quality"] == "detailed"
    assert kw["enable_image_autofix"] is True
    assert kw["model_version"] == "v2.5-20250123"


def test_tripo_missing_key_raises(tmp_path):
    cfg = TripoConfig(api_key=None)
    c = TripoClient(cfg)
    c._sdk = _FakeTripoSdk(tmp_path)
    with pytest.raises(ProviderConfigError):
        c.image_to_model(image_path="x.png", out_dir=tmp_path)
