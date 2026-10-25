#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "mtrxai/coder_transcript.hpp"
#include "mtrxai/files.hpp"
#include "mtrxai/pipeline.hpp"
#include "mtrxai/provider.hpp"
#include "mtrxai/session.hpp"
#include "mtrxai/signals.hpp"
#include "mtrxai/stream_parse.hpp"
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

TEST_CASE("stream parsers decode SSE payloads") {
  CHECK(mtrxai::stream::sse_payload("data: {\"x\":1}") == "{\"x\":1}");
  CHECK(mtrxai::stream::sse_payload("data: [DONE]") == "[DONE]");
  CHECK(mtrxai::stream::sse_payload("data:") == "");
  CHECK(mtrxai::stream::sse_payload(": keepalive") == "");
  CHECK(mtrxai::stream::sse_payload("event: content_block_delta") == "");
}

TEST_CASE("stream parsers report embedded errors") {
  CHECK(mtrxai::stream::error_message("{\"error\":\"boom\"}") == "boom");
  CHECK(mtrxai::stream::error_message(
            "{\"error\":{\"message\":\"quota\"}}") == "quota");
  CHECK(mtrxai::stream::error_message(
            "{\"type\":\"error\",\"error\":{\"message\":\"overloaded\"}}") ==
        "overloaded");
  CHECK(mtrxai::stream::error_message("{\"message\":\"fine\"}") == "");
}

TEST_CASE("Ollama NDJSON line yields message content") {
  const std::string line =
      "{\"model\":\"m\",\"message\":{\"role\":\"assistant\","
      "\"content\":\"hel\"},\"done\":false}";
  CHECK(mtrxai::stream::ollama_delta_text(line) == "hel");
  // Final stats line has no text.
  CHECK(mtrxai::stream::ollama_delta_text(
            "{\"done\":true,\"message\":{\"content\":\"\"}}") == "");
  CHECK(mtrxai::stream::ollama_delta_text("not json") == "");
}

TEST_CASE("OpenAI delta parser extracts choices[0].delta.content") {
  const std::string chunk =
      "{\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}";
  CHECK(mtrxai::stream::openai_delta_text(chunk) == "lo");
  // Finish chunk without content.
  CHECK(mtrxai::stream::openai_delta_text(
            "{\"choices\":[{\"delta\":{},"
            "\"finish_reason\":\"stop\"}]}") == "");
  // Usage-only chunk without choices.
  CHECK(mtrxai::stream::openai_delta_text(
            "{\"usage\":{\"total_tokens\":12}}") == "");
}

TEST_CASE("Anthropic delta parser reads text_delta events") {
  CHECK(mtrxai::stream::anthropic_delta_text(
            "{\"type\":\"content_block_delta\","
            "\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}") == "hi");
  CHECK(mtrxai::stream::anthropic_delta_text(
            "{\"type\":\"message_start\",\"message\":{}}") == "");
  CHECK(mtrxai::stream::anthropic_delta_text(
            "{\"type\":\"content_block_delta\","
            "\"delta\":{\"type\":\"input_json_delta\"}}") == "");
}

TEST_CASE("RawRecorder persists chunks and read_raw_text filters by turn") {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("mtrxai_raw_test_" + std::to_string(::getpid()));
  std::filesystem::remove_all(dir);
  const auto path = dir / "stage" / "raw.jsonl";
  {
    mtrxai::RawRecorder recorder(path);
    recorder.append("hel", 0);
    recorder.begin_group(1);
    recorder.append("lo ", 1);
    recorder.append("wor", 1);
  }
  // Owner-only permissions were applied at creation.
  const auto mode = std::filesystem::status(path).permissions();
  CHECK((mode & std::filesystem::perms::group_all) == std::filesystem::perms::none);
  CHECK((mode & std::filesystem::perms::others_all) == std::filesystem::perms::none);
  // Whole-file decode; the start marker contributes no text.
  CHECK(mtrxai::read_raw_text(path) == "hello wor");
  CHECK(mtrxai::read_raw_text(path, 0) == "hel");
  CHECK(mtrxai::read_raw_text(path, 1) == "lo wor");
  // A malformed line is ignored rather than corrupting the stream.
  {
    std::ofstream extra(path, std::ios::app);
    extra << "not json\n";
    extra << R"({"type":"token","turn":1,"text":"!"})" << "\n";
  }
  CHECK(mtrxai::read_raw_text(path, 1) == "lo wor!");
  std::filesystem::remove_all(dir);
}

TEST_CASE("CoderTranscript round-trips every message it appends") {
  const auto root = std::filesystem::temp_directory_path() /
                    ("mtrxai_transcript_test_" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  const auto path = root / "coder" / "messages.jsonl";

  mtrxai::CoderMessage system;
  system.role = "system";
  system.content = "You are the coding agent.";
  mtrxai::CoderMessage initial;
  initial.role = "user";
  initial.content = "Task with \"quotes\", a newline\nand UTF-8: caf\xc3\xa9 \xe2\x9c\x93";
  mtrxai::CoderMessage assistant;
  assistant.role = "assistant";
  assistant.content = R"({"action":"finish","summary":"done"})";
  assistant.thinking = "First line.\nSecond line with <angles> & symbols.";
  assistant.turn = 1;
  mtrxai::CoderMessage result;
  result.role = "user";
  result.content = "Tool result:\nSaved hello.txt (6 bytes).";
  result.turn = 1;

  {
    mtrxai::CoderTranscript transcript(path);
    transcript.append(system);
    transcript.append(initial);
    transcript.append(assistant);
    transcript.append(result);
  }

  // The parent directory is created and the file is readable only by its
  // owner; one message is one JSON line.
  const auto mode = std::filesystem::status(path).permissions();
  CHECK((mode & std::filesystem::perms::group_all) ==
        std::filesystem::perms::none);
  CHECK((mode & std::filesystem::perms::others_all) ==
        std::filesystem::perms::none);

  const auto messages = mtrxai::CoderTranscript::load(path);
  REQUIRE(messages.size() == 4);
  CHECK(messages[0].role == "system");
  CHECK(messages[0].content == system.content);
  CHECK(messages[0].turn == 0);
  CHECK(messages[1].role == "user");
  CHECK(messages[1].content == initial.content);
  CHECK(messages[1].thinking.empty());
  CHECK(messages[2].role == "assistant");
  CHECK(messages[2].content == assistant.content);
  CHECK(messages[2].thinking == assistant.thinking);
  CHECK(messages[2].turn == 1);
  CHECK(messages[3].role == "user");
  CHECK(messages[3].content == result.content);
  CHECK(messages[3].turn == 1);

  // Opening it again appends instead of truncating, which is what a resumed
  // Coder needs.
  {
    mtrxai::CoderTranscript transcript(path);
    mtrxai::CoderMessage next;
    next.role = "assistant";
    next.content = "more";
    next.turn = 2;
    transcript.append(next);
  }
  CHECK(mtrxai::CoderTranscript::load(path).size() == 5);

  // A crash can leave a partial or hand-edited line. It is skipped, and the
  // surrounding messages still load.
  {
    std::ofstream extra(path, std::ios::app);
    extra << "{\"role\":\"assistant\",\"content\":\"truncated\"\n";
    extra << "{\"role\":\"tool\",\"content\":\"unknown role\"}\n";
    extra << "{\"role\":\"assistant\"}\n";
    extra << "\n";
    extra << R"({"role":"user","content":"last","turn":3})" << "\n";
  }
  const auto recovered = mtrxai::CoderTranscript::load(path);
  REQUIRE(recovered.size() == 6);
  CHECK(recovered.back().content == "last");
  CHECK(recovered.back().turn == 3);

  CHECK(mtrxai::CoderTranscript::load(root / "missing.jsonl").empty());
  std::filesystem::remove_all(root);
}

TEST_CASE("CoderTranscript::save replaces the conversation atomically") {
  const auto root = std::filesystem::temp_directory_path() /
                    ("mtrxai_transcript_save_test_" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  const auto path = root / "coder" / "messages.jsonl";

  mtrxai::CoderMessage system;
  system.role = "system";
  system.content = "system prompt";
  mtrxai::CoderMessage initial;
  initial.role = "user";
  initial.content = "initial request";
  mtrxai::CoderMessage bad;
  bad.role = "assistant";
  bad.content = "not an action";
  bad.thinking = "why it failed";
  bad.turn = 1;
  {
    mtrxai::CoderTranscript transcript(path);
    transcript.append(system);
    transcript.append(initial);
    transcript.append(bad);
  }

  auto repaired = mtrxai::CoderTranscript::load(path);
  REQUIRE(repaired.size() == 3);
  repaired.pop_back();
  mtrxai::CoderMessage correction;
  correction.role = "user";
  correction.content = "Your previous reply was rejected.";
  correction.turn = 1;
  repaired.push_back(correction);
  mtrxai::CoderTranscript::save(path, repaired);

  const auto saved = mtrxai::CoderTranscript::load(path);
  REQUIRE(saved.size() == 3);
  CHECK(saved[0].role == "system");
  CHECK(saved[0].content == "system prompt");
  CHECK(saved[1].role == "user");
  CHECK(saved[1].content == "initial request");
  CHECK(saved[2].role == "user");
  CHECK(saved[2].content == "Your previous reply was rejected.");
  CHECK(saved[2].turn == 1);
  CHECK(saved[2].thinking.empty());

  // Rewriting also creates the parent directory and keeps the file private.
  const auto nested = root / "new" / "coder" / "messages.jsonl";
  mtrxai::CoderTranscript::save(nested, {});
  CHECK(std::filesystem::is_regular_file(nested));
  CHECK(mtrxai::CoderTranscript::load(nested).empty());
  const auto mode = std::filesystem::status(nested).permissions();
  CHECK((mode & std::filesystem::perms::group_all) ==
        std::filesystem::perms::none);
  CHECK((mode & std::filesystem::perms::others_all) ==
        std::filesystem::perms::none);

  std::filesystem::remove_all(root);
}

TEST_CASE("parse_manifest_stage reads only a string completed_stage") {
  CHECK(mtrxai::parse_manifest_stage(
            R"({"completed_stage":"prompter","updated_at":"x"})") ==
        "prompter");
  CHECK(mtrxai::parse_manifest_stage(
            R"({"completed_stage":"coder-turn-3"})") == "coder-turn-3");
  CHECK(mtrxai::parse_manifest_stage(R"({"updated_at":"x"})").empty());
  CHECK(mtrxai::parse_manifest_stage(R"({"completed_stage":42})").empty());
  CHECK(mtrxai::parse_manifest_stage("{not json").empty());
  CHECK(mtrxai::parse_manifest_stage("").empty());
}

TEST_CASE("session listing is newest first and skips completed runs") {
  const auto root = std::filesystem::temp_directory_path() /
                    ("mtrxai_sessions_test_" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  const auto add_session = [&root](const std::string& id,
                                   const std::string& manifest) {
    const auto directory = root / id;
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "request.md") << "task " << id << "\n";
    if (!manifest.empty()) {
      std::ofstream(directory / "manifest.json") << manifest;
    }
  };
  add_session("20261025-120000-000",
              R"({"completed_stage":"finished","updated_at":"2026-10-25 12:00"})");
  add_session("20261025-110000-000",
              R"({"completed_stage":"chief","updated_at":"2026-10-25 11:00"})");
  add_session("20261025-100000-000", "{not json");
  add_session("20261024-090000-000", "");
  // A newer directory without request.md is not a session and must not hide
  // real work from --resume.
  std::filesystem::create_directories(root / "20261026-000000-000");
  std::ofstream(root / "20261026-000000-000" / "manifest.json")
      << R"({"completed_stage":"request"})";

  const auto sessions = mtrxai::list_sessions(root);
  REQUIRE(sessions.size() == 4);
  CHECK(sessions[0].id == "20261025-120000-000");
  CHECK(sessions[0].stage == "finished");
  CHECK(sessions[1].id == "20261025-110000-000");
  CHECK(sessions[1].stage == "chief");
  CHECK(sessions[1].updated_at == "2026-10-25 11:00");
  CHECK(sessions[2].id == "20261025-100000-000");
  CHECK(sessions[2].stage.empty());  // unreadable manifest, still resumable
  CHECK(sessions[3].id == "20261024-090000-000");
  CHECK(sessions[3].stage.empty());

  CHECK(mtrxai::session_finished(sessions[0]));
  CHECK_FALSE(mtrxai::session_finished(sessions[1]));
  const auto latest = mtrxai::latest_unfinished_session(root);
  REQUIRE(latest.has_value());
  CHECK(latest->id == "20261025-110000-000");
  CHECK(mtrxai::read_session_request(latest->path) ==
        "task 20261025-110000-000\n");

  std::filesystem::remove_all(root);
}
