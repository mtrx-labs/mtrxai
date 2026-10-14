import json
from pathlib import Path

import pytest

from mtrxai.tools import TOOLBOX, Toolbox, set_workspace
from mtrxai.tools.files import read_file, safe_path, write_file, list_dir
from mtrxai.tools.shell import run_shell


def test_builtins_registered():
    for name in ("read_file", "write_file", "list_dir", "run_shell", "web_search", "web_fetch"):
        assert name in TOOLBOX, name


def test_workspace_jail_rejects_escape(tmp_path):
    set_workspace(tmp_path)
    with pytest.raises(PermissionError, match="escapes"):
        safe_path("../../etc/passwd")
    with pytest.raises(PermissionError):
        safe_path("/etc/passwd")


def test_file_roundtrip(tmp_path):
    set_workspace(tmp_path)
    out = write_file({"path": "sub/notes.txt", "content": "hello mtrxAI"})
    assert "wrote" in out
    assert (tmp_path / "sub" / "notes.txt").read_text() == "hello mtrxAI"
    assert "hello mtrxAI" in read_file({"path": "sub/notes.txt"})
    listing = list_dir({"path": ""})
    assert "sub/notes.txt" in listing


def test_read_missing_file_is_error_string(tmp_path):
    set_workspace(tmp_path)
    out = read_file({"path": "nope.txt"})
    assert out.startswith("ERROR:")


def test_run_shell_runs_in_workspace(tmp_path):
    set_workspace(tmp_path)
    out = run_shell({"command": "pwd && echo hi"})
    assert "exit=0" in out
    assert str(tmp_path) in out
    assert "hi" in out


def test_run_shell_failure_is_captured(tmp_path):
    set_workspace(tmp_path)
    out = run_shell({"command": "exit 3"})
    assert "exit=3" in out


def test_toolbox_execute_unknown_tool():
    tb = Toolbox(["read_file"])
    out = tb.execute({"tool": "nope", "args": {}})
    assert out.startswith("ERROR: unknown tool")


def test_toolbox_execute_missing_args():
    tb = Toolbox(["write_file"])
    out = tb.execute({"tool": "write_file", "args": {}})
    assert "missing arg" in out


def test_toolbox_rejects_unknown_names():
    import pytest

    with pytest.raises(ValueError, match="unknown tool"):
        Toolbox(["read_file", "laser_beam"])


def test_toolbox_docs_list_signatures():
    tb = Toolbox(["write_file"])
    docs = tb.docs()
    assert "write_file" in docs and "path" in docs
