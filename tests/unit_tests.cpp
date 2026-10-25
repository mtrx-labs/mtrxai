#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "mtrxai/pipeline.hpp"
#include "mtrxai/provider.hpp"
#include "mtrxai/signals.hpp"
#include "mtrxai/utf8.hpp"

TEST_CASE("llama_log_path uses state home and role") {
  const std::string path = mtrxai::llama_log_path("coder").string();
  CHECK(path.find("mtrxai/logs/coder.log") != std::string::npos);
  const std::string chief_path = mtrxai::llama_log_path("chief").string();
  CHECK(chief_path.find("mtrxai/logs/chief.log") != std::string::npos);
}

TEST_CASE("sensitive_filename blocks exact sensitive names") {
  CHECK(mtrxai::sensitive_filename(".env"));
  CHECK(mtrxai::sensitive_filename(".netrc"));
  CHECK(mtrxai::sensitive_filename(".npmrc"));
  CHECK(mtrxai::sensitive_filename(".pypirc"));
  CHECK(mtrxai::sensitive_filename("credentials.json"));

  // Case-insensitive check
  CHECK(mtrxai::sensitive_filename(".ENV"));
  CHECK(mtrxai::sensitive_filename(".Netrc"));
  CHECK(mtrxai::sensitive_filename("CREDENTIALS.JSON"));
}

TEST_CASE("sensitive_filename blocks prefix patterns") {
  CHECK(mtrxai::sensitive_filename(".env.local"));
  CHECK(mtrxai::sensitive_filename(".env.development"));
  CHECK(mtrxai::sensitive_filename(".env.production"));
  CHECK(mtrxai::sensitive_filename(".env.test.local"));

  CHECK(mtrxai::sensitive_filename("id_rsa"));
  CHECK(mtrxai::sensitive_filename("id_rsa.pub"));
  CHECK(mtrxai::sensitive_filename("id_rsa_backup"));
  CHECK(mtrxai::sensitive_filename("ID_RSA"));

  CHECK(mtrxai::sensitive_filename("id_ed25519"));
  CHECK(mtrxai::sensitive_filename("id_ed25519.pub"));
  CHECK(mtrxai::sensitive_filename("ID_ED25519"));
}

TEST_CASE("sensitive_filename blocks suffix patterns") {
  CHECK(mtrxai::sensitive_filename("server.pem"));
  CHECK(mtrxai::sensitive_filename("cert.key"));
  CHECK(mtrxai::sensitive_filename("store.p12"));
  CHECK(mtrxai::sensitive_filename("bundle.pfx"));
  CHECK(mtrxai::sensitive_filename("SERVER.PEM"));
  CHECK(mtrxai::sensitive_filename("PRIVATE.KEY"));
}

TEST_CASE("sensitive_filename handles relative paths") {
  CHECK(mtrxai::sensitive_filename("secrets/.env"));
  CHECK(mtrxai::sensitive_filename("config/credentials.json"));
  CHECK(mtrxai::sensitive_filename(".ssh/id_rsa"));
  CHECK(mtrxai::sensitive_filename("certs/cert.pem"));
}

TEST_CASE("sensitive_filename allows false positives from section 4") {
  CHECK_FALSE(mtrxai::sensitive_filename("tokenizer.cpp"));
  CHECK_FALSE(mtrxai::sensitive_filename("token.h"));
  CHECK_FALSE(mtrxai::sensitive_filename("private_impl.hpp"));
  CHECK_FALSE(mtrxai::sensitive_filename("credentials.cpp"));
  CHECK_FALSE(mtrxai::sensitive_filename("secrets_manager.go"));
  CHECK_FALSE(mtrxai::sensitive_filename("dev.env"));
  CHECK_FALSE(mtrxai::sensitive_filename("apikey_test.py"));
  CHECK_FALSE(mtrxai::sensitive_filename("passwordless.ts"));
}

TEST_CASE("sensitive_filename allows standard project files") {
  CHECK_FALSE(mtrxai::sensitive_filename("main.cpp"));
  CHECK_FALSE(mtrxai::sensitive_filename("README.md"));
  CHECK_FALSE(mtrxai::sensitive_filename("CMakeLists.txt"));
  CHECK_FALSE(mtrxai::sensitive_filename("config.json"));
  CHECK_FALSE(mtrxai::sensitive_filename("Makefile"));
  CHECK_FALSE(mtrxai::sensitive_filename("index.ts"));
}

TEST_CASE("extract_thinking separates reasoning from answer") {
  SUBCASE("standard think block") {
    const auto res = mtrxai::extract_thinking(
        "<think>\nI need to analyze this.\n</think>\nHere is the answer.");
    CHECK(res.thinking == "I need to analyze this.");
    CHECK(res.content == "Here is the answer.");
  }

  SUBCASE("case-insensitive think tags") {
    const auto res = mtrxai::extract_thinking(
        "<THINK>Capital reasoning</THINK>Answer text");
    CHECK(res.thinking == "Capital reasoning");
    CHECK(res.content == "Answer text");
  }

  SUBCASE("multiple think blocks") {
    const auto res = mtrxai::extract_thinking(
        "<think>First thought</think>Middle<think>Second thought</think>End");
    CHECK(res.thinking == "First thought\n\nSecond thought");
    CHECK(res.content == "MiddleEnd");
  }

  SUBCASE("unclosed think block") {
    const auto res = mtrxai::extract_thinking("<think>Only thinking never finished");
    CHECK(res.thinking == "Only thinking never finished");
    CHECK(res.content.empty());
  }

  SUBCASE("no thinking tags") {
    const auto res = mtrxai::extract_thinking("Direct response with no tags");
    CHECK(res.thinking.empty());
    CHECK(res.content == "Direct response with no tags");
  }
}

TEST_CASE("ModelSlot defaults max_tokens to 4096") {
  mtrxai::ModelSlot slot;
  CHECK(slot.max_tokens == 4096);
}

TEST_CASE("signals state tracking methods operate safely") {
  mtrxai::signals::set_curses_active(true);
  mtrxai::signals::set_curses_active(false);
  mtrxai::signals::set_active_child(12345);
  mtrxai::signals::set_active_child(0);
  mtrxai::signals::set_active_ollama("http://localhost:11434", "test-model");
  mtrxai::signals::clear_active_ollama();
}

TEST_CASE("Config defaults and serialization of turns, timeout, and context") {
  mtrxai::Config config;
  CHECK(config.max_turns == 40);
  CHECK(config.gguf_load_timeout_seconds == 150);

  config.max_turns = 25;
  config.gguf_load_timeout_seconds = 60;
  config.coder.context_size = 4096;
  config.chief.provider = "ollama";
  config.chief.model = "chief-m";
  config.prompter.provider = "ollama";
  config.prompter.model = "prompter-m";
  config.coder.provider = "ollama";
  config.coder.model = "coder-m";

  const auto temp_config_path =
      std::filesystem::temp_directory_path() / "mtrxai_test_config.json";
  mtrxai::save_config(config, temp_config_path);

  const auto loaded = mtrxai::load_config(temp_config_path);
  CHECK(loaded.max_turns == 25);
  CHECK(loaded.gguf_load_timeout_seconds == 60);
  CHECK(loaded.coder.context_size == 4096);
  std::filesystem::remove(temp_config_path);
}


TEST_CASE("utf8_encode covers ASCII, two-, three-, and four-byte characters") {
  CHECK(mtrxai::utf8_encode(L'A') == "A");
  CHECK(mtrxai::utf8_encode(L'\u00e9') == "\xc3\xa9");            // é
  CHECK(mtrxai::utf8_encode(L'\u20ac') == "\xe2\x82\xac");        // €
  CHECK(mtrxai::utf8_encode(L'\U0001F600') == "\xf0\x9f\x98\x80"); // 😀
  // Out-of-range code points become the replacement character.
  CHECK(mtrxai::utf8_encode(static_cast<wchar_t>(0x110000)) == "\xef\xbf\xbd");
}

TEST_CASE("utf8 cursor helpers step over multibyte characters") {
  const std::string text = "a\u00e9b";  // a, é (2 bytes), b
  CHECK(text.size() == 4);
  CHECK(mtrxai::utf8_prev_char_start(text, 4) == 3);
  CHECK(mtrxai::utf8_prev_char_start(text, 3) == 1);
  CHECK(mtrxai::utf8_prev_char_start(text, 1) == 0);
  CHECK(mtrxai::utf8_prev_char_start(text, 0) == 0);
  CHECK(mtrxai::utf8_next_char_end(text, 0) == 1);
  CHECK(mtrxai::utf8_next_char_end(text, 1) == 3);
  CHECK(mtrxai::utf8_next_char_end(text, 3) == 4);
  CHECK(mtrxai::utf8_next_char_end(text, 4) == 4);
  // Continuation bytes are never reported as character starts.
  CHECK(mtrxai::utf8_is_continuation(text[2]));
  CHECK_FALSE(mtrxai::utf8_is_continuation(text[1]));
}
