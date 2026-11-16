// ponypush — leitura do band do ponytail, montagem do payload e envio HTTP.
//
// Transporte (herdado do invpush): libcurl (POSIX) ou WinHTTP nativo
// (Windows/Schannel). Nas builds Windows o binário fica 100% estático.
#include "ponypush.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#include <unistd.h>
#endif

namespace ponypush
{

const char *const kUrlPadrao = "https://api.vandinho.com.br/api/inventory/ponytail";

// ---------------------------------------------------------------- JSON utils

std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

// Extrai o valor string bruto de uma chave simples no corpo JSON de resposta.
static std::string json_campo_string(const std::string &corpo, const char *chave)
{
    std::string agulha = "\"" + std::string(chave) + "\"";
    const auto pos = corpo.find(agulha);
    if (pos == std::string::npos) return {};
    const auto dp = corpo.find(':', pos + agulha.size());
    if (dp == std::string::npos) return {};
    auto ini = corpo.find('"', dp + 1);
    if (ini == std::string::npos) return {};
    ++ini;
    std::string out;
    for (size_t i = ini; i < corpo.size(); ++i)
    {
        if (corpo[i] == '\\' && i + 1 < corpo.size())
        {
            out += corpo[i];
            out += corpo[i + 1];
            ++i;
            continue;
        }
        if (corpo[i] == '"') break;
        out += corpo[i];
    }
    return out;
}

// 2xx só conta como sucesso com o marcador da API: corpo vazio indica proxy
// errado, não ingestão (mesma regra do invpush).
static void validar_2xx(ResultadoEnvio &r)
{
    r.ok = r.status_http >= 200 && r.status_http < 300;
    if (!r.ok)
    {
        r.erro = "HTTP " + std::to_string(r.status_http);
        return;
    }
    const std::string status_json = json_campo_string(r.corpo, "status");
    r.ok = status_json == "success" || status_json == "ok";
    if (!r.ok) r.erro = "HTTP 2xx sem marcador de sucesso: " + r.corpo;
}

// ------------------------------------------------------------------- Logging

std::string agora_log()
{
    std::tm tm_buf{};
    std::time_t agora = std::time(nullptr);
#if defined(_WIN32)
    localtime_s(&tm_buf, &agora);
#else
    localtime_r(&agora, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return buf;
}

void log_info(const std::string &msg)
{
    std::cout << "[" << agora_log() << "] " << msg << "\n";
    std::cout.flush();
}

void log_debug(const Config &cfg, const std::string &msg)
{
    if (!cfg.debug) return;
    std::cout << "[" << agora_log() << "] [DEBUG] " << msg << "\n";
    std::cout.flush();
}

void log_erro(const std::string &msg)
{
    std::cerr << "[" << agora_log() << "] " << msg << "\n";
}

// ----------------------------------------------------------------------- CSV

std::vector<std::vector<std::string>> parse_csv(const std::string &texto)
{
    std::vector<std::vector<std::string>> linhas;
    std::vector<std::string> linha;
    std::string campo;
    bool aspas = false;
    for (size_t i = 0; i < texto.size(); ++i)
    {
        const char c = texto[i];
        if (aspas)
        {
            if (c == '"' && i + 1 < texto.size() && texto[i + 1] == '"')
            {
                campo += '"';
                ++i;
            }
            else if (c == '"')
                aspas = false;
            else
                campo += c;
        }
        else if (c == '"')
            aspas = true;
        else if (c == ',')
        {
            linha.push_back(std::move(campo));
            campo.clear();
        }
        else if (c == '\n' || c == '\r')
        {
            if (c == '\r' && i + 1 < texto.size() && texto[i + 1] == '\n') ++i;
            linha.push_back(std::move(campo));
            campo.clear();
            linhas.push_back(std::move(linha));
            linha.clear();
        }
        else
            campo += c;
    }
    if (!campo.empty() || !linha.empty())
    {
        linha.push_back(std::move(campo));
        linhas.push_back(std::move(linha));
    }
    return linhas;
}

std::vector<Sessao> sessoes_do_csv(const std::string &texto)
{
    std::vector<Sessao> out;
    const auto linhas = parse_csv(texto);
    if (linhas.empty()) return out;
    const std::vector<std::string> &cab = linhas.front();
    for (size_t l = 1; l < linhas.size(); ++l)
    {
        Sessao s;
        for (size_t c = 0; c < cab.size() && c < linhas[l].size(); ++c)
            s[cab[c]] = linhas[l][c];
        const auto k = s.find("key");
        if (k != s.end() && !k->second.empty()) out.push_back(std::move(s));
    }
    return out;
}

// ------------------------------------------------------------------- Coleta

#if defined(_WIN32)

namespace
{

std::wstring wide_utf8(const std::string &s)
{
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(need > 0 ? need - 1 : 0), L'\0');
    if (need > 1)
    {
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), need);
    }
    return out;
}

// Garante uma única inicialização do Winsock por processo (gethostname).
struct WsaGuard
{
    bool ok = false;
    WsaGuard()
    {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WsaGuard()
    {
        if (ok) WSACleanup();
    }
};

WsaGuard &wsa()
{
    static WsaGuard g;
    return g;
}

// Variável de ambiente como caminho (wide: usuários com acento no nome).
std::filesystem::path env_path(const wchar_t *nome)
{
    const wchar_t *v = _wgetenv(nome);
    return (v != nullptr && *v != L'\0') ? std::filesystem::path(v) : std::filesystem::path();
}

} // namespace

std::string hostname_do_sistema()
{
    (void)wsa();
    char buf[256] = {0};
    if (gethostname(buf, sizeof(buf)) != 0) return {};
    return buf;
}

std::filesystem::path band_dir_padrao()
{
    std::filesystem::path base = env_path(L"XDG_CONFIG_HOME");
    if (base.empty())
    {
        const std::filesystem::path home = env_path(L"USERPROFILE");
        if (home.empty()) return {};
        base = home / ".config";
    }
    return base / "ponytail" / "band";
}

#else // POSIX

std::string hostname_do_sistema()
{
    char buf[256] = {0};
    if (gethostname(buf, sizeof(buf)) != 0) return {};
    return buf;
}

std::filesystem::path band_dir_padrao()
{
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && *xdg != '\0') return std::filesystem::path(xdg) / "ponytail" / "band";
    const char *home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') return {};
    return std::filesystem::path(home) / ".config" / "ponytail" / "band";
}

#endif

std::vector<Sessao> ler_band(const std::filesystem::path &dir, std::string &erro)
{
    std::vector<Sessao> out;
    std::error_code ec;
    std::vector<std::filesystem::path> arquivos;
    for (const auto &e : std::filesystem::directory_iterator(dir, ec))
    {
        if (e.is_regular_file(ec) && e.path().extension() == ".csv") arquivos.push_back(e.path());
    }
    if (ec)
    {
        erro = "nao foi possivel ler " + dir.u8string() + ": " + ec.message();
        return out;
    }
    std::sort(arquivos.begin(), arquivos.end());
    for (const auto &arq : arquivos)
    {
        std::ifstream in(arq, std::ios::binary);
        if (!in)
        {
            erro = "nao foi possivel abrir " + arq.u8string();
            return {};
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        auto sessoes = sessoes_do_csv(ss.str());
        out.insert(out.end(), std::make_move_iterator(sessoes.begin()),
                   std::make_move_iterator(sessoes.end()));
    }
    return out;
}

// ------------------------------------------------------------ Payload/envio

std::string montar_payload(const std::string &maquina, const std::vector<Sessao> &sessoes,
                           size_t inicio, size_t fim)
{
    std::string out = "{\"maquina\":\"" + json_escape(maquina) + "\",\"sessoes\":[";
    fim = std::min(fim, sessoes.size());
    for (size_t i = inicio; i < fim; ++i)
    {
        if (i > inicio) out += ',';
        out += '{';
        bool primeiro = true;
        for (const auto &[col, val] : sessoes[i])
        {
            if (!primeiro) out += ',';
            primeiro = false;
            out += '"' + json_escape(col) + "\":\"" + json_escape(val) + '"';
        }
        out += '}';
    }
    out += "]}";
    return out;
}

#if defined(_WIN32)

ResultadoEnvio enviar(const Config &cfg, const std::string &payload)
{
    ResultadoEnvio r;

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    std::wstring wurl = wide_utf8(cfg.url);
    std::wstring host, caminho;
    uc.lpszHostName = nullptr;
    uc.dwHostNameLength = 1;
    uc.lpszUrlPath = nullptr;
    uc.dwUrlPathLength = 1;
    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc))
    {
        r.erro = "URL invalida";
        return r;
    }
    host.assign(uc.lpszHostName, uc.dwHostNameLength);
    caminho.assign(uc.lpszUrlPath ? uc.lpszUrlPath : L"/", uc.dwUrlPathLength);
    if (caminho.empty()) caminho = L"/";
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    const INTERNET_PORT porta = uc.nPort;

    HINTERNET sessao = WinHttpOpen(wide_utf8("ponypush/" PONYPUSH_VERSION).c_str(),
                                   WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (sessao == nullptr)
    {
        r.erro = "falha WinHttpOpen";
        return r;
    }

    HINTERNET conexao = WinHttpConnect(sessao, host.c_str(), porta, 0);
    HINTERNET requisicao = nullptr;
    do
    {
        if (conexao == nullptr)
        {
            r.erro = "falha WinHttpConnect";
            break;
        }
        requisicao = WinHttpOpenRequest(
            conexao, L"POST", caminho.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            https ? WINHTTP_FLAG_SECURE : 0);
        if (requisicao == nullptr)
        {
            r.erro = "falha WinHttpOpenRequest";
            break;
        }

        const int timeout_ms = cfg.timeout_s * 1000;
        WinHttpSetTimeouts(requisicao, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

        if (!cfg.verificar_tls)
        {
            DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                          SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(requisicao, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
        }

        const std::wstring cabecalhos =
            L"Content-Type: application/json\r\n"
            L"Authorization: Bearer " + wide_utf8(cfg.token) + L"\r\n";
        const DWORD total = static_cast<DWORD>(payload.size());
        if (!WinHttpSendRequest(requisicao, cabecalhos.c_str(),
                                static_cast<DWORD>(cabecalhos.size()),
                                const_cast<char *>(payload.data()), total, total, 0))
        {
            r.erro = "falha WinHttpSendRequest (" + std::to_string(GetLastError()) + ")";
            break;
        }
        if (!WinHttpReceiveResponse(requisicao, nullptr))
        {
            r.erro = "falha WinHttpReceiveResponse (" + std::to_string(GetLastError()) + ")";
            break;
        }

        DWORD codigo = 0, tam = sizeof(codigo);
        WinHttpQueryHeaders(requisicao,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &codigo, &tam,
                            WINHTTP_NO_HEADER_INDEX);
        r.status_http = static_cast<long>(codigo);

        std::string corpo;
        for (;;)
        {
            DWORD disponivel = 0;
            if (!WinHttpQueryDataAvailable(requisicao, &disponivel) || disponivel == 0) break;
            std::vector<char> chunk(disponivel);
            DWORD lido = 0;
            if (!WinHttpReadData(requisicao, chunk.data(), disponivel, &lido) || lido == 0) break;
            corpo.append(chunk.data(), lido);
        }
        r.corpo = std::move(corpo);
        validar_2xx(r);
    } while (false);

    if (requisicao != nullptr) WinHttpCloseHandle(requisicao);
    if (conexao != nullptr) WinHttpCloseHandle(conexao);
    WinHttpCloseHandle(sessao);
    return r;
}

// GET com TLS verificado; status fora de 2xx => corpo vazio + erro.
std::string http_get(const std::string &url, int timeout_s, std::string &erro)
{
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    const std::wstring wurl = wide_utf8(url);
    std::wstring host, caminho;
    uc.dwHostNameLength = 1;
    uc.dwUrlPathLength = 1;
    uc.dwExtraInfoLength = 1;
    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc))
    {
        erro = "URL invalida";
        return {};
    }
    host.assign(uc.lpszHostName, uc.dwHostNameLength);
    caminho.assign(uc.lpszUrlPath ? uc.lpszUrlPath : L"/", uc.dwUrlPathLength);
    if (uc.lpszExtraInfo != nullptr) caminho.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

    HINTERNET sessao = WinHttpOpen(wide_utf8("ponypush/" PONYPUSH_VERSION).c_str(),
                                   WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (sessao == nullptr)
    {
        erro = "falha WinHttpOpen (" + std::to_string(GetLastError()) + ")";
        return {};
    }
    std::string saida;

    HINTERNET conexao = WinHttpConnect(sessao, host.c_str(), uc.nPort, 0);
    HINTERNET req = nullptr;
    do
    {
        if (conexao == nullptr) { erro = "falha WinHttpConnect"; break; }
        req = WinHttpOpenRequest(conexao, L"GET", caminho.c_str(), nullptr,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (req == nullptr) { erro = "falha WinHttpOpenRequest"; break; }
        WinHttpSetTimeouts(req, timeout_s * 1000, timeout_s * 1000,
                           timeout_s * 1000, timeout_s * 1000);
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, nullptr))
        {
            erro = "falha na requisicao (" + std::to_string(GetLastError()) + ")";
            break;
        }

        DWORD codigo = 0, tam = sizeof(codigo);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &codigo, &tam,
                            WINHTTP_NO_HEADER_INDEX);
        if (codigo < 200 || codigo >= 300) { erro = "HTTP " + std::to_string(codigo); break; }

        std::string corpo;
        for (;;)
        {
            DWORD disponivel = 0;
            if (!WinHttpQueryDataAvailable(req, &disponivel) || disponivel == 0) break;
            std::vector<char> chunk(disponivel);
            DWORD lido = 0;
            if (!WinHttpReadData(req, chunk.data(), disponivel, &lido) || lido == 0) break;
            corpo.append(chunk.data(), lido);
        }
        saida = std::move(corpo);
    } while (false);

    if (req != nullptr) WinHttpCloseHandle(req);
    if (conexao != nullptr) WinHttpCloseHandle(conexao);
    WinHttpCloseHandle(sessao);
    return saida;
}

#else

namespace
{

size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

bool curl_pronto()
{
    static const bool ok = [] { return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; }();
    return ok;
}

// POST (payload != nullptr, com token) ou GET; devolve o CURLcode.
CURLcode http_request(const char *url, const std::string *payload, const std::string *token,
                      int timeout_s, bool verificar_tls, long *status, std::string *corpo,
                      const char **erro)
{
    CURL *h = curl_easy_init();
    if (h == nullptr)
    {
        *erro = "falha ao criar handle curl";
        return CURLE_FAILED_INIT;
    }
    curl_slist *cabecalhos = nullptr;
    cabecalhos = curl_slist_append(cabecalhos, "Content-Type: application/json");
    if (token != nullptr)
    {
        const std::string auth = "Authorization: Bearer " + *token;
        cabecalhos = curl_slist_append(cabecalhos, auth.c_str());
    }

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, corpo);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_s) * 1000L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "ponypush/" PONYPUSH_VERSION);
    if (!verificar_tls)
    {
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (payload != nullptr)
    {
        curl_easy_setopt(h, CURLOPT_POST, 1L);
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, payload->c_str());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload->size()));
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, cabecalhos);
    }

    const CURLcode res = curl_easy_perform(h);
    if (res == CURLE_OK)
    {
        long codigo = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &codigo);
        *status = codigo;
    }
    else
    {
        *erro = curl_easy_strerror(res);
    }
    curl_slist_free_all(cabecalhos);
    curl_easy_cleanup(h);
    return res;
}

} // namespace

ResultadoEnvio enviar(const Config &cfg, const std::string &payload)
{
    ResultadoEnvio r;
    if (!curl_pronto())
    {
        r.erro = "falha ao inicializar libcurl";
        return r;
    }
    const char *erro = nullptr;
    const CURLcode res = http_request(cfg.url.c_str(), &payload, &cfg.token, cfg.timeout_s,
                                      cfg.verificar_tls, &r.status_http, &r.corpo, &erro);
    if (res == CURLE_OK)
        validar_2xx(r);
    else
        r.erro = erro != nullptr ? erro : "erro curl";
    return r;
}

std::string http_get(const std::string &url, int timeout_s, std::string &erro)
{
    if (!curl_pronto())
    {
        erro = "falha ao inicializar libcurl";
        return {};
    }
    long status = 0;
    std::string corpo;
    const char *e = nullptr;
    const CURLcode res = http_request(url.c_str(), nullptr, nullptr, timeout_s, true,
                                      &status, &corpo, &e);
    if (res != CURLE_OK)
    {
        erro = e != nullptr ? e : "erro curl";
        return {};
    }
    if (status < 200 || status >= 300)
    {
        erro = "HTTP " + std::to_string(status);
        return {};
    }
    return corpo;
}

#endif

// ------------------------------------------------------------------- CLI/run

void print_help(const char *prog)
{
    std::cout
        << "ponypush v" << PONYPUSH_VERSION << " — envio das sessoes do ponytail para a API central\n\n"
        << "Uso: " << prog << " [opcoes]\n"
        << "     " << prog << " config [get|set|add|unset|list|path] ...\n"
        << "     " << prog << " update [--check] [--quiet]\n\n"
        << "Opcoes:\n"
        << "  --url URL            Endpoint de ingestao (padrao: " << kUrlPadrao << ")\n"
        << "  --token TOKEN        Bearer token de coleta (env INVENTORY_COLLECTION_API_TOKEN)\n"
        << "  --maquina NOME       Sobrescreve o hostname reportado\n"
        << "  --band-dir DIR       Pasta dos CSVs (padrao: " << band_dir_padrao().u8string() << ")\n"
        << "  --timeout SEG        Timeout de cada envio em segundos (padrao: 15)\n"
        << "  --print              Mostra o payload JSON e sai sem enviar\n"
        << "  --nao-verificar-tls  Desabilita verificacao do certificado TLS\n"
        << "  -d, --debug          Dumpa payload e resposta crua\n"
        << "  -q, --quiet          Suprime mensagens de progresso\n"
        << "  -h, --help           Esta ajuda\n"
        << "  --version            Versao do agente\n";
}

bool parse_args(int argc, char *argv[], Config &cfg)
{
    auto proximo = [&](int &i) -> const char * {
        if (i + 1 >= argc) return nullptr;
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--url" || arg == "--token" || arg == "--maquina" || arg == "--band-dir" ||
            arg == "--timeout")
        {
            const char *v = proximo(i);
            if (v == nullptr) return false;
            if (arg == "--url") { cfg.url = v; cfg.cli_url = true; }
            else if (arg == "--token") { cfg.token = v; cfg.cli_token = true; }
            else if (arg == "--maquina") { cfg.maquina = v; cfg.cli_maquina = true; }
            else if (arg == "--band-dir") { cfg.band_dir = v; cfg.cli_band_dir = true; }
            else
            {
                char *fim = nullptr;
                const long t = std::strtol(v, &fim, 10);
                if (fim == v || *fim != '\0' || t <= 0 || t > 3600) return false;
                cfg.timeout_s = static_cast<int>(t);
                cfg.cli_timeout = true;
            }
        }
        else if (arg == "--print") cfg.imprimir = true;
        else if (arg == "--nao-verificar-tls") { cfg.verificar_tls = false; cfg.cli_verificar_tls = true; }
        else if (arg == "-q" || arg == "--quiet") cfg.silencioso = true;
        else if (arg == "-d" || arg == "--debug") cfg.debug = true;
        else if (arg == "-h" || arg == "--help" || arg == "--ajuda") cfg.ajuda = true;
        else if (arg == "--version" || arg == "--versao") cfg.versao = true;
        else
        {
            std::cerr << "argumento desconhecido: " << arg << "\n";
            return false;
        }
    }
    return true;
}

void apply_env(Config &cfg)
{
    if (cfg.url.empty())
    {
        const char *e = std::getenv("PONYPUSH_API_URL");
        cfg.url = (e != nullptr && *e != '\0') ? e : kUrlPadrao;
    }
    if (cfg.token.empty())
    {
        if (const char *e = std::getenv("INVENTORY_COLLECTION_API_TOKEN"); e != nullptr && *e != '\0')
            cfg.token = e;
    }
}

int run(Config cfg)
{
    const std::string hostname = !cfg.maquina.empty() ? cfg.maquina : hostname_do_sistema();
    const std::filesystem::path dir =
        !cfg.band_dir.empty() ? std::filesystem::u8path(cfg.band_dir) : band_dir_padrao();
    if (hostname.empty())
    {
        log_erro("ERRO FATAL: hostname indisponivel (use --maquina).");
        return 2;
    }

    std::string erro;
    const std::vector<Sessao> sessoes = ler_band(dir, erro);
    if (!erro.empty())
    {
        log_erro("ERRO FATAL: " + erro + " (ponytail instalado? use --band-dir).");
        return 2;
    }

    if (cfg.imprimir)
    {
        // Modo teste: payload completo com leitura real, sem rede nem token.
        std::cout << montar_payload(hostname, sessoes, 0, sessoes.size()) << "\n";
        return 0;
    }

    if (cfg.token.empty())
    {
        log_erro("ERRO FATAL: token de coleta ausente (use --token, `ponypush config set token` "
                 "ou INVENTORY_COLLECTION_API_TOKEN).");
        return 2;
    }

    if (sessoes.empty())
    {
        if (!cfg.silencioso) log_info("Nenhuma sessao em " + dir.u8string() + "; nada a enviar.");
        return 0;
    }

    if (!cfg.silencioso)
        log_info("Enviando " + std::to_string(sessoes.size()) + " sessoes de " + hostname +
                 " para " + cfg.url + "...");

    for (size_t ini = 0; ini < sessoes.size(); ini += kLote)
    {
        const std::string payload = montar_payload(hostname, sessoes, ini, ini + kLote);
        log_debug(cfg, "--- PAYLOAD ---\n" + payload + "\n---");
        const ResultadoEnvio res = enviar(cfg, payload);
        log_debug(cfg, "Status " + std::to_string(res.status_http) + " | " + res.corpo);
        if (!res.ok)
        {
            if (res.status_http > 0)
                log_erro("ERRO DO SERVIDOR: Codigo " + std::to_string(res.status_http) +
                         (res.corpo.empty() ? "" : (" | " + res.corpo)));
            else
                log_erro("FALHA DE CONEXAO: " + (res.erro.empty() ? std::string("erro desconhecido") : res.erro));
            return 3;
        }
    }

    if (!cfg.silencioso) log_info("SUCESSO: sessoes aceitas pelo servidor.");
    return 0;
}

} // namespace ponypush
