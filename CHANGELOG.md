# Changelog

O formato segue [Keep a Changelog](https://keepachangelog.com/pt-BR/1.1.0/) e
o projeto adere ao [Versionamento Semântico](https://semver.org/lang/pt-BR/).

## [1.1.0] - 2026-11-16

### Changed

- **Build Windows oculto:** o executável Windows passa a ser subsystem GUI
  (`-mwindows`), então a tarefa agendada roda sem abrir nem piscar janela de
  console. O uso interativo (`--version`, `--print`, `config`, `update`)
  reaproveita o console do terminal via `AttachConsole`, sem janela própria.

## [1.0.0] - 2026-11-15

### Added

- Agente que lê os CSVs do band do ponytail (`~/.config/ponytail/band`) e
  envia as sessões para `POST /api/inventory/ponytail`, em lotes de 500.
- Parser CSV RFC 4180 idêntico ao do board do ponytail.
- Transporte, `config`, `update` e builds Linux/Windows herdados do invpush
  v1.2.0; o 2xx no Windows agora também exige o marcador `status` da API.
- Instalador Linux com timers do usuário: envio de hora em hora e
  autoatualização diária.
