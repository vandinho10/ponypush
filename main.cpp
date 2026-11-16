#include "ponypush.hpp"
#include "selfupdate.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char *argv[])
{
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
