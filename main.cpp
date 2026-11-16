#include "ponypush.hpp"
#include "selfupdate.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// O build Windows é subsystem GUI (ver Makefile): rodando como tarefa
// agendada não cria janela de console, então não pisca. Quando lançado de um
// terminal, reaproveita o console do pai para que --version/--print/config/
// update ainda mostrem saída; sem console pai (tarefa agendada) segue mudo.
static void anexar_console_do_pai()
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    FILE *f = nullptr;
    f = std::freopen("CONOUT$", "w", stdout);
    f = std::freopen("CONOUT$", "w", stderr);
    f = std::freopen("CONIN$", "r", stdin);
    (void)f;
}
#endif

int main(int argc, char *argv[])
{
#if defined(_WIN32)
    anexar_console_do_pai();
#endif

    // Subcomando de configuração persistente (estilo git config).
    if (argc > 1 && std::string(argv[1]) == "config")
        return ponypush::comando_config(argc, argv);

    // Autoatualização via GitHub Releases (SHA256 obrigatório).
    if (argc > 1 && std::string(argv[1]) == "update")
    {
        selfupdate::Contexto ctx;
        ctx.app = "ponypush";
        ctx.versao = PONYPUSH_VERSION;
        ctx.repo_padrao = "vandinho10/ponypush";
        ctx.env_repo = "PONYPUSH_UPDATE_REPO";
        ctx.http_get = [](const std::string &url, std::string &erro) {
            return ponypush::http_get(url, 120, erro);
        };
        return selfupdate::comando_update(ctx, argc, argv, 2);
    }

    ponypush::Config cfg;
    if (!ponypush::parse_args(argc, argv, cfg))
    {
        ponypush::print_help(argv[0]);
        return EXIT_FAILURE;
    }

    if (cfg.ajuda)
    {
        ponypush::print_help(argv[0]);
        return EXIT_SUCCESS;
    }

    if (cfg.versao)
    {
        std::cout << "ponypush v" << PONYPUSH_VERSION << "\n";
        return EXIT_SUCCESS;
    }

    // Precedência: CLI > .ponypush.conf local > config global > env > padrões.
    ponypush::apply_env(cfg);
    const ponypush::NivelConfig global =
        ponypush::carregar_nivel(ponypush::caminho_config_global());
    const std::filesystem::path local_path =
        ponypush::descobrir_config_local(std::filesystem::current_path());
    const ponypush::NivelConfig local = ponypush::carregar_nivel(local_path);
    ponypush::aplicar_config(cfg, ponypush::mesclar_niveis(global, local));

    return ponypush::run(cfg);
}
