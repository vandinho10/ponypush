// Suíte de testes do ponypush (framework mínimo, table-driven).
// Cobre: parser CSV (mesmo formato do ponytail), conversão em sessões,
// leitura do diretório band, payload JSON, argumentos e configuração.
#include "test_framework.hpp"

#include "ponypush.hpp"

#include <cstdlib>
#include <fstream>

using namespace ponypush;

static const char *kCsv =
    "key,started,project,name,cost_usd,turns,agent_minutes,tokens_total,tokens_output,"
    "reuse_pct,reused,new,callers_open,tests,level,model\n"
    "a-1,2026-11-08 09:53,proj,\"Análise, com \"\"aspas\"\"\",33.33,37,0.3,837422,202,,0,0,0,not run,full,m\n"
    "\n"
    ",sem chave,,,,,,,,,,,,,,\n"
    "b-2,2026-11-09 10:00,proj,\"linha\r\nquebrada\",1.00,1,0.1,10,1,50,1,2,0,passed,lite,m\r\n";

TEST("parse_csv segue RFC 4180 como o board do ponytail")
{
    const auto l = parse_csv("a,\"b,c\",\"d\"\"e\"\n1,2\r\n,\n");
    CHECK(l.size() == 3);
    CHECK(l[0].size() == 3 && l[0][1] == "b,c" && l[0][2] == "d\"e");
    CHECK(l[1].size() == 2 && l[1][1] == "2");
    CHECK(l[2].size() == 2 && l[2][0].empty() && l[2][1].empty());
    // Sem quebra de linha final ainda fecha a última linha.
    CHECK(parse_csv("x,y").size() == 1);
    CHECK(parse_csv("").empty());
}

TEST("sessoes_do_csv mapeia cabeçalho e descarta linhas sem key")
{
    const auto s = sessoes_do_csv(kCsv);
    CHECK(s.size() == 2);
    CHECK(s[0].at("key") == "a-1");
    CHECK(s[0].at("name") == "Análise, com \"aspas\"");
    CHECK(s[0].at("reuse_pct").empty());
    CHECK(s[1].at("name") == "linha\r\nquebrada");
    CHECK(s[1].at("model") == "m");
    CHECK(sessoes_do_csv("key,name\n").empty());
}

TEST("montar_payload gera JSON com escape e respeita o intervalo do lote")
{
    std::vector<Sessao> s = {{{"key", "a"}, {"name", "x\"y"}}, {{"key", "b"}}, {{"key", "c"}}};
    CHECK(montar_payload("NB-01", s, 0, 1) ==
          "{\"maquina\":\"NB-01\",\"sessoes\":[{\"key\":\"a\",\"name\":\"x\\\"y\"}]}");
    CHECK(montar_payload("h", s, 1, 99) ==
          "{\"maquina\":\"h\",\"sessoes\":[{\"key\":\"b\"},{\"key\":\"c\"}]}");
    CHECK(montar_payload("h", s, 3, 9) == "{\"maquina\":\"h\",\"sessoes\":[]}");
}

TEST("ler_band lê todos os *.csv do diretório e ignora o resto")
{
    const auto dir = std::filesystem::temp_directory_path() / "ponypush_test_band";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "p1.csv", std::ios::binary) << kCsv;
    std::ofstream(dir / "p2.csv", std::ios::binary) << "key,name\nc-3,outra\n";
    std::ofstream(dir / "board.html", std::ios::binary) << "<html>";

    std::string erro;
    const auto s = ler_band(dir, erro);
    CHECK(erro.empty());
    CHECK(s.size() == 3);
    CHECK(s[2].at("key") == "c-3");

    ler_band(dir / "nao-existe", erro);
    CHECK(!erro.empty());
    std::filesystem::remove_all(dir);
}

TEST("band_dir_padrao segue XDG_CONFIG_HOME como o plugin")
{
#ifdef _WIN32
    _putenv_s("XDG_CONFIG_HOME", "C:\\cfg");
    CHECK(band_dir_padrao() == std::filesystem::path("C:\\cfg") / "ponytail" / "band");
    _putenv_s("XDG_CONFIG_HOME", "");
#else
    setenv("XDG_CONFIG_HOME", "/cfg", 1);
    CHECK(band_dir_padrao() == std::filesystem::path("/cfg/ponytail/band"));
    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", "/home/u", 1);
    CHECK(band_dir_padrao() == std::filesystem::path("/home/u/.config/ponytail/band"));
#endif
}

TEST("parse_args aceita opções válidas e rejeita inválidas")
{
    {
        Config c;
        const char *a[] = {"p", "--url", "https://x/y", "--band-dir", "/b", "--timeout", "30", "-q"};
        CHECK(parse_args(8, const_cast<char **>(a), c));
        CHECK(c.url == "https://x/y" && c.cli_url);
        CHECK(c.band_dir == "/b" && c.cli_band_dir);
        CHECK(c.timeout_s == 30 && c.silencioso);
    }
    struct Caso { std::vector<const char *> args; };
    const Caso invalidos[] = {
        {{"p", "--timeout", "0"}}, {{"p", "--timeout", "abc"}},
        {{"p", "--url"}}, {{"p", "--desconhecida"}},
    };
    for (const auto &cs : invalidos)
    {
        Config c;
        CHECK(!parse_args(static_cast<int>(cs.args.size()), const_cast<char **>(cs.args.data()), c));
    }
}

TEST("aplicar_config preenche só o que não veio da CLI")
{
    NivelConfig n;
    n.define("url", {"https://cfg"});
    n.define("band-dir", {"/cfg/band"});
    n.define("timeout", {"20"});
    n.define("verificar-tls", {"false"});

    Config c;
    c.url = "https://cli";
    c.cli_url = true;
    aplicar_config(c, n);
    CHECK(c.url == "https://cli");
    CHECK(c.band_dir == "/cfg/band");
    CHECK(c.timeout_s == 20);
    CHECK(!c.verificar_tls);
    CHECK(chave_valida("band-dir") && !chave_valida("ip-publico"));
}

int main()
{
    return tests::run_all();
}
