# ponypush — envio das sessões do ponytail para a API central

**ponypush** é um agente de linha de comando em **C++17** que lê os CSVs do
band do plugin ponytail (`~/.config/ponytail/band/*.csv`, uma linha por
sessão) e envia tudo para a API central (`POST /api/inventory/ponytail`,
módulo `host_inventory`). O servidor faz upsert por máquina + `key`, então
reenviar tudo a cada execução é idempotente. A visualização centralizada fica
no admin (`/ponytail`).

Derivado do [invpush](../invpush): mesmo transporte, mesmo token de coleta,
mesma configuração persistente e mesma autoatualização.

## Destaques

- **Zero dependências em runtime**: Linux vincula `libcurl` do sistema;
  Windows gera binário **100% estático** com WinHTTP (Schannel).
- **Tolerante ao formato**: cada linha vai como mapa coluna→célula; colunas
  novas do ponytail não quebram o agente (o servidor ignora o que não conhece).
- **Envio em lotes** de 500 sessões (a API aceita até 1000 por requisição).
- **Seguro**: token via config, variável de ambiente ou flag — nunca compilado
  no binário.

## Compilação

```bash
make              # build de release (Linux, libcurl4-openssl-dev)
make check        # warnings tratados como erro
make test         # suíte (CSV, sessões, payload, args, config)
make sanitize     # ASan + UBSan
./cross.sh        # Linux x86_64/aarch64/arm32 (docker) e Windows amd64/x86 (mingw)
```

## Uso

```bash
ponypush config set token <token de coleta>   # uma vez (mesmo token do invpush)
ponypush                                      # envia todas as sessões
ponypush --print                              # mostra o payload e sai
ponypush --band-dir /outro/band --debug
```

| Opção | Padrão | Descrição |
|---|---|---|
| `--url` | `https://api.vandinho.com.br/api/inventory/ponytail` | Endpoint (env `PONYPUSH_API_URL`) |
| `--token` | `$INVENTORY_COLLECTION_API_TOKEN` | Bearer token de coleta |
| `--maquina` | hostname do sistema | Nome reportado |
| `--band-dir` | `$XDG_CONFIG_HOME` ou `~/.config` + `/ponytail/band` (`%USERPROFILE%\.config\ponytail\band` no Windows) | Pasta dos CSVs |
| `--timeout` | `15` (s) | Timeout de cada envio |
| `--print` | — | Imprime o JSON e sai sem enviar |
| `--nao-verificar-tls` | verificação ativa | Desabilita a validação do certificado |
| `-d/--debug`, `-q/--quiet` | — | Verbosidade |

Configuração persistente (`ponypush config get|set|add|unset|list|path`,
`--global` padrão ou `--local` → `./.ponypush.conf`). Chaves: `url token
maquina band-dir timeout verificar-tls`. Precedência: CLI > local > global >
env > padrões.

Códigos de saída: `0` ok (inclusive sem sessões), `2` configuração/leitura,
`3` envio recusado ou falha de rede.

## Agendamento

**Linux** — o instalador baixa a release (SHA256 conferido) para
`~/.local/bin` e cria os timers do usuário `ponypush.timer` (de hora em hora)
e `ponypush-update.timer` (diário):

```bash
curl -fsSL https://raw.githubusercontent.com/vandinho10/ponypush/main/scripts/install.sh | bash
```

**Windows** — copie `ponypush-windows-amd64.exe` para
`%LOCALAPPDATA%\ponypush\ponypush.exe`, grave o token e agende:

```bat
%LOCALAPPDATA%\ponypush\ponypush.exe config set token SEU_TOKEN
schtasks /Create /SC HOURLY /TN ponypush /TR "\"%LOCALAPPDATA%\ponypush\ponypush.exe\" --quiet" /F
```

O executável Windows é subsystem GUI, então a tarefa roda totalmente oculta
(sem janela de console). Para atualizar depois: `ponypush.exe update`.

> **Apps empacotados (MSIX):** não instale a partir do terminal de um app
> empacotado como MSIX — ele virtualiza `%LOCALAPPDATA%`/`%APPDATA%` para a
> pasta `Packages\…\LocalCache` do pacote, e a tarefa agendada não enxerga o
> `.exe` nem o token. Use um terminal comum (cmd/PowerShell).

## Payload

```json
{"maquina":"NB-01","sessoes":[{"key":"25e0a816-1794749433852","started":"2026-11-15 09:30","project":"docker_projects","cost_usd":"24.25","...":"..."}]}
```

Contrato completo em `apps/api/docs/DOC_INVENTORY.md` (`POST /api/inventory/ponytail`).
