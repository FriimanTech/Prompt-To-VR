"""Provider-client shape tests with SDKs stubbed out."""

from __future__ import annotations

import json
import sys
import types as pytypes
from pathlib import Path
from unittest import mock

import httpx
import pytest
import respx

from ptv_providers import (
    AnthropicClient,
    AnthropicConfig,
    GoogleImageClient,
    GoogleImageConfig,
    ProviderCallError,
    ProviderConfigError,
    TripoClient,
    TripoConfig,
    resolve_animations,
    rig_model_for_type,
)
from ptv_providers import tripo_client as tripo_client_mod
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

V3 = "https://openapi.tripo3d.ai/v3"
MODEL_URL = "https://cdn.example.test/out/rigged.glb"


def _cfg(**overrides) -> TripoConfig:
    """Client config with the real-world waits dialled out."""

    base = {
        "api_key": "tripo-key",
        "poll_interval_s": 0.0,
        "min_upload_interval_s": 0.0,
    }
    base.update(overrides)
    return TripoConfig(**base)


def _ok(data: dict) -> httpx.Response:
    """A V3 success envelope."""

    return httpx.Response(200, json={"code": 0, "status": "success", "data": data})


def _mock_happy_path(respx_mock, *, endpoint: str) -> None:
    """Wire upload -> create -> poll -> download for one successful task."""

    respx_mock.post(f"{V3}/files").mock(
        return_value=_ok({"file_token": "file_abc123"})
    )
    respx_mock.post(f"{V3}{endpoint}").mock(return_value=_ok({"task_id": "task_1"}))
    respx_mock.get(f"{V3}/tasks/task_1").mock(
        return_value=_ok(
            {
                "task_id": "task_1",
                "status": "success",
                "output": {"model_url": MODEL_URL},
                "created_at": "2026-08-02T12:00:00Z",
                "credits_consumed": 20,
            }
        )
    )
    respx_mock.get(MODEL_URL).mock(return_value=httpx.Response(200, content=b"glb!"))


def _sent_body(respx_mock, endpoint: str) -> dict:
    request = next(
        call.request
        for call in respx_mock.calls
        if call.request.url.path.endswith(endpoint)
    )
    return json.loads(request.content)


@respx.mock
def test_tripo_image_to_model_sends_v3_body(tmp_path):
    _mock_happy_path(respx.mock, endpoint="/generation/image-to-model")
    img = tmp_path / "ref.png"
    img.write_bytes(b"PNG")

    result = TripoClient(_cfg()).image_to_model(
        image_path=img, out_dir=tmp_path / "out"
    )

    assert result.task_id == "task_1"
    assert result.status == "success"
    assert result.primary_glb is not None
    assert result.primary_glb.read_bytes() == b"glb!"

    body = _sent_body(respx.mock, "/generation/image-to-model")
    # V3 spells the checkpoint ``model`` (V2's ``model_version``) and REQUIRES
    # it -- there is no server-side default.
    assert body["model"] == "v3.1-20260211"
    assert "model_version" not in body
    # Native single-string input form, fed the token from POST /v3/files.
    assert body["input"] == "file_abc123"
    # Quality defaults: textured + PBR, image-aligned, Ultra-Mode geometry,
    # autofix on, oriented to the source image.
    assert body["texture"] is True
    assert body["pbr"] is True
    assert body["texture_alignment"] == "original_image"
    assert body["texture_quality"] == "standard"
    assert body["geometry_quality"] == "standard"
    assert body["enable_image_autofix"] is True
    assert body["auto_size"] is True
    assert body["orientation"] == "align_image"
    # V2's ``type`` discriminator is gone; the endpoint carries it now.
    assert "type" not in body


@respx.mock
def test_tripo_uploads_to_v3_files_and_reads_file_token(tmp_path):
    _mock_happy_path(respx.mock, endpoint="/generation/image-to-model")
    img = tmp_path / "ref.png"
    img.write_bytes(b"PNG")

    TripoClient(_cfg()).image_to_model(image_path=img, out_dir=tmp_path / "out")

    upload = next(
        call.request
        for call in respx.mock.calls
        if call.request.url.path.endswith("/files")
    )
    assert upload.method == "POST"
    assert b"ref.png" in upload.content
    assert upload.headers["Authorization"] == "Bearer tripo-key"


@respx.mock
def test_tripo_upload_without_file_token_fails_loudly(tmp_path):
    # V2 returned ``data.image_token``; on V3 that key is gone. If a stale
    # server or proxy hands one back we must fail rather than send garbage.
    respx.mock.post(f"{V3}/files").mock(
        return_value=_ok({"image_token": "0196-legacy-uuid"})
    )
    img = tmp_path / "ref.png"
    img.write_bytes(b"PNG")

    with pytest.raises(ProviderCallError, match="no file_token"):
        TripoClient(_cfg()).image_to_model(image_path=img, out_dir=tmp_path / "out")


@respx.mock
def test_tripo_text_to_model_omits_image_only_fields(tmp_path):
    _mock_happy_path(respx.mock, endpoint="/generation/text-to-model")

    TripoClient(_cfg()).text_to_model(
        prompt="a brass key", out_dir=tmp_path / "out"
    )

    body = _sent_body(respx.mock, "/generation/text-to-model")
    assert body["prompt"] == "a brass key"
    assert body["model"] == "v3.1-20260211"
    assert body["texture"] is True
    assert body["pbr"] is True
    assert body["geometry_quality"] == "standard"
    # No image, so no upload and none of the image-only knobs. Tripo fixes
    # texture_alignment server-side for this endpoint.
    assert "input" not in body
    assert "orientation" not in body
    assert "texture_alignment" not in body
    assert not [
        c for c in respx.mock.calls if c.request.url.path.endswith("/files")
    ]


@respx.mock
def test_tripo_model_version_defaults_when_caller_passes_none(tmp_path):
    # Regression: the pipeline used to pass ``model_version=<setting> or None``.
    # On V2 that silently downgraded to the server default (v2.5); on V3 it is
    # a hard 400, so the config must resolve it instead of omitting it.
    _mock_happy_path(respx.mock, endpoint="/generation/text-to-model")

    TripoClient(_cfg(model_version=None)).text_to_model(
        prompt="x", out_dir=tmp_path / "out"
    )

    assert _sent_body(respx.mock, "/generation/text-to-model")["model"] == (
        "v3.1-20260211"
    )


@pytest.mark.parametrize("version", ["v2.5-20250123", "P1-20260311"])
@respx.mock
def test_tripo_geometry_quality_omitted_where_unsupported(tmp_path, version):
    # v2.5 predates the parameter, and P1 rejects it outright (the
    # international service 400s even on an explicit false).
    _mock_happy_path(respx.mock, endpoint="/generation/text-to-model")

    TripoClient(_cfg(model_version=version)).text_to_model(
        prompt="x", out_dir=tmp_path / "out"
    )

    body = _sent_body(respx.mock, "/generation/text-to-model")
    assert "geometry_quality" not in body
    assert body["model"] == version
    # Everything else still applies.
    assert body["texture_quality"] == "standard"
    assert body["enable_image_autofix"] is True


def test_tripo_rejects_model_version_outside_v3_whitelist():
    # v2.5-20260210 is the auto-RIG model version, not a generation one; it is
    # a common copy-paste mistake and V3 would 400 on it mid-run.
    with pytest.raises(ProviderConfigError, match="Unsupported Tripo model"):
        TripoConfig(api_key="k", model_version="v2.5-20260210")


# ---------------------------------------------------------------------------
# Tripo auto-rigging
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "rig_type,expected",
    [
        ("biped", "v1.0-20240301"),
        ("quadruped", "v2.5-20260210"),
        ("hexapod", "v2.5-20260210"),
        ("octopod", "v2.5-20260210"),
        ("avian", "v2.5-20260210"),
        ("serpentine", "v2.5-20260210"),
        ("aquatic", "v2.5-20260210"),
        ("BIPED", "v1.0-20240301"),
    ],
)
def test_rig_model_follows_rig_type(rig_type, expected):
    # The two rigging checkpoints are not interchangeable: v1.0 is humanoid
    # only. Sending a quadruped to it does not error, it just rigs badly, so
    # this mapping is the only guard.
    assert rig_model_for_type(rig_type) == expected


def test_rig_model_rejects_unknown_rig_type():
    with pytest.raises(ProviderConfigError, match="Unknown Tripo rig_type"):
        rig_model_for_type("centaur")


def _mock_rig_check(respx_mock, *, riggable: bool, rig_type: str = "biped"):
    respx_mock.post(f"{V3}/animations/rig-check").mock(
        return_value=_ok({"task_id": "rigchk_1"})
    )
    respx_mock.get(f"{V3}/tasks/rigchk_1").mock(
        return_value=_ok(
            {
                "task_id": "rigchk_1",
                "status": "success",
                "output": {"riggable": riggable, "rig_type": rig_type},
                "credits_consumed": 0,
            }
        )
    )


@respx.mock
def test_tripo_rig_check_reports_riggability_without_downloading():
    _mock_rig_check(respx.mock, riggable=True, rig_type="quadruped")

    check = TripoClient(_cfg()).rig_check(task_id="task_src")

    assert check.riggable is True
    assert check.rig_type == "quadruped"
    # rig-check has no output.model_url, so the poll-only path must not try
    # to download anything.
    assert check.rig_model == "v2.5-20260210"
    body = _sent_body(respx.mock, "/animations/rig-check")
    # V3 spells this ``input``, NOT V2's ``original_model_task_id``.
    assert body == {"input": "task_src"}


@respx.mock
def test_tripo_rig_sends_tripo_spec_glb_and_type_matched_model(tmp_path):
    respx.mock.post(f"{V3}/animations/rig").mock(
        return_value=_ok({"task_id": "rig_1"})
    )
    respx.mock.get(f"{V3}/tasks/rig_1").mock(
        return_value=_ok(
            {
                "task_id": "rig_1",
                "status": "success",
                "output": {"model_url": MODEL_URL},
                "credits_consumed": 30,
            }
        )
    )
    respx.mock.get(MODEL_URL).mock(return_value=httpx.Response(200, content=b"glb!"))

    result = TripoClient(_cfg()).rig(
        out_dir=tmp_path / "out", task_id="task_src", rig_type="avian"
    )

    assert result.primary_glb is not None
    body = _sent_body(respx.mock, "/animations/rig")
    assert body["input"] == "task_src"
    assert body["spec"] == "tripo"
    assert body["out_format"] == "glb"
    assert body["rig_type"] == "avian"
    # Derived from rig_type, not from the generation checkpoint.
    assert body["model"] == "v2.5-20260210"


@respx.mock
def test_tripo_rig_uploads_the_glb_when_no_task_id_is_known(tmp_path):
    # Resume, or a project directory copied between machines: the generation
    # task id is gone, so the mesh itself has to be the input.
    respx.mock.post(f"{V3}/files").mock(
        return_value=_ok({"file_token": "file_rig1"})
    )
    _mock_rig_check(respx.mock, riggable=True)

    src = tmp_path / "model.glb"
    src.write_bytes(b"glTF")

    check = TripoClient(_cfg()).rig_check(glb_path=src)

    assert check.riggable is True
    assert _sent_body(respx.mock, "/animations/rig-check") == {
        "input": "file_rig1"
    }


def test_tripo_rig_without_any_input_fails_loudly():
    with pytest.raises(ProviderCallError, match="task_id or a glb_path"):
        TripoClient(_cfg()).rig_check()


# ---------------------------------------------------------------------------
# Tripo animation retargeting
# ---------------------------------------------------------------------------


def test_biped_intents_use_the_v1_namespaced_presets():
    presets, mapping, dropped = resolve_animations(
        ["idle", "walk", "run", "shoot"], "biped"
    )
    # A biped rigs on v1.0-20240301, whose presets are all ``preset:biped:*``.
    assert presets == [
        "preset:biped:idle",
        "preset:biped:walk",
        "preset:biped:run",
        "preset:biped:shoot",
    ]
    assert mapping["walk"] == "preset:biped:walk"
    assert dropped == []


def test_biped_on_v25_uses_the_short_unnamespaced_list():
    presets, _mapping, dropped = resolve_animations(
        ["idle", "attack"], "biped", "v2.5-20260210"
    )
    assert presets == ["preset:idle", "preset:slash"]
    assert dropped == []
    # dance/wave/sit only exist on the 90-preset v1.0 list.
    _p, _m, dropped2 = resolve_animations(["dance"], "biped", "v2.5-20260210")
    assert dropped2 == ["dance"]


@pytest.mark.parametrize(
    "rig_type,preset",
    [
        ("quadruped", "preset:quadruped:walk"),
        ("hexapod", "preset:hexapod:walk"),
        ("octopod", "preset:octopod:walk"),
        ("serpentine", "preset:serpentine:march"),
        ("aquatic", "preset:aquatic:march"),
    ],
)
def test_creature_rigs_have_one_locomotion_preset(rig_type, preset):
    presets, mapping, dropped = resolve_animations(
        ["idle", "walk", "run", "shoot"], rig_type
    )
    # Only locomotion exists. walk and run share the single preset, so it is
    # billed ONCE but both intents still resolve -- the runtime plays the same
    # clip faster for a run.
    assert presets == [preset]
    assert mapping == {"walk": preset, "run": preset}
    assert sorted(dropped) == ["idle", "shoot"]


def test_unknown_rig_type_fails_rather_than_guessing():
    with pytest.raises(ProviderConfigError, match="Unknown Tripo rig_type"):
        resolve_animations(["walk"], "centaur")


@respx.mock
def test_tripo_retarget_sends_a_single_animation_field_for_one_clip(tmp_path):
    _mock_happy_path(respx.mock, endpoint="/animations/retarget")

    TripoClient(_cfg()).retarget(
        out_dir=tmp_path / "out",
        rig_task_id="task_rig",
        animations=["preset:biped:walk"],
    )

    body = _sent_body(respx.mock, "/animations/retarget")
    assert body["input"] == "task_rig"
    # ``animation`` and ``animations`` are mutually exclusive; sending both
    # is a 400.
    assert body["animation"] == "preset:biped:walk"
    assert "animations" not in body
    assert body["out_format"] == "glb"
    assert body["bake_animation"] is True
    assert body["export_with_geometry"] is True
    # Tripo defaults this to false; leaving it there would bake root motion
    # into the clip and fight the engine for control of position.
    assert body["animate_in_place"] is True


@respx.mock
def test_tripo_retarget_batches_multiple_clips(tmp_path):
    _mock_happy_path(respx.mock, endpoint="/animations/retarget")

    TripoClient(_cfg()).retarget(
        out_dir=tmp_path / "out",
        rig_task_id="task_rig",
        animations=["preset:idle", "preset:walk", "preset:run"],
    )

    body = _sent_body(respx.mock, "/animations/retarget")
    assert body["animations"] == ["preset:idle", "preset:walk", "preset:run"]
    assert "animation" not in body


def test_tripo_retarget_requires_a_rig_task(tmp_path):
    # Retarget needs Tripo's own skeleton for that task; there is no
    # file-upload fallback the way rig-check has one.
    with pytest.raises(ProviderCallError, match="successful rig task"):
        TripoClient(_cfg()).retarget(
            out_dir=tmp_path, rig_task_id="", animations=["preset:walk"]
        )


def test_tripo_retarget_requires_at_least_one_animation(tmp_path):
    with pytest.raises(ProviderCallError, match="at least one preset"):
        TripoClient(_cfg()).retarget(
            out_dir=tmp_path, rig_task_id="task_rig", animations=[]
        )


@respx.mock
def test_tripo_failed_task_surfaces_error_code_and_message(tmp_path):
    respx.mock.post(f"{V3}/generation/text-to-model").mock(
        return_value=_ok({"task_id": "task_1"})
    )
    # V2's ``banned`` status is now ``failed`` + error_code 2008, so the code
    # is the only way to identify a moderation rejection.
    respx.mock.get(f"{V3}/tasks/task_1").mock(
        return_value=_ok(
            {
                "status": "failed",
                "error_code": 2008,
                "error_message": "input rejected by moderation",
            }
        )
    )

    with pytest.raises(ProviderCallError) as excinfo:
        TripoClient(_cfg()).text_to_model(prompt="x", out_dir=tmp_path / "out")
    assert "2008" in str(excinfo.value)
    assert "moderation" in str(excinfo.value)


@respx.mock
def test_tripo_success_without_model_url_fails(tmp_path):
    # V3 merges pbr_model / model / base_model into one ``model_url``; a
    # success with none of it is a broken response, not a silent no-op.
    respx.mock.post(f"{V3}/generation/text-to-model").mock(
        return_value=_ok({"task_id": "task_1"})
    )
    respx.mock.get(f"{V3}/tasks/task_1").mock(
        return_value=_ok({"status": "success", "output": {}})
    )

    with pytest.raises(ProviderCallError, match="model_url"):
        TripoClient(_cfg()).text_to_model(prompt="x", out_dir=tmp_path / "out")


@respx.mock
def test_tripo_balance_parses_decimals():
    # V2 returned integers here; V3 returns decimals.
    respx.mock.get(f"{V3}/account/balance").mock(
        return_value=_ok({"balance": 1234.56, "frozen": 20.5})
    )

    assert TripoClient(_cfg()).get_balance() == {
        "balance": 1234.56,
        "frozen": 20.5,
    }


@respx.mock
def test_tripo_auth_failure_detected_by_http_401():
    # V2 signalled auth failure with body code 1002, V3 uses code 2, and some
    # deployments still emit the old one -- so we key off the HTTP status.
    respx.mock.get(f"{V3}/account/balance").mock(
        return_value=httpx.Response(401, json={"code": 2, "message": "bad key"})
    )

    with pytest.raises(ProviderConfigError, match="HTTP 401"):
        TripoClient(_cfg()).get_balance()


@respx.mock
def test_tripo_retries_rate_limit_then_succeeds():
    # 1007 is the per-minute creation quota and clears on its own.
    route = respx.mock.get(f"{V3}/account/balance").mock(
        side_effect=[
            httpx.Response(200, json={"code": 1007, "message": "rate limited"}),
            _ok({"balance": 10.0, "frozen": 0.0}),
        ]
    )

    client = TripoClient(_cfg())
    with mock.patch.object(tripo_client_mod.time, "sleep"):
        assert client.get_balance()["balance"] == 10.0
    assert route.call_count == 2


@respx.mock
def test_tripo_does_not_retry_insufficient_credit():
    # 2010 never resolves by retrying; fail immediately with a top-up hint.
    route = respx.mock.get(f"{V3}/account/balance").mock(
        return_value=httpx.Response(
            403, json={"code": 2010, "message": "insufficient credit"}
        )
    )

    with pytest.raises(ProviderCallError, match="developers.tripo3d.ai"):
        TripoClient(_cfg()).get_balance()
    assert route.call_count == 1


def test_tripo_missing_key_raises(tmp_path):
    with pytest.raises(ProviderConfigError):
        TripoClient(TripoConfig(api_key=None)).image_to_model(
            image_path="x.png", out_dir=tmp_path
        )
