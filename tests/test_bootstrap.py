"""Project-root resolution and first-run bootstrap.

Regression tests for the RPM first-run crash: bare `mtrxai` from a directory
without settings.toml used to fall back to site-packages and die with
PermissionError. It must bootstrap a user-writable project instead.
"""

from __future__ import annotations

import pytest

import mtrxai.config as cfg
from mtrxai.config import (
    bootstrap_project,
    default_project_dir,
    find_project_root,
)


@pytest.fixture
def clean_home(monkeypatch, tmp_path):
    """Isolate cwd, user-data dir, PACKAGE_ROOT, and provide an RPM-style
    template (as /usr/share/mtrxai would be on an installed system)."""
    monkeypatch.chdir(tmp_path)
    monkeypatch.delenv("MTRXAI_PROJECT", raising=False)
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "xdg-data"))
    tpl = tmp_path / "usr-share-mtrxai"
    (tpl / "agents").mkdir(parents=True)
    (tpl / "settings.toml").write_text('workspace = "./workspace"\n')
    (tpl / "agents" / "router.yaml").write_text("name: router\n")
    (tpl / "agents" / "coder.yaml").write_text("name: coder\n")
    monkeypatch.setattr(cfg, "PACKAGE_ROOT", tmp_path / "site-packages")
    monkeypatch.setattr(cfg, "TEMPLATE_DIRS", (tpl,))
    return tmp_path


def test_repo_root_still_wins_when_it_has_settings(tmp_path, monkeypatch):
    """Dev checkout: PACKAGE_ROOT has settings.toml → found even from a subdir."""
    repo = tmp_path / "checkout"
    repo.mkdir()
    (repo / "settings.toml").write_text("")
    monkeypatch.setattr(cfg, "PACKAGE_ROOT", repo)
    monkeypatch.delenv("MTRXAI_PROJECT", raising=False)
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "xdg"))
    subdir = tmp_path / "somewhere-else"
    subdir.mkdir()
    monkeypatch.chdir(subdir)
    assert find_project_root() == repo


def test_bare_run_bootstraps_user_project(clean_home):
    """The exact RPM regression: no settings.toml anywhere → user dir."""
    root = find_project_root()
    assert root == default_project_dir()
    assert "site-packages" not in str(root)


def test_bootstrap_creates_runnable_project_from_template(clean_home):
    root = bootstrap_project(default_project_dir())
    assert (root / "settings.toml").is_file()
    assert (root / "workspace").is_dir()
    assert (root / "logs").is_dir()
    agents = list((root / "agents").glob("*.yaml"))
    assert agents, "template agents must be seeded"
    # bootstrap is idempotent + never clobbers user edits
    (root / "agents" / "router.yaml").write_text("user edit\n")
    bootstrap_project(root)
    assert (root / "agents" / "router.yaml").read_text() == "user edit\n"


def test_bootstrap_never_writes_to_the_template(clean_home, tmp_path):
    tpl = tmp_path / "tpl"
    (tpl / "agents").mkdir(parents=True)
    (tpl / "settings.toml").write_text('workspace = "./workspace"\n')
    (tpl / "agents" / "a.yaml").write_text("name: a\n")
    before = sorted(str(p) for p in tpl.rglob("*"))
    bootstrap_project(default_project_dir(), template_dir=tpl)
    assert sorted(str(p) for p in tpl.rglob("*")) == before


def test_explicit_project_without_settings_is_kept(clean_home, tmp_path):
    p = tmp_path / "explicit"
    p.mkdir()
    assert find_project_root(str(p)) == p.resolve()
    # and the CLI's bootstrap call makes it runnable
    bootstrap_project(p)
    assert (p / "settings.toml").is_file()


def test_env_project_without_settings_is_kept(clean_home, monkeypatch, tmp_path):
    p = tmp_path / "envproj"
    p.mkdir()
    monkeypatch.setenv("MTRXAI_PROJECT", str(p))
    assert find_project_root() == p.resolve()

def test_no_template_still_gives_runnable_skeleton(clean_home):
    """No template anywhere: defaults-only project that still boots."""
    real = cfg.TEMPLATE_DIRS
    cfg.TEMPLATE_DIRS = ()
    try:
        root = bootstrap_project(default_project_dir())
    finally:
        cfg.TEMPLATE_DIRS = real
    assert (root / "workspace").is_dir()
    assert not (root / "settings.toml").exists()  # defaults kick in at load time


def test_cli_main_bootstraps_before_assembling(clean_home, monkeypatch):
    """End-to-end: bare `mtrxai --mode repl` with stdin at EOF must exit 0,
    having created the user project — not crash in site-packages."""
    from mtrxai import cli

    monkeypatch.setattr("sys.argv", ["mtrxai", "--mode", "repl"])
    monkeypatch.setattr("builtins.input", lambda *a: (_ for _ in ()).throw(EOFError))
    rc = cli.main()
    assert rc == 0
    root = default_project_dir()
    assert (root / "settings.toml").is_file()
    assert (root / "workspace").is_dir()
