# Spec file for mtrxAI — hot-swappable multi-agent assistant.
#
# Build:   rpmbuild -ba packaging/mtrxai.spec
# Result:  ~/rpmbuild/RPMS/noarch/mtrxai-<version>-1.fcXX.noarch.rpm
#
# Packaging strategy:
#   - Pure-Python app, so noarch.
#   - Fedora-python deps via Requires (nothing bundled, no pip at install time).
#   - The voice extra ("hey matrix") is deliberately NOT packaged: its deps
#     (openwakeword, faster-whisper, piper-tts, webrtcvad) are not in Fedora.
#     Voice users: python -m venv + pip install -e '.[voice]' per README.

%global srcname mtrxai
%global appname mtrxai

Name:           mtrxai
Version:        0.2.0
Release:        1%{?dist}
Summary:        Hot-swappable multi-agent assistant (router + specialists, TUI, web GUI)

License:        MIT
URL:            https://github.com/mtrx-labs/mtrxai
Source0:        %{url}/archive/refs/tags/v%{version}.tar.gz
BuildArch:      noarch

BuildRequires:  python3-devel
BuildRequires:  pyproject-rpm-macros

Requires:       python3-httpx
Requires:       python3-pyyaml
Requires:       python3-rich
Requires:       python3-textual

# The web GUI is part of the package (stdlib only); nothing extra needed.
# run_shell / file tools are jailed to ./workspace by design; no elevated perms.

%description
mtrxAI is a local multi-agent assistant: one router model receives your task,
decides which specialist agent (Coder / Researcher / Writer / ...) handles it
best, and dispatches. Every model — including the router's — is hot-swappable
between API-key cloud models and local models (Ollama / raw .gguf) at runtime.

Ships with a Textual TUI (voice-capable), a browser web GUI with dark/light
themes built on a #26ab07 accent palette, and a plain REPL.

%prep
%autosetup -n %{srcname}-%{version}

%generate_buildrequires
# -x dev: pull pytest/pytest-asyncio/numpy for %check (all in Fedora).
# The `web` extra (ddgs) is deliberately NOT pulled: not in Fedora, and the
# app degrades gracefully without it (web_search returns an install hint).
%pyproject_buildrequires -x dev

%build
%pyproject_wheel

%install
%pyproject_install
%pyproject_save_files -l %{srcname}

# Ship the default agent roster + starter settings as a template. RPM users
# bootstrap with:  cp -r /usr/share/mtrxai ~/mtrxai && mtrxai --project ~/mtrxai
mkdir -p %{buildroot}%{_datadir}/%{name}
cp -r agents %{buildroot}%{_datadir}/%{name}/agents
cp settings.toml %{buildroot}%{_datadir}/%{name}/settings.toml

%check
# offline unit suite (fake:* models; no GPU, no keys, no network)
%{python3} -m pytest tests -q

%files -f %{pyproject_files}
%license LICENSE
%doc README.md
%{_bindir}/mtrxai
%dir %{_datadir}/%{name}
%dir %{_datadir}/%{name}/agents
%{_datadir}/%{name}/agents/*
%{_datadir}/%{name}/settings.toml

%changelog
* Wed Oct 14 2026 mtrx <mtrx@localhost> - 0.2.0-1
- Initial RPM package: CLI, TUI, REPL, web GUI (#26ab07 palette, dark+light).
