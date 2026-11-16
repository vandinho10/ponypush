// ponypush — Agente que envia as sessões do ponytail para a API central.
//
// Lê os CSVs do band do ponytail ($XDG_CONFIG_HOME|~/.config/ponytail/band,
// %USERPROFILE%\.config\ponytail\band no Windows — mesmo caminho do plugin) e
// envia cada linha para POST {url} com Authorization: Bearer <token de coleta>
// (módulo host_inventory da API Go, mesmo token do invpush).
//
// Payload: {"maquina":"<host>","sessoes":[{"<coluna>":"<célula>",...}]}
// As células vão como strings, indexadas pelo cabeçalho do CSV: o servidor
// interpreta as colunas conhecidas, então colunas novas do ponytail não quebram
// o agente. O upsert é por máquina + key: reenviar tudo é idempotente.
#ifndef PONYPUSH_HPP
#define PONYPUSH_HPP

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ponypush
{

// Versão do agente (injetada pelo Makefile via -DPONYPUSH_VERSION).
#ifndef PONYPUSH_VERSION
#define PONYPUSH_VERSION "dev"
#endif

// Endpoint padrão de ingestão (pode ser sobrescrito por flag/env/config).
extern const char *const kUrlPadrao;

// Sessões por requisição (a API aceita no máximo 1000).
constexpr size_t kLote = 500;

// Uma linha do CSV: coluna -> célula.
using Sessao = std::map<std::string, std::string>;

// Configuração resolvida: CLI > config local > config global > env > padrões.
struct Config
{
    std::string url;      // env PONYPUSH_API_URL
    std::string token;    // env INVENTORY_COLLECTION_API_TOKEN
    std::string maquina;  // vazio = hostname do sistema
    std::string band_dir; // vazio = diretório padrão do ponytail
    int timeout_s = 15;
    bool verificar_tls = true;
    bool imprimir = false;   // --print: mostra o payload e sai sem enviar
    bool silencioso = false; // -q/--quiet
    bool debug = false;      // -d/--debug: dumpa payload/resposta crua
    bool ajuda = false;
    bool versao = false;

    bool cli_url = false;
    bool cli_token = false;
    bool cli_maquina = false;
    bool cli_band_dir = false;
    bool cli_timeout = false;
    bool cli_verificar_tls = false;
};

// Resultado de uma requisição HTTP.
struct ResultadoEnvio
{
    bool ok = false;
    long status_http = 0;
    std::string corpo;
    std::string erro;
};

// ---- CLI ----
bool parse_args(int argc, char *argv[], Config &cfg);
void apply_env(Config &cfg);
void print_help(const char *prog);

// ---- Coleta ----
std::string hostname_do_sistema();

// Diretório do band do ponytail: $XDG_CONFIG_HOME/ponytail/band ou
// <home>/.config/ponytail/band (home = HOME; USERPROFILE no Windows).
std::filesystem::path band_dir_padrao();

// Lê todos os *.csv do diretório (ordem alfabética). Linhas sem `key` são
// ignoradas. `erro` preenchido se o diretório não puder ser lido.
std::vector<Sessao> ler_band(const std::filesystem::path &dir, std::string &erro);

// GET HTTPS com TLS verificado (usado pelo `ponypush update`); falha => vazio + erro.
std::string http_get(const std::string &url, int timeout_s, std::string &erro);

// Log com timestamp: [YYYY-MM-DD HH:MM:SS] mensagem.
std::string agora_log();
void log_info(const std::string &msg);
void log_debug(const Config &cfg, const std::string &msg);
void log_erro(const std::string &msg);

// ---- Funções puras (cobertas pela suíte de testes) ----
std::string json_escape(const std::string &s);

// Parser CSV RFC 4180 (mesmo do board do ponytail): aspas duplas, "" escapa
// aspas, vírgula e quebra de linha (\n ou \r\n) dentro de aspas.
std::vector<std::vector<std::string>> parse_csv(const std::string &texto);

// Converte o texto de um CSV em sessões (primeira linha = cabeçalho).
std::vector<Sessao> sessoes_do_csv(const std::string &texto);

// ---- Payload e envio ----
std::string montar_payload(const std::string &maquina, const std::vector<Sessao> &sessoes,
                           size_t inicio, size_t fim);
ResultadoEnvio enviar(const Config &cfg, const std::string &payload);

// Orquestração principal (retorna código de saída do processo).
int run(Config cfg);

// ------------------------- configuração persistente ------------------------

// Nível de configuração: conjunto ordenado de chaves com valores múltiplos.
struct NivelConfig
{
    std::vector<std::pair<std::string, std::vector<std::string>>> entradas;

    bool vazia() const;
    const std::vector<std::string> *busca(const std::string &chave) const;
    void define(const std::string &chave, std::vector<std::string> valores);
    void acrescenta(const std::string &chave, const std::vector<std::string> &valores);
    void remove(const std::string &chave);
};

bool chave_valida(const std::string &chave);
bool valor_bool(const std::string &valor, bool padrao);

// Global: $PONYPUSH_CONFIG_HOME/ponypush/config (teste/sandbox) ou o diretório
// de config do usuário ($XDG_CONFIG_HOME|~/.config; %APPDATA% no Windows).
std::filesystem::path caminho_config_global();

// Local: primeiro `.ponypush.conf` a partir de `inicio` subindo até a raiz.
std::filesystem::path descobrir_config_local(const std::filesystem::path &inicio);

NivelConfig carregar_nivel(const std::filesystem::path &arquivo);
bool salvar_nivel(const std::filesystem::path &arquivo, const NivelConfig &nivel);
NivelConfig mesclar_niveis(const NivelConfig &global, const NivelConfig &local);
void aplicar_config(Config &cfg, const NivelConfig &efetivo);
int comando_config(int argc, char *argv[]);

} // namespace ponypush

#endif // PONYPUSH_HPP
