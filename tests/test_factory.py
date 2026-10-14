import pytest

from mtrxai.models.base import ModelError
from mtrxai.models.factory import make_backend
from mtrxai.models.fake import FakeBackend
from mtrxai.models.gguf import GGUFBackend
from mtrxai.models.ollama import OllamaBackend
from mtrxai.models.openai_compat import AnthropicBackend, OpenAICompatBackend

PROVIDERS = {
    "ollama": {"base_url": "http://127.0.0.1:11434"},
    "openai": {"base_url": "https://api.openai.com/v1", "api_key_env": "OPENAI_API_KEY"},
    "anthropic": {"api_key_env": "ANTHROPIC_API_KEY"},
    "llamacpp": {"base_url": "http://10.0.0.5:8080/v1", "api_key_env": ""},
}


def test_ollama_ref_parses_model_colon():
    b = make_backend("ollama:qwen2.5-coder:7b", PROVIDERS)
    assert isinstance(b, OllamaBackend)
    assert b.model == "qwen2.5-coder:7b"


def test_openai_uses_provider_config():
    b = make_backend("openai:gpt-4o", PROVIDERS)
    assert isinstance(b, OpenAICompatBackend)
    assert b.base_url == "https://api.openai.com/v1"
    assert b.model == "gpt-4o"


def test_custom_compat_endpoint():
    b = make_backend("llamacpp:my-model", PROVIDERS)
    assert isinstance(b, OpenAICompatBackend)
    assert b.base_url == "http://10.0.0.5:8080/v1"


def test_anthropic_backend():
    b = make_backend("anthropic:claude-sonnet-4-20250514", PROVIDERS)
    assert isinstance(b, AnthropicBackend)
    assert b.model == "claude-sonnet-4-20250514"


def test_fake_backend():
    assert isinstance(make_backend("fake:demo", PROVIDERS), FakeBackend)


def test_gguf_missing_file_raises_with_hint():
    with pytest.raises(ModelError, match="not found"):
        make_backend("gguf:/nonexistent/model.gguf", PROVIDERS)


def test_unknown_provider_lists_known():
    with pytest.raises(ModelError, match="unknown provider"):
        make_backend("grok:x", PROVIDERS)


def test_malformed_ref():
    with pytest.raises(ModelError, match="provider:model"):
        make_backend("noseparator", PROVIDERS)
        make_backend("ollama:", PROVIDERS)
